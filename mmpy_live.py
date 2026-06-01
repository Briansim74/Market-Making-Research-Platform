
import time
import hmac
import hashlib
import requests
import websocket
import threading
import json

# 1. Binance Broker (REAL API layer) 100–2000ms variable
class BinanceBroker:
    def __init__(self, api_key, api_secret, base_url):
        self.api_key = api_key
        self.api_secret = api_secret
        self.base_url = base_url

    # -------------------------
    # REST signing
    # -------------------------
    def _sign(self, params: dict):
        query = "&".join([f"{k}={v}" for k, v in params.items()])
        return hmac.new(
            self.api_secret.encode(),
            query.encode(),
            hashlib.sha256
        ).hexdigest()

    # -------------------------
    # Place order
    # -------------------------
    def place_limit(self, symbol, side, qty, price, order_type="LIMIT"):
        endpoint = "/api/v3/order"

        params = {
            "symbol": symbol,
            "side": side,
            "type": order_type,
            "timeInForce": "GTC",
            "quantity": qty,
            "price": price,
            "timestamp": int(time.time() * 1000)
        }

        params["signature"] = self._sign(params)

        headers = {"X-MBX-APIKEY": self.api_key}

        r = requests.post(self.base_url + endpoint, params=params, headers=headers)
        return r.json()

    # -------------------------
    # Cancel order
    # -------------------------
    def cancel_order(self, symbol, order_id):
        endpoint = "/api/v3/order"

        params = {
            "symbol": symbol,
            "orderId": order_id,
            "timestamp": int(time.time() * 1000)
        }

        params["signature"] = self._sign(params)

        headers = {"X-MBX-APIKEY": self.api_key}

        r = requests.delete(self.base_url + endpoint, params=params, headers=headers)
        return r.json()

    # -------------------------
    # Open orders sync
    # -------------------------
    def get_open_orders(self, symbol):
        endpoint = "/api/v3/openOrders"

        params = {
            "symbol": symbol,
            "timestamp": int(time.time() * 1000)
        }

        params["signature"] = self._sign(params)

        headers = {"X-MBX-APIKEY": self.api_key}

        r = requests.get(self.base_url + endpoint, params=params, headers=headers)
        return r.json()

# 2. Order Manager (source of truth)
class OrderManager:
    def __init__(self):
        self.orders = {}   # order_id -> order state
        self.archive = {}

    def upsert(self, order):
        self.orders[order["orderId"]] = order

    def cancel_local(self, order_id):
        if order_id in self.orders:
            self.orders[order_id]["status"] = "CANCELLED"

    def mark_pending_cancel(self, order_id):
        if order_id in self.orders:
            self.orders[order_id]["status"] = "PENDING_CANCEL"

    def on_cancel_ack(self, order_id):
        self.orders[order_id]["status"] = "CANCELLED"

    def apply_fill(self, fill_event, state):
        oid = fill_event["i"]

        if oid in self.orders:
            order = self.orders[oid]

            order["executedQty"] = float(fill_event["l"])
            order["status"] = fill_event["X"]

            # only trigger economic update on actual fills
            if fill_event["X"] in ("PARTIALLY_FILLED", "FILLED"):
                price = float(fill_event["L"])
                qty = float(fill_event["l"])
                side = fill_event["S"]

                # route into portfolio logic
                state.on_fill(price, qty, side) # route to on_fill to update inventory

    def finalize_order(self, order_id):
        order = self.orders.get(order_id)

        if order["status"] in ("CANCELLED", "FILLED", "REJECTED"):
            # optional: move to archive instead of deleting
            self.archive[order_id] = order
            del self.orders[order_id]

# 3. Live Execution Layer (your MM brain)

# This replaces your current Execution order management logic.

class LiveExecution:
    def __init__(self, config, state, broker, order_manager):
        self.config = config
        self.state = state
        self.broker = broker
        self.om = order_manager

        self.last_bid = None
        self.last_ask = None

        self.max_inv = 10

    # -------------------------
    # RISK GUARD
    # -------------------------
    def risk_check(self):
        if abs(self.state.inventory) > self.max_inv:
            return False
        return True

    # -------------------------
    # MAIN QUOTE ENGINE
    # -------------------------
    def place_quotes(self, signal):

        if not self.risk_check():
            return

        book = self.state.market_book
        bid_tick, bid_size = book.best_bid()
        ask_tick, ask_size = book.best_ask()

        best_bid = self.config.from_tick(bid_tick)
        best_ask = self.config.from_tick(ask_tick)

        mid = (best_bid + best_ask) / 2

        # --- your model ---
        fair, microprice = signal["fair"], signal["microprice"]
        skew = signal["skew"]

        reservation = fair + skew
        alpha = 0.3

        struct_delta = alpha * (reservation - microprice)

        center = microprice + struct_delta

        spread = 0.01  # replace with your model
        half = spread / 2

        bid = center - half
        ask = center + half

        # prevent crossing
        bid = min(bid, best_bid)
        ask = max(ask, best_ask)

        # -------------------------
        # CANCEL OLD
        # -------------------------
        for oid, o in list(self.om.orders.items()):
            if o["side"] == "BUY":
                self.broker.cancel_order(self.config.symbol, oid)
                self.om.cancel_local(oid)

            if o["side"] == "SELL":
                self.broker.cancel_order(self.config.symbol, oid)
                self.om.cancel_local(oid)

        # -------------------------
        # PLACE NEW
        # -------------------------
        bid_resp = self.broker.place_limit(
            self.config.symbol,
            "BUY",
            qty=1,
            price=bid
        )

        ask_resp = self.broker.place_limit(
            self.config.symbol,
            "SELL",
            qty=1,
            price=ask
        )

        self.om.upsert(bid_resp)
        self.om.upsert(ask_resp)

        self.last_bid = bid
        self.last_ask = ask

# 4. User Data Stream (THIS is what makes it “real”)
class BinanceUserStream:
    def __init__(self, broker, order_manager, state):
        self.broker = broker
        self.om = order_manager
        self.state = state

    def start(self):

        def on_message(ws, msg):
            data = json.loads(msg)

            # -----------------------------
            # ORDER UPDATE EVENT
            # -----------------------------
            if data["e"] in ("executionReport", "ORDER_TRADE_UPDATE"): # Spot User Stream → executionReport, Futures User Stream → ORDER_TRADE_UPDATE

                order = data.get("o", data)  # futures uses "o"

                oid = order["i"]  # orderId
                status = order["X"]  # eg. NEW, PARTIALLY_FILLED, FILLED, CANCELED - ORDER STATUS (truth state of order lifecycle)
                exec_type = order["x"]  #eg. NEW, TRADE, CANCELED - EVENT TYPE (what just happened)

                # -----------------------------
                # 1. UPSERT BASE STATE
                # -----------------------------
                self.om.upsert({
                    "orderId": oid,
                    "status": status,
                    "side": order["S"],
                    "price": float(order["p"]) if "p" in order else None,
                    "executedQty": float(order.get("z", 0.0))
                })

                # -----------------------------
                # 2. HANDLE FILLS
                # -----------------------------
                if exec_type == "TRADE":
                    fill_qty = float(order["l"])
                    fill_price = float(order["L"])

                    self.om.apply_fill({
                        "i": oid,
                        "l": fill_qty,
                        "L": fill_price,
                        "X": status
                    })

                    # update inventory (correct directional logic)
                    if order["S"] == "BUY":
                        self.state.inventory += fill_qty
                    else:
                        self.state.inventory -= fill_qty

                # -----------------------------
                # 3. HANDLE CANCELLATIONS
                # -----------------------------
                if status == "CANCELED":
                    self.on_cancel_ack(oid)
                    self.om.finalize_order(oid)

                elif status == "REJECTED":
                    self.om.update_status(oid, "REJECTED")
                    self.om.finalize_order(oid)

                elif status == "CANCEL_REJECTED":
                    self.on_cancel_reject(oid)
                    # DO NOT CHANGE ORDER STATE

        # websocket connection (pseudo)
        self.broker.start_user_stream(on_message)
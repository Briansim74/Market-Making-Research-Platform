#pragma once //include this file once per compile
#include <set>
#include <map>
#include <deque>
#include <queue>
#include <mutex>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <ranges>
#include <random>
#include <format>
#include <cctype>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <utility>
#include <cstdint>
#include <optional>
#include <iostream>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <unordered_map>
#include <condition_variable>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <curl/curl.h>
#include <cpr/cpr.h>
#include "simdjson.h"
#include <nlohmann/json.hpp>
#include <xgboost/c_api.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <ftxui/dom/table.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "mmpy_structs.hpp" //structs
#include "mmpy_config_orderbook.hpp" //market config & orderbook

using std::cout;
using json = nlohmann::json;
using std_string = std::string;

using namespace std;
using namespace arrow;
using namespace ftxui;
using namespace std::chrono;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using ssl_stream = boost::asio::ssl::stream<tcp::socket>;
using ws_stream  = websocket::stream<ssl_stream>;

class State {
public:
    MarketConfig& config;
    const json& params;
    OrderBook market_book;
    double last_mid = 0.0;
    MarketFeatureState market_feature_state;

    optional<Signal> last_signal;

    double order_imbalance = 0.0;
    double trade_imbalance = 0.0;
    double ewma_var = 0.0;

    double inventory = 0.0;
    double cash = 100000.0;
    double initial_cash = 100000.0;
    double realized_pnl = 0.0;
    double avg_entry_price = 0.0;

    deque<double> equity_history;
    deque<double> return_history;
    double last_equity = 0.0;

    double maker_fee_rate;
    double taker_fee_rate;
    double fees_paid = 0.0;

    unordered_map<std_string, unordered_map<int64_t, double>> queue_flow;
    unordered_map<std_string, unordered_map<int64_t, double>> my_queue_position;
    unordered_map<double, deque<Trade>> trade_buckets;
    double window_ms = 1000;

    double last_trade_ts = 0;
    optional<Trade> last_trade;
    double last_depth_ts = 0;
    Order* last_fill_candidate = nullptr;
    Order* last_order_update = nullptr;

    bool initialized = false;

    State(MarketConfig& config, const json& params)
        : config(config), params(params), market_book(config, params),
        maker_fee_rate(params["fees"]["maker_fee_rate"].get<double>()),
        taker_fee_rate(params["fees"]["taker_fee_rate"].get<double>()) {}

    double get_vol(){
        return sqrt(ewma_var);
    }

    void update_vol(){
        double alpha = 0.90;
        double mid = market_book.mid();

        if(last_mid != 0.0){
            double r = log(mid / last_mid);
            ewma_var = alpha * ewma_var + (1 - alpha) * r * r;
        }
        last_mid = mid;
    }

    void compute_order_imbalance(){
        auto [bid_tick, bid_size] = market_book.best_bid();
        auto [ask_tick, ask_size] = market_book.best_ask();

        order_imbalance = (bid_size - ask_size) / (bid_size + ask_size + 1e-9);
    }

    void on_fill(const double& price, double qty, const std_string& side, const std_string& liquidity){

        double old_inv = inventory;
        double old_avg = avg_entry_price;

        double fill_value = price * qty;
        double fee_rate = (liquidity == "maker") ? maker_fee_rate : taker_fee_rate;
        double fee = fill_value * fee_rate;
        fees_paid += fee;

        if(side == "BUY"){
            if(old_inv < 0){
                double close_qty = min(qty, abs(old_inv));
                realized_pnl += close_qty * (old_avg - price);

                old_inv += close_qty;
                qty -= close_qty;
            }

            inventory = old_inv;

            if(qty > 0){
                double new_inv = old_inv + qty;

                if(old_inv > 0){
                    avg_entry_price = (old_avg * old_inv + price * qty) / new_inv;
                }
                else avg_entry_price = price;

                inventory = new_inv;
            }
            cash -= (fill_value + fee);
        }

        else{ // SELL
            if(old_inv > 0){
                double close_qty = min(qty, old_inv);
                realized_pnl += close_qty * (price - old_avg);

                old_inv -= close_qty;
                qty -= close_qty;
            }

            inventory = old_inv;

            if(qty > 0){
                double new_inv = old_inv - qty;

                if(old_inv < 0){
                    avg_entry_price = (old_avg * abs(old_inv) + price * qty) / abs(new_inv);
                }
                else avg_entry_price = price;

                inventory = new_inv;
            }
            cash += (fill_value - fee);
        }

        if(inventory == 0.0) avg_entry_price = 0.0;
    }

    double get_unrealized_pnl(double mid){
        return inventory * (mid - avg_entry_price);
    }

    double get_pnl(double mid){
        return realized_pnl + inventory * (mid - avg_entry_price) - fees_paid;
    }

    double get_value(auto& m, const std_string& side, const int64_t& price){
        auto it1 = m.find(side);
        if(it1 == m.end()) return 0.0;

        auto it2 = it1->second.find(price);
        if(it2 == it1->second.end()) return 0.0;

        return it2->second;
    }

    double compute_queue_ahead(const std_string& side, const int64_t& price_tick){
        double my_pos = get_value(my_queue_position, side, price_tick);
        double flow   = get_value(queue_flow, side, price_tick);

        double ahead = my_pos - flow;
        return max(0.0, ahead);
    }

    void reset_queue_ahead(const std_string& side, const int64_t& price_tick){
        my_queue_position[side].erase(price_tick);
        queue_flow[side].erase(price_tick);
    }

    void update_queue_from_depth(const Depth& entry){

        for(auto& [price_tick, q]: entry.bid_delta){
            
            auto it = market_book.bids.find(price_tick);
            double old_qty = (it != market_book.bids.end()) ? it->second : 0.0;
            double new_qty = q;

            if(old_qty > 0.0){
                double depletion = max(0.0, old_qty - new_qty);
                queue_flow["bids"][price_tick] += depletion;
            }
        }

        for(auto& [price_tick, q]: entry.ask_delta){
            
            auto it = market_book.asks.find(price_tick);
            double old_qty = (it != market_book.asks.end()) ? it->second : 0.0;
            double new_qty = static_cast<double>(q);

            if(old_qty > 0.0){
                double depletion = max(0.0, old_qty - new_qty);
                queue_flow["asks"][price_tick] += depletion;
            }
        }
    }

    template <typename T> void push_limited(deque<T>& dq, const T& value, size_t maxlen = 10){
        dq.push_back(value);
        if(dq.size() > maxlen) dq.pop_front();
    }

    void update_market_feature_state(){
        auto& mfs = market_feature_state;
        auto& book = market_book;

        auto [bid_tick, bid_size] = book.best_bid();
        auto [ask_tick, ask_size] = book.best_ask();

        double best_bid = config.from_tick(bid_tick);
        double best_ask = config.from_tick(ask_tick);

        double mid = (best_bid + best_ask) / 2.0;
        double spread = best_ask - best_bid;
        double microprice = (best_ask * bid_size + best_bid * ask_size) /(bid_size + ask_size + 1e-9);

        last_mid = mid;
        double mid_return = (mid - last_mid) / last_mid;
        
        double quote_churn;
        if(mfs.prev_best_bid == 0.0 && mfs.prev_best_ask == 0.0){
            quote_churn = 0.0;
        }
        else{
            double bid_delta = abs(best_bid - mfs.prev_best_bid);
            double ask_delta = abs(best_ask - mfs.prev_best_ask);
            quote_churn = bid_delta + ask_delta;
        }

        mfs.prev_best_bid = best_bid;
        mfs.prev_best_ask = best_ask;

        push_limited(mfs.mid_returns, mid_return);
        push_limited(mfs.spread, spread);
        push_limited(mfs.order_imbalance, order_imbalance);
        push_limited(mfs.trade_imbalance, trade_imbalance);
        push_limited(mfs.quote_churn, quote_churn);
        push_limited(mfs.inventory, inventory);
        push_limited(mfs.microprice_error, mid - microprice);
    }

    Regime get_regime(){
        auto& mfs = market_feature_state;

        auto mean = [](const deque<double>& q){
            if(q.empty()) return 0.0;
            double s = 0;
            for(auto x: q) s += x;
            return s / q.size();
        };

        auto stddev = [](const deque<double>& q){
            if(q.size() < 2) return 0.0;
            double m = 0;
            for(auto x: q) m += x;
            m /= q.size();

            double var = 0;
            for(auto x: q) var += (x - m) * (x - m);
            return sqrt(var / q.size());
        };

        Regime regime;
        regime.volatility = stddev(mfs.mid_returns);
        regime.spread = mean(mfs.spread);
        regime.order_imbalance = mean(mfs.order_imbalance);
        regime.trade_imbalance = mean(mfs.trade_imbalance);
        regime.quote_churn = mean(mfs.quote_churn);
        regime.inventory = mean(mfs.inventory);
        regime.inventory_vol = stddev(mfs.inventory);
        regime.microprice_error = mean(mfs.microprice_error);

        return regime;
    }

    void update_ml_realization(){
        auto& mfs = market_feature_state;
        double now = config.now_ms();

        while(!mfs.ml_predictions.empty() && now - mfs.ml_predictions.front().ts >= mfs.ml_horizon_ms){
            auto& entry = mfs.ml_predictions.front();
            mfs.ml_predictions.pop_front();
            
            entry.realized = log(last_mid / entry.reservation);
            mfs.ml_signal_log.push_back(entry);
        }
    }

    void update_performance(){
        auto [bid_tick, bid_size] = market_book.best_bid();
        auto [ask_tick, ask_size] = market_book.best_ask();

        if(bid_size <= 0 || ask_size <= 0) return;

        double bid = config.from_tick(bid_tick);
        double ask = config.from_tick(ask_tick);

        double mid = (bid + ask) / 2.0;
        double equity = cash + inventory * mid;

        if(last_equity > 0.0 && equity > 0.0){
            double r = log(equity / last_equity);

            if(isfinite(r)) return_history.push_back(r);
        }

        last_equity = equity;
        equity_history.push_back(equity);
    }

    double compute_sharpe(){
        if(return_history.size() < 30) return 0.0;

        vector<double> returns;
        returns.reserve(return_history.size());

        for(double r: return_history){
            if(isfinite(r)) returns.push_back(r);
        }

        if(returns.size() < 30){
            cout << "SHARPE - len(returns) < 30:" << NAN << "\n";
            return NAN;
        }

        double mean = 0.0;
        for(double r: returns) mean += r;
        mean /= returns.size();

        double var = 0.0;
        for(double r: returns) var += (r - mean) * (r - mean);
        var /= returns.size();

        double std = sqrt(var);
        if(std == 0.0 || isnan(std)){
            cout << "SHARPE - std == 0 / isnan(std):" << NAN << "\n";
            return NAN;
        }

        double sharpe_ratio = mean / (std + 1e-9);
        cout << "SHARPE: " << sharpe_ratio << "\n";

        return sharpe_ratio;
    }
};
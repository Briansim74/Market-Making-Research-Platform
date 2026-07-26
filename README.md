# Market-Making-Research-Platform
Research framework for market making as a coupled system of alpha, execution, inventory, and regime-dependent liquidity provision.

Built for live L2 market data, deterministic replay, and execution-aware signal evaluation.

## Core Insight
Market making is not a forecasting problem.

It is a conditional execution problem under microstructure constraints:
- alpha identifies where edge exists
- regimes determine when edge is valid
- toxicity determines whether liquidity should be provided
- execution determines whether edge is realized
- fees + queue dynamics determine whether edge survives

Most theoretical edge is lost through execution and adverse selection, not prediction error.

## System Overview
```
Market Data / Replay
        │
        ▼
Order Book State
        │
        ▼
Regime Detection (GMM clustering)
        │
        ▼
Alpha Stack
 ├─ Microstructure Alpha (fast signal)
 ├─ Structural Alpha (slow microstructure)
 └─ ML Residual Alpha (XGBoost correction)
        │
        ▼
Toxicity Model (expected markout)
        │
        ▼
Quote / Skew / Size Decision
        │
        ▼
Execution Model (queue + cancellations + fees)
        │
        ▼
Inventory / Risk
        │
        ▼
Dataset Generation & Research
```

Supports:
- live trading
- paper trading
- full historical replay

## Regime Model
Unsupervised Gaussian Mixture Model (GMM) using:
- volatility
- spread
- order imbalance
- trade imbalance
- quote churn
- inventory
- inventory volatility
- microprice error

Results:
- discrete market regimes with materially different alpha expression.

Key observations:
- regimes gate alpha effectiveness rather than generate edge.

## Alpha Stack
### 1. Microprice Alpha
Top-of-book imbalance signal.
- strongest at 100 - 500ms horizons
- captures immediate order book pressure
- fast-decaying but directionally informative

### 2. Structural Alpha
Slower microstructure signal:
- volatility
- order/trade imbalance
- inventory pressure
- microprice deviation

More stable estimate of short-horizon fair value than microprice alone.

### 3. Residual ML (XGBoost)
Learns residual drift after structured fair value:
```
log(mid_{t+h} / fair_value_t)
```

Behavior:
- weak short-horizon signal
- improves at 1 - 5s horizons
- acts as correction layer, not primary alpha

## Toxicity Model (Execution Risk)
Predicts expected markout conditional on execution:
```
T(x) = E[future markout_h ∣ fill, state]
```

Inputs:
- imbalance
- spread
- volatility
- microprice deviation
- inventory pressure
- queue position

Outputs:
- negative → toxic liquidity
- positive → favorable execution

Role:
- execution gating / participation filter
- used for skewing and size adjustment

Results:
- strong short-horizon predictability of adverse selection
- improves execution quality more than standalone PnL

## Execution Model
### 1. Fill Model
Deterministic fill model driven by:
- trade-driven depletion (explicitly modeled)
- depth-driven depletion (cancellations, additions, quote churn estimation, hawkes)
- hawkes excitation

```
Q_t (Queue_ahead) = Q_t-1 - trade-driven depletion - (β_raw * multiplier * excitation) * (depth-driven depletion)
```

Passive fill modeled as stochastic Poisson queue depletion: (Old)
```
P(fill) = 1 − exp(−(λ_t / Q_t) ⋅ Δ_t)
```

Key correction:
- queue reduction is not equivalent to executed volume

### 2. Cost Model
```
PnL = spread capture + alpha − fees − adverse selection − slippage
```

Key implications:
- fees + turnover materially impact viability
- small execution inefficiencies dominate marginal signal gains

### 3. Latency Model
Execution timing significantly affects realized outcomes.

Future work:
- order placement latency
- cancellation latency
- exchange acknowledgement delays

## Inventory Model
Inventory treated as a continuous risk state.

Reservation price adjusts based on:
- inventory level
- volatility
- regime

Objective:
- balance spread capture vs directional exposure under changing conditions.

## Key Results
#### 1. Alpha hierarchy is stable
- structural alpha > microprice > residual ML (short horizon)

#### 2. Regimes improve selection, not edge
- reduce drawdowns
- improve consistency
- do not independently generate alpha

#### 3. Toxicity is execution-side signal
- strong markout predictability
- improves fill quality
- can reduce PnL if overused (over-filtering)

#### 4. Execution dominates marginal alpha gains
- queue dynamics + cancellations + fees significantly alter outcomes
- naive alpha materially degrades under realistic execution

## Edge Decomposition
Market making performance decomposes into:
- alpha: where edge exists
- regime: when edge is valid
- toxicity: whether to participate
- execution: whether edge is realized
- fees: whether edge survives
- latency: how fast edge is captured

## Core Takeaway
Performance is driven primarily by selective participation under realistic microstructure constraints, not predictive accuracy alone.

The dominant improvement lever is not stronger signals, but tighter coupling between:
- signal generation
- execution modeling
- adverse selection filtering
- regime-aware participation

## Live Execution (Binance Futures Testnet)
The platform includes a full live execution layer on Binance Futures Testnet, closing the loop from signal generation → quoting → execution → fills → PnL, and validating microstructure assumptions against real exchange behavior.

### Execution Stack
- Broker Layer (BinanceBroker)
  - REST order placement/cancel with HMAC authentication
  - user data stream (listenKey) for order lifecycle tracking
  - position reconciliation via positionRisk
  - session keepalive and recovery handling
- Execution Engine (LiveExecution)
  - inventory- and volatility-adjusted asymmetric quoting
  - toxicity-aware participation and sizing
  - queue-aware cancel/replace logic
  - real-time trade-flow ingestion for state updates
- User Stream Handler (BinanceUserStream)
  - WebSocket ORDER_TRADE_UPDATE processing
  - handles NEW / TRADE / CANCELED / REJECTED states
  - maintains queue position estimates at entry
  - synchronizes internal execution state with exchange events

## Key Live Findings
####  1. Latency is structural (~600-700ms end-to-end)
- signal → execution exhibits ~600-700ms latency dominated by REST gateway, network RTT, and WebSocket propagation.
- latency is a first-order state variable; it must be explicitly modeled in fill probability and queue dynamics.

####  2. Market data is asynchronous (~150-300ms bursty updates)
- order book updates arrive in event-driven bursts rather than fixed intervals due to batched WebSocket delivery.
- L2 data is intrinsically irregular; fixed-timestep assumptions distort microstructure inference.

####  3. Execution dominates signal quality
- queue position, cancellation timing, and latency differentials materially affect realized outcomes.
- most theoretical alpha is reshaped by execution mechanics rather than prediction error.

####  4. Alpha-execution decomposition holds in practice
- alpha identifies directional bias, toxicity captures adverse selection risk, and execution determines realized PnL.
- short-horizon variance is dominated by execution noise rather than signal quality.

####  5. Queue state is first-class signal
- queue position, cancellation dynamics, and trade-driven depletion are primary drivers of fill probability.
- validates explicit modeling of queue-ahead, cancellation resets, and trade-flow-based depletion.

## Updated Core Takeaway (Reinforced)
Market making is not constrained by predictive signal quality.

It is constrained by:
- latency uncertainty
- queue position randomness
- execution path dependence
- regime-dependent liquidity fragility

In live conditions, execution dynamics dominate alpha, and the primary optimization problem becomes:
- selective participation under microstructure and latency constraints, not prediction.

## Future Directions
- Exchange-specific latency and acknowledgement modeling. (in-progress)
- Hawkes-process based order-flow forecasting. (completed)
- C++ execution engine for low-latency simulation. (in-progress)
- Live paper-trading and liquidity-provision deployment. (completed)
- Live validation for:
  - fill probability estimates
  - toxicity predictions
  - regime classifications

against real Binance executions under small-capital deployment.

## Example Output
Below are sample outputs illustrating the engine decision mechanics.

### Trading Terminal Dashboard with L2 Market Data and Strategy Parameters
<img width="700" height="1300" alt="React Trading Terminal" src="https://github.com/Briansim74/Market-Making-Research-Platform/blob/main/react.png"/>

</br></br></br>

<details>
<summary><strong>Detailed Implementation & Usage</strong></summary>

# Market-Making-Research-Platform
A research framework for studying how alpha generation, execution quality, inventory risk, and market regime interact in electronic liquidity provision.

The project combines live market data ingestion, deterministic replay simulation, market microstructure research, and event-driven execution modeling within a unified architecture.

## Motivation
Passive market making is often presented as a spread-capture problem.

In practice, profitability emerges from the interaction of:
- directional alpha
- execution quality
- inventory management
- market regime

A market maker can possess predictive signals but fail due to poor execution.

Likewise, a market maker can execute efficiently yet lose money through adverse selection.

The objective of this project is to understand how these components interact and contribute to realized PnL.

## Key Contributions
- Developed a regime-aware market-making framework using unsupervised state clustering.
- Implemented queue-aware fill simulation using Poisson-based trade-flow modeling.
- Built a toxicity model for adverse-selection estimation using execution markouts.
- Combined structural, microstructure, and residual ML alpha models.
- Evaluated the interaction between alpha generation, execution quality, and transaction costs.

## Dataset Generation
The platform automatically records:
- order book snapshots
- trade events
- quote updates
- fills
- execution decisions
- queue state
- regime state
- signal values

This enables repeatable research workflows for:
- alpha modeling
- regime analysis
- execution quality
- fill probability estimation
- adverse selection studies

## System Architecture
```
Market Data Feed / Replay Feed
        │
        ▼
Order Book State
        │
        ▼
Regime Detection
        │
        ▼
Signal Generation
 ├─ Microstructure Alpha
 ├─ Structural Alpha
 └─ ML Residual Alpha
        │
        ▼
Toxicity Filter
        │
        ▼
Quote Construction
        │
        ▼
Execution Engine
        │
        ▼
Inventory & Risk Management
        │
        ▼
Dataset Generation & Research
```

The same architecture supports:
- Live exchange connectivity
- Deterministic historical replay
- Dataset generation
- Strategy research
- Execution analysis

## Regime Research
#### Research Question
```
Why does strategy performance vary dramatically across market conditions?
```

#### Methodology
An unsupervised Gaussian Mixture Model (GMM) clusters market states using:
- volatility
- spread
- order imbalance
- trade imbalance
- quote churn
- inventory
- inventory volatility
- microprice error

Future outcomes are evaluated separately using:
- forward returns
- realized volatility
- directional persistence

This prevents look-ahead bias while allowing regimes to be interpreted economically.

#### Findings
Three recurring market states emerged.

| regime | volatility | spread | order_imbalance | trade_imbalance | quote_churn | inventory | inventory_vol | microprice_error | future_return | future_volatility | future_direction |
|--------|------------|--------|-----------------|-----------------|-------------|-----------|---------------|------------------|---------------|-------------------|------------------|	
| 0      | 1.93       |	 2.11  | 0.00            | -0.15           | NaN         | 0.02      | 1.44          | -0.10            | +0.000012     | 0.000010          | +0.043785        |
| 1      | 0.05       |	-0.08  | 0.05            | -0.00           | NaN         | 0.69      | -0.05         | -0.03            | -0.000003     | 0.000009          | -0.058434        |
| 2      | -0.11      |	-0.08  | -0.10           | 0.02            | NaN         | -1.31     | -0.05         | 0.08             | +0.000003     | 0.000008          | -0.072946        |

#### Regime 0 - Liquidity Shock / Breakout
Characteristics:
- Extreme volatility
- Wide spreads
- Large microprice dislocations
- Strong directional persistence

Outcomes:
- Highest alpha concentration
- Strongest future directional bias
- Controlled directional liquidity provision

#### Regime 1 - Toxic
Characteristics:
- Moderate volatility
- Wide spreads

Outcomes:
- Adverse selection dominates
- Baseline market-making environment

#### Regime 2 - Quiet Market
Characteristics:
- Low volatility
- Low imbalance
- Stable liquidity

Outcome:
- Mean-reverting behavior
- Little directional edge
- Baseline market-making environment

## Alpha Research
### 1. Microprice Modeling
Microprice is the weighted midpoint using top-of-book liquidity and is commonly used as a short-horizon estimate of fair value.

#### Research Question
```
Is microprice a statistically and economically meaningful predictor of future mid-price movement?
```

#### Findings
| Time Horizon | Residual_IC  | Residual_Rank_IC | HitRate      | PnLProxy     | SharpeProxy   |
|--------------|--------------|------------------|--------------|--------------|---------------|
| 100	       | 0.1686       | 0.3242           | 0.06518      | 0.0000       | 0.0335        |
| 500          | 0.1573       | 0.4194           | 0.1946       | 0.0000       | 0.0589        |
| 1000         | 0.1485       | 0.4186           | 0.2966       | 0.0000       | 0.0684        |
| 5000         | 0.1159       | 0.3010           | 0.5399       | 0.0000       | 0.1040        |

Microprice appears most effective in predicting future mid at very short horizons:
- 100ms
- 500ms

Microprice consistently demonstrated:
- Positive information coefficient (IC)
- Positive rank IC
- Directional hit rate above random
- Positive economic value in market-making backtests

### 2. Structural Alpha Modeling
Microprice captures immediate order-book pressure, but a significant portion of future price movement emerges from slower microstructure dynamics that evolve over longer horizons.

#### Research Question
```
Can slower microstructure features explain future price movement beyond what is already captured by microprice?
```

Structural alpha was developed using slower microstructure features such as:
- volatility
- microprice
- order imbalance
- trade imbalance
- inventory target

These features were used to construct fair value and inventory-aware quote skew.

The objective was to determine whether slower market-state information contains predictive power that is complementary to short-horizon microprice signals.

Structural alpha could potentially provide an orthogonal source of alpha beyond both the mid price and microprice, allowing reservation prices to better reflect evolving market conditions.

### 3. ML Residual Alpha Modeling
#### Research Question
```
Can machine learning predict the residual error remaining after structural and microprice adjustments?
```

#### Methodology
The gradient-boosted regression model (XGBoost) is implemented as a single residual model trained on:
```
log(future mid / (mid + struct_delta + micro_signal_delta))
```
This reframes the model not as a price predictor, but as a correction model over a structured reservation price

#### Feature Set
- spread
- order imbalance
- trade imbalance
- inventory
- volatility
- queue_ahead_bid
- queue_ahead_ask

#### Findings
ML residual signal demonstrated:
- NaN IC and rank IC in the short term horizons
- Relatively weak hitrate of less than 50% for all horizons

| Time Horizon | Residual_IC  | Residual_Rank_IC | HitRate      | PnLProxy     | SharpeProxy   |
|--------------|--------------|------------------|--------------|--------------|---------------|
| 100	       | NaN          | NaN              | 0.5005       |-0.0000       |-0.0002        |
| 500          | NaN          | NaN              | 0.4893       |-0.0000       |-0.0017        |
| 1000         | 0.2053       | -0.0504          | 0.3188       | 0.0000       | 0.07425       |
| 5000         | 0.5276       | 0.1421           | 0.5276       | 0.0000       | 0.1530        |

Residual signal appears most effective at longer time horizons:
- 1000ms
- 5000ms

This suggests the residual layer is attempting to extract the small residual component of future price movement that remains unexplained after:
- microprice alpha
- structural alpha

This is a significantly harder prediction problem than forecasting future price directly.

As a result, the residual layer behaves less like a standalone alpha signal and more like a residual drift estimator, identifying subtle longer-horizon effects that survive after the primary sources of edge have already been removed.

## Toxicity Research - Adverse Selection Estimation

#### Research Question
```
Can future adverse selection be predicted from the current microstructure state at the time of execution?
```

Rather than forecasting future prices, the objective is to estimate the expected quality of a fill before liquidity is provided.

Formally:
```
T(x) = E[future markout_h ∣ fill, state]
```

where:
- negative values → toxic fills (adverse selection)
- positive values → favorable liquidity provision

This reframes toxicity from a heuristic filter into a learned execution risk signal.

#### Label Construction
The target variable is future markout relative to execution price at:
- 100ms
- 500ms
- 1000ms
- 5000ms

This captures:
- immediate adverse selection
- short-term informed order flow
- delayed post-trade drift

#### Feature Set
The model uses microstructure state variables available at the time of execution:
- microprice deviation
- order imbalance
- trade imbalance
- spread
- volatility
- inventory / inventory pressure
- queue ahead (bid/ask depth proxy)

These jointly describe:
- liquidity pressure
- queue priority
- market activity
- informed flow conditions

#### Methodology
A gradient-boosted regression model (XGBoost) is trained to predict expected markout.

Outputs are interpreted as an estimate of fill quality:
- strongly negative prediction → potentially toxic fill
- near-zero prediction → neutral fill
- positive prediction → favorable liquidity provision

The toxicity model is incorporated directly into execution logic through:
- dynamic spread adjustments
- quote skewing
- order size modulation

This allows the market maker to reduce exposure during periods of elevated adverse-selection risk.

#### Findings
| Time Horizon | Residual_IC  | Residual_Rank_IC | HitRate      | PnLProxy     | SharpeProxy   |
|--------------|--------------|------------------|--------------|--------------|---------------|
| 100	       | 0.3648       | 0.4310           | 0.7639       | 7.6840       | 0.3954        |
| 500          | 0.2697       | 0.3774           | 0.7429       | 9.2498       | 0.3343        |
| 1000         | 0.2872       | 0.3833           | 0.7396       | 12.3904      | 0.3407        |
| 5000         | 0.1515       | 0.1528           | 0.5627       | 14.5507      | 0.1385        |

The model appears most effective at:
- 100ms
- 500ms
- 1000ms

The toxicity model demonstrated:
- positive IC across all horizons
- positive Rank IC across all horizons
- strong directional accuracy at short horizons
- significant economic value as an execution-quality filter

Hit rates above 70% at shorter horizons suggest that adverse selection is highly predictable from local order-book conditions.

Unlike the residual alpha model, the toxicity model is not attempting to predict future prices. Instead, it predicts whether a prospective fill is likely to be favorable or unfavorable.

This makes toxicity fundamentally an execution model rather than an alpha model.

The results suggest that a meaningful portion of market-making performance comes not from predicting future price movement, but from avoiding executions during periods of elevated adverse-selection risk.

## Execution Research
Market-making performance also comes from execution rather than alpha.

The execution simulator tracks:
- queue position estimates
- queue depletion
- passive fill probability
- fill candidates
- order lifecycle events
- maker / taker fees

The fill simulator intentionally separates alpha generation from execution realization, allowing execution assumptions to be studied independently from predictive signals.

### 1. Fill Modeling - Poisson Process
A passive order at the best bid or ask is not automatically executable.

Accurate simulation requires:
- queue position tracking
- queue depletion modeling
- realistic fill assumptions

#### Fill Probability Model
Instead of treating fills as deterministic events, the system models passive execution as a Poisson arrival / depletion process.

For a resting limit order at a given price level:

Let
```
Q_t = estimated queue ahead
λ_t = observed trade flow rate (BUY/SELL aggressor intensity)
Δ Q_t = expected queue depletion from market orders
```

We approximate queue depletion events follow a Poisson process

So fill probability over a small horizon Δt (100ms - on_depth time horizon):
```
P(fill) = 1 − exp(−(λ_t / Q_t) ⋅ Δ_t)
```

where: 
- f(Q_t) increases with proximity in queue (smaller queue → higher fill chance)
- trade flow acts as the intensity driver

#### Queue-Ahead Estimation
The system tracks:
- Initial queue position when order is placed
- Continuous depletion via observed depth updates

Net queue ahead estimate:
- queue_ahead = max(0, Q_initial − observed depletion)

This is continuously updated from live / replay book deltas.

#### Cancellation-Aware Fill Dynamics
A key extension is that queue depletion is not purely execution-driven.

The model explicitly accounts for:
- hidden liquidity removal
- order cancellations at the same price level
- stochastic liquidity disappearance

So observed queue reduction is decomposed into:
- trades (aggressive liquidity taking)
- cancellations (passive liquidity withdrawal)

This is critical because:
- treating all queue reduction as trades overestimates fill probability

#### Cancellation Simulation Effect
By incorporating cancellation-like behavior into queue depletion:
- fill timing / probability becomes more realistic
- passive quoting becomes more conservative under instability

#### Findings
This changes execution from deterministic “if queue position → fill” to probabilistic “fill as stochastic function of flow + queue + cancellations”

### 2. Transaction Fees
Transaction fees are a first-order component of execution quality, not a secondary accounting detail.

In market making, profitability is determined by:

```
gross edge (alpha + spread capture) − execution costs (fees + adverse selection + slippage)
```

Among these, transaction fees are unique because they:
- scale directly with trading frequency
- apply asymmetrically (maker vs taker)
- can flip the sign of marginal edge
- interact with regime-dependent fill probability
- 
### 3. Latency Awareness
Execution timing significantly affects realized outcomes.

Future work includes:
- order placement latency
- cancellation latency
- exchange acknowledgement delays

## Inventory & Risk Management
Inventory is treated as a dynamic state variable rather than a hard constraint.

Reservation prices are adjusted according to:
- current inventory
- alpha strength
- volatility
- market regime

The objective is to balance:
- spread capture
- directional conviction
- inventory risk

while maintaining competitive quotes.

## Performance Summary Across Configurations
| Configuration	                                                   | Total PnL  | Sharpe  | PnL / Fill | Fees / Fill | Notes                                                           |
|------------------------------------------------------------------|------------|---------|------------|-------------|-----------------------------------------------------------------|
| Mid (No Fees)                                                    | -283.53    | -0.0079 | -0.0308    | 0.0000      | Baseline, no execution realism                                  |
| Mid                                                              | -259.10	| +0.0030 | -0.0279    | 0.0065      | Fees slightly improve realism; no structural change             |
| Mid + Regime                                                     | -23.59	| -0.0088 | -0.0086    | 0.0046      | Regime alone insufficient                                       |
| Mid + Regime + Toxicity                                          | +625.25    | -0.0104 | +0.1171    | 0.0325      | Strong interaction: toxicity unlocks edge                       |
| Mid + Toxicity                                                   | -259.10	| +0.003  | +0.2783    | 0.0873      | Toxicity alone has limited effect on weak signal                |
| Mid + Micro signal                                               | +318.67	| -0.0086 | +0.2783    | 0.016       | Microprice introduces short-horizon edge                        |
| Mid + Micro signal + Toxicity                                    | +161.19    | -0.0100 | +0.0884    | 0.0504      | Toxicity reduces adverse selection but removes trades           |
| Mid + Micro signal + Regime                                      | +215.01	| -0.0100 | +0.6826    | 0.0901      | Regime stabilizes micro alpha                                   |
| Mid + Micro signal + Regime + Toxicity                           | +313.91    | -0.0122 | +0.1741    | 0.0471      | Weaker with regime and toxicity filters                         |
| Mid + Struct_delta                                               | +382.17	| -0.0100 | +0.2028    | 0.0726      | Structural alpha stronger than micro                            |
| Mid + Struct_delta + Toxicity                                    | +270.13	| -0.0045 | +0.2244    | 0.0946      | Better fill quality, lower throughput                           |
| Mid + Struct_delta + Regime                                      | +526.31	| -0.0076 | +0.365     | 0.0998      | Strong combined alpha                                           |
| Mid + Struct_delta + Regime + Toxicity                           | +390.46	| -0.0149 | +0.3136    | 0.1003      | Toxicity worsens PnL per fill                                   |
| Mid + Struct_delta + Micro signal                                | +422.33	| +0.0026 | +0.2799    | 0.0962      | Complementary alpha sources                                     |
| Mid + Struct_delta + Micro signal + Regime                       | +505.19	| +0.0051 | +0.3853    | 0.1237      | Regime improves consistency                                     |
| Mid + Struct_delta + Micro signal + Regime + Toxicity            | +435.73	| -0.0118 | +0.3557    | 0.0894      | Reduces fees per fill but overall slightly lower PnL per fill   |
| Mid + Struct_delta + Micro signal + Residual + Regime + Toxicity | +444.46	| -0.0082 | +0.4601    | 0.1155      | Full conditional system, fewer but higher quality fills         |

## Key Updated Findings
#### 1. Alpha and Execution Are Distinct Sources of Edge
The research suggests that profitability comes from two separate mechanisms:
- Alpha models determine where future price movement is likely to occur.
- Execution models determine whether that edge is actually captured.

Microprice and structural alpha generate the majority of directional profitability, while toxicity, fill modeling, and regime classification determine how efficiently that edge is realized.

#### 2. Micro + Structural Alpha Remain the Core Predictive Signals
Across all configurations, the largest improvements over baseline originate from:
- microprice alpha
- structural alpha

Structural alpha consistently outperforms microprice individually, while the combination produces the strongest standalone signal stack.

This suggests that short-horizon order-book pressure and slower market-state information contain complementary predictive information.

#### 3. Regimes Concentrate Alpha Rather Than Create It
Regime conditioning is one of the strongest improvements observed in the research.

However, regimes do not create directional edge on their own:
| Configuration	                                                   | Total PnL  |
|------------------------------------------------------------------|------------|
| Mid                                                              | -259.10	|
| Mid + Regime                                                     | -23.56	|
| Mid + Toxicity                                                   | -259.10	|
| Mid + Struct_delta                                               | +382.17	|
| Mid + Struct_delta + Regime                                      | +526.31	|

The primary role of regimes is therefore:
- identifying when alpha is likely to be effective
- suppressing participation in unfavorable environments

Regimes define when edge is available, not where it comes from.

#### 4. Toxicity Is an Execution Model, Not an Alpha Model
The toxicity model produced some of the strongest predictive statistics in the project:
- positive IC across all horizons
- rank IC up to ~0.43
- hit rates above 70% at short horizons

These results are significantly stronger than those observed for the residual alpha model.

However, toxicity often reduces total PnL because it removes trading opportunities.

Its primary effect is:
- reducing adverse selection
- increasing execution selectivity
- improving liquidity quality

The toxicity model therefore behaves more like a risk filter than a profit-maximizing signal.

#### 5. Residual ML Captures the Hardest Remaining Signal
After:
- microprice alpha
- structural alpha

have already been extracted, very little predictive information remains.

The residual model attempts to forecast only this remaining component.

As expected:
- IC is weak at short horizons
- hit rates remain near random
- performance improves primarily at longer horizons

This suggests the residual model is solving a substantially harder problem than the primary alpha layers.

Rather than generating a new source of edge, it acts as a residual drift estimator and execution-quality filter.

#### 6. Execution Costs Materially Alter Strategy Economics
Introducing maker and taker fees changed both realized profitability and strategy rankings.

The results show that:
- small per-fill fees compound rapidly under high turnover
- execution costs interact with fill probability and trade frequency
- marginal alpha can disappear once fees are included

This confirms that realistic execution modeling requires:
- transaction fees
- queue dynamics
- fill probability estimation
- adverse-selection modeling

Ignoring fees materially overstates market-making profitability.

## Core Takeaway
Market making is not a spread-capture problem and it is not purely a forecasting problem.

It is a conditional execution problem where:
- microprice and structural alpha determine where edge exists.
- regimes determine when edge is likely to be exploitable.
- toxicity models determine whether liquidity should be provided.
- execution quality determines how much of that edge is ultimately realized.
- transaction costs determine whether the remaining edge survives.

The strongest result of the research is that profitability increasingly comes from selective participation rather than stronger prediction.

## Live Execution (Binance Futures Testnet)
The platform has been extended with a fully live execution layer connected to Binance Futures Testnet, enabling end-to-end validation of the market-making stack from signal → quote → execution → fill → PnL.

This closes the loop between theoretical microstructure modeling and real exchange behavior.

### Execution Architecture
The live system is composed of three tightly coupled components:
- Broker Layer (BinanceBroker)
  - REST-based order placement and cancellation
  - HMAC-signed authenticated requests
  - User data stream (listenKey) for order lifecycle tracking
  - Position reconciliation via positionRisk endpoint
  - Keepalive thread to maintain session validity
- Execution Engine (LiveExecution)
  - Asymmetric quoting based on:
    - volatility scaling
    - inventory risk skew
    - toxicity-aware sizing
  - Queue-aware order management (cancel/replace logic)
  - Inventory-bounded risk guardrails
  - Trade-flow ingestion for microstructure state updates
- User Stream Handler (BinanceUserStream)
  - WebSocket-based order lifecycle tracking (ORDER_TRADE_UPDATE)
  - Handles:
    - NEW (order accepted + live)
    - TRADE (partial / full fills)
    - CANCELED / REJECTED states
  - Maintains queue position estimates at join time
  - Synchronizes execution state with exchange-confirmed events

## Key Live Findings
#### 1. End-to-End Order Latency (Signal → Live Confirmation)
Observed delay between order submission and exchange “LIVE” confirmation:
```
~600-700ms typical round-trip latency
```

This includes:
- network RTT (client → Binance API)
- API gateway validation (auth, margin, filters)
- WebSocket event propagation delay
- local system scheduling + processing

#### Interpretation
This latency is not abnormal for REST-based market making on Binance Futures.

It confirms:
- REST execution is not HFT-grade
- microsecond-level assumptions do not hold in this mode
- execution modeling must explicitly include latency as a state variable

#### 2. Market Data Update Cadence (~200ms bursts)
After isolating the execution engine, depth updates were still observed at:
```
~150-300ms burst intervals
```
#### Key Insight
This is not system-induced latency.

It reflects natural characteristics of the exchange feed, specifically:
- order book updates are event-driven, not periodic
- updates arrive only when top-of-book changes
- low activity periods produce bursty diffusion patterns
- WebSocket delivery is batched for efficiency

#### Interpretation
What appears as a “fixed 200ms tick” is actually:
```
sparse microstructure activity + batched propagation
```

This is important because it invalidates any assumption of uniform temporal resolution in L2 data.

#### 3. Execution Reality vs Model Assumptions
The live system confirms a key design assumption of the platform:
```
Most theoretical alpha is destroyed or reshaped by execution mechanics rather than signal quality.
```

Observed effects:
- queue position changes dominate fill probability
- small latency differences materially affect execution priority
- cancellation timing significantly impacts realized spread capture

#### 4. Alpha vs Execution Separation Holds
Empirically validated decomposition:
- Alpha stack correctly identifies directional bias
- Toxicity model strongly predicts adverse selection risk
- Execution layer determines whether alpha survives

However:
- execution noise often dominates short-horizon PnL variability

This reinforces the system design principle:
- Market making performance is primarily an execution filtering problem, not a prediction problem.

#### 5. Queue Dynamics Are First-Class State
Live behavior confirms:
- queue position at join time is a meaningful latent variable
- cancellations reset queue assumptions entirely
- fills are highly sensitive to micro-tick positioning and churn

This validates explicit modeling of:
- queue ahead estimation
- cancellation-driven queue decay
- trade-flow-induced depletion

## Updated Core Takeaway (Reinforced)
Market making is not constrained by signal generation quality.

It is constrained by:
- latency uncertainty
- queue position randomness
- execution path dependency
- regime-dependent liquidity fragility

Final empirical takeaway:
- In live conditions, execution dynamics dominate alpha quality.
- The dominant optimization surface is not prediction — it is participation timing under microstructure constraints.

## Future Directions
- Exchange-specific latency and acknowledgement modeling.
- Hawkes-process based order-flow forecasting.
- C++ execution engine for low-latency simulation.
- Live paper-trading and liquidity-provision deployment. (completed)
- Live validation for:
  - fill probability estimates
  - toxicity predictions
  - regime classifications

against real Binance executions under small-capital deployment.

## Example Output
Below are sample outputs illustrating how the engine behaves.


### Trading Terminal Dashboard with Market Data and Strategy Parameters
<img width="700" height="1300" alt="React Trading Terminal" src="https://github.com/Briansim74/Market-Making-Research-Platform/blob/main/react.png"/>

</details>
</br></br>

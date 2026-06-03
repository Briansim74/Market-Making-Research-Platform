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

## Alpha Research
### Research Question
Can future short-horizon price movement be predicted from order book state?

### Microprice Modeling
Microprice is commonly used as a short-horizon estimate of fair value:

Microprice is the weighted midpoint using top-of-book liquidity.

Research objective:
```
Is microprice a statistically and economically meaningful predictor of future mid-price movement?
```

#### Findings
Microprice consistently demonstrated:
- Positive information coefficient (IC)
- Positive rank IC
- Directional hit rate above random
- Positive economic value in market-making backtests

Replacing a mid-price reservation model with microprice-based reservation pricing transformed the strategy from loss-making to profitable.

#### Backtest Results
| Model	             | Total PnL |
|--------------------|-----------|
| Mid Only           | 	-305     |
| Mid + Micro signal |  +1001    |

Microprice appears most effective at very short horizons:
- 100ms
- 500ms
- 1000ms

### Structural Alpha Modeling
Microprice captures immediate order-book pressure but fails to explain all future price movement.

Structural alpha was developed using slower microstructure features such as:
- volatility
- microprice
- order imbalance
- inventory target

#### Findings
Structural signals produced stronger performance than microprice alone and captured a different component of future price formation.

#### Backtest Results
| Model	             | Total PnL |
|--------------------|-----------|
| Mid + Struct Delta |  +1489    |

The combination of structural and microstructure alpha generated significantly larger gains than either signal individually.

| Model	                            | Total PnL |
|-----------------------------------|-----------|
| Mid + Struct Delta + Micro signal | +3086     |

This suggests both signals contain complementary information.

### ML Residual Alpha Modeling
Research objective:
```
Can machine learning predict the residual error remaining after structural and microprice adjustments?
```

The model attempts to forecast:
Future Mid Price − Reservation Price (mid + struct_delta + micro_signal_delta)

rather than forecasting future prices directly.

Features include:
- spread
- order imbalance
- trade imbalance
- inventory
- volatility
- queue position estimates
- residual pricing errors
- microprice deviations

#### Findings
Machine learning improved risk-adjusted performance but reduced raw profitability.

| Model	                                 | Total PnL | Sharpe |
|----------------------------------------|-----------|--------|
| Mid + Struct_delta + Micro signal      | +3086     | 0.016  |
| Mid + Struct_delta + Micro signal + ML | +1984	   | 0.024  |

This suggests the ML layer primarily acts as a trade-quality filter:
- fewer trades
- lower turnover
- improved timing
- reduced inventory excursions

rather than generating large additional directional edge, also more responsible for predicting longer drift horizons (5000ms).

## Regime Research
### Research Question
Why does strategy performance vary dramatically across market conditions?

### Methodology
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

### Findings
Three recurring market states emerged.

### Regime 0 - Balanced Market
Characteristics:
- Moderate volatility
- Stable spreads
- Low directional persistence

Outcome:
- Little directional edge
- Baseline market-making environment

### Regime 1 - Quiet Market
Characteristics:
- Low volatility
- Low imbalance
- Stable liquidity

Outcome:
- Mean-reverting behavior
- Limited directional opportunity

### Regime 2 - Liquidity Shock / Breakout
Characteristics:
- Extreme volatility
- Wide spreads
- Large microprice dislocations
- Strong directional persistence

Outcome:
- Highest alpha concentration
- Strongest future directional bias

This regime generated the majority of predictive opportunity observed during research.

## Execution Modeling
Market-making performance also comes from execution rather than alpha.

The execution simulator tracks:
- queue position estimates
- queue depletion
- passive fill probability
- fill candidates
- order lifecycle events

### Fill Modeling Matters
A passive order at the best bid or ask is not automatically executable.

Accurate simulation requires:
- queue position tracking
- queue depletion modeling
- realistic fill assumptions

### Latency Matters
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

## Key Lessons Learned
### Alpha Alone Is Not Enough
Predictive signals can be profitable in research while failing in execution.

### Execution Dominates Realized Performance
Queue position, fill quality, and latency frequently outweigh improvements in predictive accuracy.

### Market Regimes Matter
The same quoting strategy performs differently across liquidity and volatility states.

Adaptive behavior is more effective than static quoting rules.

### Market Making Is a Multi-Layer Problem
Profitable liquidity provision emerges from the interaction of:
- alpha forecasting
- execution quality
- inventory management
- market regime awareness

No individual component is sufficient in isolation.

## Current Research Directions
- Regime-conditioned quoting
- Fill probability modeling
- Adverse selection attribution
- Latency modeling
- Adaptive spread construction
- Regime-dependent inventory targets
- Market-making attribution analysis

## Core Takeaway
Market making is not simply capturing the bid-ask spread.

It is an adaptive decision process where alpha forecasting, execution quality, inventory management, and market regime jointly determine realized profitability.

## Example Output
Below are sample outputs illustrating how the engine behaves.


### Trading Terminal Dashboard with Market Data and Strategy Parameters
<img width="700" height="1300" alt="React Trading Terminal" src="https://github.com/Briansim74/Market-Making-Research-Platform/blob/main/react.png"/>

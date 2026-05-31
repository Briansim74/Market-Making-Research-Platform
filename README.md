# Market-Making-Research-Platform
Designed for researching how alpha, execution quality, inventory risk, and market regime interact in electronic liquidity provision.

## Trading Problem
Passive market making is not purely an execution problem.

Profitability depends on balancing three competing objectives:
- capturing spread
- avoiding adverse selection
- managing inventory risk

The challenge is that market conditions evolve continuously.

Liquidity, volatility, queue dynamics, and directional pressure change the quality of passive quotes over time.

## Core Idea
This project studies market making as a real-time decision process driven by:
- market microstructure signals
- short-horizon alpha forecasts
- regime-dependent behavior
- execution and queue-position dynamics

Rather than quoting fixed spreads, the system adapts quoting behavior based on current market state.

## System Architecture

Market Data

↓

Order Book State

↓

Alpha Model

↓

Regime Model

↓

Quote Construction

↓

Execution Layer

↓

Inventory & Risk Management

↓

Dataset Generation / Research

The platform supports both:
- live exchange feeds
- deterministic replay simulation

allowing research and production logic to share the same architecture.

## Alpha Modeling
### Trading Problem
Microprice is often treated as a short-horizon estimate of fair value.

However, microprice itself exhibits systematic prediction error.

The question becomes:
- When does microprice fail, and can that failure be predicted from current market conditions?

### Core Idea
The alpha model learns the conditional forecast error of microprice.

Baseline prediction:
future mid ≈ current microprice

### Model objective:
predict future deviation from current microprice using market state features.

### Features include:
- spread
- order imbalance
- trade imbalance
- volatility
- inventory state
- queue position estimates
- microprice

The resulting signal estimates short-horizon directional drift relative to current fair value.

## Regime Modeling
### Trading Problem
Market making performance is highly regime-dependent.

Strategies that perform well during stable liquidity conditions may underperform during trending or volatile periods.

### Core Idea

An unsupervised regime model clusters market states using:
- volatility
- spread behavior
- order imbalance
- trade imbalance
- quote activity
- inventory dynamics
- microprice behavior

Each regime is evaluated against future outcomes such as:
- forward returns
- realized volatility
- directional persistence

This allows the market maker to adapt quoting behavior according to observed market conditions.

## Execution Modeling
### Trading Problem
Displayed liquidity is not executable liquidity.

Fill probability depends on queue position and future order flow.

### Core Idea
The execution layer estimates queue priority and liquidity consumption using order book updates.

Key components:
- queue position tracking
- queue depletion estimation
- passive fill simulation
- adverse selection measurement
- fill markout analysis

Execution quality is evaluated independently from directional alpha.

## Inventory & Risk Management
Inventory is treated as a state variable rather than a hard constraint.

The strategy dynamically adjusts reservation prices and quoting behavior according to:
- current inventory
- volatility conditions
- alpha strength
- detected regime

This creates a feedback loop between market state, inventory exposure, and execution decisions.

## Research Infrastructure
The platform automatically records:
- order book snapshots
- trades
- quote updates
- fills
- execution events

and generates datasets for:
- alpha research
- regime analysis
- adverse selection studies
- fill markout modeling
- execution quality evaluation

## Key Research Questions
This project is designed to investigate:
- When does microprice provide predictive value?
- Which market states produce adverse selection?
- How does queue position impact fill quality?
- Which regimes favor passive liquidity provision?
- How should inventory management adapt to changing conditions?
- How much PnL comes from spread capture versus directional edge?

## Key Insights
Market making profitability emerges from the interaction of:
- alpha quality
- execution quality
- inventory control
- market regime

No individual component is sufficient in isolation.

Execution without alpha becomes adverse selection.

Alpha without execution becomes unrealized opportunity.

Inventory control without regime awareness can dominate spread capture gains.

## Core Takeaway
Profitable market making is an adaptive liquidity provision problem where alpha forecasting, execution quality, inventory management, and market regime jointly determine realized PnL.

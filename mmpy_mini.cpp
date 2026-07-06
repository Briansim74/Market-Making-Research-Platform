#pragma warning(disable: 4834)
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
#include "mmpy_dashboard.hpp" //dashboard classes
#include "mmpy_feed.hpp" // feeds
#include "mmpy_state.hpp" //state & market feature state
#include "mmpy_recorder.hpp" //dataset recorder

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

class RegimeModel {
public:
    using Vec = vector<double>;
    using Mat = vector<vector<double>>;

    std_string model_name;
    std_string target;

    Mat means;
    vector<Mat> cov_inv;

    Vec log_det_cov;
    Vec log_weights;

    Vec scaler_mean;
    Vec scaler_scale;

    int n_regimes;
    int horizon_ms;
    vector<std_string> feature_cols;
    vector<std_string> regime_labels;

    int K;
    int D;

    // reusable buffers (IMPORTANT OPTIMIZATION)
    Vec x_input;
    Vec x_scaled;
    Vec diff;
    Vec scores;

    static constexpr double LOG_2PI = 1.8378770664093453;

    RegimeModel(const json& artifact){
        model_name = artifact["model_name"].get<std_string>();
        target = artifact["target"].get<std_string>();

        means = artifact["means"].get<Mat>();
        cov_inv = artifact["cov_inv"].get<vector<Mat>>();

        log_det_cov = artifact["log_det_cov"].get<Vec>();
        log_weights = artifact["log_weights"].get<Vec>();
   
        scaler_mean = artifact["scaler_mean"].get<Vec>();
        scaler_scale = artifact["scaler_scale"].get<Vec>();

        n_regimes = artifact["n_regimes"].get<int>();
        horizon_ms = artifact["horizon_ms"].get<int>();
        feature_cols = artifact["feature_cols"].get<vector<std_string>>();
        regime_labels = artifact["regime_labels"].get<vector<std_string>>();

        K = means.size();
        D = means[0].size();

        // allocate once
        x_input.resize(D);
        x_scaled.resize(D);
        diff.resize(D);
        scores.resize(K);

        cout << "INITIALIZED: " << model_name << " target: " << target << "\n";
    }

    void pack_features(const Regime& regime){
        x_input[0] = regime.volatility;
        x_input[1] = regime.spread;
        x_input[2] = regime.order_imbalance;
        x_input[3] = regime.trade_imbalance;
        x_input[4] = regime.quote_churn;
        x_input[5] = regime.inventory;
        x_input[6] = regime.inventory_vol;
        x_input[7] = regime.microprice_error;
    }

    void scale(){
        for(int i = 0; i < D; i++){
            x_scaled[i] = (x_input[i] - scaler_mean[i]) / scaler_scale[i];
        }
    }

    tuple<std_string, int, double> predict(const Regime& regime){

        pack_features(regime);
        scale();

        int best_k = -1; //gaussian component regime
        double best_score = -numeric_limits<double>::infinity();

        for(int k = 0; k < K; k++){
            const auto& mu = means[k];
            const auto& inv = cov_inv[k];

            // compute diff = x - mu
            for(int i = 0; i < D; i++){
                diff[i] = x_scaled[i] - mu[i];
            }

            // symmetric quadratic form (OPTIMIZED)
            double quad = 0.0;

            for(int i = 0; i < D; i++){
                double di = diff[i];
                const double* row = inv[i].data();

                quad += di * row[i] * di;

                for(int j = i + 1; j < D; j++){
                    double dj = diff[j];
                    quad += 2.0 * di * row[j] * dj;
                }
            }

            double logp = log_weights[k] - 0.5 * (D * LOG_2PI + log_det_cov[k] + quad);

            scores[k] = logp;

            if(logp > best_score){
                best_score = logp;
                best_k = k;
            }
        }

        // log-sum-exp normalization (stable softmax)
        double max_log = best_score;
        double sum = 0.0;

        for(double s : scores) sum += exp(s - max_log);

        double log_norm = max_log + log(sum);
        double prob = exp(scores[best_k] - log_norm);

        return {regime_labels[best_k], best_k, prob};
    }
};
class MicroSignalModel {
public:
    std_string model_name;
    std_string target;

    double horizon_ms;
    double intercept;
    double beta;

    double ic;
    double rank_ic;

    MicroSignalModel(const json& artifact){
        model_name = artifact["model_name"].get<std_string>();
        target = artifact["target"].get<std_string>();
        horizon_ms = artifact["horizon_ms"].get<double>();
        intercept = artifact["intercept"].get<double>();
        beta = artifact["beta"].get<double>();
        ic = artifact["ic"].get<double>();
        rank_ic = artifact["rank_ic"].get<double>();

        cout << "INITIALIZED: " << model_name << " target: " << target << "\n";
    }

    double predict(const Features& features){
        double micro_signal = features.microprice_dev / features.mid;
        // double fair_bias = ic * beta * micro_signal; // using λ = IC is a heuristic, but it is not what linear regression estimates.
        return intercept + beta * micro_signal;
    }
};

class ResidualModel {
public:
    std_string model_name;
    std_string target;

    BoosterHandle booster;
    vector<std_string> feature_cols;
    uint64_t horizon_ms;
    int D;
    vector<float> x_input;
    DMatrixHandle dmat = nullptr;

    ~ResidualModel(){
        if(dmat) XGDMatrixFree(dmat);
    }

    ResidualModel(const json& artifact){

        model_name = artifact["model_name"].get<std_string>();
        feature_cols = artifact["feature_cols"].get<vector<std_string>>();
        target = artifact["target"].get<std_string>();
        horizon_ms = artifact["horizon_ms"].get<uint64_t>();
        D = artifact["feature_dim"].get<int>();

        if(D < 7) throw runtime_error("feature_dim too small");
        x_input.resize(D);

        std_string model_file = artifact["model_file"].get<std_string>();
        XGBoosterCreate(nullptr, 0, &booster);
        XGBoosterLoadModel(booster, model_file.c_str());

        cout << "INITIALIZED: " << model_name << " target: " << target << "\n";
    }

    void pack_features(const Features& f){
        x_input[0] = f.spread;
        x_input[1] = f.order_imbalance;
        x_input[2] = f.trade_imbalance;
        x_input[3] = f.inventory;
        x_input[4] = f.volatility;
        x_input[5] = f.queue_ahead_bid;
        x_input[6] = f.queue_ahead_ask;
    }

    double predict(const Features& f){
        pack_features(f);

        if(dmat) XGDMatrixFree(dmat);
        XGDMatrixCreateFromMat(x_input.data(), 1, D, NAN, &dmat);

        bst_ulong out_len;
        const float* out_result;

        XGBoosterPredict(
            booster,
            dmat,
            0,          // option_mask
            0,          // ntree_limit
            0,          // training = false (IMPORTANT)
            &out_len,
            &out_result
        );

        return out_result[0];
    }
};

class ToxicityModel {
public:
    std_string model_name;
    std_string target;

    BoosterHandle booster;
    vector<std_string> feature_cols;
    uint64_t horizon_ms;
    int D;
    vector<float> x_input;
    DMatrixHandle dmat = nullptr;

    ~ToxicityModel(){
        if(dmat) XGDMatrixFree(dmat);
    }

    ToxicityModel(const json& artifact){

        model_name = artifact["model_name"].get<std_string>();
        feature_cols = artifact["feature_cols"].get<vector<std_string>>();
        target = artifact["target"].get<std_string>();
        horizon_ms = artifact["horizon_ms"].get<uint64_t>();
        D = artifact["feature_dim"].get<int>();
        
        if(D < 7) throw runtime_error("feature_dim too small");
        x_input.resize(D);

        std_string model_file = artifact["model_file"].get<std_string>();
        XGBoosterCreate(nullptr, 0, &booster);
        XGBoosterLoadModel(booster, model_file.c_str());
        // if(XGBoosterLoadModel(booster, model_file.c_str()) != 0){
        //     throw runtime_error("Failed to load model");
        // }

        cout << "INITIALIZED: " << model_name << ", target: " << target << "\n";
    }

    void pack_features(const Features& f){
        x_input[0] = f.microprice_dev;
        x_input[1] = f.order_imbalance;
        x_input[2] = f.trade_imbalance;
        x_input[3] = f.volatility;
        x_input[4] = f.spread;
        x_input[5] = f.queue_ahead_bid;
        x_input[6] = f.queue_ahead_ask;
    }

    double predict(const Features& f){
        pack_features(f);

        if(dmat) XGDMatrixFree(dmat);
        XGDMatrixCreateFromMat(x_input.data(), 1, D, NAN, &dmat);

        bst_ulong out_len;
        const float* out_result;

        XGBoosterPredict(
            booster,
            dmat,
            0,          // option_mask
            0,          // ntree_limit
            0,          // training = false (IMPORTANT)
            &out_len,
            &out_result
        );

        return out_result[0];
    }
};

class MarketMakingStrategy {
public:
    MarketConfig& config;
    const json& params;
    double gamma;
    std_string folder_path;

    // Models
    std_string struct_model;
    unique_ptr<RegimeModel> regime_model;
    unique_ptr<MicroSignalModel> micro_signal_model;
    unique_ptr<ResidualModel> residual_model;
    unique_ptr<ToxicityModel> toxicity_model;

    MarketMakingStrategy(MarketConfig& config, const json& params)
        : config(config), params(params){
        gamma = params["gamma"].get<double>();
        folder_path = params["folder_path"].get<std_string>();

        struct_model = params["models"]["struct_model"].get<std_string>();
        regime_model       = load_model<RegimeModel>("regime_model");
        micro_signal_model = load_model<MicroSignalModel>("micro_signal_model");
        residual_model     = load_model<ResidualModel>("residual_model");
        toxicity_model     = load_model<ToxicityModel>("toxicity_model");
    }

    template<typename T> unique_ptr<T> load_model(const std_string& model){
        if(params["models"][model].get<std_string>().empty()) return nullptr;

        std_string file = folder_path + "/" + params["models"][model].get<std_string>() + ".json";
        ifstream f(file);
        json artifact;
        f >> artifact;

        return make_unique<T>(artifact);
    }

    // -------------------------
    // CORE ALPHA SIGNALS
    // -------------------------
    pair<double, double> compute_fair_and_micro(State& state, const Policy& policy){
        
        auto& book = state.market_book;

        auto [bid_tick, bid_size] = book.best_bid();
        auto [ask_tick, ask_size] = book.best_ask();

        double best_bid = config.from_tick(bid_tick);
        double best_ask = config.from_tick(ask_tick);

        double microprice = (best_ask * bid_size + best_bid * ask_size) / (bid_size + ask_size + 1e-9);

        double fair = microprice
            + policy.alpha_order_imb * state.order_imbalance // order imbalance, resting flow
            + policy.alpha_trade_imb * state.trade_imbalance; // trade imbalance, aggressive trade flow

        return {fair, microprice};
    }

    double compute_spread(const Features& features, const Policy& policy, const Toxicity& toxicity){
    
        double sigma = features.volatility;

        double base = 0.03; // keep base spread 0.03, since testnet futures have unusually wide spread
        double vol_component = 3.0 * sigma;

        double raw_spread = max(base, vol_component);
        double spread = raw_spread * policy.spread_multiplier * (1.0 + toxicity.k1 * toxicity.tox);

        return spread;
    }

    double compute_skew(State& state, const Policy& policy){
        
        double sigma = state.get_vol();
        double mid = state.market_book.mid();

        double effective_inventory = state.inventory - policy.inventory_target;
        
        return -effective_inventory * gamma * sigma * mid;
    }

    double compute_signal_quality(const auto& log){

        const size_t n = log.size();

        if(n < 20) return 1.0;
        // if(n < 10) return 0.0; trust or no trust?

        vector<double> pred_arr, realized_arr;
        pred_arr.reserve(n);
        realized_arr.reserve(n);

        for(const auto& x: log){
            pred_arr.push_back(x.pred);
            realized_arr.push_back(x.realized);
        }

        auto mean = [](const vector<double>& v){
            double sum = 0.0;
            for(double x: v) sum += x;
            return sum / v.size();
        };

        double mean_pred = mean(pred_arr);
        double mean_realized = mean(realized_arr);

        // -----------------------------
        // Pearson IC
        // -----------------------------
        double cov = 0.0, var_pred = 0.0, var_realized = 0.0;

        for(size_t i = 0; i < n; i++){
            double dev_pred = pred_arr[i] - mean_pred;
            double dev_realized = realized_arr[i] - mean_realized;

            cov += dev_pred * dev_realized;
            var_pred += dev_pred * dev_pred;
            var_realized += dev_realized * dev_realized;
        }

        double ic = 0.0;
        if(var_pred > 1e-12 && var_realized > 1e-12)
            ic = cov / sqrt(var_pred * var_realized);

        if(isnan(ic)) ic = 0.0;

        // -----------------------------
        // Rank IC (Spearman)
        // -----------------------------
        auto rank = [&](const vector<double>& v){
            vector<size_t> id_arr(n);
            vector<double> rank_arr(n);

            iota(id_arr.begin(), id_arr.end(), 0);
            sort(id_arr.begin(), id_arr.end(), [&](const size_t& a, const size_t& b){
                if (v[a] < v[b]) return true;
                if (v[a] > v[b]) return false;
                return a < b;
            });

            size_t i = 0;
            while(i < n){
                size_t j = i;

                // find tie group
                while (j + 1 < n && abs(v[id_arr[j + 1]] - v[id_arr[i]]) < 1e-12) j++;
        
                // average rank for ties
                double avg_rank = (i + j) / 2.0;

                for(size_t k = i; k <= j; k++) rank_arr[id_arr[k]] = avg_rank;

                i = j + 1;
            }
            return rank_arr;
        };

        vector<double> rank_pred = rank(pred_arr);
        vector<double> rank_realized = rank(realized_arr);

        double mean_pred_rank = mean(rank_pred);
        double mean_realized_rank = mean(rank_realized);

        double cov_rank = 0.0, var_pred_rank = 0.0, var_realized_rank = 0.0;

        for(size_t i = 0; i < n; i++){
            double dev_pred_rank = rank_pred[i] - mean_pred_rank;
            double dev_realized_rank = rank_realized[i] - mean_realized_rank;

            cov_rank += dev_pred_rank * dev_realized_rank;
            var_pred_rank  += dev_pred_rank * dev_pred_rank;
            var_realized_rank  += dev_realized_rank * dev_realized_rank;
        }

        double rank_ic = 0.0;
        if(var_pred_rank > 1e-12 && var_realized_rank > 1e-12)
            rank_ic = cov_rank / sqrt(var_pred_rank * var_realized_rank);

        if(isnan(rank_ic)) rank_ic = 0.0;

        // -----------------------------
        // Directional accuracy
        // -----------------------------
        double hits = 0.0;
        for(size_t i = 0; i < n; i++){
            if((pred_arr[i] > 0) == (realized_arr[i] > 0)) hits += 1.0;
        }

        double hit_rate = hits / n;
        double directional_score = (hit_rate - 0.5) * 2.0;  // [-1, 1]

        // -----------------------------
        // Final score
        // -----------------------------
        double vol_pred = sqrt(var_pred / n);
        double stability_penalty = exp(-vol_pred * 50.0);

        double raw = 0.5 * ic + 0.3 * rank_ic + 0.2 * directional_score;
        double signal_quality = stability_penalty * tanh(3.0 * raw);

        return 0.3 + 1.4 * ((signal_quality + 1.0) / 2.0);
    }

    Policy detect_regime(const Regime& regime){
        
        Policy policy;
        
        if(regime_model == nullptr) return policy;

        auto [pred_regime, regime_id, prob] = regime_model->predict(regime);

        policy.regime = pred_regime;
        policy.regime_id = regime_id;
        policy.regime_prob = prob;

        if(pred_regime == "trending"){
            policy.alpha_order_imb = 0.6;
            policy.alpha_trade_imb = 0.2;
            policy.alpha_struct = 0.8;
            policy.spread_multiplier = 2.0;
            policy.k0 = 1.2;
            policy.inventory_target = (regime.trade_imbalance > 0) ? 1.0 : -1.0;
        }
        else if(pred_regime == "toxic"){
            policy.alpha_order_imb = 0.05;
            policy.alpha_trade_imb = 0.01;
            policy.alpha_struct = 0.4;
            policy.spread_multiplier = 1.5;
            policy.k0 = 0.5;
            policy.inventory_target = 0.0;
        }
        else{ // low_vol / normal / competitive
            policy.alpha_order_imb = 0.15;
            policy.alpha_trade_imb = 0.05;
            policy.alpha_struct = 0.2;
            policy.spread_multiplier = 0.7;
            policy.k0 = 1.0;
            policy.inventory_target = 0.0;
        }

        return policy;
    }

    double compute_struct_delta(const Features& features, const Policy& policy){
        
        if(struct_model != "blended_AS") return 0.0;

        double reservation = features.fair + features.skew;
        double struct_center = features.mid + policy.alpha_struct * (reservation - features.mid);
        double struct_delta = struct_center - features.mid;

        return struct_delta;
    }

    double compute_micro_signal_delta(const Features& features){

        if(micro_signal_model == nullptr) return 0.0;

        double fair_bias = micro_signal_model->predict(features);
        double micro_signal_delta = features.mid * fair_bias;

        return micro_signal_delta;
    }

    pair<double, double> compute_residual_delta(State& state, double struct_delta, double micro_signal_delta, 
                                                const Features& features, const Policy& policy){
        
        if(residual_model == nullptr) return {0.0, 0.0};

        double reservation = features.mid + struct_delta + micro_signal_delta;
        double expected_return = residual_model->predict(features);
        double residual_signal_quality = compute_signal_quality(state.mfs.residual_signal_log);

        ResidualPred residual_pred;
        residual_pred.ts = state.last_depth_ts;
        residual_pred.horizon_ms = residual_model->horizon_ms;
        residual_pred.pred = expected_return;
        residual_pred.reservation = reservation;
        state.mfs.residual_predictions.push_back(residual_pred);

        double effective_k = policy.k0 * residual_signal_quality;
        double residual_center = reservation * exp(expected_return * effective_k);

        return {residual_center - reservation, residual_signal_quality};
    }

    Toxicity compute_toxicity(const Features& features){
        
        Toxicity toxicity;

        if(toxicity_model == nullptr) return toxicity;

        double prediction = toxicity_model->predict(features);
        // double signal_quality = compute_toxicity_signal_quality(state.mfs.toxicity_signal_log);

        toxicity.tox = -prediction; //trained on future markout, negative markout = toxic
        // toxicity.k1 *= signal_quality;
        // toxicity.k2 *= signal_quality;
        // toxicity.pred = prediction;

        return toxicity;
    }

    // -------------------------
    // FINAL QUOTE GENERATION
    // -------------------------
    Signal generate_quotes(State& state){

        auto& book = state.market_book;

        auto [bid_tick, bid_size] = book.best_bid();
        auto [ask_tick, ask_size] = book.best_ask();

        double best_bid = config.from_tick(bid_tick);
        double best_ask = config.from_tick(ask_tick);

        double mid = (best_bid + best_ask) / 2.0;

        Regime regime = state.get_regime();
        Policy policy = detect_regime(regime);

        auto [fair, microprice] = compute_fair_and_micro(state, policy);
        double skew = compute_skew(state, policy);

        Features features;
        features.mid = mid;
        features.fair = fair;
        features.skew = skew;
        features.microprice = microprice;
        features.microprice_dev = microprice - mid;
        features.spread = best_ask - best_bid;
        features.order_imbalance = state.order_imbalance;
        features.trade_imbalance = state.trade_imbalance;
        features.inventory = state.inventory;
        features.volatility = state.get_vol();
        features.queue_ahead_bid = state.compute_queue_ahead("bids", config.to_tick(best_bid));
        features.queue_ahead_ask = state.compute_queue_ahead("asks", config.to_tick(best_ask));

        // -------------------------
        // ALPHA STACK
        // -------------------------
        double struct_delta = compute_struct_delta(features, policy);

        double micro_signal_delta = compute_micro_signal_delta(features);

        auto [residual_delta, residual_signal_quality] = compute_residual_delta(state, struct_delta, micro_signal_delta, features, policy);

        double center = mid + struct_delta + micro_signal_delta + residual_delta;
        Toxicity toxicity = compute_toxicity(features);

        double spread = compute_spread(features, policy, toxicity);
        double half = spread / 2.0;

        double bid = center - half;
        double ask = center + half;
        double tick = config.tick_size;

        bid = min(bid, best_bid);
        ask = max(ask, best_ask);

        if(bid >= ask){
            bid = best_bid - tick;
            ask = best_ask + tick;
        }

        bid = config.round_price(bid);
        ask = config.round_price(ask);

        double bid_delta = abs(best_bid - state.mfs.prev_best_bid);
        double ask_delta = abs(best_ask - state.mfs.prev_best_ask);

        Signal signal;
        signal.ts = state.last_depth_ts;
        signal.mid = features.mid;
        signal.microprice = features.microprice;
        signal.microprice_dev = features.microprice_dev;
        signal.microprice_error = features.mid - features.microprice;
        signal.spread = features.spread;
        signal.best_bid = best_bid;
        signal.best_ask = best_ask;

        signal.order_imbalance = features.order_imbalance;
        signal.trade_imbalance = features.trade_imbalance;
        signal.volatility = features.volatility;
        signal.queue_ahead_bid = features.queue_ahead_bid;
        signal.queue_ahead_ask = features.queue_ahead_ask;

        signal.inventory = features.inventory;
        signal.realized_pnl = state.realized_pnl;
        signal.unrealized_pnl = state.get_unrealized_pnl(mid);
        signal.total_pnl = state.get_pnl(mid);
        signal.equity = state.cash + state.inventory * mid;

        signal.fair = features.fair;
        signal.skew = features.skew;
        signal.struct_delta = struct_delta;
        signal.micro_signal_delta = micro_signal_delta;
        signal.residual_delta = residual_delta;
        signal.reservation = center;

        signal.regime = policy.regime;
        signal.regime_id = policy.regime_id;
        signal.regime_prob = policy.regime_prob;
        signal.alpha_order_imb = policy.alpha_order_imb;
        signal.alpha_trade_imb = policy.alpha_trade_imb;
        signal.alpha_struct = policy.alpha_struct;
        signal.k0 = policy.k0;
        signal.spread_multiplier = policy.spread_multiplier;
        signal.inventory_target = policy.inventory_target;
        signal.signal_quality = residual_signal_quality;
        signal.toxicity = toxicity;

        signal.bid_delta = bid_delta;
        signal.ask_delta = ask_delta;
        signal.quote_churn = bid_delta + ask_delta;

        signal.my_bid = bid;
        signal.my_ask = ask;

        return signal;
    }
};
class Execution {
public:
    virtual ~Execution() = default;
};
class BinanceBroker {
public:
    BinanceBroker(MarketConfig&, const json&) {}
};
class LiveExecution : public Execution {
public:
    LiveExecution(MarketConfig&, State&, BinanceBroker&, DatasetRecorder&, const json&) {}
};
class BinanceUserStream {
public:
    BinanceUserStream(State& state, LiveExecution& execution,
                      BinanceBroker& broker, DatasetRecorder& recorder) {}
    void start(){};
};
class PaperExecution : public Execution {
public:
    PaperExecution(MarketConfig&, State&, DatasetRecorder&, const json&) {}
};
class Engine {
public:
    MarketConfig& config;
    const json& params;
    State& state;
    MarketMakingStrategy& strategy;
    Execution& execution;
    DatasetRecorder& recorder;

    EventNotifier& execution_event;
    EventNotifier& dashboard_event;
    std_string header;

    Engine(MarketConfig& config, State& state, MarketMakingStrategy& strategy, Execution& execution,
            DatasetRecorder& recorder, EventNotifier& execution_event, EventNotifier& dashboard_event, const json& params)
        : config(config), state(state), strategy(strategy), execution(execution), recorder(recorder),
        execution_event(execution_event), dashboard_event(dashboard_event), params(params) {build_header();}

    void on_trade_event(const Trade& trade){
        recorder.log_trade(trade);
        // execution.process_trade(trade);

        {
            lock_guard<mutex> lock(dashboard_event.signal_mtx);
            dashboard_event.signal_pending = true;
        }
        dashboard_event.signal_cv.notify_one();
    }

    void on_depth_event(){
        if(!state.initialized) return;

        Signal signal = strategy.generate_quotes(state);
        recorder.log_snapshot(signal);

        {
            lock_guard<mutex> lock(execution_event.signal_mtx);
            state.last_signal = signal;
            execution_event.signal_pending = true;
        }

        {
            lock_guard<mutex> lock(dashboard_event.signal_mtx);
            dashboard_event.signal_pending = true;
        }

        execution_event.signal_cv.notify_one();
        dashboard_event.signal_cv.notify_one();
        // cout << "signal sending, locking..., ts: " << signal.ts << "\n";
    }

    std_string tradeToString(const optional<Trade>& trade){
        return trade ? format("{:<5} | {:>10.4f} | {:>8.6f}", trade->side, trade->price, trade->qty) : "—";
    }

    std_string orderToString(const Order* order){
        return order ? format("{:<5} | {:>10.4f} | {:>8.6f} [{}]", 
            order->side, config.from_tick(order->price_tick), order->qty, order->status) : "—";
    }

    Snapshot build_snapshot(){
        Snapshot snap;

        auto& book = state.market_book;

        auto [bid_tick, bid_size] = book.best_bid();
        auto [ask_tick, ask_size] = book.best_ask();

        double best_bid = config.from_tick(bid_tick);
        double best_ask = config.from_tick(ask_tick);

        double mid = (best_bid + best_ask) / 2.0;
        double spread = best_ask - best_bid;
        double microprice = (best_ask * bid_size + best_bid * ask_size) /(bid_size + ask_size + 1e-9);

        double bid_queue = state.compute_queue_ahead("bids", config.to_tick(best_bid));
        double ask_queue = state.compute_queue_ahead("asks", config.to_tick(best_ask));

        snap.title.header = header;
        snap.title.regime = state.last_signal ? state.last_signal->regime : "";
        snap.title.pnl_pct = state.get_pnl(mid) / state.initial_cash * 100;

        snap.market.mid = mid;
        snap.market.microprice = microprice;
        snap.market.spread = spread;
        snap.market.best_bid = best_bid;
        snap.market.best_ask = best_ask;
        snap.market.bid_size = bid_size;
        snap.market.ask_size = ask_size;
        snap.market.ewma_vol = state.get_vol();
        snap.market.order_imbalance = state.order_imbalance;
        snap.market.trade_imbalance = 0.0;
        snap.market.trade = tradeToString(state.last_trade);

        snap.regime.regime = state.last_signal ? state.last_signal->regime : "";
        snap.regime.confidence = state.last_signal ? state.last_signal->regime_prob : 0.0;

        snap.signals.fair = state.last_signal ? state.last_signal->fair : 0.0;
        snap.signals.skew = state.last_signal ? state.last_signal->skew : 0.0;
        snap.signals.reservation = state.last_signal ? state.last_signal->reservation : 0.0;
        snap.signals.alpha_order_imb = state.last_signal ? state.last_signal->alpha_order_imb : 0.0;
        snap.signals.alpha_trade_imb = state.last_signal ? state.last_signal->alpha_trade_imb : 0.0;
        snap.signals.alpha_struct = state.last_signal ? state.last_signal->alpha_struct : 0.0;
        snap.signals.k0 = state.last_signal ? state.last_signal->k0 : 0.0;
        snap.signals.spread_multiplier = state.last_signal ? state.last_signal->spread_multiplier : 0.0;
        snap.signals.inventory_target = state.last_signal ? state.last_signal->inventory_target : 0.0;
        snap.signals.signal_quality = state.last_signal ? state.last_signal->signal_quality : 0.0;
        snap.signals.tox = state.last_signal ? state.last_signal->toxicity.tox : 0.0;
        snap.signals.k1 = state.last_signal ? state.last_signal->toxicity.k1 : 0.0;
        snap.signals.k2 = state.last_signal ? state.last_signal->toxicity.k2 : 0.0;

        snap.quotes.my_bid = 0.0;
        snap.quotes.my_ask = 0.0;
        snap.quotes.current_bid_size = 0.0;
        snap.quotes.current_ask_size = 0.0;

        snap.execution.bid_queue = bid_queue;
        snap.execution.ask_queue = ask_queue;
        snap.execution.bid_pressure = bid_queue / (bid_size + 1e-9);
        snap.execution.ask_pressure = ask_queue / (ask_size + 1e-9);
        snap.execution.buy_order = orderToString(nullptr);
        snap.execution.sell_order = orderToString(nullptr);
        snap.execution.last_fill_candidate = orderToString(state.last_fill_candidate);
        snap.execution.last_order_update = orderToString(state.last_order_update);

        snap.risk.inventory = state.inventory;
        snap.risk.realized_pnl = state.realized_pnl;
        snap.risk.unrealized_pnl = state.get_unrealized_pnl(mid);
        snap.risk.fees_paid = state.fees_paid;
        snap.risk.total_pnl = state.get_pnl(mid);

        snap.system.time = config.format_ms_precise(config.now_ms());
        snap.system.last_trade_ts = config.format_ms_precise(state.last_trade_ts);
        snap.system.last_depth_ts = config.format_ms_precise(state.last_depth_ts);

        return snap;
    }

    void build_header(){
        std_string parts;

        auto add = [&](const std_string& s){
            if(s.empty()) return;
            if(!parts.empty()) parts += " | ";
            parts += s;
        };

        add(params["models"]["struct_model"].get<std_string>());
        add(params["models"]["regime_model"].get<std_string>());
        add(params["models"]["micro_signal_model"].get<std_string>());
        add(params["models"]["residual_model"].get<std_string>());
        add(params["models"]["toxicity_model"].get<std_string>());
        add(params["mode"].get<std_string>());
        add(params["exchange"].get<std_string>());
        add(params["instrument"].get<std_string>());

        header = parts;
    }
};

class TradingSystem {
public:
    const json& params;
    MarketConfig config;
    State state;

    MarketMakingStrategy strategy;
    DatasetRecorder recorder;

    EventNotifier dashboard_event;
    EventNotifier execution_event;
    SnapshotStore snapshot_store;
    DashboardTerminal dashboard_terminal;
    DashboardServer dashboard_server;

    unique_ptr<Execution> execution;
    unique_ptr<BinanceBroker> broker;
    unique_ptr<BinanceUserStream> user_stream;

    unique_ptr<Engine> engine;
    unique_ptr<Feed> feed;

    atomic<bool> engine_running{false};
    atomic<bool> dashboard_running{false};

    vector<thread> threads;

    TradingSystem(const json& params) : 
        params(params), config(params), state(config, params), strategy(config, params), recorder(config, state, params),
        dashboard_terminal(snapshot_store), dashboard_server(snapshot_store, params) {initialize();}

    void initialize(){
        std_string mode = params["mode"].get<std_string>();
        if(mode == "live"){
            broker = make_unique<BinanceBroker>(config, params);
            execution = make_unique<LiveExecution>(config, state, *broker, recorder, params);
            user_stream = make_unique<BinanceUserStream>(state, *dynamic_cast<LiveExecution*>(execution.get()), *broker, recorder);
        }
        
        else if(mode != "live"){
            execution = make_unique<PaperExecution>(config, state, recorder, params);
        }

        engine = make_unique<Engine>(config, state, strategy, *execution, recorder, execution_event, dashboard_event, params);

        std_string exchange = params["exchange"].get<std_string>();
        auto on_trade_event = [this](const Trade& trade) {engine->on_trade_event(trade);};
        auto on_depth_event = [this]() {engine->on_depth_event();};
        auto log_event = [this](const std_string& type, const uint64_t& ts, const std_string& msg) {recorder.log_event(type, ts, msg);};
        auto log_orderbook_snapshot = [this](const json& snapshot) {recorder.log_orderbook_snapshot(snapshot);};
        
        if(exchange == "binance_spot" && mode != "replay"){
            feed = make_unique<BinanceSpotFeed>(config, state, on_trade_event, on_depth_event, log_event, log_orderbook_snapshot, params);
        }

        else if(exchange == "binance_futures" && mode != "replay"){
            feed = make_unique<BinanceFuturesFeed>(config, state, on_trade_event, on_depth_event, log_event, log_orderbook_snapshot, params);
        }

        else if(exchange == "binance_spot" && mode == "replay"){
            feed = make_unique<BinanceSpotReplayFeed>(config, state, on_trade_event, on_depth_event, log_event, log_orderbook_snapshot, params);
        }

        else if(exchange == "binance_futures" && mode == "replay"){
            feed = make_unique<BinanceFuturesReplayFeed>(config, state, on_trade_event, on_depth_event, log_event, log_orderbook_snapshot, params);
        }
    }

    void start(){
        engine_running = true;
        dashboard_running = true;

        // dashboard_terminal.start();
        dashboard_server.start();

        feed->start();

        if(user_stream) user_stream->start();

        start_dashboard_loop();
        start_execution_loop();

        // Wait 5 seconds
        this_thread::sleep_for(seconds(5)); //FOR TESTING
        
        recorder.export_orderbook_snapshot();
        recorder.export_event_parquet();
    }

    void start_execution_loop(){
        threads.emplace_back([this](){
            while(engine_running){
                Signal signal;
                
                {
                    unique_lock<mutex> lock(execution_event.signal_mtx);
                    execution_event.signal_cv.wait(lock, [this]{
                        return execution_event.signal_pending || !engine_running;});

                    if(!engine_running) break;

                    signal = *state.last_signal;
                    execution_event.signal_pending = false;
                }
                // execution->place_quotes(signal);
                // cout << "execution signal received, unlocking... ts: " << signal.ts << "\n";
            }
        });
    }

    void start_dashboard_loop(){
        threads.emplace_back([this](){
            while(dashboard_running){
                {
                    unique_lock<mutex> lock(dashboard_event.signal_mtx);
                    dashboard_event.signal_cv.wait(lock, [this]{
                        return dashboard_event.signal_pending || !dashboard_running;});

                    if(!dashboard_running) break;
                    dashboard_event.signal_pending = false;
                }

                Snapshot snap = engine->build_snapshot();
                snapshot_store.set(snap);

                // dashboard_terminal.refresh();
                dashboard_server.publish();
                // cout << "dashboard signal received, unlocking... ts: " << snap.system.last_depth_ts << "\n";
            }
        });
    }

    void run_forever(){
        while(engine_running) this_thread::sleep_for(seconds(1));
        shutdown();
    }

    void shutdown(){
        cout << "INTERRUPT RECEIVED - SHUTTING DOWN\n";

        engine_running = false;
        dashboard_running = false;

        // dashboard_terminal.stop();
        dashboard_server.stop();

        for(auto& t: threads){
            if(t.joinable()) t.join();
        }

        cout << "\n";
        recorder.shutdown();
    }
};

int main(){
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);

    if(rc != CURLE_OK){
        cerr << "curl init failed: " << curl_easy_strerror(rc) << endl;
        return 1;
    }

    // std_string path = "D:\\OneDrive\\Trading\\manifest_live.json";
    std_string path = "D:\\OneDrive\\Trading\\manifest_replay.json";
    
    
    // cout << "Enter manifest path: ";
    // getline(cin, path);
    // path = path.substr(1, path.size() - 2);
    
    ifstream f(path);

    if(!f.is_open()){
        cerr << "Cannot open manifest\n";
        return 1;
    }
    cout << path << "\n";
    json params;
    f >> params;

    TradingSystem system(params);

    system.start();

    system.run_forever();

    curl_global_cleanup();

    return 0;
}
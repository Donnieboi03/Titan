#ifndef ENGINE_RUNTIME_TYPES_H
#define ENGINE_RUNTIME_TYPES_H

#include "order_engine.h"
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace scheduler 
{ 
    using WorkerId = std::size_t;
    class JobScheduler; 
    struct Job;
}

namespace backtest 
{

    namespace math
    {
        // 1.00 USD is 10,000 ticks -> 0.01 USD (1 cent) is 100 ticks
        constexpr double PRICE_TICK = 10000.0;
        inline engine::Price dollars_to_ticks(double dollars) { return static_cast<engine::Price>(std::round(dollars * PRICE_TICK)); }
        constexpr inline double ticks_to_dollars(engine::Price ticks) { return static_cast<double>(ticks) / PRICE_TICK; }

        // 1 BTC is 100,000 ticks -> 0.00001 BTC (~$1.00) is 1 tick
        constexpr uint32_t QTY_TICK = 100000;
        inline engine::Quantity qty_to_internal(double value) { return static_cast<engine::Quantity>(std::round(value * QTY_TICK)); }
        constexpr inline double internal_to_qty(engine::Quantity internal_val) { return static_cast<double>(internal_val) / QTY_TICK; }

        // Thresholds updated to match the 10,000 ticks-per-dollar scale
        constexpr inline engine::Quantity get_QTY_TICK(engine::Price price_in_ticks)
        {
            // $1.00 threshold     (1.00 * 10,000 = 10,000 ticks)
            // $100.00 threshold   (100.00 * 10,000 = 1,000,000 ticks)
            // $10,000.00 threshold (10,000.00 * 10,000 = 100,000,000 ticks)

            if (price_in_ticks <= 10000)         return QTY_TICK;       // Under $1: Whole units only
            if (price_in_ticks <= 1000000)       return QTY_TICK / 100; // Under $100: 2 decimals (0.01)
            if (price_in_ticks <= 100000000)     return QTY_TICK / 1000; // Under $10k: 3 decimals (0.001)

            // Default for BTC prices ($100k+): 5 decimals (0.00001)
            // Smallest trade is 1 internal unit (~$1.00 value at $100k BTC)
            return 1;
        }
    }

    namespace user
    {
        using UserId = std::uint32_t; // Id for each unique user
        // Reserved UserId's
        constexpr UserId IPO_HOLDER = 0; // User to hold IPO state
        constexpr UserId INVALID_USER_ID = static_cast<UserId>(-1);

        class User;   // forward
        class UserView; // forward (observational-only handle; see engine_runtime.h)

        // Lock-free user state snapshot for main-thread reads (double-buffered like MarketSnapshot).
        // Single-ticker: each strategy is tied to one ticker at registration.
        struct UserSnapshot
        {
            UserId user_id{INVALID_USER_ID};
            double capital{0.0};
            double realized_pnl{0.0};
            double total_volume{0.0};
            std::string ticker;
            double position{0.0};
            double avg_price{0.0};
            double unrealized_pnl{0.0};

            // Run-stats (updated each quantum; reset via EngineRuntime::reset_user)
            double sum_pnl_deltas{0.0};
            double sum_sq_pnl_deltas{0.0};
            std::size_t n_returns{0};
            double max_drawdown_pct{0.0};

            double mean_return() const {
                return (n_returns > 0) ? sum_pnl_deltas / static_cast<double>(n_returns) : 0.0;
            }
            double variance_of_returns() const {
                if (n_returns <= 1) return 0.0;
                const double n = static_cast<double>(n_returns);
                const double mean = sum_pnl_deltas / n;
                return (sum_sq_pnl_deltas - sum_pnl_deltas * mean) / (n - 1.0);
            }
        };

        // Strategy callback: use pointer to allow null-checks and match call-sites
        using Strategy = std::function<void(User*)>;
    }

    namespace runtime 
    {
        using EngineId = std::uint32_t; // Id for each unique Engine
        static constexpr EngineId INVALID_ENGINE_ID = static_cast<EngineId>(-1);

        class EngineRuntime; // forward declaration

        // Simulation metrics structure for async simulation tracking (double-buffered)
        struct SimulationMetrics {
            // Core metrics (no atomics - double buffered instead)
            std::size_t market_updates_processed{0};
            std::size_t orders_placed{0};
            std::size_t orders_filled{0};
            std::size_t orders_cancelled{0};
            std::size_t orders_edited{0};
            std::size_t orders_replaced{0};
            
            // Performance metrics
            double simulation_time_seconds{0.0};
            
            // Engine utilization
            std::size_t peak_open_orders{0};
            std::size_t final_open_orders{0};
            
            // Market data
            double initial_price{0.0};
            double final_price{0.0};
            
            // System metrics
            std::size_t cache_entries{0};
            bool simulation_running{false};
            
            // Computed rates (no storage; calculated when called)
            double orders_per_second() const {
                return (simulation_time_seconds > 0.0) ? static_cast<double>(orders_placed + orders_cancelled + orders_edited + orders_replaced) / simulation_time_seconds : 0.0;
            }
            double updates_per_second() const {
                return (simulation_time_seconds > 0.0) ? static_cast<double>(market_updates_processed) / simulation_time_seconds : 0.0;
            }
            
            // Reset all metrics for new simulation
            void reset() {
                market_updates_processed = 0;
                orders_placed = 0;
                orders_filled = 0;
                orders_cancelled = 0;
                orders_edited = 0;
                orders_replaced = 0;
                simulation_time_seconds = 0.0;
                peak_open_orders = 0;
                final_open_orders = 0;
                initial_price = 0.0;
                final_price = 0.0;
                cache_entries = 0;
                simulation_running = false;
            }
        };

        // Submit path: main thread reads (worker_id_, capacity_, ticker_, ipo_shares_)
        struct alignas(engine::CACHE_LINE) OrderEngineInfoSubmitPath
        {
            scheduler::WorkerId worker_id_{0};
            std::size_t capacity_{0};
            std::string ticker_;
            engine::Quantity ipo_shares_{0};
        };

        // Worker path: worker thread writes orders_since_quantum_, touches engine_
        struct alignas(engine::CACHE_LINE) OrderEngineInfoWorkerPath
        {
            std::unique_ptr<engine::OrderEngine> engine_;
            std::size_t orders_since_quantum_{0};
            const engine::MarketSnapshot* snapshot_ptr_{nullptr};
            std::unordered_map<engine::OrderId, user::UserId> order_to_user_;
        };

        // Engine-first: one per engine; by_user indexed by user_id
        struct alignas(engine::CACHE_LINE) EngineOrders
        {
            std::vector<std::unordered_set<engine::OrderId>> by_user;
        };

        struct OrderEngineInfo
        {
            OrderEngineInfoSubmitPath submit_;
            OrderEngineInfoWorkerPath worker_;
            std::vector<std::size_t> user_indices_;  // indices into users_ for strategies on this engine

            // Cache-line aligned to prevent false sharing
            alignas(engine::CACHE_LINE) SimulationMetrics sim_metrics_[2];  // Double-buffered simulation metrics
            alignas(engine::CACHE_LINE) std::atomic<int> sim_metrics_index_{0};  // 0 or 1, which buffer readers see

            // Default Constructor
            OrderEngineInfo() = default;

            // Constructor for in-place construction
            OrderEngineInfo(std::size_t capacity, bool verbose, engine::Quantity ipo_shares, scheduler::WorkerId worker_id, EngineId engine_id)
                : submit_{ worker_id, capacity, {}, ipo_shares },
                  worker_{ std::make_unique<engine::OrderEngine>(capacity, verbose, true, static_cast<std::uint16_t>(engine_id)), 0 }
            {}
            
            // Get writable metrics (for simulation/writer thread)
            SimulationMetrics& get_write_metrics() noexcept { 
                return sim_metrics_[1 - sim_metrics_index_.load(std::memory_order_relaxed)]; 
            }
            
            // Get readable metrics (for public API/readers)
            const SimulationMetrics& get_read_metrics() const noexcept { 
                return sim_metrics_[sim_metrics_index_.load(std::memory_order_acquire)]; 
            }
            
            // Publish metrics (swap buffers atomically)
            void publish_metrics() noexcept {
                int write_idx = 1 - sim_metrics_index_.load(std::memory_order_relaxed);
                sim_metrics_index_.store(write_idx, std::memory_order_release);
            }
            
            // Custom move constructor (handle atomic member)
            OrderEngineInfo(OrderEngineInfo&& other) noexcept
                : submit_(std::move(other.submit_)),
                  worker_(std::move(other.worker_)),
                  user_indices_(std::move(other.user_indices_)),
                  sim_metrics_index_(other.sim_metrics_index_.load(std::memory_order_relaxed))
            {
                sim_metrics_[0] = other.sim_metrics_[0];
                sim_metrics_[1] = other.sim_metrics_[1];
            }

            // Custom move assignment (handle atomic member)
            OrderEngineInfo& operator=(OrderEngineInfo&& other) noexcept
            {
                if (this != &other) {
                    submit_ = std::move(other.submit_);
                    worker_ = std::move(other.worker_);
                    user_indices_ = std::move(other.user_indices_);
                    sim_metrics_[0] = other.sim_metrics_[0];
                    sim_metrics_[1] = other.sim_metrics_[1];
                    sim_metrics_index_.store(other.sim_metrics_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                return *this;
            }
            
            // Delete copy operations
            OrderEngineInfo(const OrderEngineInfo&) = delete;
            OrderEngineInfo& operator=(const OrderEngineInfo&) = delete;
        };

        // Type aliases for maps 
        using EngineMap = std::vector<OrderEngineInfo>; // EngineId to EngineInfo
        using TickerMap = std::unordered_map<std::string, EngineId>; // Ticker to EngineId
        using UserOrderMap = std::vector<EngineOrders>;  // engine-first: user_orders_[engine_id].by_user[user_id] = set
    }

} // namespace backtest

#endif // ENGINE_RUNTIME_TYPES_H

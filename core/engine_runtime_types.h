#ifndef ENGINE_RUNTIME_TYPES_H
#define ENGINE_RUNTIME_TYPES_H

#include "order_engine.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace scheduler 
{ 
    using WorkerId = std::size_t;
    class JobScheduler; 
    class Job;
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

        class User; // forward
        // Strategy callback: use pointer to allow null-checks and match call-sites
        using Strategy = std::function<void(User*)>;
    }

    namespace runtime 
    {
        using EngineId = std::uint32_t; // Id for each unique Engine

        class EngineRuntime; // forward declaration

        // Simulation metrics structure for async simulation tracking (double-buffered)
        struct SimulationMetrics {
            // Core metrics (no atomics - double buffered instead)
            std::size_t market_updates_processed{0};
            std::size_t orders_placed{0};
            std::size_t orders_filled{0};
            std::size_t orders_cancelled{0};
            
            // Performance metrics
            double simulation_time_seconds{0.0};
            double orders_per_second{0.0};
            double updates_per_second{0.0};
            
            // Engine utilization
            std::size_t peak_open_orders{0};
            std::size_t final_open_orders{0};
            double average_utilization_percent{0.0};
            
            // Market data
            double initial_price{0.0};
            double final_price{0.0};
            std::size_t unique_price_levels{0};
            
            // System metrics
            std::size_t cache_entries{0};
            bool simulation_running{false};
            
            // Reset all metrics for new simulation
            void reset() {
                market_updates_processed = 0;
                orders_placed = 0;
                orders_filled = 0;
                orders_cancelled = 0;
                simulation_time_seconds = 0.0;
                orders_per_second = 0.0;
                updates_per_second = 0.0;
                peak_open_orders = 0;
                final_open_orders = 0;
                average_utilization_percent = 0.0;
                initial_price = 0.0;
                final_price = 0.0;
                unique_price_levels = 0;
                cache_entries = 0;
                simulation_running = false;
            }
        };

        struct OrderEngineInfo
        {
            std::string ticker_; // Ticker string for this engine (cached for fast lookup)
            std::unique_ptr<engine::OrderEngine> engine_;  // Engine Object (now pointer)
            std::size_t capacity_; // Order pool capacity for this engine (used for simulate batch sizing)
            std::size_t orders_since_quantum_{0}; // Per-engine quantum counter (worker-only access)
            scheduler::WorkerId worker_id_; // Id for Worker
            engine::Quantity ipo_shares_; // Initial IPO
            
            // Cache-line aligned to prevent false sharing
            alignas(CACHE_LINE) SimulationMetrics sim_metrics_[2];  // Double-buffered simulation metrics
            alignas(CACHE_LINE) std::atomic<int> sim_metrics_index_{0};  // 0 or 1, which buffer readers see
            
            
            // Default Constructor
            OrderEngineInfo()
            :engine_(nullptr), capacity_(0)
            {}

            // Constructor for in-place construction
            OrderEngineInfo(std::size_t capacity, bool verbose, engine::Quantity ipo_shares, scheduler::WorkerId worker_id, EngineId engine_id)
                : engine_(std::make_unique<engine::OrderEngine>(capacity, verbose, true, static_cast<std::uint16_t>(engine_id))),
                capacity_(capacity), ipo_shares_(ipo_shares), worker_id_(worker_id)
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
                : engine_(std::move(other.engine_)),
                  ticker_(std::move(other.ticker_)),
                  capacity_(other.capacity_),
                  ipo_shares_(other.ipo_shares_),
                  worker_id_(other.worker_id_),
                  orders_since_quantum_(other.orders_since_quantum_),
                  sim_metrics_index_(other.sim_metrics_index_.load(std::memory_order_relaxed))
            {
                sim_metrics_[0] = other.sim_metrics_[0];
                sim_metrics_[1] = other.sim_metrics_[1];
            }
            
            // Custom move assignment (handle atomic member)
            OrderEngineInfo& operator=(OrderEngineInfo&& other) noexcept
            {
                if (this != &other) {
                    engine_ = std::move(other.engine_);
                    ticker_ = std::move(other.ticker_);
                    capacity_ = other.capacity_;
                    ipo_shares_ = other.ipo_shares_;
                    worker_id_ = other.worker_id_;
                    orders_since_quantum_ = other.orders_since_quantum_;
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
        using UserOrderMap = std::vector<std::vector<std::unordered_set<engine::OrderId>>>; // UserId to EngineId to Orders
    }

} // namespace backtest

#endif // ENGINE_RUNTIME_TYPES_H

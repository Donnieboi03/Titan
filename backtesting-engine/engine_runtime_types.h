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
        inline double ticks_to_dollars(engine::Price ticks) { return static_cast<double>(ticks) / PRICE_TICK; }

        // 1 BTC is 100,000 ticks -> 0.00001 BTC (~$1.00) is 1 tick
        constexpr uint32_t QTY_TICK = 100000;
        inline engine::Quantity qty_to_internal(double value) { return static_cast<engine::Quantity>(std::round(value * QTY_TICK)); }
        inline double internal_to_qty(engine::Quantity internal_val) { return static_cast<double>(internal_val) / QTY_TICK; }

        // Thresholds updated to match the 10,000 ticks-per-dollar scale
        inline engine::Quantity get_QTY_TICK(engine::Price price_in_ticks)
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

        struct OrderEngineInfo
        {
            std::unique_ptr<engine::OrderEngine> engine_;  // Engine Object (now pointer)
            std::string ticker_; // Ticker string for this engine (cached for fast lookup)
            engine::Quantity ipo_shares_; // Initial IPO
            scheduler::WorkerId worker_id_; // Id for Worker
            std::size_t orders_since_quantum_{0}; // Per-engine quantum counter (worker-only access)

            // Default Constructor
            OrderEngineInfo()
            :engine_(nullptr)
            {}

            // Constructor for in-place construction
            OrderEngineInfo(std::size_t capacity, bool verbose, engine::Quantity ipo_shares, scheduler::WorkerId worker_id, EngineId engine_id)
                : engine_(std::make_unique<engine::OrderEngine>(capacity, verbose, true, static_cast<std::uint16_t>(engine_id))),
                ipo_shares_(ipo_shares), worker_id_(worker_id)
            {}
        };

        // Type aliases for maps 
        using EngineMap = std::vector<OrderEngineInfo>; // EngineId to EngineInfo
        using TickerMap = std::unordered_map<std::string, EngineId>; // Ticker to EngineId
        using UserOrderMap = std::vector<std::vector<std::unordered_set<engine::OrderId>>>; // UserId to EngineId to Orders
    }

} // namespace backtest

#endif // ENGINE_RUNTIME_TYPES_H

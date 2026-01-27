#ifndef ENGINE_RUNTIME_H
#define ENGINE_RUNTIME_H

#include "order_engine.h"
#include "job_scheduler.h"
#include "trading_strategy.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cstdint>
#include <atomic>

// Forward declarations
namespace scheduler { class JobScheduler; }
class User;

namespace runtime
{
    // Type aliases
    using UserId = std::uint32_t;
    using EngineId = std::uint32_t;
    
    constexpr UserId IPO_HOLDER = 0;
    constexpr UserId INVALID_USER_ID = static_cast<UserId>(-1);

    // Math utilities
    namespace math
    {
        constexpr double PRICE_TICK = 10000.0;
        constexpr uint32_t QTY_TICK = 100000;

        inline engine::Price dollars_to_ticks(double dollars);
        inline double ticks_to_dollars(engine::Price ticks);
        inline engine::Quantity qty_to_internal(double value);
        inline double internal_to_qty(engine::Quantity internal_val);
        inline engine::Quantity get_QTY_TICK(engine::Price price_in_ticks);
    }

    // OrderEngineInfo structure
    struct OrderEngineInfo;

    // Type aliases for maps
    using EngineMap = std::vector<OrderEngineInfo>;
    using TickerMap = std::unordered_map<std::string, EngineId>;
    // Vector indexed by user_id (0 = IPO_HOLDER, 1+ = registered users)
    // Inner vector indexed by engine_id for O(1) double-indexing
    using UserOrderMap = std::vector<std::vector<std::unordered_set<engine::OrderId>>>;

    // EngineRuntime class declaration
    class EngineRuntime
    {
    public:
        // Delete copy constructor and assignment operator
        EngineRuntime(const EngineRuntime&) = delete;
        EngineRuntime& operator=(const EngineRuntime&) = delete;
        
        // Singleton instance accessor
        static EngineRuntime& get_instance(std::size_t num_threads = 1, std::size_t default_capacity = 1048576, bool verbose = false, std::size_t quantum_orders = 64);
        
        // Reset instance to allow reinitialization
        static void reset_instance();
        
        // Stock registration
        bool register_stock(const std::string& ticker, double ipo_price, double ipo_qty, std::size_t capacity = 0);
        bool unregister_stock(const std::string& ticker);
        void reset();

        // Order submission (asynchronous - returns bool for success/failure)
        bool submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty, UserId user_id = INVALID_USER_ID);
        bool submit_market_order(const std::string& ticker, engine::OrderSide side, double qty, UserId user_id = INVALID_USER_ID);
        bool submit_cancel_order(const std::string& ticker, engine::OrderId order_id, UserId user_id = INVALID_USER_ID);
        bool submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, UserId user_id = INVALID_USER_ID);

        // Market data queries (synchronous)
        double get_market_price(const std::string& ticker) const;
        double get_best_bid(const std::string& ticker) const;
        double get_best_ask(const std::string& ticker) const;
        const engine::OrderInfo* get_order(const std::string& ticker, engine::OrderId order_id) const;
        std::vector<std::pair<double, double>> get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth = 10) const;

        // Utilities
        std::vector<std::string> list_tickers() const noexcept;
        const engine::OrderEngine* get_engine(const std::string& ticker) const;
        bool set_auto_match(const std::string& ticker, bool auto_match);
        bool get_auto_match(const std::string& ticker) const;
        
        // Processing
        void process_pending_orders();
        void process_pending_orders(const std::string& ticker);
        bool all_jobs_completed() const noexcept;
        
        // User order management
        std::vector<engine::OrderId> get_positions(UserId user_id, const std::string& ticker) const;
        bool has_sufficient_shares(UserId user_id, const std::string& ticker, engine::Quantity qty) const;
        
        // Order statistics
        std::size_t get_placed_count(const std::string& ticker) const;
        std::size_t get_cancelled_count(const std::string& ticker) const;
        std::size_t get_filled_count(const std::string& ticker) const;
        std::size_t get_open_count(const std::string& ticker) const;
        
        // Strategy registration and quantum execution
        User* register_strategy(User::Strategy strategy, double starting_capital = 100000.0);
        // Quantum control (configured at startup)
        std::size_t get_quantum() const noexcept;

    private:
        // Private constructor for singleton
        EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool verbose, std::size_t quantum_orders);
        ~EngineRuntime();
        
        // Internal implementation
        class Impl;
        Impl* pimpl_;
        
        static inline bool instance_initialized_ = false;
    };
}

#endif // ENGINE_RUNTIME_H

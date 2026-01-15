#pragma once
#include "order_engine.cpp"
#include "job_scheduler.cpp"
#include "../tools/arena.cpp"
#include "../tools/ring_buffer.cpp"
#include <unordered_set>
#include <cmath>
#include <thread>
#include <mutex>

namespace math
{
    // 1.00 USD is 10,000 ticks -> 0.01 USD (1 cent) is 100 ticks
    constexpr double PRICE_TICK = 10000.0;
    inline engine::Price dollars_to_ticks(double dollars) { return static_cast<engine::Price>(std::round(dollars * PRICE_TICK)); }
    inline double ticks_to_dollars(engine::Price ticks) { return static_cast<double>(ticks) / PRICE_TICK; }

    // 1 BTC is 100,000 ticks -> 0.00001 BTC (~&1.00) is 1 tick
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

using UserId = std::uint32_t;
constexpr UserId IPO_HOLDER = 0;  // IPO holder owns all initial shares

using EngineId = std::uint32_t;
using RequestId = std::uint64_t;

struct OrderEngineInfo
{
    engine::OrderEngine engine_;  // Engine Object
    engine::Quantity ipo_shares_; // Intial IPO
    EngineId engine_id_; // Id for Engine
    scheduler::WorkerId worker_id_; // Id for Worker
    std::size_t batch_counter_; // Per-engine auto-batching counter
    
    // Constructor for in-place construction
    OrderEngineInfo(const std::string& ticker, std::size_t capacity, bool verbose, 
        engine::Quantity ipo_shares, EngineId engine_id, scheduler::WorkerId worker_id)
    :engine_(ticker, capacity, verbose, true),  // auto_match = true
    ipo_shares_(ipo_shares),
    engine_id_(engine_id),
    worker_id_(worker_id),
    batch_counter_(0)
    {}
};

enum class RequestStatus : std::uint8_t
{
    Pending,    // Request has been created but not yet submitted
    InProgress, // Request is currently being processed by a worker
    Completed,  // Request finished successfully and result is available
    Failed,     // Request finished but encountered an error
    Cancelled   // Request was cancelled before completion
};

// Different Request Types
enum class ResultKind : std::uint8_t
{
    None,
    OrderId,
    Price,
    Bool,
};

struct RequestRecord 
{
    RequestId request_id;
    std::atomic<bool> ready;
    ResultKind kind;       
    RequestStatus status; 

    union { // Union Types
        engine::OrderId   order_id;
        engine::Price     price;       
        bool              ok;          
    } result;

    RequestRecord() : request_id(0), ready(false), kind(ResultKind::None), status(RequestStatus::Pending) {}
};

using RequestBuffer = RingBuffer<RequestRecord>;

// Forward declaration
class EngineRuntime;

class RequestHandle 
{
public:
    RequestHandle(RequestId id, EngineRuntime* runtime) : request_id_(id), runtime_(runtime) {}
    
    bool ready() const noexcept;
    
    template<typename T>
    T get_result() const noexcept;

private:
    RequestId request_id_;
    EngineRuntime* runtime_;
};

static std::atomic<RequestId> next_request_id_{1};

using EngineMap = std::unordered_map<std::string, OrderEngineInfo>;

// Type alias for user order tracking
using UserOrderMap = std::unordered_map<UserId, std::unordered_map<std::string, std::unordered_set<engine::OrderId>>>;

class EngineRuntime
{
public:
    // Delete copy constructor and assignment operator
    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;
    
    // Singleton instance accessor
    static EngineRuntime& get_instance(std::size_t num_threads = 1, std::size_t default_capacity = 32768, std::size_t batch_size = 0, bool _verbose = true, bool blocking = true)
    {
        static EngineRuntime instance(num_threads, default_capacity, batch_size, _verbose, blocking);
        return instance;
    }
    
    // Register a new stock in the exchange
    bool register_stock(const std::string& _ticker, engine::Price _ipo_price, engine::Quantity _ipo_qty, std::size_t capacity = 0);
    
    // Unregister a stock from the exchange
    bool unregister_stock(const std::string& _ticker);
    
    // Reset the entire runtime state
    void reset();

    // Order operations - all return handles for async operation
    RequestHandle limit_order(const std::string& _ticker, engine::OrderSide _side, engine::Price _price, engine::Quantity _qty, UserId user_id = 1);
    RequestHandle market_order(const std::string& _ticker, engine::OrderSide _side, engine::Quantity _qty, UserId user_id = 1);
    RequestHandle cancel_order(const std::string& _ticker, engine::OrderId order_id, UserId user_id = 1);

    // Get operations - all return handles for async operation
    RequestHandle get_order(const std::string& _ticker, engine::OrderId order_id);
    RequestHandle get_market_price(const std::string& _ticker);
    RequestHandle get_best_bid(const std::string& _ticker);
    RequestHandle get_best_ask(const std::string& _ticker);

    // Synchronous methods that don't need job scheduling
    std::vector<std::pair<engine::Price, engine::Quantity>> get_market_depth(const std::string& _ticker, engine::OrderSide _side, std::size_t depth = 10) const;
    std::vector<std::string> list_tickers() const noexcept;
    const engine::OrderEngine* get_engine(const std::string& _ticker);
    bool set_auto_match(const std::string& _ticker, bool auto_match);
    bool get_auto_match(const std::string& _ticker) const;
    
    // Batch execution
    void execute_batch() noexcept;
    void execute_batch(scheduler::WorkerId worker_id) noexcept;
    
    // Status checks
    bool all_jobs_completed() const noexcept { return scheduler_.is_complete(); }
    bool is_engine_completed(const std::string& _ticker) const;
    
    // Configuration
    void set_blocking_mode(bool blocking) noexcept { blocking_mode_ = blocking; }
    bool get_blocking_mode() const noexcept { return blocking_mode_; }
    void set_batch_capacity(std::size_t batch_size) noexcept;
    std::size_t get_batch_capacity() const noexcept { return batch_capacity_; }
    
    // User order management
    std::vector<engine::OrderId> get_positions(UserId user_id, const std::string& ticker) const;
    bool has_sufficient_shares(UserId user_id, const std::string& ticker, engine::Quantity qty) const;
    
    // Helper methods for RequestHandle
    bool is_request_ready(RequestId req_id) const noexcept;
    
    template<typename T>
    T get_request_result(RequestId req_id) const noexcept;

private:
    EngineMap stock_exchange_;  // Maps ticker -> OrderEngineInfo (contains engine, ipo_shares, engine_id)
    scheduler::JobScheduler scheduler_;
    RequestBuffer request_buffer_; // Ring buffer for request results
    std::unordered_map<RequestId, RequestRecord*> pending_requests_; // Map request ID to record
    mutable std::mutex request_mutex_; // Mutex for thread-safe request access
    std::size_t num_workers_;  // Number of worker threads
    std::size_t default_capacity_; // Default capacity for new OrderEngines
    std::size_t batch_capacity_;  // Default capacity for Worker Batches
    EngineId next_engine_id_;  // Counter for assigning engine IDs
    bool verbose_; // Verbose Mode
    bool blocking_mode_;  // True = wait for completion, False = async
    std::atomic<bool> main_loop_running_{false}; // Main loop control
    std::thread main_loop_thread_; // Main loop thread
    std::atomic<bool> notification_thread_running_{false}; // Notification thread control
    std::thread notification_thread_; // Notification thread for verbose output
    RingBuffer<std::string> notification_buffer_; // Buffer for notification messages
    mutable std::mutex notification_mutex_; // Mutex for notification buffer
    
    // Order ownership tracking: user_orders_[user_id][ticker] = {order_ids}
    UserOrderMap user_orders_;
    
    // Private constructor for singleton
    EngineRuntime(std::size_t num_threads, std::size_t default_capacity, std::size_t batch_size, bool _verbose, bool blocking);
    
    ~EngineRuntime();
    
    // Thread management
    void start_main_loop();
    void stop_main_loop();
    void start_notification_thread();
    void stop_notification_thread();
    
    // Processing loops
    void main_loop();
    void notification_loop();
    void process_request_buffer();
    void process_notifications();
    
    // Utilities
    RequestId generate_request_id();
    void notify(const std::string& message);
    
    // Request completion
    void complete_request(RequestId req_id, ResultKind kind, RequestStatus status);
    
    template<typename T>
    void complete_request(RequestId req_id, ResultKind kind, RequestStatus status, T result);
};
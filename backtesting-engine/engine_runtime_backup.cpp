#pragma once
#include "order_engine.cpp"
#include "job_scheduler.cpp"
#include "../tools/ring_buffer.cpp"
#include <unordered_set>
#include <cmath>
#include <thread>
#include <mutex>
#include <chrono>
#include <optional>

namespace runtime
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

    using UserId = std::uint32_t;
    constexpr UserId IPO_HOLDER = 0;  // IPO holder owns all initial shares
    constexpr UserId INVALID_USER_ID = static_cast<UserId>(-1);  // Invalid user ID for untracked orders

    using EngineId = std::uint32_t;

    struct OrderEngineInfo
    {
        engine::OrderEngine engine_;  // Engine Object
        engine::Quantity ipo_shares_; // Initial IPO
        scheduler::WorkerId worker_id_; // Id for Worker
        
        // Constructor for in-place construction
        OrderEngineInfo(const std::string& ticker, std::size_t capacity, bool verbose, 
            engine::Quantity ipo_shares, scheduler::WorkerId worker_id)
        :engine_(ticker, capacity, verbose, true),  // auto_match = true
        ipo_shares_(ipo_shares),
        worker_id_(worker_id)
        {}
    };



    using EngineMap = std::unordered_map<EngineId, OrderEngineInfo>;
    using TickerMap = std::unordered_map<std::string, EngineId>;

    // Type alias for user order tracking: user_orders_[user_id][engine_id] = {order_ids}
    using UserOrderMap = std::unordered_map<UserId, std::unordered_map<EngineId, std::unordered_set<engine::OrderId>>>;

    class EngineRuntime
    {
    public:
        // Delete copy constructor and assignment operator
        EngineRuntime(const EngineRuntime&) = delete;
        EngineRuntime& operator=(const EngineRuntime&) = delete;
        
        // Singleton instance accessor
        static EngineRuntime& get_instance(std::size_t num_threads = 1, std::size_t default_capacity = 1048576, bool _verbose = false);
        
        // Register a new stock in the exchange
        bool register_stock(const std::string& _ticker, double _ipo_price, double _ipo_qty, std::size_t capacity = 0);
        
        // Unregister a stock from the exchange
        bool unregister_stock(const std::string& _ticker);
        
        // Reset instance to allow reinitialization with new parameters
        static void reset_instance();
        
        // Clear the runtime state (engines, orders, etc.)
        void reset();

        // === ASYNCHRONOUS WRITE OPERATIONS (submit jobs) ===
        void submit_limit_order(const std::string& _ticker, engine::OrderSide _side, double _price, double _qty, UserId user_id = INVALID_USER_ID);
        void submit_market_order(const std::string& _ticker, engine::OrderSide _side, double _qty, UserId user_id = INVALID_USER_ID);
        void submit_cancel_order(const std::string& _ticker, engine::OrderId order_id, UserId user_id = INVALID_USER_ID);
        void submit_edit_order(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, UserId user_id = INVALID_USER_ID);

        // === SYNCHRONOUS READ OPERATIONS (direct access) ===
        std::optional<double> get_market_price(const std::string& _ticker) const;
        std::optional<double> get_best_bid(const std::string& _ticker) const;
        std::optional<double> get_best_ask(const std::string& _ticker) const;
        const engine::OrderInfo* get_order(const std::string& _ticker, engine::OrderId order_id) const;
        std::vector<std::pair<double, double>> get_market_depth(const std::string& _ticker, engine::OrderSide _side, std::size_t depth = 10) const;

        // Synchronous methods that don't need job scheduling
        std::vector<std::string> list_tickers() const noexcept;
        const engine::OrderEngine* get_engine(const std::string& _ticker) const;
        bool set_auto_match(const std::string& _ticker, bool auto_match);
        bool get_auto_match(const std::string& _ticker) const;
        
        // Process all pending write operations
        void process_pending_orders();
        void process_pending_orders(const std::string& _ticker);
        
        // Status checks
        bool all_jobs_completed() const noexcept { return scheduler_.is_complete(); }
        
        // User order management
        std::vector<engine::OrderId> get_positions(UserId user_id, const std::string& ticker) const;
        bool has_sufficient_shares(UserId user_id, const std::string& ticker, engine::Quantity qty) const;
        
        // Order count verification methods
        std::size_t get_placed_count(const std::string& ticker) const;
        std::size_t get_cancelled_count(const std::string& ticker) const;
        std::size_t get_filled_count(const std::string& ticker) const;
        std::size_t get_open_count(const std::string& ticker) const;

    private:
        static inline bool instance_initialized_ = false;  // Track if instance has been created
        
        EngineMap engines_;  // Maps engine_id -> OrderEngineInfo
        TickerMap ticker_to_engine_id_; // Maps ticker -> engine_id for user-facing API
        scheduler::JobScheduler scheduler_;
        std::size_t num_workers_;  // Number of worker threads
        std::size_t default_capacity_; // Default capacity for new OrderEngines
        EngineId next_engine_id_;  // Counter for assigning engine IDs
        bool verbose_; // Verbose Mode
        
        std::atomic<bool> notification_thread_running_{false}; // Notification thread control
        std::thread notification_thread_; // Notification thread for verbose output
        RingBuffer<std::string> notification_buffer_; // Buffer for notification messages
        mutable std::mutex notification_mutex_; // Mutex for notification buffer
        
        // Order ownership tracking: user_orders_[user_id][ticker] = {order_ids}
        UserOrderMap user_orders_;
        
        // Private constructor for singleton
        EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool _verbose);
        
        ~EngineRuntime();
        
        // Thread management
        void start_notification_thread();
        void stop_notification_thread();
        
        // Processing loops
        void notification_loop();
        void process_notifications();
        
        // Utilities
        void notify(const std::string& message);
    };

    // EngineRuntime Implementation
    EngineRuntime& runtime::EngineRuntime::get_instance(std::size_t num_threads, std::size_t default_capacity, bool _verbose)
    {
        static EngineRuntime* instance_ptr = nullptr;
        
        if (!instance_initialized_) {
            if (instance_ptr != nullptr) {
                delete instance_ptr;
            }
            instance_ptr = new EngineRuntime(num_threads, default_capacity, _verbose);
            instance_initialized_ = true;
        }
        
        return *instance_ptr;
    }

    void runtime::EngineRuntime::reset_instance()
    {
        instance_initialized_ = false;
    }

    runtime::EngineRuntime::EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool _verbose)
        : scheduler_(num_threads), 
        num_workers_(num_threads), 
        default_capacity_(default_capacity),
        next_engine_id_(0),
        verbose_(_verbose),
        notification_buffer_(1000)
    {
        if (verbose_)
            std::cout << "[RUNTIME] Starting EngineRuntime with " << num_threads 
                    << " workers, capacity " << default_capacity << std::endl;
        
        if (verbose_) {
            start_notification_thread();
        }
    }

    runtime::EngineRuntime::~EngineRuntime()
    {
        if (notification_thread_running_) {
            stop_notification_thread();
        }
    }



    void runtime::EngineRuntime::start_notification_thread()
    {
        if (verbose_ && !notification_thread_running_) {
            notification_thread_running_ = true;
            notification_thread_ = std::thread(&EngineRuntime::notification_loop, this);
        }
    }

    void runtime::EngineRuntime::stop_notification_thread()
    {
        notification_thread_running_ = false;
        if (notification_thread_.joinable()) {
            notification_thread_.join();
        }
    }



    void runtime::EngineRuntime::notification_loop()
    {
        while (notification_thread_running_) {
            process_notifications();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void runtime::EngineRuntime::process_notifications()
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        while (!notification_buffer_.empty()) {
            std::string message = notification_buffer_.front();
            notification_buffer_.pop();
            std::cout << message << std::endl;
        }
    }



    void EngineRuntime::notify(const std::string& message)
    {
        if (verbose_ && notification_thread_running_) {
            std::lock_guard<std::mutex> lock(notification_mutex_);
            notification_buffer_.push(message);
        }
    }

    bool EngineRuntime::register_stock(const std::string& _ticker, double _ipo_price, double _ipo_qty, std::size_t capacity)
    {
        try {
            // Verify ticker before creating engine
            if (_ticker.empty()) {
                notify("[REGISTER] ERROR: Empty ticker provided");
                return false;
            }
            
            // IF ipo price or qty is less than or equal to 0
            if (_ipo_price <= 0.0 || _ipo_qty <= 0.0) {
                notify("[REGISTER] ERROR: IPO Price/Quantity must be > 0 for " + _ticker);
                return false;
            }
            
            // If ticker is already in Exchange then error
            if (ticker_to_engine_id_.find(_ticker) != ticker_to_engine_id_.end()) {
                notify("[REGISTER] ERROR: Stock " + _ticker + " already exists");
                return false;
            }

            // Convert user-facing values to internal format
            engine::Price ipo_price_ticks = math::dollars_to_ticks(_ipo_price);
            engine::Quantity ipo_qty_ticks = math::qty_to_internal(_ipo_qty);

            // Use provided capacity or default
            std::size_t engine_capacity = capacity > 0 ? capacity : default_capacity_;
            
            // Assign engine ID for job routing
            EngineId engine_id = next_engine_id_++;

            // Construct OrderEngineInfo directly in the engines map
            auto [it, inserted] = engines_.emplace(
                engine_id,
                OrderEngineInfo(_ticker, engine_capacity, verbose_, ipo_qty_ticks, 
                            engine_id % num_workers_)
            );
            
            if (!inserted) {
                notify("[REGISTER] ERROR: Failed to create engine for " + _ticker);
                return false;
            }
            
            // Add ticker to engine_id mapping
            ticker_to_engine_id_[_ticker] = engine_id;
            
            // Place initial sell at IPO Price and IPO Quantity (from IPO holder)
            std::vector<engine::EngineMsg> msgs;
            engine::OrderId ipo_order = it->second.engine_.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, ipo_price_ticks, ipo_qty_ticks, msgs);
            
            // Track IPO order ownership
            user_orders_[IPO_HOLDER][engine_id].insert(ipo_order);
            
            // Process IPO messages
            for (const auto& msg : msgs) {
                switch (msg.kind) {
                    case engine::EventKind::ACCEPT:
                        notify("[IPO] Order " + std::to_string(ipo_order) + " accepted for " + _ticker);
                        break;
                    case engine::EventKind::REJECT:
                        notify("[IPO] Order " + std::to_string(ipo_order) + " rejected for " + _ticker);
                        break;
                    default:
                        break;
                }
            }
            
            notify("[REGISTER] Registered " + _ticker + " with IPO: " + 
                std::to_string(_ipo_qty) + " shares @ $" + std::to_string(_ipo_price) + 
                " (owned by user " + std::to_string(IPO_HOLDER) + ")");
            
            return true;
        } catch(const std::exception& e) {
            notify("[REGISTER] ERROR for " + _ticker + ": " + e.what());
            return false;
        }
    }

    bool EngineRuntime::unregister_stock(const std::string& _ticker)
    {
        try
        {
            // Verify ticker before processing
            if (_ticker.empty()) {
                notify("[UNREGISTER] ERROR: Empty ticker provided");
                return false;
            }
            
            // Find the ticker-to-engine mapping
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                notify("[UNREGISTER] ERROR: Stock " + _ticker + " does not exist");
                return false;
            }
            
            EngineId engine_id = ticker_it->second;
            auto engine_it = engines_.find(engine_id);
            if (engine_it == engines_.end()) {
                notify("[UNREGISTER] ERROR: Engine not found for " + _ticker);
                return false;
            }

            auto& engine_info = engine_it->second;

            // Wait for worker to finish batch
            scheduler_.process_jobs_on(engine_info.worker_id_);
            
            // Remove from both maps
            ticker_to_engine_id_.erase(_ticker);
            engines_.erase(engine_id);
            
            // Erase all user orders for this engine_id
            for (auto& [user_id, user_engines] : user_orders_) {
                user_engines.erase(engine_id);
            }
            
            notify("[UNREGISTER] Unregistered " + _ticker);
            
            return true;
        }
        catch(const std::exception& e)
        {
            notify("[UNREGISTER] ERROR for " + _ticker + ": " + e.what());
            return false;
        }
    }

    void EngineRuntime::reset()
    {
        try
        {
            scheduler_.process_jobs(); // Wait for pending jobs
            engines_.clear(); // Clear Engines
            ticker_to_engine_id_.clear(); // Clear ticker lookup map
            user_orders_.clear(); // Clear User Orders
            
            // Reset counters
            next_engine_id_ = 0;
            
            notify("[RESET] Reset complete - all stocks and orders cleared");
        }
        catch(const std::exception& e)
        {
            notify("[RESET] ERROR: " + std::string(e.what()));
        }
    }

    void runtime::EngineRuntime::submit_limit_order(const std::string& _ticker, engine::OrderSide _side, double _price, double _qty, UserId user_id)
    {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            notify("[LIMIT ORDER] ERROR: Ticker " + _ticker + " not found");
            return;
        }
        
        EngineId engine_id = ticker_it->second;
        auto engine_it = engines_.find(engine_id);
        if (engine_it == engines_.end()) {
            notify("[LIMIT ORDER] ERROR: Engine not found for " + _ticker);
            return;
        }
        
        // Convert user-facing values to internal format
        engine::Price price_ticks = math::dollars_to_ticks(_price);
        engine::Quantity qty_ticks = math::qty_to_internal(_qty);
        
        auto& engine_info = engine_it->second;
        
        // Use engine_id for direct lookup and inline execution
        auto job = scheduler::make_job([engine_id, _side, price_ticks, qty_ticks, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            auto engine_it = runtime_ptr->engines_.find(engine_id);
            if (engine_it == runtime_ptr->engines_.end()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[LIMIT ORDER] ERROR: Engine not found");
                return;
            }
            
            if (price_ticks <= 0 || qty_ticks <= 0) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[LIMIT ORDER] ERROR: Invalid price/qty");
                return;
            }
            
            // VALIDATE OWNERSHIP BEFORE SUBMITTING (only for registered users)
            if (_side == engine::OrderSide::ASK && user_id != INVALID_USER_ID) {
                auto user_it = runtime_ptr->user_orders_.find(user_id);
                if (user_it != runtime_ptr->user_orders_.end()) {
                    auto engine_map_it = user_it->second.find(engine_id);
                    if (engine_map_it != user_it->second.end()) {
                        engine::Quantity total_owned = 0;
                        for (engine::OrderId order_id : engine_map_it->second) {
                            auto order = engine_it->second.engine_.get_order(order_id);
                            if (order != nullptr && order->side_ == engine::OrderSide::ASK && 
                                order->status_ == engine::OrderStatus::OPEN) {
                                total_owned += order->qty_;
                            }
                        }
                        if (total_owned < qty_ticks) {
                            if (runtime_ptr->verbose_) runtime_ptr->notify("[LIMIT ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                            return;
                        }
                    }
                }
            }
            
            // Early verbose check to avoid message processing overhead
            engine::OrderId order_id;
            if (runtime_ptr->verbose_) {
                std::vector<engine::EngineMsg> msgs;
                order_id = engine_it->second.engine_.place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs);
                
                // Process all messages from the vector
                for (const auto& msg : msgs) {
                    switch (msg.kind) {
                        case engine::EventKind::ACCEPT:
                            runtime_ptr->notify("[LIMIT ORDER] Order " + std::to_string(order_id) + " accepted");
                            break;
                        case engine::EventKind::FILL:
                            runtime_ptr->notify("[LIMIT ORDER] Order " + std::to_string(order_id) + " filled");
                            break;
                        case engine::EventKind::REJECT:
                            runtime_ptr->notify("[LIMIT ORDER] Order " + std::to_string(order_id) + " rejected");
                            break;
                        default:
                            break;
                    }
                }
            } else {
                // Fast path: skip message processing entirely
                order_id = engine_it->second.engine_.place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks);
            }
            
            if (order_id != engine::INVALID_ID && user_id != INVALID_USER_ID) {
                runtime_ptr->user_orders_[user_id][engine_id].insert(order_id);
            }
        }, engine_id); // Use engine_id as owner_id
        
        // Use submit_job_on with stored worker_id
        scheduler_.submit_job_on(engine_info.worker_id_, std::move(job));
    }

    void runtime::EngineRuntime::submit_market_order(const std::string& _ticker, engine::OrderSide _side, double _qty, UserId user_id)
    {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            notify("[MARKET ORDER] ERROR: Ticker " + _ticker + " not found");
            return;
        }
        
        EngineId engine_id = ticker_it->second;
        auto engine_it = engines_.find(engine_id);
        if (engine_it == engines_.end()) {
            notify("[MARKET ORDER] ERROR: Engine not found for " + _ticker);
            return;
        }
        
        // Convert user-facing quantity to internal format
        engine::Quantity qty_ticks = math::qty_to_internal(_qty);
        
        auto& engine_info = engine_it->second;
        
        auto job = scheduler::make_job([engine_id, _side, qty_ticks, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            auto engine_it = runtime_ptr->engines_.find(engine_id);
            if (engine_it == runtime_ptr->engines_.end()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: Engine not found");
                return;
            }
            
            if (qty_ticks <= 0) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: Invalid quantity");
                return;
            }
            
            // VALIDATE OWNERSHIP BEFORE SUBMITTING (only for registered users)
            if (_side == engine::OrderSide::ASK && user_id != INVALID_USER_ID) {
                auto user_it = runtime_ptr->user_orders_.find(user_id);
                if (user_it != runtime_ptr->user_orders_.end()) {
                    auto user_engine_it = user_it->second.find(engine_id);
                    if (user_engine_it != user_it->second.end()) {
                        engine::Quantity total_owned = 0;
                        for (engine::OrderId order_id : user_engine_it->second) {
                            auto order = engine_it->second.engine_.get_order(order_id);
                            if (order != nullptr && order->side_ == engine::OrderSide::ASK && 
                                order->status_ == engine::OrderStatus::OPEN) {
                                total_owned += order->qty_;
                            }
                        }
                        if (total_owned < qty_ticks) {
                            if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                            return;
                        }
                    }
                }
            }
            
            engine::Price market_price = (_side == engine::OrderSide::BID) ? 
                engine_it->second.engine_.get_best_ask() : engine_it->second.engine_.get_best_bid();
            
            if (market_price == static_cast<engine::Price>(-1)) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: No market price available");
                return;
            }
            
            engine::OrderId order_id;
            if (runtime_ptr->verbose_) {
                std::vector<engine::EngineMsg> msgs;
                order_id = engine_it->second.engine_.place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks, msgs);
                
                for (const auto& msg : msgs) {
                    switch (msg.kind) {
                        case engine::EventKind::ACCEPT:
                            runtime_ptr->notify("[MARKET ORDER] Order " + std::to_string(order_id) + " accepted");
                            break;
                        case engine::EventKind::FILL:
                            runtime_ptr->notify("[MARKET ORDER] Order " + std::to_string(order_id) + " filled");
                            break;
                        case engine::EventKind::REJECT:
                            runtime_ptr->notify("[MARKET ORDER] Order " + std::to_string(order_id) + " rejected");
                            break;
                        default:
                            break;
                    }
                }
            } else {
                order_id = engine_it->second.engine_.place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks);
            }
            
            if (order_id != engine::INVALID_ID && user_id != INVALID_USER_ID) {
                runtime_ptr->user_orders_[user_id][engine_id].insert(order_id);
            }
        }, engine_id);
        
        scheduler_.submit_job_on(engine_info.worker_id_, std::move(job));
    }

    void runtime::EngineRuntime::submit_cancel_order(const std::string& _ticker, engine::OrderId order_id, UserId user_id)
    {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            notify("[CANCEL ORDER] ERROR: Ticker " + _ticker + " not found");
            return;
        }
        
        EngineId engine_id = ticker_it->second;
        auto engine_it = engines_.find(engine_id);
        if (engine_it == engines_.end()) {
            notify("[CANCEL ORDER] ERROR: Engine not found for " + _ticker);
            return;
        }
        
        auto& engine_info = engine_it->second;
        
        auto job = scheduler::make_job([engine_id, order_id, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            auto engine_it = runtime_ptr->engines_.find(engine_id);
            if (engine_it == runtime_ptr->engines_.end()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] ERROR: Engine not found");
                return;
            }
            
            engine::EngineMsg msg;
            bool success = engine_it->second.engine_.cancel_order(order_id, msg);
            
            bool actually_cancelled = false;
            
            if (success) {
                switch (msg.kind) {
                    case engine::EventKind::ACCEPT:
                        if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] Order " + std::to_string(order_id) + " cancelled");
                        actually_cancelled = true;
                        break;
                    case engine::EventKind::REJECT:
                        if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] Order " + std::to_string(order_id) + " cancel rejected");
                        break;
                    default:
                        break;
                }
            } else {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] ERROR: Failed to cancel order " + std::to_string(order_id));
            }
            
            if (actually_cancelled && user_id != INVALID_USER_ID) {
                auto user_it = runtime_ptr->user_orders_.find(user_id);
                if (user_it != runtime_ptr->user_orders_.end()) {
                    auto user_engine_it = user_it->second.find(engine_id);
                    if (user_engine_it != user_it->second.end()) {
                        user_engine_it->second.erase(order_id);
                    }
                }
            }
        }, engine_id);
        
        scheduler_.submit_job_on(engine_info.worker_id_, std::move(job));
    }

    void runtime::EngineRuntime::submit_edit_order(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, UserId user_id)
    {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            notify("[EDIT ORDER] ERROR: Ticker " + _ticker + " not found");
            return;
        }
        
        EngineId engine_id = ticker_it->second;
        auto engine_it = engines_.find(engine_id);
        if (engine_it == engines_.end()) {
            notify("[EDIT ORDER] ERROR: Engine not found for " + _ticker);
            return;
        }
        
        // Convert user-facing values to internal format
        engine::Price price_ticks = math::dollars_to_ticks(new_price);
        engine::Quantity qty_ticks = math::qty_to_internal(new_qty);
        
        auto& engine_info = engine_it->second;
        
        auto job = scheduler::make_job([engine_id, order_id, price_ticks, qty_ticks, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            auto engine_it = runtime_ptr->engines_.find(engine_id);
            if (engine_it == runtime_ptr->engines_.end()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Engine not found");
                return;
            }
            
            if (price_ticks <= 0 || qty_ticks <= 0) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Invalid new price/qty");
                return;
            }
            
            const engine::OrderInfo* order = engine_it->second.engine_.get_order(order_id);
            if (order == nullptr) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
                return;
            }
            
            // VALIDATE OWNERSHIP BEFORE EDITING (only for registered users)
            if (order->side_ == engine::OrderSide::ASK && user_id != INVALID_USER_ID) {
                auto user_it = runtime_ptr->user_orders_.find(user_id);
                if (user_it != runtime_ptr->user_orders_.end()) {
                    auto user_engine_it = user_it->second.find(engine_id);
                    if (user_engine_it != user_it->second.end()) {
                        engine::Quantity total_owned = 0;
                        for (engine::OrderId owned_order_id : user_engine_it->second) {
                            auto owned_order = engine_it->second.engine_.get_order(owned_order_id);
                            if (owned_order != nullptr && owned_order->side_ == engine::OrderSide::ASK && 
                                owned_order->status_ == engine::OrderStatus::OPEN) {
                                total_owned += owned_order->qty_;
                            }
                        }
                        if (total_owned < qty_ticks) {
                            if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                            return;
                        }
                    }
                }
            }
            
            std::vector<engine::EngineMsg> msgs;
            engine::OrderId result = engine_it->second.engine_.edit_order(order_id, order->side_, price_ticks, qty_ticks, msgs);
            
            if (runtime_ptr->verbose_) {
                for (const auto& msg : msgs) {
                    switch (msg.kind) {
                        case engine::EventKind::ACCEPT:
                            runtime_ptr->notify("[EDIT ORDER] Order " + std::to_string(order_id) + " edited");
                            break;
                        case engine::EventKind::REJECT:
                            runtime_ptr->notify("[EDIT ORDER] Order " + std::to_string(order_id) + " edit rejected");
                            break;
                        default:
                            break;
                    }
                }
            }
            
            if (result == engine::INVALID_ID) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Failed to edit order " + std::to_string(order_id));
            }
        }, engine_id);
        
        scheduler_.submit_job_on(engine_info.worker_id_, std::move(job));
    }

    // === SYNCHRONOUS READ OPERATIONS ===

    std::optional<double> EngineRuntime::get_market_price(const std::string& _ticker) const
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                engine::Price price = engine_it->second.engine_.get_market_price();
                if (price != static_cast<engine::Price>(-1)) {
                    return math::ticks_to_dollars(price);
                }
            }
        }
        return std::nullopt;
    }

    std::optional<double> EngineRuntime::get_best_bid(const std::string& _ticker) const
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                engine::Price price = engine_it->second.engine_.get_best_bid();
                if (price != static_cast<engine::Price>(-1)) {
                    return math::ticks_to_dollars(price);
                }
            }
        }
        return std::nullopt;
    }

    std::optional<double> EngineRuntime::get_best_ask(const std::string& _ticker) const
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                engine::Price price = engine_it->second.engine_.get_best_ask();
                if (price != static_cast<engine::Price>(-1)) {
                    return math::ticks_to_dollars(price);
                }
            }
        }
        return std::nullopt;
    }

    const engine::OrderInfo* EngineRuntime::get_order(const std::string& _ticker, engine::OrderId order_id) const
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                return engine_it->second.engine_.get_order(order_id);
            }
        }
        return nullptr;
    }

    // Synchronous methods that don't need job scheduling
    std::vector<std::pair<double, double>> EngineRuntime::get_market_depth(const std::string& _ticker, engine::OrderSide _side, std::size_t depth) const
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                auto internal_depth = engine_it->second.engine_.get_market_depth(_side, depth);
                std::vector<std::pair<double, double>> user_depth;
                user_depth.reserve(internal_depth.size());
                
                for (const auto& [price_ticks, qty_ticks] : internal_depth) {
                    user_depth.emplace_back(
                        math::ticks_to_dollars(price_ticks),
                        math::internal_to_qty(qty_ticks)
                    );
                }
                return user_depth;
            }
        }
        return {};
    }

    std::vector<std::string> EngineRuntime::list_tickers() const noexcept
    {
        std::vector<std::string> tickers;
        tickers.reserve(ticker_to_engine_id_.size());
        for (const auto& [ticker, _] : ticker_to_engine_id_) {
            tickers.push_back(ticker);
        }
        return tickers;
    }

    const engine::OrderEngine* EngineRuntime::get_engine(const std::string& _ticker) const
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                return &engine_it->second.engine_;
            }
        }
        return nullptr;
    }

    bool runtime::EngineRuntime::set_auto_match(const std::string& _ticker, bool auto_match)
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                engine_it->second.engine_.set_auto_match(auto_match);
                return true;
            }
        }
        return false;
    }

    bool runtime::EngineRuntime::get_auto_match(const std::string& _ticker) const
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                return engine_it->second.engine_.get_auto_match();
            }
        }
        return false;
    }

    // === PROCESSING METHODS ===

    void EngineRuntime::process_pending_orders()
    {
        scheduler_.process_jobs();
    }

    void runtime::EngineRuntime::process_pending_orders(const std::string& _ticker)
    {
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it != ticker_to_engine_id_.end()) {
            auto engine_it = engines_.find(ticker_it->second);
            if (engine_it != engines_.end()) {
                scheduler_.process_jobs_on(engine_it->second.worker_id_);
            }
        }
    }

    std::vector<engine::OrderId> EngineRuntime::get_positions(UserId user_id, const std::string& ticker) const
    {
        // Convert ticker to engine_id for lookup
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            return {};
        }
        
        EngineId engine_id = ticker_it->second;
        std::vector<engine::OrderId> positions;
        auto user_it = user_orders_.find(user_id);
        if (user_it != user_orders_.end()) {
            auto engine_id_it = user_it->second.find(engine_id);
            if (engine_id_it != user_it->second.end()) {
                positions.reserve(engine_id_it->second.size());
                for (engine::OrderId order_id : engine_id_it->second) {
                    positions.push_back(order_id);
                }
            }
        }
        return positions;
    }

    bool runtime::EngineRuntime::has_sufficient_shares(UserId user_id, const std::string& ticker, engine::Quantity qty) const
    {
        try {
            auto user_it = user_orders_.find(user_id);
            if (user_it == user_orders_.end()) return false;
            
            auto ticker_it = ticker_to_engine_id_.find(ticker);
            if (ticker_it == ticker_to_engine_id_.end()) return false;
            
            EngineId engine_id = ticker_it->second;
            auto engine_id_it = user_it->second.find(engine_id);
            if (engine_id_it == user_it->second.end()) return false;
            
            auto engine_it = engines_.find(engine_id);
            if (engine_it == engines_.end()) return false;
            
            engine::Quantity total_owned = 0;
            const engine::OrderEngine& engine = engine_it->second.engine_;
            for (engine::OrderId order_id : engine_id_it->second) {
                auto order = engine.get_order(order_id);
                if (order != nullptr && order->side_ == engine::OrderSide::ASK && 
                    order->status_ == engine::OrderStatus::OPEN) {
                    total_owned += order->qty_;
                }
            }
            
            return total_owned >= qty;
        } catch (...) {
            return false;
        }
    }

    // Order count verification methods implementation
    std::size_t runtime::EngineRuntime::get_placed_count(const std::string& ticker) const {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) return 0;
        
        auto engine_it = engines_.find(ticker_it->second);
        if (engine_it == engines_.end()) return 0;
        
        return engine_it->second.engine_.placed_count();
    }

    std::size_t runtime::EngineRuntime::get_cancelled_count(const std::string& ticker) const {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) return 0;
        
        auto engine_it = engines_.find(ticker_it->second);
        if (engine_it == engines_.end()) return 0;
        
        return engine_it->second.engine_.cancelled_count();
    }

    std::size_t runtime::EngineRuntime::get_filled_count(const std::string& ticker) const {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) return 0;
        
        auto engine_it = engines_.find(ticker_it->second);
        if (engine_it == engines_.end()) return 0;
        
        return engine_it->second.engine_.filled_count();
    }

    std::size_t runtime::EngineRuntime::get_open_count(const std::string& ticker) const {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) return 0;
        
        auto engine_it = engines_.find(ticker_it->second);
        if (engine_it == engines_.end()) return 0;
        
        return engine_it->second.engine_.open_count();
    }
}
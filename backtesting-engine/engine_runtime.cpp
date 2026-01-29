#pragma once
#include "order_engine.cpp"
#include "../tools/job_scheduler.cpp"
#include "../tools/lazy_queue.cpp"
#include <unordered_set>
#include <functional>
#include <cmath>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <atomic>

namespace runtime { class EngineRuntime; }

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
    
    struct User
    {
    public:
        using Strategy = std::function<void(User*)>;
        
        User()
        : user_id_(INVALID_USER_ID)
        {}

        User(Strategy&& strategy, EngineRuntime* runtime, UserId user_id, double initial_capital = 100000.0)
        : strategy_(std::move(strategy)), runtime_(runtime), user_id_(user_id), capital_(initial_capital), realized_pnl_(0.0), total_volume_(0.0) 
        {}

        // Called on each book update (every quantum)
        void on_book_update()
        {
            if (strategy_)
                strategy_(this);
        }

        // Getters
        UserId get_user_id() const { return user_id_; }
        EngineRuntime* get_runtime() const { return runtime_; }

        // Order submission wrapper methods (automatically use this user's ID)
        bool submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double quantity);
        bool submit_market_order(const std::string& ticker, engine::OrderSide side, double quantity);
        bool submit_cancel_order(const std::string& ticker, engine::OrderId order_id);
        bool submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_quantity);

        // Position query wrapper methods
        std::vector<engine::OrderId> get_open_positions(const std::string& ticker) const;

        // Market data access (convenience methods)
        double get_best_bid(const std::string& ticker) const;
        double get_best_ask(const std::string& ticker) const;
        std::vector<std::pair<double, double>> get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth) const;

        // Position tracking
        double get_position(const std::string& ticker) const;
        const std::unordered_map<std::string, double>& get_all_positions() const;

        // Capital and PnL
        double get_capital() const { return capital_; }
        double get_realized_pnl() const { return realized_pnl_; }
        double get_unrealized_pnl(const std::string& ticker, double current_price) const;
        double get_total_volume() const { return total_volume_; }

        // Internal methods (called by runtime on fills)
        void update_position(const std::string& ticker, double qty, double price);
        void update_realized_pnl(double pnl);
        // Reservation management (exposed to runtime)
        void reserve_on_accept(engine::OrderId order_id, engine::OrderSide side, double qty, double price) {
            if (order_id == engine::INVALID_ID) return;
            // If a previous reservation exists for this order, release it first
            auto it_prev_cash = reserved_cash_.find(order_id);
            if (it_prev_cash != reserved_cash_.end()) {
                capital_ += it_prev_cash->second;
                reserved_cash_.erase(it_prev_cash);
            }
            reserved_side_[order_id] = side;
            reserved_qty_[order_id] = qty;
            if (side == engine::OrderSide::BID) {
                double amt = qty * price;
                reserved_cash_[order_id] = amt;
                // take from capital (move to reserved)
                capital_ -= amt;
            } else {
                // ASK: reserve shares (no cash movement at accept)
                reserved_cash_[order_id] = 0.0;
            }
        }

        void release_reservation(engine::OrderId order_id) {
            if (order_id == engine::INVALID_ID) return;
            auto itc = reserved_cash_.find(order_id);
            if (itc != reserved_cash_.end()) {
                double amt = itc->second;
                // refund reserved cash to capital
                capital_ += amt;
                reserved_cash_.erase(itc);
            }
            auto itq = reserved_qty_.find(order_id);
            if (itq != reserved_qty_.end()) reserved_qty_.erase(itq);
            auto its = reserved_side_.find(order_id);
            if (its != reserved_side_.end()) reserved_side_.erase(its);
        }

        void apply_fill(const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price) {
            if (order_id == engine::INVALID_ID) return;
            // Reconcile reservation and cash
            auto it_side = reserved_side_.find(order_id);
            if (it_side != reserved_side_.end() && it_side->second == side) {
                // Reserved info present
                double reserved_total_qty = 0.0;
                auto it_rq = reserved_qty_.find(order_id);
                if (it_rq != reserved_qty_.end()) reserved_total_qty = it_rq->second;
                double reserved_cash_total = 0.0;
                auto it_rc = reserved_cash_.find(order_id);
                if (it_rc != reserved_cash_.end()) reserved_cash_total = it_rc->second;

                if (side == engine::OrderSide::BID) {
                    // For buys: adjust capital by difference between reserved portion and executed cost
                    if (reserved_total_qty > 0.0 && reserved_cash_total > 0.0) {
                        double portion_reserved = reserved_cash_total * (qty / reserved_total_qty);
                        double executed_cost = qty * price;
                        double delta = portion_reserved - executed_cost;
                        // refund (positive) or charge extra (negative)
                        capital_ += delta;
                        // decrement reservation
                        reserved_cash_[order_id] = reserved_cash_total - portion_reserved;
                        reserved_qty_[order_id] = reserved_total_qty - qty;
                        if (reserved_qty_[order_id] <= 0.0) {
                            reserved_qty_.erase(order_id);
                            reserved_cash_.erase(order_id);
                            reserved_side_.erase(order_id);
                        }
                    } else {
                        // No reservation recorded; fall back to charging executed cost
                        capital_ -= qty * price;
                    }
                } else {
                    // ASK: when filled, receive proceeds
                    capital_ += qty * price;
                    if (it_rq != reserved_qty_.end()) {
                        double remain = it_rq->second - qty;
                        if (remain <= 0.0) {
                            reserved_qty_.erase(order_id);
                            reserved_side_.erase(order_id);
                            reserved_cash_.erase(order_id);
                        } else {
                            reserved_qty_[order_id] = remain;
                        }
                    }
                }
            } else {
                // No reservation present; default behavior: apply cash move
                if (side == engine::OrderSide::BID) capital_ -= qty * price;
                else capital_ += qty * price;
            }

            // Update position (signed qty) and allow update_position to handle realized pnl
            double signed_qty = (side == engine::OrderSide::BID) ? qty : -qty;
            if (!ticker.empty()) {
                update_position(ticker, signed_qty, price);
            }
        }
        // Allow runtime to call reservation helpers
        friend class EngineRuntime;

    private:
        Strategy strategy_;
        EngineRuntime* runtime_;
        UserId user_id_;

        double capital_;
        double realized_pnl_;
        double total_volume_;
        
        std::unordered_map<std::string, double> positions_;
        std::unordered_map<std::string, double> avg_prices_;

        // Reservations for accepted (but not yet filled) orders
        std::unordered_map<engine::OrderId, double> reserved_cash_; // cash reserved for BID orders (in dollars)
        std::unordered_map<engine::OrderId, double> reserved_qty_;  // qty reserved for order id (in asset units)
        std::unordered_map<engine::OrderId, engine::OrderSide> reserved_side_; // side for reservation
        // Reservation management (private storage)
    };


    using EngineId = std::uint32_t;

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

    using EngineMap = std::vector<OrderEngineInfo>;
    using TickerMap = std::unordered_map<std::string, EngineId>;

    // Type alias for user order tracking: user_orders_[user_id][engine_id] = {order_ids}
    // Vector indexed by user_id (0 = IPO_HOLDER, 1+ = registered users)
    // Inner vector indexed by engine_id for O(1) access
    using UserOrderMap = std::vector<std::vector<std::unordered_set<engine::OrderId>>>;

    class EngineRuntime
    {
    public:
        // Delete copy constructor and assignment operator
        EngineRuntime(const EngineRuntime&) = delete;
        EngineRuntime& operator=(const EngineRuntime&) = delete;
        
        // Singleton instance accessor
        static EngineRuntime& get_instance(std::size_t num_threads = 1, std::size_t default_capacity = 1048576, bool _verbose = false, std::size_t quantum_orders = 64);
        
        // Register a new stock in the exchange
        bool register_stock(const std::string& _ticker, double _ipo_price, double _ipo_qty, std::size_t capacity = 0);
        
        // Unregister a stock from the exchange
        bool unregister_stock(const std::string& _ticker);
        
        // Reset instance to allow reinitialization with new parameters
        static void reset_instance();
        
        // Clear the runtime state (engines, orders, etc.)
        void reset();

        // === ASYNCHRONOUS WRITE OPERATIONS (submit jobs) ===
        bool submit_limit_order(const std::string& _ticker, engine::OrderSide _side, double _price, double _qty, UserId user_id = INVALID_USER_ID);
        bool submit_market_order(const std::string& _ticker, engine::OrderSide _side, double _qty, UserId user_id = INVALID_USER_ID);
        bool submit_cancel_order(const std::string& _ticker, engine::OrderId order_id, UserId user_id = INVALID_USER_ID);
        bool submit_edit_order(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, UserId user_id = INVALID_USER_ID);

        // === SYNCHRONOUS READ OPERATIONS (direct access) ===
        double get_market_price(const std::string& _ticker) const;
        double get_best_bid(const std::string& _ticker) const;
        double get_best_ask(const std::string& _ticker) const;
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
        bool all_jobs_completed() noexcept { return scheduler_.is_complete(); }
        
        // User order management
        std::vector<engine::OrderId> get_positions(UserId user_id, const std::string& ticker) const;
        bool has_sufficient_shares(UserId user_id, const std::string& ticker, engine::Quantity qty) const;
        
        // Order count verification methods (via snapshot)
        std::size_t get_placed_count(const std::string& ticker) const;
        std::size_t get_cancelled_count(const std::string& ticker) const;
        std::size_t get_filled_count(const std::string& ticker) const;
        std::size_t get_open_count(const std::string& ticker) const;
        
        // === STRATEGY & EVENT LOOP ===
        // Register a trading strategy and return pointer to User object
        User* register_strategy(User::Strategy strategy, double starting_capital = 100000.0);
        
        // Get configured quantum
        std::size_t get_quantum() const noexcept { return quantum_orders_; }
        // Batch size control (forwarded to job scheduler)
        void set_batch_size(std::size_t n) noexcept;
        std::size_t get_batch_size() const noexcept;

    private:
        static inline bool instance_initialized_ = false;  // Track if instance has been created
        
        EngineMap engines_info_;  // Maps engine_id -> OrderEngineInfo
        TickerMap ticker_to_engine_id_; // Maps ticker -> engine_id for user-facing API
        scheduler::JobScheduler scheduler_;
        std::size_t num_workers_;  // Number of worker threads
        std::size_t default_capacity_; // Default capacity for new OrderEngines
        bool verbose_; // Verbose Mode
        
        std::atomic<bool> notification_thread_running_{false}; // Notification thread control
        std::thread notification_thread_; // Notification thread for verbose output
        LazyQueue<std::string> notification_buffer_; // Buffer for notification messages
        mutable std::mutex notification_mutex_; // Mutex for notification buffer
        std::condition_variable notification_cv_; // Sleep-lock for notification thread
        
        // Order ownership tracking: user_orders_[user_id][engine_id] = {order_ids}
        // Index 0 reserved for IPO_HOLDER, registered users start from index 1
        UserOrderMap user_orders_;
        // Reverse map: external OrderId -> UserId
        std::unordered_map<engine::OrderId, UserId> order_to_user_;
        
        // Fast snapshot access cache: store pointer to the engine so we can fetch
        // the currently active snapshot via `get_snapshot()` (avoids stale-buffer bug)
        std::vector<engine::OrderEngine*> snapshot_cache_;
        
        // Strategy management (raw pointers, caller manages lifetime)
        
        std::vector<User> users_;
        
        // Quantum execution control (configured at construction)
        const std::size_t quantum_orders_; // Quantum in order count (immutable after construction)
        // Runtime-enforced batch size for flushing jobs to workers
        std::size_t runtime_batch_size_;
        // Global quantum counter for strategy execution (shared across engines)
        std::atomic<std::size_t> global_orders_since_quantum_{0};
        
        
        // Private constructor for singleton
        EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool _verbose, std::size_t quantum_orders);
        
        ~EngineRuntime();
        
        // Thread management
        void start_notification_thread() noexcept;
        void stop_notification_thread() noexcept;
        
        // Processing loops
        void notification_loop() noexcept;
        
        // Snapshot management (internal)
        void update_snapshot_internal(EngineId engine_id) noexcept;
        const engine::MarketSnapshot* get_snapshot_fast(EngineId engine_id) const noexcept;
        
        // Order tracking for quantum (per-engine)
        void increment_order_counter(EngineId engine_id) noexcept;
        
        // Utilities
        void notify(const std::string& message) noexcept;
        
        // Internal wrapper to submit jobs to a worker while enforcing runtime batch-size
        void submit_job_on_worker(scheduler::WorkerId worker_id, scheduler::Job&& job) noexcept;
    };

    // EngineRuntime Implementation
    EngineRuntime& runtime::EngineRuntime::get_instance(std::size_t num_threads, std::size_t default_capacity, bool _verbose, std::size_t quantum_orders)
    {
        static EngineRuntime* instance_ptr = nullptr;
        
        if (!instance_initialized_) 
        {
            if (instance_ptr != nullptr) 
            {
                delete instance_ptr;
            }
            instance_ptr = new EngineRuntime(num_threads, default_capacity, _verbose, quantum_orders);
            instance_initialized_ = true;
        }
        
        return *instance_ptr;
    }

    void runtime::EngineRuntime::reset_instance()
    {
        auto& runtime = get_instance();
        runtime.reset();
        instance_initialized_ = false;
    }

    runtime::EngineRuntime::EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool _verbose, std::size_t quantum_orders)
        : scheduler_(num_threads), 
        num_workers_(num_threads), 
        default_capacity_(default_capacity),
        verbose_(_verbose),
        quantum_orders_(quantum_orders),
        global_orders_since_quantum_(0),
        notification_buffer_(1000)
    {
        // Initialize runtime batch size to scheduler's batch capacity by default
        runtime_batch_size_ = scheduler_.get_batch_capacity();
        engines_info_.reserve(100);  // Reserve space for up to 100 engine pointers
        
        // ensure we have room for common workloads. Index 0 (IPO HOLDER) is reserved
        users_.reserve(1000);    // Reserve space for up to 100 users
        user_orders_.reserve(users_.capacity());
        // Ensure IPO holder slot exists
        if (user_orders_.empty()) {
            user_orders_.resize(1);
        }

        if (verbose_)
            std::cout << "[RUNTIME] Starting EngineRuntime with " << num_threads
                    << " workers, capacity " << default_capacity << std::endl;

        if (verbose_)
        {
            start_notification_thread();
        }
    }

    runtime::EngineRuntime::~EngineRuntime()
    {
        if (notification_thread_running_) 
        {
            stop_notification_thread();
        }
    }

    

    void runtime::EngineRuntime::start_notification_thread() noexcept
    {
        if (verbose_ && !notification_thread_running_) 
        {
            notification_thread_running_ = true;
            notification_thread_ = std::thread(&EngineRuntime::notification_loop, this);
        }
    }

    void runtime::EngineRuntime::stop_notification_thread() noexcept
    {
        notification_thread_running_ = false;
        // Wake notification thread so it can exit promptly
        notification_cv_.notify_one();
        if (notification_thread_.joinable()) 
        {
            notification_thread_.join();
        }
    }



    void runtime::EngineRuntime::notification_loop() noexcept
    {
        std::unique_lock<std::mutex> lock(notification_mutex_);
        while (notification_thread_running_) 
        {
            // Wait until there is a message or the thread is asked to stop
            notification_cv_.wait(lock, [this]() {
                return !notification_buffer_.empty() || !notification_thread_running_;
            });

            // Drain available messages
            while (!notification_buffer_.empty()) {
                std::string message = notification_buffer_.front();
                notification_buffer_.pop();
                // Unlock while performing IO to avoid blocking producers
                lock.unlock();
                std::cout << message << std::endl;
                lock.lock();
            }
        }
    }

    void EngineRuntime::notify(const std::string& message) noexcept
    {
        if (verbose_ && notification_thread_running_) 
        {
            {
                std::lock_guard<std::mutex> lock(notification_mutex_);
                notification_buffer_.push(message);
            }
            // Wake the notification thread
            notification_cv_.notify_one();
        }
    }

    void runtime::EngineRuntime::update_snapshot_internal(EngineId engine_id) noexcept
    {
        if (engine_id < engines_info_.size()) {
            engines_info_[engine_id].engine_->update_snapshot();
        }
    }

    const engine::MarketSnapshot* runtime::EngineRuntime::get_snapshot_fast(EngineId engine_id) const noexcept
    {
        if (engine_id < snapshot_cache_.size() && snapshot_cache_[engine_id]) {
            return &snapshot_cache_[engine_id]->get_snapshot();
        }
        return nullptr;
    }

    bool EngineRuntime::register_stock(const std::string& _ticker, double _ipo_price, double _ipo_qty, std::size_t capacity)
    {
        try {
            // Verify ticker before creating engine
            if (_ticker.empty()) 
            {
                notify("[REGISTER] ERROR: Empty ticker provided");
                return false;
            }
            // IF ipo price or qty is less than or equal to 0
            if (_ipo_price <= 0.0 || _ipo_qty <= 0.0) 
            {
                notify("[REGISTER] ERROR: IPO Price/Quantity must be > 0 for " + _ticker);
                return false;
            }
            
            // If ticker is already in Exchange then error
            if (ticker_to_engine_id_.find(_ticker) != ticker_to_engine_id_.end()) 
            {
                notify("[REGISTER] ERROR: Stock " + _ticker + " already exists");
                return false;
            }
            
            // Check engine limit (vector capacity reserved at 100)
            if (engines_info_.size() >= 100) 
            {
                notify("[REGISTER] ERROR: Maximum engine limit (100) reached");
                return false;
            }

            // Convert user-facing values to internal format
            engine::Price ipo_price_ticks = math::dollars_to_ticks(_ipo_price);
            engine::Quantity ipo_qty_ticks = math::qty_to_internal(_ipo_qty);

            // Use provided capacity or default
            std::size_t engine_capacity = capacity > 0 ? std::min(capacity, default_capacity_) : default_capacity_;
            
            // Calculate engine ID from vector size before emplacing
            EngineId engine_id = engines_info_.size();

            // Add OrderEngineInfo to engines vector
            engines_info_.emplace_back(engine_capacity, verbose_, ipo_qty_ticks, engine_id % num_workers_, engine_id);
            // Cache ticker string on the engine info for fast reverse lookup
            if (engine_id < engines_info_.size()) {
                engines_info_[engine_id].ticker_ = _ticker;
            }
            
            // Add ticker to engine_id mapping
            ticker_to_engine_id_[_ticker] = engine_id;
            
            // Expand snapshot cache if needed and cache snapshot pointer
            if (snapshot_cache_.size() <= engine_id) {
                snapshot_cache_.resize(engine_id + 1, nullptr);
            }
            // Store engine pointer so reads fetch the currently active snapshot
            snapshot_cache_[engine_id] = engines_info_[engine_id].engine_.get();
            
            // Expand all existing users' engine vectors to accommodate new engine
            for (auto& user_engines : user_orders_) {
                if (user_engines.size() <= engine_id) {
                    user_engines.resize(engine_id + 1);
                }
            }
            
            // Place initial sell at IPO Price and IPO Quantity (from IPO holder)
            std::vector<engine::EngineMsg> msgs;
            engine::OrderId ipo_order = engines_info_[engine_id].engine_->place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, ipo_price_ticks, ipo_qty_ticks, msgs);
            
            // Update snapshot so IPO order is visible
            engines_info_[engine_id].engine_->update_snapshot();
            
            // Ensure user_orders_ vector has space for IPO_HOLDER (index 0)
            if (user_orders_.empty()) {
                user_orders_.resize(1);
                user_orders_[IPO_HOLDER].resize(engine_id + 1);
            }
            
            // Track IPO order ownership
            user_orders_[IPO_HOLDER][engine_id].insert(ipo_order);
            // Reverse map entry for IPO
            order_to_user_[ipo_order] = IPO_HOLDER;
            
            // Process IPO messages
            for (const auto& msg : msgs) 
            {
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
            if (_ticker.empty()) 
            {
                notify("[UNREGISTER] ERROR: Empty ticker provided");
                return false;
            }
            
            // Find the ticker-to-engine mapping
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) 
            {
                notify("[UNREGISTER] ERROR: Stock " + _ticker + " does not exist");
                return false;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) 
            {
                notify("[UNREGISTER] ERROR: Engine not found for " + _ticker);
                return false;
            }

            auto& engine_info = engines_info_[engine_id];

            // Wait for worker to finish batch
            scheduler_.process_jobs_on(engine_info.worker_id_);
            
            // Remove from ticker map (engine stays in vector to preserve indices)
            ticker_to_engine_id_.erase(_ticker);
            
            // Clear snapshot cache entry
            if (engine_id < snapshot_cache_.size()) {
                snapshot_cache_[engine_id] = nullptr;
            }
            
            // Clear all user orders for this engine_id and remove reverse mappings
            for (size_t user_id = 0; user_id < user_orders_.size(); ++user_id) 
            {
                if (engine_id < user_orders_[user_id].size()) {
                    // Erase reverse-map entries for each order owned by this user on the engine
                    for (const auto &oid : user_orders_[user_id][engine_id]) {
                        order_to_user_.erase(oid);
                    }
                    user_orders_[user_id][engine_id].clear();
                }
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
            engines_info_.clear(); // Clear Engines
            ticker_to_engine_id_.clear(); // Clear ticker lookup map
            user_orders_.clear(); // Clear User Orders
            order_to_user_.clear(); // Clear reverse map
            snapshot_cache_.clear(); // Clear snapshot cache
            
            notify("[RESET] Reset complete - all stocks and orders cleared");
        }
        catch(const std::exception& e)
        {
            notify("[RESET] ERROR: " + std::string(e.what()));
        }
    }

    bool runtime::EngineRuntime::submit_limit_order(const std::string& _ticker, engine::OrderSide _side, double _price, double _qty, UserId user_id)
    {
        try {
            // Verify ticker exists and get engine_id
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) 
            {
                throw std::invalid_argument("Ticker not found: " + _ticker);
                return false;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) 
            {
                throw std::runtime_error("Engine not found for ticker: " + _ticker);
                return false;
            }
            
            // Convert user-facing values to internal format
            engine::Price price_ticks = math::dollars_to_ticks(_price);
            engine::Quantity qty_ticks = math::qty_to_internal(_qty);
            if (price_ticks <= 0 || qty_ticks <= 0) 
            {
                throw std::runtime_error("Invalid price/qty: " + std::to_string(price_ticks) + "/" + std::to_string(qty_ticks));
                return false;
            }

            auto& engine_info = engines_info_[engine_id];
            
            // Use engine_id for direct lookup and inline execution
            auto job = scheduler::make_job([engine_id, _side, price_ticks, qty_ticks, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            if (engine_id >= runtime_ptr->engines_info_.size()) 
            {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[LIMIT ORDER] ERROR: Engine not found");
                return;
            }
            
            // VALIDATE OWNERSHIP BEFORE SUBMITTING (only for registered users)
            if (_side == engine::OrderSide::ASK && user_id != INVALID_USER_ID) 
            {
                if (user_id < runtime_ptr->user_orders_.size() && 
                    engine_id < runtime_ptr->user_orders_[user_id].size()) 
                {
                    engine::Quantity total_owned = 0;
                    for (engine::OrderId order_id : runtime_ptr->user_orders_[user_id][engine_id]) 
                    {
                        auto order = runtime_ptr->engines_info_[engine_id].engine_->get_order(order_id);
                        if (order != nullptr && order->side_ == engine::OrderSide::ASK && 
                            order->status_ == engine::OrderStatus::OPEN) 
                        {
                            total_owned += order->qty_;
                        }
                    }
                    if (total_owned < qty_ticks) 
                    {
                        if (runtime_ptr->verbose_) runtime_ptr->notify("[LIMIT ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                        return;
                    }
                }
            }
            
            // Always collect engine messages so runtime can process fills
            engine::OrderId order_id;
            {
                std::vector<engine::EngineMsg> msgs;
                order_id = runtime_ptr->engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs);

                for (const auto& msg : msgs) {
                    // Notify if verbose
                    if (runtime_ptr->verbose_) {
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

                    // ACCEPT EVENT: reserve capital/shares but do not update positions
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        if (user_id != INVALID_USER_ID && order_id != engine::INVALID_ID) {
                            try {
                                const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                                if (!ticker.empty() && user_id != IPO_HOLDER) {
                                    double qty = runtime::math::internal_to_qty(qty_ticks);
                                    double price = runtime::math::ticks_to_dollars(price_ticks);
                                    runtime_ptr->users_[user_id - 1].reserve_on_accept(order_id, _side, qty, price);
                                }
                            } catch (...) { }
                        }
                    }

                    
                    
                    // FILL EVENT then Update Position + Remove from Set
                    if ((msg.kind == engine::EventKind::FILL || msg.kind == engine::EventKind::PARTIAL_FILL) &&
                        msg.qty > 0 && msg.price != static_cast<engine::Price>(-1)) {
                        try 
                        {
                            const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                            if (!ticker.empty()) {
                                double qty = runtime::math::internal_to_qty(msg.qty);
                                double price = runtime::math::ticks_to_dollars(msg.price);
                                // Fast O(1) owner lookup via reverse map instead of scanning all users
                                auto oit = runtime_ptr->order_to_user_.find(msg.order_id);
                                if (oit != runtime_ptr->order_to_user_.end()) {
                                    runtime::UserId uid = oit->second;
                                    if (uid > 0 && uid < runtime_ptr->user_orders_.size() && engine_id < runtime_ptr->user_orders_[uid].size()) {
                                        auto &s = runtime_ptr->user_orders_[uid][engine_id];
                                        if (s.find(msg.order_id) != s.end()) {
                                            // Reconcile reservation and update position on fill
                                            runtime_ptr->users_[uid - 1].apply_fill(ticker, msg.order_id, msg.side, qty, price);
                                            // ONLY erase if fully filled
                                            if (msg.kind == engine::EventKind::FILL) {
                                                s.erase(msg.order_id);
                                                runtime_ptr->order_to_user_.erase(msg.order_id);
                                            }
                                        }
                                    }
                                }
                            }
                        } catch (...) 
                        { }
                    }
                }
            }
            
            
            // If Order is open and User is valid track order from user
            if (order_id != engine::INVALID_ID && user_id != INVALID_USER_ID) 
            {
                runtime_ptr->user_orders_[user_id][engine_id].insert(order_id);
                runtime_ptr->order_to_user_[order_id] = user_id;
            }
        
            // Increment order counter for quantum tracking
            if (order_id != engine::INVALID_ID) {
                runtime_ptr->increment_order_counter(engine_id);
            }

        }, engine_id); // Use engine_id as owner_id
        
            // Use runtime wrapper to enforce batch-size then submit
            submit_job_on_worker(engine_info.worker_id_, std::move(job));
            return true;
        } catch (const std::exception& e) {
            notify("[LIMIT ORDER] EXCEPTION: " + std::string(e.what()));
            return false;
        } catch (...) {
            notify("[LIMIT ORDER] EXCEPTION: Unknown error");
            return false;
        }
    }

    bool runtime::EngineRuntime::submit_market_order(const std::string& _ticker, engine::OrderSide _side, double _qty, UserId user_id)
    {
        try {
            // Verify ticker exists and get engine_id
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) 
            {
                throw std::invalid_argument("Ticker not found: " + _ticker);
                return false;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) 
            {
                throw std::runtime_error("Engine not found for ticker: " + _ticker);
                return false;
            }
            
            // Convert user-facing quantity to internal format
            engine::Quantity qty_ticks = math::qty_to_internal(_qty);
            if (qty_ticks <= 0) 
            {
                throw std::runtime_error("Invalid qty:" + std::to_string(qty_ticks));
                return false;
            }
            
            auto& engine_info = engines_info_[engine_id];
        
        auto job = scheduler::make_job([engine_id, _side, qty_ticks, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            if (engine_id >= runtime_ptr->engines_info_.size()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: Engine not found");
                return;
            }
            
            // VALIDATE OWNERSHIP BEFORE SUBMITTING (only for registered users)
            if (_side == engine::OrderSide::ASK && user_id != INVALID_USER_ID) {
                if (user_id < runtime_ptr->user_orders_.size() &&
                    engine_id < runtime_ptr->user_orders_[user_id].size()) {
                    engine::Quantity total_owned = 0;
                    for (engine::OrderId order_id : runtime_ptr->user_orders_[user_id][engine_id]) {
                        auto order = runtime_ptr->engines_info_[engine_id].engine_->get_order(order_id);
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
            
            const engine::MarketSnapshot& snap = runtime_ptr->engines_info_[engine_id].engine_->get_snapshot();
            engine::Price market_price = (_side == engine::OrderSide::BID) ? snap.best_ask : snap.best_bid;
            
            if (market_price == static_cast<engine::Price>(-1)) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: No market price available");
                return;
            }
            
            engine::OrderId order_id;
            {
                std::vector<engine::EngineMsg> msgs;
                order_id = runtime_ptr->engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks, msgs);
                for (const auto& msg : msgs) {
                    if (runtime_ptr->verbose_) {
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

                    // ACCEPT EVENT: reserve capital/shares but do not update positions
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        if (user_id != INVALID_USER_ID && order_id != engine::INVALID_ID) {
                            try {
                                const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                                if (!ticker.empty() && user_id != IPO_HOLDER) {
                                    double qty = runtime::math::internal_to_qty(qty_ticks);
                                    double price = runtime::math::ticks_to_dollars(market_price);
                                    runtime_ptr->users_[user_id - 1].reserve_on_accept(order_id, _side, qty, price);
                                }
                            } catch (...) { }
                        }
                    }

                    
                    
                    // FILL EVENT then Update Position + Remove from Set
                    if ((msg.kind == engine::EventKind::FILL || msg.kind == engine::EventKind::PARTIAL_FILL) &&
                        msg.qty > 0 && msg.price != static_cast<engine::Price>(-1)) {
                        try 
                        {
                            const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                            if (!ticker.empty()) {
                                double qty = runtime::math::internal_to_qty(msg.qty);
                                double price = runtime::math::ticks_to_dollars(msg.price);
                                // Fast O(1) owner lookup via reverse map instead of scanning all users
                                auto oit = runtime_ptr->order_to_user_.find(msg.order_id);
                                if (oit != runtime_ptr->order_to_user_.end()) {
                                    runtime::UserId uid = oit->second;
                                    if (uid > 0 && uid < runtime_ptr->user_orders_.size() && engine_id < runtime_ptr->user_orders_[uid].size()) {
                                        auto &s = runtime_ptr->user_orders_[uid][engine_id];
                                        if (s.find(msg.order_id) != s.end()) {
                                            // Reconcile reservation and update position on fill
                                            runtime_ptr->users_[uid - 1].apply_fill(ticker, msg.order_id, msg.side, qty, price);
                                            // ONLY erase if fully filled
                                            if (msg.kind == engine::EventKind::FILL) {
                                                s.erase(msg.order_id);
                                                runtime_ptr->order_to_user_.erase(msg.order_id);
                                            }
                                        }
                                    }
                                }
                            }
                        } catch (...) 
                        { }
                    }
                }
            }
            
            // If Order is open and User is valid track order from user
            if (order_id != engine::INVALID_ID && user_id != INVALID_USER_ID) {
                runtime_ptr->user_orders_[user_id][engine_id].insert(order_id);
            }
            
            // Increment order counter for quantum tracking
            if (order_id != engine::INVALID_ID) {
                runtime_ptr->increment_order_counter(engine_id);
            }
        }, engine_id);

            submit_job_on_worker(engine_info.worker_id_, std::move(job));
            return true;
        } catch (const std::exception& e) {
            notify("[MARKET ORDER] EXCEPTION: " + std::string(e.what()));
            return false;
        } catch (...) {
            notify("[MARKET ORDER] EXCEPTION: Unknown error");
            return false;
        }
    }

    bool runtime::EngineRuntime::submit_cancel_order(const std::string& _ticker, engine::OrderId order_id, UserId user_id)
    {
        try {
            // Verify ticker exists and get engine_id
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + _ticker);
                return false;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) {
                throw std::runtime_error("Engine not found for ticker: " + _ticker);
                return false;
            }
            
            auto& engine_info = engines_info_[engine_id];
            
            auto job = scheduler::make_job([engine_id, order_id, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            if (engine_id >= runtime_ptr->engines_info_.size()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] ERROR: Engine not found");
                return;
            }

            const engine::OrderInfo* order = runtime_ptr->engines_info_[engine_id].engine_->get_order(order_id);
            if (order == nullptr) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
                return;
            }
            
            engine::EngineMsg msg;
            bool success = runtime_ptr->engines_info_[engine_id].engine_->cancel_order(order_id, msg);
            if (runtime_ptr->verbose_) {
                switch (msg.kind) 
                {
                    case engine::EventKind::ACCEPT:
                        if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] Order " + std::to_string(order_id) + " cancelled");
                        break;
                    case engine::EventKind::REJECT:
                        if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] Order " + std::to_string(order_id) + " cancel rejected");
                        break;
                    default:
                        break;
                }
            }

            // If cancel accepted: release any reservation for this order
            if (msg.kind == engine::EventKind::ACCEPT) {
                if (user_id != INVALID_USER_ID && msg.order_id != engine::INVALID_ID) {
                    try {
                        const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                        if (!ticker.empty() && user_id != IPO_HOLDER) {
                            runtime_ptr->users_[user_id - 1].release_reservation(msg.order_id);
                        }
                    } catch (...) { }
                }
            }

            
            

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
            
            // Untrack order if cancelled successfully (and reservation already released above)
                if (actually_cancelled && user_id != INVALID_USER_ID) {
                if (user_id < runtime_ptr->user_orders_.size() && engine_id < runtime_ptr->user_orders_[user_id].size()) {
                    runtime_ptr->user_orders_[user_id][engine_id].erase(order_id);
                    runtime_ptr->order_to_user_.erase(order_id);
                }
            }
            
            // Increment order counter for quantum tracking
            runtime_ptr->increment_order_counter(engine_id);
        }, engine_id);

            submit_job_on_worker(engine_info.worker_id_, std::move(job));
            return true;
        } catch (const std::exception& e) {
            notify("[CANCEL ORDER] EXCEPTION: " + std::string(e.what()));
            return false;
        } catch (...) {
            notify("[CANCEL ORDER] EXCEPTION: Unknown error");
            return false;
        }
    }

    bool runtime::EngineRuntime::submit_edit_order(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, UserId user_id)
    {
        try {
            // Verify ticker exists and get engine_id
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + _ticker);
                return false;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) {
                throw std::runtime_error("Engine not found for ticker: " + _ticker);
                return false;
            }
            
            // Convert user-facing values to internal format
            engine::Price price_ticks = math::dollars_to_ticks(new_price);
            engine::Quantity qty_ticks = math::qty_to_internal(new_qty);
            if (price_ticks <= 0 || qty_ticks <= 0) 
            {
                throw std::runtime_error("Invalid price/qty: " + std::to_string(price_ticks) + "/" + std::to_string(qty_ticks));
                return false;
            }
            
            auto& engine_info = engines_info_[engine_id];
            
            auto job = scheduler::make_job([engine_id, order_id, price_ticks, qty_ticks, user_id, runtime_ptr = this]() {
            // Direct O(1) lookup by engine_id
            if (engine_id >= runtime_ptr->engines_info_.size()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Engine not found");
                return;
            }
        
            const engine::OrderInfo* order = runtime_ptr->engines_info_[engine_id].engine_->get_order(order_id);
            if (order == nullptr) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
                return;
            }
            
            // VALIDATE OWNERSHIP BEFORE EDITING (only for registered users)
            if (order->side_ == engine::OrderSide::ASK && user_id != INVALID_USER_ID) {
                if (user_id < runtime_ptr->user_orders_.size() &&
                    engine_id < runtime_ptr->user_orders_[user_id].size()) {
                    engine::Quantity total_owned = 0;
                    for (engine::OrderId owned_order_id : runtime_ptr->user_orders_[user_id][engine_id]) {
                        auto owned_order = runtime_ptr->engines_info_[engine_id].engine_->get_order(owned_order_id);
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
            
            std::vector<engine::EngineMsg> msgs;
            engine::OrderId result = runtime_ptr->engines_info_[engine_id].engine_->edit_order(order_id, order->side_, price_ticks, qty_ticks, msgs);

            for (const auto& msg : msgs) {
                if (runtime_ptr->verbose_) {
                    switch (msg.kind) {
                        case engine::EventKind::ACCEPT:
                            runtime_ptr->notify("[EDIT ORDER] Order " + std::to_string(order_id) + " edited");
                            break;
                        case engine::EventKind::FILL:
                            runtime_ptr->notify("[EDIT ORDER] Order " + std::to_string(order_id) + " filled");
                            break;
                        case engine::EventKind::REJECT:
                            runtime_ptr->notify("[EDIT ORDER] Order " + std::to_string(order_id) + " edit rejected");
                            break;
                        default:
                            break;
                    }
                }

                // If order accepted: reserve funds/shares but do not update positions
                if (msg.kind == engine::EventKind::ACCEPT) {
                    if (user_id != INVALID_USER_ID && result != engine::INVALID_ID) {
                        try {
                            const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                            if (!ticker.empty() && user_id != IPO_HOLDER) {
                                double qty = runtime::math::internal_to_qty(qty_ticks);
                                double price = runtime::math::ticks_to_dollars(price_ticks);
                                runtime_ptr->users_[user_id - 1].reserve_on_accept(result, order->side_, qty, price);
                            }
                        } catch (...) { }
                    }
                }

                if ((msg.kind == engine::EventKind::FILL || msg.kind == engine::EventKind::PARTIAL_FILL) &&
                    msg.qty > 0 && msg.price != static_cast<engine::Price>(-1)) {
                    try {
                        const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                        if (!ticker.empty()) {
                            double qty = runtime::math::internal_to_qty(msg.qty);
                            double price = runtime::math::ticks_to_dollars(msg.price);
                            // Fast O(1) owner lookup via reverse map instead of scanning all users
                            auto oit = runtime_ptr->order_to_user_.find(msg.order_id);
                            if (oit != runtime_ptr->order_to_user_.end()) {
                                runtime::UserId uid = oit->second;
                                if (uid > 0 && uid < runtime_ptr->user_orders_.size() && engine_id < runtime_ptr->user_orders_[uid].size()) {
                                    auto &s = runtime_ptr->user_orders_[uid][engine_id];
                                    if (s.find(msg.order_id) != s.end()) {
                                        runtime_ptr->users_[uid - 1].apply_fill(ticker, msg.order_id, msg.side, qty, price);
                                        s.erase(msg.order_id);
                                        runtime_ptr->order_to_user_.erase(oit);
                                    }
                                }
                            }
                        }
                    } catch (...) { }
                }
            }
            
            if (result == engine::INVALID_ID) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Failed to edit order " + std::to_string(order_id));
            }
            
            // Increment order counter for quantum tracking
            runtime_ptr->increment_order_counter(engine_id);
        }, engine_id);

            submit_job_on_worker(engine_info.worker_id_, std::move(job));
            return true;
        } catch (const std::exception& e) {
            notify("[EDIT ORDER] EXCEPTION: " + std::string(e.what()));
            return false;
        } catch (...) {
            notify("[EDIT ORDER] EXCEPTION: Unknown error");
            return false;
        }
    }

    // === SYNCHRONOUS READ OPERATIONS ===

    double EngineRuntime::get_market_price(const std::string& _ticker) const
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return -1.0;
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (snap && snap->market_price != static_cast<engine::Price>(-1)) {
                return math::ticks_to_dollars(snap->market_price);
            }
            return -1.0;
        } catch (...) {
            return -1.0;
        }
    }

    double EngineRuntime::get_best_bid(const std::string& _ticker) const
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return -1.0;
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (snap && snap->best_bid != static_cast<engine::Price>(-1)) {
                return math::ticks_to_dollars(snap->best_bid);
            }
            return -1.0;
        } catch (const std::exception& e) {
            std::cout << "[ERROR TO FIX]" << e.what() << std::endl;
            return -1.0;
        }
    }

    double EngineRuntime::get_best_ask(const std::string& _ticker) const
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return -1.0;
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (snap && snap->best_ask != static_cast<engine::Price>(-1)) {
                return math::ticks_to_dollars(snap->best_ask);
            }
            return -1.0;
        } catch (...) {
            return -1.0;
        }
    }

    const engine::OrderInfo* EngineRuntime::get_order(const std::string& _ticker, engine::OrderId order_id) const
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return nullptr;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) {
                return nullptr;
            }
            
            return engines_info_[engine_id].engine_->get_order(order_id);
        } catch (...) {
            return nullptr;
        }
    }

    // Synchronous methods that don't need job scheduling
    std::vector<std::pair<double, double>> EngineRuntime::get_market_depth(const std::string& _ticker, engine::OrderSide _side, std::size_t depth) const
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + _ticker);
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (!snap) {
                throw std::runtime_error("Snapshot not available for ticker: " + _ticker);
            }
            
            std::vector<std::pair<double, double>> user_depth;
            
            if (_side == engine::OrderSide::BID) {
                size_t count = std::min(static_cast<size_t>(snap->bid_levels), depth);
                user_depth.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    user_depth.emplace_back(
                        math::ticks_to_dollars(snap->bid_prices[i]),
                        math::internal_to_qty(snap->bid_depth[i])
                    );
                }
            } else {
                size_t count = std::min(static_cast<size_t>(snap->ask_levels), depth);
                user_depth.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    user_depth.emplace_back(
                        math::ticks_to_dollars(snap->ask_prices[i]),
                        math::internal_to_qty(snap->ask_depth[i])
                    );
                }
            }
            return user_depth;
        } catch (...) {
            return {};
        }
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
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return nullptr;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) {
                return nullptr;
            }
            
            return engines_info_[engine_id].engine_.get();
        } catch (...) {
            return nullptr;
        }
    }

    bool runtime::EngineRuntime::set_auto_match(const std::string& _ticker, bool auto_match)
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return false;
            }

            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) {
                return false;
            }

            auto& engine_info = engines_info_[engine_id];

            // Schedule the auto-match toggle on the engine's worker thread
            auto job = scheduler::make_job([engine_id, auto_match, runtime_ptr = this]() {
                if (engine_id >= runtime_ptr->engines_info_.size()) return;
                runtime_ptr->engines_info_[engine_id].engine_->set_auto_match(auto_match);
            }, engine_id);

            submit_job_on_worker(engine_info.worker_id_, std::move(job));
            return true;
        } catch (...) {
            return false;
        }
    }

    bool runtime::EngineRuntime::get_auto_match(const std::string& _ticker) const
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return false;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) {
                return false;
            }
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }

            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (!snap) return false;
            return snap->auto_match;
        } catch (...) {
            return false;
        }
    }

    // === PROCESSING METHODS ===

    void EngineRuntime::process_pending_orders()
    {
        try {
            // Wait for all worker jobs to complete
            scheduler_.process_jobs();
        } catch (...) {
            // Silent failure for background processing
        }
    }

    void runtime::EngineRuntime::process_pending_orders(const std::string& _ticker)
    {
        try {
            auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                return;
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= engines_info_.size()) {
                return;
            }
            
            // Wait for the engine's worker to finish
            scheduler_.process_jobs_on(engines_info_[engine_id].worker_id_);
        } catch (...) {
            // Silent failure for background processing
        }
    }

    void runtime::EngineRuntime::set_batch_size(std::size_t n) noexcept
    {
        // Enforce a minimum of 1
        runtime_batch_size_ = (n == 0) ? 1 : n;
    }

    std::size_t runtime::EngineRuntime::get_batch_size() const noexcept
    {
        return runtime_batch_size_;
    }

    void runtime::EngineRuntime::submit_job_on_worker(scheduler::WorkerId worker_id, scheduler::Job&& job) noexcept
    {
        // Defensive: ensure worker id valid
        if (worker_id >= scheduler_.get_worker_count()) return;

        // If pending jobs for this worker reach or exceed runtime batch size,
        // flush/execute them before adding another job to keep batches bounded.
        if (runtime_batch_size_ > 0) {
            const std::size_t pending = scheduler_.pending_jobs_on(worker_id);
            if (pending + 1 >= runtime_batch_size_) {
                scheduler_.process_jobs_on(worker_id);
            }
        }

        scheduler_.submit_job_on(worker_id, std::forward<scheduler::Job>(job));
    }

    std::vector<engine::OrderId> EngineRuntime::get_positions(UserId user_id, const std::string& ticker) const
    {
        try {
            // Convert ticker to engine_id for lookup
            auto ticker_it = ticker_to_engine_id_.find(ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + ticker);
            }
            
            EngineId engine_id = ticker_it->second;
            std::vector<engine::OrderId> positions;
            if (user_id < user_orders_.size() && engine_id < user_orders_[user_id].size()) {
                positions.reserve(user_orders_[user_id][engine_id].size());
                for (engine::OrderId order_id : user_orders_[user_id][engine_id]) {
                    positions.push_back(order_id);
                }
            }
            return positions;
        } catch (...) {
            return {};
        }
    }

    bool runtime::EngineRuntime::has_sufficient_shares(UserId user_id, const std::string& ticker, engine::Quantity qty) const
    {
        try {
            if (user_id >= user_orders_.size()) {
                throw std::invalid_argument("User not found: " + std::to_string(user_id));
            }
            
            auto ticker_it = ticker_to_engine_id_.find(ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + ticker);
            }
            
            EngineId engine_id = ticker_it->second;
            if (engine_id >= user_orders_[user_id].size() || user_orders_[user_id][engine_id].empty()) {
                return false; // User has no positions in this ticker
            }
            
            if (engine_id >= engines_info_.size()) {
                return false;
            }
            
            engine::Quantity total_owned = 0;
            const engine::OrderEngine& engine = *engines_info_[engine_id].engine_;
            for (engine::OrderId order_id : user_orders_[user_id][engine_id]) {
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

    // Order count verification methods implementation (via snapshot)
    std::size_t runtime::EngineRuntime::get_placed_count(const std::string& ticker) const {
        try {
            auto ticker_it = ticker_to_engine_id_.find(ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + ticker);
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (!snap) {
                throw std::runtime_error("Snapshot not available for ticker: " + ticker);
            }
            return snap->placed_count;
        } catch (...) {
            return 0;
        }
    }

    std::size_t runtime::EngineRuntime::get_cancelled_count(const std::string& ticker) const {
        try {
            auto ticker_it = ticker_to_engine_id_.find(ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + ticker);
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (!snap) {
                throw std::runtime_error("Snapshot not available for ticker: " + ticker);
            }
            return snap->cancelled_count;
        } catch (...) {
            return 0;
        }
    }

    std::size_t runtime::EngineRuntime::get_filled_count(const std::string& ticker) const {
        try {
            auto ticker_it = ticker_to_engine_id_.find(ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + ticker);
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (!snap) {
                throw std::runtime_error("Snapshot not available for ticker: " + ticker);
            }
            return snap->filled_count;
        } catch (...) {
            return 0;
        }
    }

    std::size_t runtime::EngineRuntime::get_open_count(const std::string& ticker) const {
        try {
            auto ticker_it = ticker_to_engine_id_.find(ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
                throw std::invalid_argument("Ticker not found: " + ticker);
            }
            
            EngineId engine_id = ticker_it->second;
            // In batch mode (quantum == 0) perform a synchronous snapshot
            // update so callers observing the API get the latest state.
            if (quantum_orders_ == 0) {
                const_cast<EngineRuntime*>(this)->update_snapshot_internal(engine_id);
            }
            
            const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
            if (!snap) {
                throw std::runtime_error("Snapshot not available for ticker: " + ticker);
            }
            return snap->open_count;
        } catch (...) {
            return 0;
        }
    }

    // === STRATEGY & EVENT LOOP IMPLEMENTATION ===
    runtime::User* runtime::EngineRuntime::register_strategy(User::Strategy strategy, double starting_capital)
    {
        if (!strategy)
        {
            throw std::invalid_argument("Cannot register null strategy");
        }
        
        // Calculate user_id before emplacing (start from 1, 0 is reserved for IPO_HOLDER)
        runtime::UserId user_id = users_.size() + 1;
        
        // Create User with strategy, runtime pointer, user_id, and starting capital
        users_.emplace_back(std::move(strategy), this, user_id, starting_capital);
        
        if (verbose_) 
        {
            notify("[RUNTIME] Registered strategy for user " + std::to_string(user_id) + 
                    " with capital $" + std::to_string(starting_capital));
        }
        // Ensure user_orders_ has space for this user and per-engine entries
        if (user_id >= user_orders_.size()) {
            user_orders_.resize(user_id + 1);
        }

        // Make sure inner vector has an entry for each engine so
        // later accesses like user_orders_[user_id][engine_id]
        // are safe even if no orders exist yet.
        if (user_orders_[user_id].size() < engines_info_.size()) {
            user_orders_[user_id].resize(engines_info_.size());
        }

        // pre-reserve small bucket count for the user's sets
        for (auto &s : user_orders_[user_id]) {
            s.reserve(64);
        }

        return &users_.back();
    }
    
    
    void runtime::EngineRuntime::increment_order_counter(EngineId engine_id) noexcept
    {
        // If quantum configured as batch mode (0), do not track intermediate snapshots
        if (quantum_orders_ == 0) {
            return;
        }

        if (engine_id >= engines_info_.size()) {
            return;
        }

        auto &info = engines_info_[engine_id];
        std::size_t quantum = quantum_orders_;

        // Increment the per-engine counter (only the engine worker should call this)
        std::size_t count = ++info.orders_since_quantum_;

        // If per-engine quantum reached, reset counter and publish snapshot for this engine
        if (quantum != 0 && count >= quantum)
        {
            info.orders_since_quantum_ = 0;
            update_snapshot_internal(engine_id);
        }

        // Always increment the global quantum counter on every order. When the
        // global counter reaches the configured quantum, notify all strategies
        // once and reset the global counter. This decouples strategy execution
        // frequency (global) from snapshot publication (per-engine).
        const std::size_t prev = global_orders_since_quantum_.fetch_add(1, std::memory_order_relaxed);
        if (prev + 1 >= quantum)
        {
            global_orders_since_quantum_.store(0, std::memory_order_relaxed);
            for (auto& user : users_) {
                user.on_book_update();
            }
        }
    }
}

// User method implementations
inline bool runtime::User::submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double quantity)
{
    return runtime_->submit_limit_order(ticker, side, price, quantity, user_id_);
}

inline bool runtime::User::submit_market_order(const std::string& ticker, engine::OrderSide side, double quantity)
{
    return runtime_->submit_market_order(ticker, side, quantity, user_id_);
}

inline bool runtime::User::submit_cancel_order(const std::string& ticker, engine::OrderId order_id)
{
    return runtime_->submit_cancel_order(ticker, order_id, user_id_);
}

inline bool runtime::User::submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_quantity)
{
    return runtime_->submit_edit_order(ticker, order_id, new_price, new_quantity, user_id_);
}

inline std::vector<engine::OrderId> runtime::User::get_open_positions(const std::string& ticker) const
{
    return runtime_->get_positions(user_id_, ticker);
}

inline double runtime::User::get_best_bid(const std::string& ticker) const
{
    return runtime_->get_best_bid(ticker);
}

inline double runtime::User::get_best_ask(const std::string& ticker) const
{
    return runtime_->get_best_ask(ticker);
}

inline std::vector<std::pair<double, double>> runtime::User::get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth) const
{
    return runtime_->get_market_depth(ticker, side, depth);
}

inline double runtime::User::get_position(const std::string& ticker) const
{
    auto it = positions_.find(ticker);
    return it != positions_.end() ? it->second : 0.0;
}

inline const std::unordered_map<std::string, double>& runtime::User::get_all_positions() const
{
    return positions_;
}

inline double runtime::User::get_unrealized_pnl(const std::string& ticker, double current_price) const
{
    auto it = positions_.find(ticker);
    if (it == positions_.end()) return 0.0;

    auto avg_it = avg_prices_.find(ticker);
    if (avg_it == avg_prices_.end()) return 0.0;

    return it->second * (current_price - avg_it->second);
}

inline void runtime::User::update_position(const std::string& ticker, double qty, double price)
{
    double prev_pos = 0.0;
    auto it = positions_.find(ticker);
    if (it != positions_.end()) prev_pos = it->second;

    // Compute realized pnl when this fill reduces or flips exposure
    // If signs differ, some quantity is closing existing position
    double realized = 0.0;
    if (prev_pos != 0.0 && ((prev_pos > 0 && qty < 0) || (prev_pos < 0 && qty > 0))) {
        double closing_qty = std::min(std::abs(prev_pos), std::abs(qty));
        // Get previous average price (if present)
        double prev_avg = 0.0;
        auto a_it = avg_prices_.find(ticker);
        if (a_it != avg_prices_.end()) prev_avg = a_it->second;

        if (prev_pos > 0 && qty < 0) {
            // Long being sold
            realized = closing_qty * (price - prev_avg);
        } else if (prev_pos < 0 && qty > 0) {
            // Short being covered
            realized = closing_qty * (prev_avg - price);
        }
        // Update realized pnl and capital for closed portion
        realized_pnl_ += realized;
        capital_ += realized; // realize gains/losses into capital
    }

    // Update position and average price
    double new_pos = prev_pos + qty;
    positions_[ticker] = new_pos;

    if (new_pos != 0.0) {
        // Recalculate average price for remaining/net position
        double prev_avg = 0.0;
        auto a_it = avg_prices_.find(ticker);
        if (a_it != avg_prices_.end()) prev_avg = a_it->second;

        // If previous position had same sign as new trade, update weighted avg
        if ((prev_pos >= 0 && qty >= 0) || (prev_pos <= 0 && qty <= 0)) {
            double total_qty = std::abs(prev_pos) + std::abs(qty);
            if (total_qty > 0) {
                avg_prices_[ticker] = (prev_avg * std::abs(prev_pos) + price * std::abs(qty)) / total_qty;
            } else {
                avg_prices_[ticker] = price;
            }
        } else {
            // If position decreased or flipped, preserve previous avg for remaining portion
            if (std::abs(new_pos) > 0) {
                // remaining side keeps previous avg
                avg_prices_[ticker] = prev_avg;
            } else {
                // Position closed
                avg_prices_.erase(ticker);
            }
        }
    } else {
        // Position fully closed
        avg_prices_.erase(ticker);
    }

    // Update capital for the trade amount (buys reduce capital, sells increase)
    // NOTE: capital impact for accepted orders is handled via reservation logic.
    // Here we only update realized pnl and positions; do not deduct/add trade cash
    // because cash was reserved on ACCEPT and reconciled on fills via apply_fill().
    total_volume_ += std::abs(qty);
}



inline void runtime::User::update_realized_pnl(double pnl) {
    realized_pnl_ += pnl;
}
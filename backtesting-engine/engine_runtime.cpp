#pragma once
#include "engine_runtime.h"

// EngineRuntime Singlton Managment Implmentation
backtest::runtime::EngineRuntime& backtest::runtime::EngineRuntime::get_instance(std::size_t num_threads, std::size_t default_capacity, bool _verbose, std::size_t quantum_orders)
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

void backtest::runtime::EngineRuntime::reset_instance()
{
    auto& runtime = get_instance();
    
    try
    {
        // Process Jobs and Clear Memory
        runtime.scheduler_.process_jobs(); // Wait for pending jobs
        runtime.engines_info_.clear(); // Clear Engines
        runtime.ticker_to_engine_id_.clear(); // Clear ticker lookup map
        runtime.user_orders_.clear(); // Clear User Orders
        runtime.order_to_user_.clear(); // Clear reverse map
        runtime.snapshot_cache_.clear(); // Clear snapshot cache
        
        // toggle OFF intilized flag (allowing for get_instance to create new refrence)
        instance_initialized_ = false;
        
        runtime.notify("[RESET] Reset complete - all stocks and orders cleared");
    }
    catch(const std::exception& e)
    {
        runtime.notify("[RESET] ERROR: " + std::string(e.what()));
    }
}

backtest::runtime::EngineRuntime::EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool _verbose, std::size_t quantum_orders)
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

backtest::runtime::EngineRuntime::~EngineRuntime()
{
    if (notification_thread_running_) 
    {
        stop_notification_thread();
    }
}


// Notifcation Managment Implmentation
void backtest::runtime::EngineRuntime::start_notification_thread() noexcept
{
    if (verbose_ && !notification_thread_running_) 
    {
        notification_thread_running_ = true;
        notification_thread_ = std::thread(&EngineRuntime::notification_loop, this);
    }
}

void backtest::runtime::EngineRuntime::stop_notification_thread() noexcept
{
    notification_thread_running_ = false;
    // Wake notification thread so it can exit promptly
    notification_cv_.notify_one();
    if (notification_thread_.joinable()) 
    {
        notification_thread_.join();
    }
}

void backtest::runtime::EngineRuntime::notification_loop() noexcept
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

void backtest::runtime::EngineRuntime::notify(const std::string& message) noexcept
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


// Snapshot Managment Implmentation
void backtest::runtime::EngineRuntime::update_snapshot_internal(EngineId engine_id) noexcept
{
    if (engine_id < engines_info_.size()) {
        engines_info_[engine_id].engine_->update_snapshot();
    }
}

const engine::MarketSnapshot* backtest::runtime::EngineRuntime::get_snapshot_fast(EngineId engine_id) const noexcept
{
    if (engine_id < snapshot_cache_.size() && snapshot_cache_[engine_id]) {
        return &snapshot_cache_[engine_id]->get_snapshot();
    }
    return nullptr;
}

bool backtest::runtime::EngineRuntime::register_stock(const std::string& _ticker, double _ipo_price, double _ipo_qty, std::size_t capacity)
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
            user_orders_[backtest::user::IPO_HOLDER].resize(engine_id + 1);
        }
        
        // Track IPO order ownership
        user_orders_[backtest::user::IPO_HOLDER][engine_id].insert(ipo_order);
        // Reverse map entry for IPO
        order_to_user_[ipo_order] = backtest::user::IPO_HOLDER;
        
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
            " (owned by user " + std::to_string(backtest::user::IPO_HOLDER) + ")");
        
        return true;
    } catch(const std::exception& e) {
        notify("[REGISTER] ERROR for " + _ticker + ": " + e.what());
        return false;
    }
}

bool backtest::runtime::EngineRuntime::unregister_stock(const std::string& _ticker)
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

bool backtest::runtime::EngineRuntime::submit_limit_order(const std::string& _ticker, engine::OrderSide _side, double _price, double _qty, backtest::user::UserId user_id)
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
        if (_side == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) 
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
                    if (user_id != backtest::user::INVALID_USER_ID && order_id != engine::INVALID_ID) {
                        try {
                            const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                            if (!ticker.empty() && user_id != backtest::user::IPO_HOLDER) {
                                double qty = backtest::math::internal_to_qty(qty_ticks);
                                double price = backtest::math::ticks_to_dollars(price_ticks);
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
                            double qty = backtest::math::internal_to_qty(msg.qty);
                            double price = backtest::math::ticks_to_dollars(msg.price);
                            // Fast O(1) owner lookup via reverse map instead of scanning all users
                            auto oit = runtime_ptr->order_to_user_.find(msg.order_id);
                            if (oit != runtime_ptr->order_to_user_.end()) {
                                backtest::user::UserId uid = oit->second;
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
        if (order_id != engine::INVALID_ID && user_id != backtest::user::INVALID_USER_ID) 
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

bool backtest::runtime::EngineRuntime::submit_market_order(const std::string& _ticker, engine::OrderSide _side, double _qty, backtest::user::UserId user_id)
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
        if (_side == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
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
                    if (user_id != backtest::user::INVALID_USER_ID && order_id != engine::INVALID_ID) {
                        try {
                            const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                            if (!ticker.empty() && user_id != backtest::user::IPO_HOLDER) {
                                double qty = backtest::math::internal_to_qty(qty_ticks);
                                double price = backtest::math::ticks_to_dollars(market_price);
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
                            double qty = backtest::math::internal_to_qty(msg.qty);
                            double price = backtest::math::ticks_to_dollars(msg.price);
                            // Fast O(1) owner lookup via reverse map instead of scanning all users
                            auto oit = runtime_ptr->order_to_user_.find(msg.order_id);
                            if (oit != runtime_ptr->order_to_user_.end()) {
                                backtest::user::UserId uid = oit->second;
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
        if (order_id != engine::INVALID_ID && user_id != backtest::user::INVALID_USER_ID) {
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

bool backtest::runtime::EngineRuntime::submit_cancel_order(const std::string& _ticker, engine::OrderId order_id, backtest::user::UserId user_id)
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
            if (user_id != backtest::user::INVALID_USER_ID && msg.order_id != engine::INVALID_ID) {
                try {
                    const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                    if (!ticker.empty() && user_id != backtest::user::IPO_HOLDER) {
                        // Note: release_reservation not in User class header - commented out
                        // runtime_ptr->users_[user_id - 1].release_reservation(msg.order_id);
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
            if (actually_cancelled && user_id != backtest::user::INVALID_USER_ID) {
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

bool backtest::runtime::EngineRuntime::submit_edit_order(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, backtest::user::UserId user_id)
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
        if (order->side_ == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
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
                if (user_id != backtest::user::INVALID_USER_ID && result != engine::INVALID_ID) {
                    try {
                        const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                        if (!ticker.empty() && user_id != backtest::user::IPO_HOLDER) {
                            double qty = backtest::math::internal_to_qty(qty_ticks);
                            double price = backtest::math::ticks_to_dollars(price_ticks);
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
                        double qty = backtest::math::internal_to_qty(msg.qty);
                        double price = backtest::math::ticks_to_dollars(msg.price);
                        // Fast O(1) owner lookup via reverse map instead of scanning all users
                        auto oit = runtime_ptr->order_to_user_.find(msg.order_id);
                        if (oit != runtime_ptr->order_to_user_.end()) {
                            backtest::user::UserId uid = oit->second;
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

double backtest::runtime::EngineRuntime::get_market_price(const std::string& _ticker) const
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

double backtest::runtime::EngineRuntime::get_best_bid(const std::string& _ticker) const
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

double backtest::runtime::EngineRuntime::get_best_ask(const std::string& _ticker) const
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

const engine::OrderInfo* backtest::runtime::EngineRuntime::get_order(const std::string& _ticker, engine::OrderId order_id) const
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
std::vector<std::pair<double, double>> backtest::runtime::EngineRuntime::get_market_depth(const std::string& _ticker, engine::OrderSide _side, std::size_t depth) const
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

std::vector<std::string> backtest::runtime::EngineRuntime::list_tickers() const noexcept
{
    std::vector<std::string> tickers;
    tickers.reserve(ticker_to_engine_id_.size());
    for (const auto& [ticker, _] : ticker_to_engine_id_) {
        tickers.push_back(ticker);
    }
    return tickers;
}

const engine::OrderEngine* backtest::runtime::EngineRuntime::get_engine(const std::string& _ticker) const
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

bool backtest::runtime::EngineRuntime::set_auto_match(const std::string& _ticker, bool auto_match)
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

bool backtest::runtime::EngineRuntime::get_auto_match(const std::string& _ticker) const
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

void backtest::runtime::EngineRuntime::process_pending_orders()
{
    try {
        // Wait for all worker jobs to complete
        scheduler_.process_jobs();
    } catch (...) {
        // Silent failure for background processing
    }
}

void backtest::runtime::EngineRuntime::process_pending_orders(const std::string& _ticker)
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

void backtest::runtime::EngineRuntime::set_batch_size(std::size_t n) noexcept
{
    // Enforce a minimum of 1
    runtime_batch_size_ = (n == 0) ? 1 : n;
}

std::size_t backtest::runtime::EngineRuntime::get_batch_size() const noexcept
{
    return runtime_batch_size_;
}

std::vector<engine::OrderId> backtest::runtime::EngineRuntime::user_get_positions(backtest::user::UserId user_id, const std::string& ticker) const
{
    try {
        if (user_id >= user_orders_.size()) {
            return {};
        }
        
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            return {};
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= user_orders_[user_id].size()) {
            return {};
        }
        
        std::vector<engine::OrderId> positions;
        positions.reserve(user_orders_[user_id][engine_id].size());
        
        for (engine::OrderId order_id : user_orders_[user_id][engine_id]) {
            positions.push_back(order_id);
        }
        
        return positions;
    } catch (...) {
        return {};
    }
}

bool backtest::runtime::EngineRuntime::user_has_sufficient_shares(backtest::user::UserId user_id, const std::string& ticker, engine::Quantity qty) const
{
    try {
        if (user_id >= user_orders_.size()) {
            return false;
        }
        
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            return false;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= user_orders_[user_id].size() || user_orders_[user_id][engine_id].empty()) {
            return false;
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

void backtest::runtime::EngineRuntime::submit_job_on_worker(scheduler::WorkerId worker_id, scheduler::Job&& job) noexcept
{
    // Defensive: ensure worker id valid
    if (worker_id >= scheduler_.get_worker_count()) return;

    // If pending jobs for this worker reach or exceed runtime batch size,
    // trigger async processing before adding another job to keep batches bounded.
    if (runtime_batch_size_ > 0) {
        const std::size_t pending = scheduler_.pending_jobs_on(worker_id);
        if (pending + 1 >= runtime_batch_size_) {
            scheduler_.process_jobs_on_async(worker_id);
        }
    }

    scheduler_.submit_job_on(worker_id, std::forward<scheduler::Job>(job));
}

// Order count verification methods implementation (via snapshot)
std::size_t backtest::runtime::EngineRuntime::get_placed_count(const std::string& ticker) const {
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

std::size_t backtest::runtime::EngineRuntime::get_cancelled_count(const std::string& ticker) const {
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

std::size_t backtest::runtime::EngineRuntime::get_filled_count(const std::string& ticker) const {
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

std::size_t backtest::runtime::EngineRuntime::get_open_count(const std::string& ticker) const {
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
backtest::user::User* backtest::runtime::EngineRuntime::register_strategy(backtest::user::Strategy strategy, double starting_capital)
{
    if (!strategy)
    {
        throw std::invalid_argument("Cannot register null strategy");
    }
    
    // Calculate user_id before emplacing (start from 1, 0 is reserved for IPO_HOLDER)
    backtest::user::UserId user_id = users_.size() + 1;
    
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

    // Pre-allocate reservation vectors to avoid frequent reallocations.
    // Use a modest default derived from engine capacity to balance memory vs growth.
    try {
        auto &u = users_.back();
        std::size_t reserve_size = std::max<std::size_t>(1024, default_capacity_ / 16);
        u.reserved_cash_.assign(reserve_size, 0.0);
        u.reserved_qty_.assign(reserve_size, 0.0);
        u.reserve_side_.assign(reserve_size, engine::OrderSide::BID);
    } catch (...) {
        // Best-effort; if allocation fails, continue without crashing.
    }

    return &users_.back();
}


void backtest::runtime::EngineRuntime::increment_order_counter(EngineId engine_id) noexcept
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

// User method implementations
// Non-inline User member implementations
void backtest::user::User::reserve_on_accept(engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    // Ensure reservation vectors are large enough
    if (order_id >= reserved_cash_.size()) {
        std::size_t new_size = order_id + 1;
        reserved_cash_.resize(new_size, 0.0);
        reserved_qty_.resize(new_size, 0.0);
        reserve_side_.resize(new_size, engine::OrderSide::BID);
    }
    
    // Reserve funds/shares based on order side
    if (side == engine::OrderSide::BID) {
        // Buy order: reserve cash
        reserved_cash_[order_id] = qty * price;
        capital_ -= qty * price;
    } else {
        // Sell order: reserve shares (qty)
        reserved_qty_[order_id] = qty;
    }
    reserve_side_[order_id] = side;
}

void backtest::user::User::apply_fill(const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    // Update position first
    double signed_qty = (side == engine::OrderSide::BID) ? qty : -qty;
    update_position(ticker, signed_qty, price);
    
    // Release reservation for filled portion
    if (order_id < reserved_cash_.size()) {
        if (side == engine::OrderSide::BID) {
            // Buy: cash was reserved, now we've used it
            double filled_cost = qty * price;
            reserved_cash_[order_id] -= filled_cost;
            if (reserved_cash_[order_id] < 0.0) reserved_cash_[order_id] = 0.0;
        } else {
            // Sell: shares were reserved, now we've sold them and get cash
            reserved_qty_[order_id] -= qty;
            if (reserved_qty_[order_id] < 0.0) reserved_qty_[order_id] = 0.0;
            capital_ += qty * price;  // Add sale proceeds
        }
    }
}
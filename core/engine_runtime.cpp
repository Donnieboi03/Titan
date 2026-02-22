#include "engine_runtime.h"
#include "market_data_stream.h"
#include <algorithm>
#include <functional>
#include <chrono>
#include <utility>

// File-scope pointer so both get_instance() and reset_instance() can manage lifetime.
static backtest::runtime::EngineRuntime* s_instance_ptr = nullptr;

// EngineRuntime Singleton Management Implementation
backtest::runtime::EngineRuntime& backtest::runtime::EngineRuntime::get_instance(std::size_t num_threads, std::size_t default_capacity, bool _verbose, std::size_t quantum_orders)
{
    if (!instance_initialized_)
    {
        // TODO: Remove delete here; only reset_instance() should delete. Create only when s_instance_ptr == nullptr to avoid reentrancy UB.
        delete s_instance_ptr;
        s_instance_ptr = new EngineRuntime(num_threads, default_capacity, _verbose, quantum_orders);
        instance_initialized_ = true;
    }

    return *s_instance_ptr;
}

void backtest::runtime::EngineRuntime::reset_instance()
{
    if (!s_instance_ptr) return;

    try
    {
        s_instance_ptr->scheduler_.process_jobs();
        s_instance_ptr->stop_event_management_thread();

        // Clear all runtime state
        s_instance_ptr->engines_info_.clear();
        s_instance_ptr->ticker_to_engine_id_.clear();
        s_instance_ptr->user_orders_.clear();
        s_instance_ptr->order_to_user_.clear();
        s_instance_ptr->snapshot_cache_.clear();
        s_instance_ptr->users_.clear();
        s_instance_ptr->users_per_engine_.clear();
        s_instance_ptr->user_strategy_engine_id_.clear();
        s_instance_ptr->record_enabled_.clear();
        s_instance_ptr->record_path_override_.clear();

        instance_initialized_ = false;

        // Delete the old object so no stale references can be used after this call.
        delete s_instance_ptr;
        s_instance_ptr = nullptr;
    }
    catch(const std::exception& e)
    {
        // Can't notify — runtime may be partially torn down.
        (void)e;
    }
}

bool backtest::runtime::EngineRuntime::register_stock(std::string&& ticker, double _ipo_price, double _ipo_qty, std::size_t capacity)
{
    try {
        // Verify ticker before creating engine
        if (ticker.empty()) 
        {
            notify("[REGISTER] ERROR: Empty ticker provided");
            return false;
        }
        // IF ipo price or qty is less than or equal to 0
        if (_ipo_price <= 0.0 || _ipo_qty <= 0.0) 
        {
            notify("[REGISTER] ERROR: IPO Price/Quantity must be > 0 for " + ticker);
            return false;
        }
        
        // If ticker is already in Exchange then error
        if (ticker_to_engine_id_.find(ticker) != ticker_to_engine_id_.end()) 
        {
            notify("[REGISTER] ERROR: Stock " + ticker + " already exists");
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
        auto it = ticker_to_engine_id_.emplace(std::forward<std::string>(ticker), engine_id).first;
        engines_info_[engine_id].ticker_ = it->first;
        
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

        // Grow per-engine recording state and per-engine strategy list
        while (record_enabled_.size() <= engine_id) {
            record_enabled_.push_back(std::make_unique<std::atomic<bool>>(false));
            record_path_override_.push_back({});
        }
        while (users_per_engine_.size() <= engine_id) {
            users_per_engine_.push_back({});
        }

        // Place initial sell at IPO Price and IPO Quantity (from IPO holder)
        engine::OrderId ipo_order;
        if (users_.empty()) {
            ipo_order = engines_info_[engine_id].engine_->place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, ipo_price_ticks, ipo_qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            ipo_order = engines_info_[engine_id].engine_->place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, ipo_price_ticks, ipo_qty_ticks, msgs);
            for (const auto& msg : msgs) {
                switch (msg.kind) {
                    case engine::EventKind::ACCEPT:
                        notify("[IPO] Order " + std::to_string(ipo_order) + " accepted for " + it->first);
                        break;
                    case engine::EventKind::REJECT:
                        notify("[IPO] Order " + std::to_string(ipo_order) + " rejected for " + it->first);
                        break;
                    default:
                        break;
                }
            }
        }
        
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
        
        notify("[REGISTER] Registered " + it->first + " with IPO: " + 
            std::to_string(_ipo_qty) + " shares @ $" + std::to_string(_ipo_price) + 
            " (owned by user " + std::to_string(backtest::user::IPO_HOLDER) + ")");
        
        return true;
    } catch(const std::exception& e) {
        notify("[REGISTER] ERROR: " + std::string(e.what()));
        return false;
    }
}

bool backtest::runtime::EngineRuntime::unregister_stock(std::string&& ticker)
{
    try
    {
        // Verify ticker before processing
        if (ticker.empty()) 
        {
            notify("[UNREGISTER] ERROR: Empty ticker provided");
            return false;
        }
        
        // Find the ticker-to-engine mapping
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) 
        {
            notify("[UNREGISTER] ERROR: Stock " + ticker + " does not exist");
            return false;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) 
        {
            notify("[UNREGISTER] ERROR: Engine not found for " + ticker);
            return false;
        }

        // Unregister all strategies associated with this ticker (iterate over a copy)
        if (engine_id < users_per_engine_.size())
        {
            std::vector<std::size_t> indices = users_per_engine_[engine_id];
            for (std::size_t idx : indices)
            {
                if (idx < users_.size())
                {
                    backtest::user::UserId uid = users_[idx].get_user_id();
                    if (uid != backtest::user::INVALID_USER_ID)
                        unregister_strategy(uid);
                }
            }
        }

        auto& engine_info = engines_info_[engine_id];

        if (engine_id < record_enabled_.size() && record_enabled_[engine_id]) {
            record_enabled_[engine_id]->store(false, std::memory_order_relaxed);
        }

        // Wait for worker to finish batch
        scheduler_.process_jobs_on(engine_info.worker_id_);
        
        // Remove from ticker map (engine stays in vector to preserve indices)
        std::string unreg_ticker = ticker_it->first;
        ticker_to_engine_id_.erase(ticker_it);
        
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
        
        notify("[UNREGISTER] Unregistered " + unreg_ticker);
        
        return true;
    }
    catch(const std::exception& e)
    {
        notify("[UNREGISTER] ERROR: " + std::string(e.what()));
        return false;
    }
}

engine::OrderId backtest::runtime::EngineRuntime::submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty, backtest::user::UserId user_id)
{
    return submit_limit_order_async_impl(ticker, side, price, qty, user_id);
}

engine::OrderId backtest::runtime::EngineRuntime::submit_limit_order_async_impl(const std::string& _ticker, engine::OrderSide _side, double _price, double _qty, backtest::user::UserId user_id)
{
    try {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) 
        {
            throw std::invalid_argument("Ticker not found: " + _ticker);
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) 
        {
            throw std::runtime_error("Engine not found for ticker: " + _ticker);
        }
        
        // Convert user-facing values to internal format
        engine::Price price_ticks = math::dollars_to_ticks(_price);
        engine::Quantity qty_ticks = math::qty_to_internal(_qty);
        if (price_ticks <= 0 || qty_ticks <= 0) 
        {
            throw std::runtime_error("Invalid price/qty: " + std::to_string(price_ticks) + "/" + std::to_string(qty_ticks));
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
        
        engine::OrderId order_id;
        const bool use_message_path = !runtime_ptr->users_.empty() || runtime_ptr->notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = runtime_ptr->engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = runtime_ptr->users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [rt = runtime_ptr](engine::OrderId id) { return rt->order_to_user_.find(id) != rt->order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = runtime_ptr->users_.empty() ? nullptr : &fill_filter_fn;
            order_id = runtime_ptr->engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                runtime_ptr->notify_order_event("[LIMIT ORDER]", order_id, msg.kind);
                if (user_id != user::INVALID_USER_ID) {
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        runtime_ptr->handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, price_ticks);
                    }
                    runtime_ptr->handle_fill_event(msg, engine_id);
                }
            }
        }
        
        runtime_ptr->track_user_order(order_id, user_id, engine_id);
    
        // Increment order counter for quantum tracking
        if (order_id != engine::INVALID_ORDER_ID && user_id == user::INVALID_USER_ID) {
            runtime_ptr->increment_order_counter(engine_id);
        }

    }, engine_id); // Use engine_id as owner_id
    
        // Use runtime wrapper to enforce batch-size then submit
        submit_job_on_worker(engine_info.worker_id_, std::move(job));
        return engine::INVALID_ORDER_ID;  // Async path: order ID not available at return
    } catch (const std::exception& e) {
        notify("[LIMIT ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        notify("[LIMIT ORDER] EXCEPTION: Unknown error");
        return engine::INVALID_ORDER_ID;
    }
}

engine::OrderId backtest::runtime::EngineRuntime::submit_limit_order_sync_impl(const std::string& _ticker, engine::OrderSide _side, double _price, double _qty, backtest::user::UserId user_id)
{
    try {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            if (verbose_) notify("[LIMIT ORDER] ERROR: Ticker not found: " + _ticker);
            return engine::INVALID_ORDER_ID;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) {
            if (verbose_) notify("[LIMIT ORDER] ERROR: Engine not found");
            return engine::INVALID_ORDER_ID;
        }
        
        // Convert user-facing values to internal format
        engine::Price price_ticks = math::dollars_to_ticks(_price);
        engine::Quantity qty_ticks = math::qty_to_internal(_qty);
        if (price_ticks <= 0 || qty_ticks <= 0) {
            if (verbose_) notify("[LIMIT ORDER] ERROR: Invalid price/qty");
            return engine::INVALID_ORDER_ID;
        }
        
        // VALIDATE OWNERSHIP BEFORE SUBMITTING (only for registered users)
        if (_side == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
            if (user_id < user_orders_.size() && engine_id < user_orders_[user_id].size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId oid : user_orders_[user_id][engine_id]) {
                    auto order = engines_info_[engine_id].engine_->get_order(oid);
                    if (order != nullptr && order->side_ == engine::OrderSide::ASK && 
                        order->status_ == engine::OrderStatus::OPEN) {
                        total_owned += order->qty_;
                    }
                }
                if (total_owned < qty_ticks) {
                    if (verbose_) notify("[LIMIT ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                    return engine::INVALID_ORDER_ID;
                }
            }
        }
        
        engine::OrderId order_id;
        const bool use_message_path = !users_.empty() || notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [this](engine::OrderId id) { return order_to_user_.find(id) != order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = users_.empty() ? nullptr : &fill_filter_fn;
            order_id = engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                notify_order_event("[LIMIT ORDER]", order_id, msg.kind);
                if (user_id != user::INVALID_USER_ID) {
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, price_ticks);
                    }
                    handle_fill_event(msg, engine_id);
                }
            }
        }
        
        track_user_order(order_id, user_id, engine_id);
        if (order_id != engine::INVALID_ORDER_ID && user_id == user::INVALID_USER_ID) {
            increment_order_counter(engine_id);
        }
        
        return order_id;
    } catch (const std::exception& e) {
        notify("[LIMIT ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        notify("[LIMIT ORDER] EXCEPTION: Unknown error");
        return engine::INVALID_ORDER_ID;
    }
}

engine::OrderId backtest::runtime::EngineRuntime::submit_market_order(const std::string& ticker, engine::OrderSide side, double qty, backtest::user::UserId user_id)
{
    return submit_market_order_async_impl(ticker, side, qty, user_id);
}

engine::OrderId backtest::runtime::EngineRuntime::submit_market_order_async_impl(const std::string& _ticker, engine::OrderSide _side, double _qty, backtest::user::UserId user_id)
{
    try {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) 
        {
            throw std::invalid_argument("Ticker not found: " + _ticker);
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) 
        {
            throw std::runtime_error("Engine not found for ticker: " + _ticker);
        }
        
        // Convert user-facing quantity to internal format
        engine::Quantity qty_ticks = math::qty_to_internal(_qty);
        if (qty_ticks <= 0) 
        {
            throw std::runtime_error("Invalid qty:" + std::to_string(qty_ticks));
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
        const bool use_message_path = !runtime_ptr->users_.empty() || runtime_ptr->notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = runtime_ptr->engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = runtime_ptr->users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [rt = runtime_ptr](engine::OrderId id) { return rt->order_to_user_.find(id) != rt->order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = runtime_ptr->users_.empty() ? nullptr : &fill_filter_fn;
            order_id = runtime_ptr->engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                runtime_ptr->notify_order_event("[MARKET ORDER]", order_id, msg.kind);
                if (user_id != user::INVALID_USER_ID) {
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        runtime_ptr->handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, market_price);
                    }
                    runtime_ptr->handle_fill_event(msg, engine_id);
                }
            }
        }
        
        runtime_ptr->track_user_order(order_id, user_id, engine_id);
        if (order_id != engine::INVALID_ORDER_ID && user_id == user::INVALID_USER_ID) {
            runtime_ptr->increment_order_counter(engine_id);
        }
    }, engine_id);

        submit_job_on_worker(engine_info.worker_id_, std::move(job));
        return engine::INVALID_ORDER_ID;  // Async path: order ID not available at return
    } catch (const std::exception& e) {
        notify("[MARKET ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        notify("[MARKET ORDER] EXCEPTION: Unknown error");
        return engine::INVALID_ORDER_ID;
    }
}

engine::OrderId backtest::runtime::EngineRuntime::submit_market_order_sync_impl(const std::string& _ticker, engine::OrderSide _side, double _qty, backtest::user::UserId user_id)
{
    try {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            if (verbose_) notify("[MARKET ORDER] ERROR: Ticker not found: " + _ticker);
            return engine::INVALID_ORDER_ID;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) {
            if (verbose_) notify("[MARKET ORDER] ERROR: Engine not found");
            return engine::INVALID_ORDER_ID;
        }
        
        // Convert user-facing quantity to internal format
        engine::Quantity qty_ticks = math::qty_to_internal(_qty);
        if (qty_ticks <= 0) {
            if (verbose_) notify("[MARKET ORDER] ERROR: Invalid qty");
            return engine::INVALID_ORDER_ID;
        }
        
        // VALIDATE OWNERSHIP BEFORE SUBMITTING (only for registered users)
        if (_side == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
            if (user_id < user_orders_.size() && engine_id < user_orders_[user_id].size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId oid : user_orders_[user_id][engine_id]) {
                    auto order = engines_info_[engine_id].engine_->get_order(oid);
                    if (order != nullptr && order->side_ == engine::OrderSide::ASK && 
                        order->status_ == engine::OrderStatus::OPEN) {
                        total_owned += order->qty_;
                    }
                }
                if (total_owned < qty_ticks) {
                    if (verbose_) notify("[MARKET ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                    return engine::INVALID_ORDER_ID;
                }
            }
        }
        
        // Get market price from snapshot
        const engine::MarketSnapshot& snap = engines_info_[engine_id].engine_->get_snapshot();
        engine::Price market_price = (_side == engine::OrderSide::BID) ? snap.best_ask : snap.best_bid;
        
        if (market_price == static_cast<engine::Price>(-1)) {
            if (verbose_) notify("[MARKET ORDER] ERROR: No market price available");
            return engine::INVALID_ORDER_ID;
        }
        
        engine::OrderId order_id;
        const bool use_message_path = !users_.empty() || notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [this](engine::OrderId id) { return order_to_user_.find(id) != order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = users_.empty() ? nullptr : &fill_filter_fn;
            order_id = engines_info_[engine_id].engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                notify_order_event("[MARKET ORDER]", order_id, msg.kind);
                if (user_id != user::INVALID_USER_ID) {
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, market_price);
                    }
                    handle_fill_event(msg, engine_id);
                }
            }
        }
        
        track_user_order(order_id, user_id, engine_id);
        if (order_id != engine::INVALID_ORDER_ID && user_id == user::INVALID_USER_ID) {
            increment_order_counter(engine_id);
        }
        
        return order_id;
    } catch (const std::exception& e) {
        notify("[MARKET ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        notify("[MARKET ORDER] EXCEPTION: Unknown error");
        return engine::INVALID_ORDER_ID;
    }
}

bool backtest::runtime::EngineRuntime::submit_cancel_order(const std::string& ticker, engine::OrderId order_id, backtest::user::UserId user_id)
{
    return submit_cancel_order_async_impl(ticker, order_id, user_id);
}

bool backtest::runtime::EngineRuntime::submit_cancel_order_async_impl(const std::string& _ticker, engine::OrderId order_id, backtest::user::UserId user_id)
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
            if (user_id != backtest::user::INVALID_USER_ID && msg.order_id != engine::INVALID_ORDER_ID) {
                try {
                    const std::size_t user_idx = static_cast<std::size_t>(user_id) - 1;
                    if (user_id != backtest::user::IPO_HOLDER && user_idx < runtime_ptr->users_.size()) {
                        runtime_ptr->sync_order_api_.release_reservation_for_user(&runtime_ptr->users_[user_idx], order_id);
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

bool backtest::runtime::EngineRuntime::submit_cancel_order_sync_impl(const std::string& _ticker, engine::OrderId order_id, backtest::user::UserId user_id)
{
    try {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            if (verbose_) notify("[CANCEL ORDER] ERROR: Ticker not found: " + _ticker);
            return false;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) {
            if (verbose_) notify("[CANCEL ORDER] ERROR: Engine not found");
            return false;
        }
        
        const engine::OrderInfo* order = engines_info_[engine_id].engine_->get_order(order_id);
        if (order == nullptr) {
            if (verbose_) notify("[CANCEL ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
            return false;
        }
        
        // Direct engine call
        engine::EngineMsg msg;
        bool success = engines_info_[engine_id].engine_->cancel_order(order_id, msg);
        
        if (verbose_) {
            switch (msg.kind) {
                case engine::EventKind::ACCEPT:
                    notify("[CANCEL ORDER] Order " + std::to_string(order_id) + " cancelled");
                    break;
                case engine::EventKind::REJECT:
                    notify("[CANCEL ORDER] Order " + std::to_string(order_id) + " cancel rejected");
                    break;
                default:
                    break;
            }
        }
        
        bool actually_cancelled = false;
        if (success && msg.kind == engine::EventKind::ACCEPT) {
            actually_cancelled = true;
        }

        // Release reservation and untrack order if cancelled successfully
        if (actually_cancelled && user_id != backtest::user::INVALID_USER_ID) {
            const std::size_t user_idx = static_cast<std::size_t>(user_id) - 1;
            if (user_id != backtest::user::IPO_HOLDER && user_idx < users_.size()) {
                sync_order_api_.release_reservation_for_user(&users_[user_idx], order_id);
            }
            if (user_id < user_orders_.size() && engine_id < user_orders_[user_id].size()) {
                user_orders_[user_id][engine_id].erase(order_id);
                order_to_user_.erase(order_id);
            }
        }
        
        // Increment order counter for quantum tracking
        increment_order_counter(engine_id);
        
        return actually_cancelled;
    } catch (const std::exception& e) {
        notify("[CANCEL ORDER] EXCEPTION: " + std::string(e.what()));
        return false;
    } catch (...) {
        notify("[CANCEL ORDER] EXCEPTION: Unknown error");
        return false;
    }
}

bool backtest::runtime::EngineRuntime::submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, backtest::user::UserId user_id)
{
    return submit_edit_order_async_impl(ticker, order_id, new_price, new_qty, user_id);
}

bool backtest::runtime::EngineRuntime::submit_edit_order_async_impl(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, backtest::user::UserId user_id)
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
        bool result = runtime_ptr->engines_info_[engine_id].engine_->edit_order(order_id, order->side_, price_ticks, qty_ticks, msgs);

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
                if (user_id != backtest::user::INVALID_USER_ID && result) {
                    try {
                        const std::string &ticker = runtime_ptr->engines_info_[engine_id].ticker_;
                        if (!ticker.empty() && user_id != backtest::user::IPO_HOLDER) {
                            double qty = backtest::math::internal_to_qty(qty_ticks);
                            double price = backtest::math::ticks_to_dollars(price_ticks);
                            runtime_ptr->sync_order_api_.reserve_on_accept_to_user(&runtime_ptr->users_[user_id - 1], ticker, order_id, order->side_, qty, price);
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
                                    runtime_ptr->sync_order_api_.apply_fill_to_user(&runtime_ptr->users_[uid - 1], ticker, msg.order_id, msg.side, qty, price);
                                    s.erase(msg.order_id);
                                    runtime_ptr->order_to_user_.erase(oit);
                                }
                            }
                        }
                    }
                } catch (...) { }
            }
        }
        
        if (!result) {
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

bool backtest::runtime::EngineRuntime::submit_edit_order_sync_impl(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, backtest::user::UserId user_id)
{
    try {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            if (verbose_) notify("[EDIT ORDER] ERROR: Ticker not found: " + _ticker);
            return false;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) {
            if (verbose_) notify("[EDIT ORDER] ERROR: Engine not found");
            return false;
        }
        
        // Convert user-facing values to internal format
        engine::Price price_ticks = math::dollars_to_ticks(new_price);
        engine::Quantity qty_ticks = math::qty_to_internal(new_qty);
        if (price_ticks <= 0 || qty_ticks <= 0) {
            if (verbose_) notify("[EDIT ORDER] ERROR: Invalid price/qty");
            return false;
        }
        
        const engine::OrderInfo* order = engines_info_[engine_id].engine_->get_order(order_id);
        if (order == nullptr) {
            if (verbose_) notify("[EDIT ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
            return false;
        }
        
        // VALIDATE OWNERSHIP BEFORE EDITING (only for registered users)
        if (order->side_ == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
            if (user_id < user_orders_.size() && engine_id < user_orders_[user_id].size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId owned_order_id : user_orders_[user_id][engine_id]) {
                    auto owned_order = engines_info_[engine_id].engine_->get_order(owned_order_id);
                    if (owned_order != nullptr && owned_order->side_ == engine::OrderSide::ASK && 
                        owned_order->status_ == engine::OrderStatus::OPEN) {
                        total_owned += owned_order->qty_;
                    }
                }
                if (total_owned < qty_ticks) {
                    if (verbose_) notify("[EDIT ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                    return false;
                }
            }
        }
        
        // Direct engine call
        std::vector<engine::EngineMsg> msgs;
        bool result = engines_info_[engine_id].engine_->edit_order(order_id, order->side_, price_ticks, qty_ticks, msgs);
        
        for (const auto& msg : msgs) {
            if (verbose_) {
                switch (msg.kind) {
                    case engine::EventKind::ACCEPT:
                        notify("[EDIT ORDER] Order " + std::to_string(order_id) + " edited");
                        break;
                    case engine::EventKind::FILL:
                        notify("[EDIT ORDER] Order " + std::to_string(order_id) + " filled");
                        break;
                    case engine::EventKind::REJECT:
                        notify("[EDIT ORDER] Order " + std::to_string(order_id) + " edit rejected");
                        break;
                    default:
                        break;
                }
            }
            
            // ACCEPT EVENT: reserve funds/shares
            if (msg.kind == engine::EventKind::ACCEPT) {
                if (user_id != backtest::user::INVALID_USER_ID && result) {
                    try {
                        const std::string &ticker = engines_info_[engine_id].ticker_;
                        if (!ticker.empty() && user_id != backtest::user::IPO_HOLDER) {
                            double qty = backtest::math::internal_to_qty(qty_ticks);
                            double price = backtest::math::ticks_to_dollars(price_ticks);
                            sync_order_api_.reserve_on_accept_to_user(&users_[user_id - 1], ticker, order_id, order->side_, qty, price);
                        }
                    } catch (...) { }
                }
            }
            
            // FILL EVENT: update position and remove from set
            if ((msg.kind == engine::EventKind::FILL || msg.kind == engine::EventKind::PARTIAL_FILL) &&
                msg.qty > 0 && msg.price != static_cast<engine::Price>(-1)) {
                try {
                    const std::string &ticker = engines_info_[engine_id].ticker_;
                    if (!ticker.empty()) {
                        double qty = backtest::math::internal_to_qty(msg.qty);
                        double price = backtest::math::ticks_to_dollars(msg.price);
                        auto oit = order_to_user_.find(msg.order_id);
                        if (oit != order_to_user_.end()) {
                            backtest::user::UserId uid = oit->second;
                            if (uid > 0 && uid < user_orders_.size() && engine_id < user_orders_[uid].size()) {
                                auto &s = user_orders_[uid][engine_id];
                                if (s.find(msg.order_id) != s.end()) {
                                    sync_order_api_.apply_fill_to_user(&users_[uid - 1], ticker, msg.order_id, msg.side, qty, price);
                                    if (msg.kind == engine::EventKind::FILL) {
                                        s.erase(msg.order_id);
                                        order_to_user_.erase(msg.order_id);
                                    }
                                }
                            }
                        }
                    }
                } catch (...) { }
            }
        }
        
        if (!result) {
            if (verbose_) notify("[EDIT ORDER] ERROR: Failed to edit order " + std::to_string(order_id));
            return false;
        }
        
        // Increment order counter for quantum tracking
        increment_order_counter(engine_id);
        
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
            update_snapshot_internal(engine_id);
        }
        
        const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
        if (snap) {
            // Calculate mid-price from best bid/ask (most accurate current market price)
            if (snap->best_bid != static_cast<engine::Price>(-1) && 
                snap->best_ask != static_cast<engine::Price>(-1)) {
                double mid_price = (math::ticks_to_dollars(snap->best_bid) + 
                                   math::ticks_to_dollars(snap->best_ask)) / 2.0;
                return mid_price;
            }
            // If only ask available (IPO case), use ask price
            else if (snap->best_ask != static_cast<engine::Price>(-1)) {
                return math::ticks_to_dollars(snap->best_ask);
            }
            // If only bid available, use bid price
            else if (snap->best_bid != static_cast<engine::Price>(-1)) {
                return math::ticks_to_dollars(snap->best_bid);
            }
            // Fallback to last trade price if no bid/ask available
            else if (snap->market_price != static_cast<engine::Price>(-1)) {
                return math::ticks_to_dollars(snap->market_price);
            }
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
            update_snapshot_internal(engine_id);
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
            update_snapshot_internal(engine_id);
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
            update_snapshot_internal(engine_id);
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
            update_snapshot_internal(engine_id);
        }

        const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
        if (!snap) return false;
        return snap->auto_match;
    } catch (...) {
        return false;
    }
}

// === PROCESSING METHODS ===

void backtest::runtime::EngineRuntime::set_batch_size(std::size_t n) noexcept
{
    // Enforce a minimum of 1
    runtime_batch_size_.store((n <= 0) ? 1 : n, std::memory_order_relaxed);
}

std::size_t backtest::runtime::EngineRuntime::get_batch_size() const noexcept
{
    return runtime_batch_size_.load(std::memory_order_relaxed);
}

std::size_t backtest::runtime::EngineRuntime::get_quantum() const noexcept
{
    return quantum_orders_;
}

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

void backtest::runtime::EngineRuntime::process_pending_orders_async()
{
    try {
        // Trigger async processing without waiting
        scheduler_.process_jobs_async();
    } catch (...) {
        // Silent failure for background processing
    }
}

void backtest::runtime::EngineRuntime::process_pending_orders_async(const std::string& _ticker)
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
        
        // Trigger async processing for specific worker without waiting
        scheduler_.process_jobs_on_async(engines_info_[engine_id].worker_id_);
    } catch (...) {
        // Silent failure for background processing
    }
}

bool backtest::runtime::EngineRuntime::simulate
(
    std::string&& filepath,
    std::string&& ticker,
    std::size_t target_orders,
    std::size_t price_sample_size,
    double shares_outstanding,
    std::string&& record_path
)
{   
    std::unique_ptr<stream::L2Stream> parser; // L2 stream for replay
    double initial_price = 100.0;  // IPO Price
    try 
    {
        parser = std::make_unique<stream::L2Stream>(std::forward<std::string>(filepath));
        // Test parser through IPO setup
        stream::L2Update ipo_update;
        double sample_sum = 0.0;
        std::size_t sample_count = 0;
        while (parser->parse_next(ipo_update) && sample_count < price_sample_size) 
        {
            if (!ipo_update.is_snapshot && ipo_update.price > 0.0) 
            {
                sample_sum += ipo_update.price;
                ++sample_count;
                break;
            }
        }

        if (sample_count > 0) initial_price = sample_sum / sample_count;
    } catch (const std::exception& e) 
    {
        notify("[SIMULATE] ERROR: " + std::string(e.what()));
        return false; // Parser Failed
    }
    
    // Register Stock (pass copy of ticker so we keep it for engine_id lookup below)
    if(!register_stock(std::string(ticker), initial_price, shares_outstanding)) return false;
    if (!record_path.empty()) set_record(std::string(ticker), true, std::forward<std::string>(record_path));
    auto engine_id = ticker_to_engine_id_[ticker]; // Engine Id
    auto& engine_info = engines_info_[engine_id]; // Engine Info
    engine_info.get_write_metrics().reset(); // Info Sim Metrics

    // Set and Publish Intial Price
    engine_info.get_write_metrics().initial_price = initial_price;
    engine_info.get_write_metrics().simulation_running = true;
    engine_info.publish_metrics();
    engine_info.get_write_metrics().simulation_running = true;
    
    // Create simulation job
    auto simulation_job = scheduler::make_job([this, parser = std::move(parser), engine_id, target_orders]() mutable {
        
        // Fail if Engine Id does not exist
        if (engine_id >= engines_info_.size()) return;
        
        auto& engine_info = engines_info_[engine_id]; // Capture engine info
        auto* engine = engine_info.engine_.get(); // Capture engine
        
        // Fail if engine is nullptr
        if (!engine) return;
    
        // Start timing
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Queue-then-periodically-drain: orders are queued (auto_match off), then we drain every batch
        engine->set_auto_match(true);
        // Use this symbol's engine capacity for threshold and batch sizing (from OrderEngineInfo)
        std::size_t engine_capacity = engine_info.capacity_;
        std::size_t batch_size = runtime_batch_size_.load(std::memory_order_relaxed);
        // Default batch size from symbol capacity when global batch size is 0 or larger than engine capacity
        if (batch_size == 0 || batch_size > engine_capacity)
            batch_size = (engine_capacity > 0) ? (engine_capacity / 16) : 50000;
        const std::size_t drain_interval = (batch_size > 0) ? batch_size : 50000;

        // Setup simulation variables - reuse moved parser
        std::unordered_map<uint64_t, double> price_level_cache;
        std::unordered_set<engine::Price> unique_prices;
        std::size_t updates = 0;
        std::size_t peak_open_orders = 0;
        std::size_t utilization_sum = 0;
        std::size_t utilization_samples = 0;
        
        // Parser is already created and moved into lambda - no need to recreate
        price_level_cache.reserve(1 << 16); // Pre-allocate for performance
        unique_prices.reserve(1 << 16); // Pre-allocate unique_prices
        
        // Key encoding function for price cache
        auto make_key = [](double price, char side) -> uint64_t 
        {
            engine::Price price_ticks = backtest::math::dollars_to_ticks(price);
            uint64_t side_bit = (side == 'b' || side == 'B') ? 0 : 1;
            return (static_cast<uint64_t>(price_ticks) << 1) | side_bit;
        };
        
        stream::L2Update update;
        std::vector<engine::EngineMsg> msgs; // reused across iterations to avoid repeated alloc/free
        msgs.reserve(16);
        // Main simulation loop with direct engine calls and metrics tracking
        while (parser->parse_next(update)) 
        {
            if (update.is_snapshot) continue;
            ++updates;
            
            if (update.price > 0.0) 
            {
                engine::OrderSide side = (update.side == 'b' || update.side == 'B')
                                        ? engine::OrderSide::BID : engine::OrderSide::ASK;
                uint64_t key = make_key(update.price, update.side);
                
                // Cache operations
                auto [it, inserted] = price_level_cache.try_emplace(key, 0.0);
                double prev_amount = it->second;
                double delta = update.amount - prev_amount;
                it->second = update.amount;
                
                // Skip zero deltas
                if (delta != 0.0) 
                {
                    // Tick conversion
                    engine::Price price_ticks = math::dollars_to_ticks(update.price);
                    unique_prices.insert(price_ticks);
                    engine::Quantity qty_ticks = math::qty_to_internal(std::abs(delta));

                    engine::OrderId order_id;
                    if (users_.empty()) {
                        if (delta > 0.0) {
                            order_id = engine->place_order(side, engine::OrderType::LIMIT, price_ticks, qty_ticks);
                        } else {
                            engine::OrderSide opposite = (side == engine::OrderSide::BID) ? engine::OrderSide::ASK : engine::OrderSide::BID;
                            order_id = engine->place_order(opposite, engine::OrderType::LIMIT, price_ticks, qty_ticks);
                        }
                    } else {
                        msgs.clear();
                        std::function<bool(engine::OrderId)> fill_filter_fn = [this](engine::OrderId id) { return order_to_user_.find(id) != order_to_user_.end(); };
                        if (delta > 0.0) {
                            order_id = engine->place_order(side, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs, false, &fill_filter_fn);
                        } else {
                            engine::OrderSide opposite = (side == engine::OrderSide::BID) ? engine::OrderSide::ASK : engine::OrderSide::BID;
                            order_id = engine->place_order(opposite, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs, false, &fill_filter_fn);
                        }
                        for (const auto& msg : msgs) {
                            this->notify_order_event("[LIMIT ORDER]", order_id, msg.kind);
                            this->handle_fill_event(msg, engine_id);
                        }
                    }
                    if (order_id != engine::INVALID_ORDER_ID) this->increment_order_counter(engine_id);
                }
            }
            
            // Periodically drain queue: turn auto_match on to drain, then off again
            // if (updates % drain_interval == 0)
            // {
            //     engine->set_auto_match(true);
            //     engine->set_auto_match(false);
            // }

            // Process at batch intervals (metrics)
            if (batch_size > 0 && updates % batch_size == 0) 
            {
                // Capture snapshot for compatible metrics design
                engine->update_snapshot();
                const engine::MarketSnapshot& snapshot = engine->get_snapshot();

                std::size_t current_open = snapshot.open_count;
                if (engine_capacity > 0) 
                {
                    utilization_sum += current_open;
                    ++utilization_samples;
                }
                
                // Update metrics only during batch processing for efficiency
                auto& metrics = engine_info.get_write_metrics();
                metrics.market_updates_processed = updates;
                metrics.orders_placed = snapshot.placed_count;
                metrics.orders_filled = snapshot.filled_count;
                metrics.final_open_orders = snapshot.open_count;
                metrics.peak_open_orders = snapshot.open_count > metrics.peak_open_orders ? 
                snapshot.open_count : metrics.peak_open_orders;
                metrics.orders_cancelled = snapshot.cancelled_count;
                metrics.cache_entries = price_level_cache.size();
                metrics.unique_price_levels = unique_prices.size();
                if (utilization_samples > 0 && engine_capacity > 0) 
                {
                    double avg_util = (static_cast<double>(utilization_sum) / utilization_samples) / engine_capacity * 100.0;
                    metrics.average_utilization_percent = avg_util;
                }
                
                // Publish metrics for readers
                engine_info.publish_metrics();
            }
            
            // Check target limit (only if target > 0)
            if (target_orders > 0 && updates >= target_orders) break;
        }
        
        // Final drain: match any remaining queued orders before final metrics
        // engine->set_auto_match(true);
        // Update snapshot for final metrics capture
        engine->update_snapshot(); 
        const engine::MarketSnapshot& final_snapshot = engine->get_snapshot();
        
        // Final metrics update with snapshot-based values for compatibility
        auto& final_metrics = engine_info.get_write_metrics();
        final_metrics.orders_placed = final_snapshot.placed_count;
        final_metrics.orders_filled = final_snapshot.filled_count;
        final_metrics.orders_cancelled = final_snapshot.cancelled_count;
        final_metrics.final_open_orders = final_snapshot.open_count;
        final_metrics.peak_open_orders = final_snapshot.open_count > final_metrics.peak_open_orders ?  
        final_snapshot.open_count : final_metrics.peak_open_orders;
        final_metrics.cache_entries = price_level_cache.size();
        final_metrics.unique_price_levels = unique_prices.size();
        
        auto end_time = std::chrono::high_resolution_clock::now();
        double seconds = std::chrono::duration<double>(end_time - start_time).count();
        
        if (utilization_samples > 0 && engine_capacity > 0) 
        {
            double avg_util = (static_cast<double>(utilization_sum) / utilization_samples) / engine_capacity * 100.0;
            final_metrics.average_utilization_percent = avg_util;
        }

        if (seconds > 0.0) {
            final_metrics.orders_per_second = final_metrics.orders_placed / seconds;
            final_metrics.updates_per_second = updates / seconds;
        }

        // Final price estimate from snapshot
        double final_price = -1.0;
        if (final_snapshot.market_price != static_cast<engine::Price>(-1)) {
            final_price = math::ticks_to_dollars(final_snapshot.market_price);
        } else if (final_snapshot.best_bid != static_cast<engine::Price>(-1) &&
                   final_snapshot.best_ask != static_cast<engine::Price>(-1)) {
            final_price = (math::ticks_to_dollars(final_snapshot.best_bid) + math::ticks_to_dollars(final_snapshot.best_ask)) / 2.0;
        } else if (final_snapshot.best_bid != static_cast<engine::Price>(-1)) {
            final_price = math::ticks_to_dollars(final_snapshot.best_bid);
        } else if (final_snapshot.best_ask != static_cast<engine::Price>(-1)) {
            final_price = math::ticks_to_dollars(final_snapshot.best_ask);
        }
        if (final_price >= 0.0) {
            final_metrics.final_price = final_price;
        }

        // Store final timing and completion
        final_metrics.simulation_time_seconds = seconds;
        // Mark simulation as complete BEFORE publishing
        final_metrics.simulation_running = false;
        
        // Publish final metrics with running=false so readers see completion
        engine_info.publish_metrics();
        
    }, engine_id);
    
    // Submit simulation job to the engine's worker - ASYNC, NO WAITING
    submit_job_on_worker(engine_info.worker_id_, std::move(simulation_job));
    
    // Return immediately - simulation runs asynchronously
    // User must call process methods to execute the job
    return true;
}

// Async simulation status methods implementation

bool backtest::runtime::EngineRuntime::is_simulation_running(const std::string& ticker) const
{
    auto ticker_it = ticker_to_engine_id_.find(ticker);
    if (ticker_it == ticker_to_engine_id_.end()) {
        return false;
    }
    
    EngineId engine_id = ticker_it->second;
    if (engine_id >= engines_info_.size()) {
        return false;
    }
    
    const auto& engine_info = engines_info_[engine_id];
    return engine_info.get_read_metrics().simulation_running;
}


backtest::runtime::SimulationMetrics backtest::runtime::EngineRuntime::get_simulation_metrics(const std::string& ticker) const
{
    auto ticker_it = ticker_to_engine_id_.find(ticker);
    if (ticker_it == ticker_to_engine_id_.end()) {
        return backtest::runtime::SimulationMetrics{};  // Return default-constructed metrics
    }
    
    EngineId engine_id = ticker_it->second;
    if (engine_id >= engines_info_.size()) {
        return backtest::runtime::SimulationMetrics{};
    }
    
    const auto& engine_info = engines_info_[engine_id];
    return engine_info.get_read_metrics();
}


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
            update_snapshot_internal(engine_id);
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
            update_snapshot_internal(engine_id);
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
            update_snapshot_internal(engine_id);
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
            update_snapshot_internal(engine_id);
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

std::size_t backtest::runtime::EngineRuntime::get_capacity(const std::string& ticker) const {
    try {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            throw std::invalid_argument("Ticker not found: " + ticker);
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size() || !engines_info_[engine_id].engine_) {
            throw std::runtime_error("Engine not available for ticker: " + ticker);
        }
        
        // Return the per-engine capacity stored in OrderEngineInfo (set at register_stock)
        return engines_info_[engine_id].capacity_;
    } catch (...) {
        return 0;
    }
}

std::size_t backtest::runtime::EngineRuntime::get_utilization(const std::string& ticker) const {
    try {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            throw std::invalid_argument("Ticker not found: " + ticker);
        }
        
        EngineId engine_id = ticker_it->second;
        if (quantum_orders_ == 0) {
            update_snapshot_internal(engine_id);
        }
        
        const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
        if (!snap) {
            throw std::runtime_error("Snapshot not available for ticker: " + ticker);
        }
        
        // Utilization = open orders (orders that haven't been freed yet)
        return snap->open_count;
    } catch (...) {
        return 0;
    }
}

std::size_t backtest::runtime::EngineRuntime::get_pending_count(const std::string& ticker) const {
    try {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            throw std::invalid_argument("Ticker not found: " + ticker);
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) {
            throw std::runtime_error("Engine not available for ticker: " + ticker);
        }
        
        scheduler::WorkerId worker_id = engines_info_[engine_id].worker_id_;
        return scheduler_.pending_jobs_on(worker_id);
    } catch (...) {
        return 0;
    }
}

bool backtest::runtime::EngineRuntime::order_exists(const std::string& ticker, engine::OrderId order_id) const {
    try {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            return false;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size() || !engines_info_[engine_id].engine_) {
            return false;
        }
        
        const engine::OrderInfo* info = engines_info_[engine_id].engine_->get_order(order_id);
        return info != nullptr;
    } catch (...) {
        return false;
    }
}

backtest::user::User* backtest::runtime::EngineRuntime::register_strategy(std::string&& ticker, backtest::user::Strategy strategy, double starting_capital)
{
    if (!strategy)
    {
        throw std::invalid_argument("Cannot register null strategy");
    }

    auto ticker_it = ticker_to_engine_id_.find(ticker);
    if (ticker_it == ticker_to_engine_id_.end())
    {
        if (verbose_) notify("[RUNTIME] ERROR: Cannot register strategy — ticker not found: " + ticker);
        return nullptr;
    }
    EngineId engine_id = ticker_it->second;
    if (engine_id >= engines_info_.size())
    {
        if (verbose_) notify("[RUNTIME] ERROR: Cannot register strategy — engine not found for " + ticker);
        return nullptr;
    }

    while (users_per_engine_.size() <= engine_id)
        users_per_engine_.push_back({});

    backtest::user::UserId user_id = 0;
    std::size_t idx = 0;
    bool reuse_slot = false;

    // Look for an unregistered (invalid) slot to reuse
    for (std::size_t i = 0; i < users_.size(); ++i)
    {
        if (users_[i].get_user_id() == backtest::user::INVALID_USER_ID)
        {
            idx = i;
            user_id = static_cast<backtest::user::UserId>(i + 1);
            reuse_slot = true;
            break;
        }
    }

    if (!reuse_slot)
    {
        // No hole: append new user (user_id = 1-based index of new slot)
        user_id = static_cast<backtest::user::UserId>(users_.size() + 1);
        idx = users_.size();
        users_.emplace_back(std::move(strategy), this, user_id, starting_capital, &sync_order_api_);
        user_strategy_engine_id_.push_back(engine_id);
    }
    else
    {
        // Fill the hole: construct User in place at users_[idx]
        users_[idx] = backtest::user::User(std::move(strategy), this, user_id, starting_capital, &sync_order_api_);
        if (idx < user_strategy_engine_id_.size())
            user_strategy_engine_id_[idx] = engine_id;
        else
            user_strategy_engine_id_.resize(idx + 1, engine_id);
    }

    users_per_engine_[engine_id].push_back(idx);

    if (verbose_)
    {
        notify("[RUNTIME] Registered strategy for user " + std::to_string(user_id) +
                " on " + ticker + " with capital $" + std::to_string(starting_capital));
    }

    // Ensure user_orders_ has space for this user and per-engine entries
    if (user_id >= user_orders_.size())
        user_orders_.resize(user_id + 1);

    if (user_orders_[user_id].size() < engines_info_.size())
        user_orders_[user_id].resize(engines_info_.size());

    for (auto& s : user_orders_[user_id])
        s.reserve(64);

    // Pre-allocate reservation vectors via bridge (User is no longer friend of EngineRuntime)
    try
    {
        std::size_t reserve_size = std::max<std::size_t>(1024, default_capacity_ / 16);
        sync_order_api_.setup_user_reservations(&users_[idx], reserve_size);
    }
    catch (...) { /* best-effort */ }

    return &users_[idx];
}

bool backtest::runtime::EngineRuntime::unregister_strategy(backtest::user::UserId user_id)
{
    if (user_id < 1 || user_id > users_.size())
        return false;
    const std::size_t idx = user_id - 1;
    if (users_[idx].get_user_id() == backtest::user::INVALID_USER_ID)
        return false; // already unregistered

    // Remove this user's index from users_per_engine_[strategy_engine_id]
    EngineId strategy_engine_id = (idx < user_strategy_engine_id_.size()) ? user_strategy_engine_id_[idx] : 0;
    if (strategy_engine_id < users_per_engine_.size())
    {
        std::vector<std::size_t>& per_engine = users_per_engine_[strategy_engine_id];
        auto it = std::find(per_engine.begin(), per_engine.end(), idx);
        if (it != per_engine.end())
        {
            *it = per_engine.back();
            per_engine.pop_back();
        }
    }

    // Remove all of this user's orders from order_to_user_
    if (user_id < user_orders_.size())
    {
        for (EngineId engine_id = 0; engine_id < user_orders_[user_id].size(); ++engine_id)
        {
            for (engine::OrderId order_id : user_orders_[user_id][engine_id])
                order_to_user_.erase(order_id);
            user_orders_[user_id][engine_id].clear();
        }
    }

    // Invalidate the user slot (same address, so existing User* becomes a dead/invalid user)
    users_[idx] = backtest::user::User();

    if (verbose_)
        notify("[RUNTIME] Unregistered strategy for user " + std::to_string(user_id));

    return true;
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

std::vector<engine::OrderId> backtest::runtime::EngineRuntime::user_get_active_orders(backtest::user::UserId user_id, const std::string& ticker) const
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
        
        // Return only orders from the user_orders_ set, which is maintained to contain
        // only currently active orders (removed on fill/cancel)
        std::vector<engine::OrderId> active_orders;
        const auto& order_set = user_orders_[user_id][engine_id];
        active_orders.reserve(order_set.size());
        
        for (engine::OrderId order_id : order_set) {
            active_orders.push_back(order_id);
        }
        
        return active_orders;
    } catch (...) {
        return {};
    }
}

bool backtest::runtime::EngineRuntime::user_has_sufficient_shares(backtest::user::UserId user_id, const std::string& ticker, engine::Quantity qty) const
{
    try {
        const std::size_t user_idx = static_cast<std::size_t>(user_id) - 1;
        if (user_id == user::INVALID_USER_ID || user_idx >= users_.size()) return false;

        const user::User& u = users_[user_idx];
        double net_position = u.get_position(ticker);
        double already_committed = u.get_committed_sell_qty(ticker);
        double available = net_position - already_committed;
        return available >= math::internal_to_qty(qty);
    } catch (...) {
        return false;
    }
}

backtest::runtime::EngineRuntime::EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool _verbose, std::size_t quantum_orders)
    : scheduler_(num_threads), 
    num_workers_(num_threads), 
    default_capacity_(default_capacity),
    verbose_(_verbose),
    quantum_orders_(quantum_orders),
    log_buffer_(LOG_BUFFER_CAPACITY),
    record_buffer_(RECORD_BUFFER_CAPACITY),
    sync_order_api_(this)
{
    // Initialize runtime batch size to scheduler's batch capacity by default
    runtime_batch_size_.store(scheduler_.get_batch_capacity(), std::memory_order_relaxed);
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

    start_event_management_thread();
}

backtest::runtime::EngineRuntime::~EngineRuntime()
{
    if (event_management_thread_running_) 
    {
        stop_event_management_thread();
    }
}

void backtest::runtime::EngineRuntime::start_event_management_thread() noexcept
{
    if (!event_management_thread_running_.exchange(true)) 
    {
        event_management_thread_ = std::thread(&EngineRuntime::event_management_loop, this);
    }
}

void backtest::runtime::EngineRuntime::stop_event_management_thread() noexcept
{
    // Flush producer buffers so event thread can drain any pending log/record data
    log_buffer_.try_flush();
    record_buffer_.try_flush();
    // Wait for event thread to drain (JobScheduler pattern)
    while (record_buffer_.pending_reads() > 0 || log_buffer_.pending_reads() > 0)
        std::this_thread::yield();
    event_management_thread_running_.store(false);
    if (event_management_thread_.joinable()) 
    {
        event_management_thread_.join();
    }
    // Flush and close all open record streams (event management thread is stopped)
    for (auto& kv : record_streams_) 
    {
        if (kv.second) {
            kv.second->flush();
        }
    }
    record_streams_.clear();
}

void backtest::runtime::EngineRuntime::event_management_loop() noexcept
{
    while (event_management_thread_running_.load(std::memory_order_acquire)) 
    {
        bool did_work = false;

        // Drain record buffer first (lazy-open L2Stream per engine_id, write updates)
        RecordItem item;
        while (record_buffer_.try_pop(item)) 
        {
            did_work = true;
            EngineId eid = item.first;
            const stream::L2Update& update = item.second;
            if (eid >= engines_info_.size()) continue;
            const std::string& ticker = engines_info_[eid].ticker_;
            std::string path = (eid < record_path_override_.size() && !record_path_override_[eid].empty())
                ? record_path_override_[eid] : ticker + ".csv";
            auto& streams = record_streams_;
            auto it = streams.find(eid);
            if (it == streams.end()) 
            {
                auto ptr = std::make_unique<stream::L2Stream>(path, stream::StreamMode::Write);
                it = streams.emplace(eid, std::move(ptr)).first;
            }
            if (it->second) it->second->write(update);
        }

        // Drain log buffer (cout)
        std::string msg;
        while (log_buffer_.try_pop(msg)) 
        {
            did_work = true;
            std::cout << msg << std::endl;
        }

        if (!did_work) 
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void backtest::runtime::EngineRuntime::notify(std::string&& message) noexcept
{
    if (!verbose_) return;
    while (!log_buffer_.try_emplace(std::move(message)))
    {
        log_buffer_.try_flush();
        std::this_thread::yield();
    }
}

void backtest::runtime::EngineRuntime::record(EngineId engine_id, const stream::L2Update& update) noexcept
{
    while (!record_buffer_.try_emplace(engine_id, update))
    {
        record_buffer_.try_flush();
        std::this_thread::yield();
    }
}

void backtest::runtime::EngineRuntime::record_book_snapshot(EngineId engine_id) noexcept
{
    const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
    if (!snap) return;

    const int64_t ts = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    stream::L2Update u;
    u.timestamp = ts;
    u.is_snapshot = true;

    for (std::uint8_t i = 0; i < snap->bid_levels; ++i) {
        u.price = math::ticks_to_dollars(snap->bid_prices[i]);
        u.amount = math::internal_to_qty(snap->bid_depth[i]);
        u.side = 'b';
        record(engine_id, u);
    }
    for (std::uint8_t i = 0; i < snap->ask_levels; ++i) {
        u.price = math::ticks_to_dollars(snap->ask_prices[i]);
        u.amount = math::internal_to_qty(snap->ask_depth[i]);
        u.side = 'a';
        record(engine_id, u);
    }
}

void backtest::runtime::EngineRuntime::set_record(std::string&& ticker, bool enable) noexcept
{
    auto it = ticker_to_engine_id_.find(ticker);
    if (it == ticker_to_engine_id_.end()) return;
    EngineId eid = it->second;
    if (eid >= record_enabled_.size() || !record_enabled_[eid]) return;
    record_enabled_[eid]->store(enable, std::memory_order_relaxed);
}

void backtest::runtime::EngineRuntime::set_record(std::string&& ticker, bool enable, std::string&& path_override) noexcept
{
    auto it = ticker_to_engine_id_.find(ticker);
    if (it == ticker_to_engine_id_.end()) return;
    EngineId eid = it->second;
    if (eid >= record_enabled_.size() || !record_enabled_[eid]) return;
    record_enabled_[eid]->store(enable, std::memory_order_relaxed);
    if (eid < record_path_override_.size()) 
    {
        record_path_override_[eid] = enable ? std::forward<std::string>(path_override) : std::string();
    }
}

bool backtest::runtime::EngineRuntime::get_record(const std::string& ticker) const noexcept
{
    auto it = ticker_to_engine_id_.find(ticker);
    if (it == ticker_to_engine_id_.end()) return false;
    EngineId eid = it->second;
    if (eid >= record_enabled_.size() || !record_enabled_[eid]) return false;
    return record_enabled_[eid]->load(std::memory_order_relaxed);
}

void backtest::runtime::EngineRuntime::update_snapshot_internal(EngineId engine_id) const noexcept
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

    // If per-engine quantum reached, reset counter, publish snapshot, record book to L2 (if enabled), and run strategies for this engine only
    if (quantum != 0 && count >= quantum)
    {
        info.orders_since_quantum_ = 0;
        update_snapshot_internal(engine_id);
        if (engine_id < record_enabled_.size() && record_enabled_[engine_id]->load(std::memory_order_relaxed))
            record_book_snapshot(engine_id);
        if (engine_id < users_per_engine_.size())
        {
            for (std::size_t idx : users_per_engine_[engine_id])
            {
                if (idx < users_.size())
                    users_[idx].on_book_update();
            }
        }
    }
}

// User method implementations

void backtest::runtime::EngineRuntime::submit_job_on_worker(scheduler::WorkerId worker_id, scheduler::Job&& job) noexcept
{
    // Defensive: ensure worker id valid
    if (worker_id >= scheduler_.get_worker_count()) return;

    // If pending jobs for this worker reach or exceed runtime batch size,
    // trigger async processing before adding another job to keep batches bounded.
    if (runtime_batch_size_.load(std::memory_order_relaxed) > 0) {
        const std::size_t pending = scheduler_.pending_jobs_on(worker_id);
        if (pending + 1 >= runtime_batch_size_.load(std::memory_order_relaxed)) {
            scheduler_.process_jobs_on_async(worker_id);
        }
    }

    scheduler_.submit_job_on(worker_id, std::forward<scheduler::Job>(job));
}


void backtest::runtime::EngineRuntime::notify_order_event(const std::string& prefix, engine::OrderId order_id, engine::EventKind event_kind) noexcept
{
    if (!verbose_) return;

    switch (event_kind) {
        case engine::EventKind::ACCEPT:
            if (notify_order_.load(std::memory_order_acquire)) {
                notify(prefix + " Order " + std::to_string(order_id) + " accepted");
            }
            break;
        case engine::EventKind::FILL:
            if (notify_order_.load(std::memory_order_acquire)) {
                notify(prefix + " Order " + std::to_string(order_id) + " filled");
            }
            break;
        case engine::EventKind::PARTIAL_FILL:
            if (notify_order_.load(std::memory_order_acquire)) {
                notify(prefix + " Order " + std::to_string(order_id) + " partially filled");
            }
            break;
        case engine::EventKind::REJECT:
            notify(prefix + " Order " + std::to_string(order_id) + " rejected");
            break;
        case engine::EventKind::CANCEL:
            if (notify_order_.load(std::memory_order_acquire)){
                notify(prefix + " Order " + std::to_string(order_id) + " cancelled");
            }
            break;
        default:
            break;
    }
}

void backtest::runtime::EngineRuntime::track_user_order(engine::OrderId order_id, user::UserId user_id, EngineId engine_id) noexcept
{
    if (order_id == engine::INVALID_ORDER_ID || user_id == user::INVALID_USER_ID) return;

    // Grow tracking tables on demand — strategies may be registered before stocks
    if (user_id >= user_orders_.size())
        user_orders_.resize(user_id + 1);
    if (engine_id >= user_orders_[user_id].size())
        user_orders_[user_id].resize(engine_id + 1);

    user_orders_[user_id][engine_id].insert(order_id);
    order_to_user_[order_id] = user_id;
}

void backtest::runtime::EngineRuntime::handle_accept_event(engine::OrderId order_id, user::UserId user_id, EngineId engine_id, 
                                                           engine::OrderSide side, engine::Quantity qty_ticks, engine::Price price_ticks) noexcept
{
    if (user_id == user::INVALID_USER_ID || order_id == engine::INVALID_ORDER_ID) return;
    
    try {
        const std::string &ticker = engines_info_[engine_id].ticker_;
        if (!ticker.empty() && user_id != user::IPO_HOLDER) {
            double qty = math::internal_to_qty(qty_ticks);
            double price = math::ticks_to_dollars(price_ticks);
            sync_order_api_.reserve_on_accept_to_user(&users_[user_id - 1], ticker, order_id, side, qty, price);
        }
    } catch (...) { }
}

void backtest::runtime::EngineRuntime::handle_fill_event(const engine::EngineMsg& msg, EngineId engine_id) noexcept
{
    // Fast-path: only FILL/PARTIAL_FILL with valid payload matter for attribution
    if (msg.kind != engine::EventKind::FILL && msg.kind != engine::EventKind::PARTIAL_FILL) return;
    if (msg.qty == 0 || msg.price == static_cast<engine::Price>(-1)) return;

    // Fast-path: check user ownership before any other work —
    // the vast majority of fills are data-stream orders (never in this map).
    auto oit = order_to_user_.find(msg.order_id);
    if (oit == order_to_user_.end()) return;

    try {
        const std::string &ticker = engines_info_[engine_id].ticker_;
        if (ticker.empty()) return;
        
        double qty = math::internal_to_qty(msg.qty);
        double price = math::ticks_to_dollars(msg.price);
        
        user::UserId uid = oit->second;
        if (uid > 0 && uid < user_orders_.size() && engine_id < user_orders_[uid].size()) {
            auto &s = user_orders_[uid][engine_id];
            if (s.find(msg.order_id) != s.end()) {
                sync_order_api_.apply_fill_to_user(&users_[uid - 1], ticker, msg.order_id, msg.side, qty, price);
                if (msg.kind == engine::EventKind::FILL) {
                    s.erase(msg.order_id);
                    order_to_user_.erase(msg.order_id);
                }
            }
        }
    } catch (...) { }
}



// Non-inline User member implementations (order_slot_idx is in engine_runtime.h)
void backtest::user::User::reserve_on_accept(const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    const std::size_t idx = backtest::runtime::order_slot_idx(order_id);
    // Ensure reservation vectors are large enough
    if (idx >= reserved_cash_.size()) {
        std::size_t new_size = idx + 1;
        reserved_cash_.resize(new_size, 0.0);
        reserved_qty_.resize(new_size, 0.0);
        reserve_side_.resize(new_size, engine::OrderSide::BID);
        reserved_ticker_.resize(new_size, "");
    }

    reserve_side_[idx]    = side;
    reserved_ticker_[idx] = ticker;

    // Reserve funds/shares based on order side
    if (side == engine::OrderSide::BID) {
        reserved_cash_[idx] = qty * price;
        capital_ -= qty * price;
    } else {
        reserved_qty_[idx] = qty;
        committed_sell_qty_[ticker] += qty;
    }
}

void backtest::user::User::release_reservation(engine::OrderId order_id)
{
    const std::size_t idx = backtest::runtime::order_slot_idx(order_id);
    if (idx >= reserved_cash_.size()) return;

    if (reserve_side_[idx] == engine::OrderSide::BID) {
        capital_ += reserved_cash_[idx];
        reserved_cash_[idx] = 0.0;
    } else {
        const std::string& ticker = reserved_ticker_[idx];
        auto it = committed_sell_qty_.find(ticker);
        if (it != committed_sell_qty_.end()) {
            it->second -= reserved_qty_[idx];
            if (it->second < 0.0) it->second = 0.0;
        }
        reserved_qty_[idx] = 0.0;
    }
    reserved_ticker_[idx] = "";
}

void backtest::user::User::apply_fill(const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    // Update position first
    double signed_qty = (side == engine::OrderSide::BID) ? qty : -qty;
    update_position(ticker, signed_qty, price);

    const std::size_t idx = backtest::runtime::order_slot_idx(order_id);
    // Release reservation for filled portion
    if (idx < reserved_cash_.size()) {
        if (side == engine::OrderSide::BID) {
            double filled_cost = qty * price;
            reserved_cash_[idx] -= filled_cost;
            if (reserved_cash_[idx] < 0.0) reserved_cash_[idx] = 0.0;
        } else {
            reserved_qty_[idx] -= qty;
            if (reserved_qty_[idx] < 0.0) reserved_qty_[idx] = 0.0;
            capital_ += qty * price;

            // Decrement O(1) committed sell counter
            auto it = committed_sell_qty_.find(ticker);
            if (it != committed_sell_qty_.end()) {
                it->second -= qty;
                if (it->second < 0.0) it->second = 0.0;
            }
        }
    }
}

// ===== UserAPI IMPLEMENTATIONS =====

engine::OrderId backtest::runtime::UserAPI::submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty, user::UserId user_id)
{
    return runtime_ ? runtime_->submit_limit_order_sync_impl(ticker, side, price, qty, user_id) : engine::INVALID_ORDER_ID;
}

engine::OrderId backtest::runtime::UserAPI::submit_market_order(const std::string& ticker, engine::OrderSide side, double qty, user::UserId user_id)
{
    return runtime_ ? runtime_->submit_market_order_sync_impl(ticker, side, qty, user_id) : engine::INVALID_ORDER_ID;
}

bool backtest::runtime::UserAPI::submit_cancel_order(const std::string& ticker, engine::OrderId order_id, user::UserId user_id)
{
    return runtime_ ? runtime_->submit_cancel_order_sync_impl(ticker, order_id, user_id) : false;
}

bool backtest::runtime::UserAPI::submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id)
{
    return runtime_ ? runtime_->submit_edit_order_sync_impl(ticker, order_id, new_price, new_qty, user_id) : false;
}

void backtest::runtime::UserAPI::apply_fill_to_user(user::User* u, const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    if (u) u->apply_fill(ticker, order_id, side, qty, price);
}

void backtest::runtime::UserAPI::reserve_on_accept_to_user(user::User* u, const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    if (u) u->reserve_on_accept(ticker, order_id, side, qty, price);
}

void backtest::runtime::UserAPI::release_reservation_for_user(user::User* u, engine::OrderId order_id)
{
    if (u) u->release_reservation(order_id);
}

void backtest::runtime::UserAPI::setup_user_reservations(user::User* u, std::size_t reserve_size)
{
    if (!u) return;
    u->reserved_cash_.assign(reserve_size, 0.0);
    u->reserved_qty_.assign(reserve_size, 0.0);
    u->reserve_side_.assign(reserve_size, engine::OrderSide::BID);
    u->reserved_ticker_.assign(reserve_size, "");
}

// ===== USER CLASS METHOD IMPLEMENTATIONS =====

engine::OrderId backtest::user::User::submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double quantity)
{
    if (sync_order_api_) return sync_order_api_->submit_limit_order(ticker, side, price, quantity, user_id_);
    return engine::INVALID_ORDER_ID;
}

engine::OrderId backtest::user::User::submit_market_order(const std::string& ticker, engine::OrderSide side, double quantity)
{
    if (sync_order_api_) return sync_order_api_->submit_market_order(ticker, side, quantity, user_id_);
    return engine::INVALID_ORDER_ID;
}

bool backtest::user::User::submit_cancel_order(const std::string& ticker, engine::OrderId order_id)
{
    if (sync_order_api_) return sync_order_api_->submit_cancel_order(ticker, order_id, user_id_);
    return false;
}

bool backtest::user::User::submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_quantity)
{
    if (sync_order_api_) return sync_order_api_->submit_edit_order(ticker, order_id, new_price, new_quantity, user_id_);
    return false;
}
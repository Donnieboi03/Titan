#include "engine_runtime.h"
#include "market_data_stream.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <chrono>
#include <thread>
#include <utility>
#include <string>

// File-scope pointer so both get_instance() and reset_instance() can manage lifetime.
static backtest::runtime::EngineRuntime* s_instance_ptr = nullptr;

// EngineRuntime Singleton Management Implementation
backtest::runtime::EngineRuntime& backtest::runtime::EngineRuntime::get_instance(std::size_t num_threads, bool _verbose, std::size_t quantum_orders, std::size_t max_capacity, std::size_t max_engine_count, std::size_t max_strategies)
{
    if (!instance_initialized_)
    {
        // TODO: Remove delete here; only reset_instance() should delete. Create only when s_instance_ptr == nullptr to avoid reentrancy UB.
        delete s_instance_ptr;
        s_instance_ptr = new EngineRuntime(num_threads, _verbose, quantum_orders, max_capacity, max_engine_count, max_strategies);
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
        s_instance_ptr->users_.clear();
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

bool backtest::runtime::EngineRuntime::register_stock(const std::string& ticker, double _ipo_price, double _ipo_qty, std::size_t capacity)
{
    try {
        // Verify ticker before creating engine
        if (ticker.empty()) 
        {
            if (verbose_) notify("[REGISTER] ERROR: Empty ticker provided");
            return false;
        }
        
        // IF ipo price or qty is less than or equal to 0
        if (_ipo_price <= 0.0 || _ipo_qty <= 0.0) 
        {
            if (verbose_) notify("[REGISTER] ERROR: IPO Price/Quantity must be > 0 for " + ticker);
            return false;
        }
        
        // If ticker is already in Exchange then error
        if (ticker_to_engine_id_.find(ticker) != ticker_to_engine_id_.end()) 
        {
            if (verbose_) notify("[REGISTER] ERROR: Stock " + ticker + " already exists");
            return false;
        }
        
        // Check engine limit (vector capacity reserved at construction)
        if (engines_info_.size() >= max_engine_count_)
        {
            if (verbose_) notify("[REGISTER] ERROR: Maximum engine limit (" + std::to_string(max_engine_count_) + ") reached");
            return false;
        }

        // Convert user-facing values to internal format
        engine::Price ipo_price_ticks = math::dollars_to_ticks(_ipo_price);
        engine::Quantity ipo_qty_ticks = math::qty_to_internal(_ipo_qty);

        // Use provided capacity or default
        std::size_t engine_capacity = capacity > 0 ? std::min(capacity, max_capacity_) : max_capacity_;
        
        // Calculate engine ID from vector size before emplacing
        EngineId engine_id = engines_info_.size();

        // Add OrderEngineInfo to engines vector
        engines_info_.emplace_back(engine_capacity, verbose_, ipo_qty_ticks, engine_id % num_workers_, engine_id);
        auto it = ticker_to_engine_id_.emplace(std::move(ticker), engine_id).first;
        engines_info_[engine_id].submit_.ticker_ = it->first;
        
        // Engine-first: one EngineOrders per engine; new engine gets default empty by_user
        user_orders_.resize(engines_info_.size());

        // Grow per-engine recording state and per-engine strategy list
        while (record_enabled_.size() <= engine_id) {
            record_enabled_.push_back(std::make_unique<std::atomic<bool>>(false));
            record_path_override_.push_back({});
        }
        // Place initial sell at IPO Price and IPO Quantity (from IPO holder)
        engine::OrderId ipo_order;
        if (users_.empty()) {
            ipo_order = engines_info_[engine_id].worker_.engine_->place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, ipo_price_ticks, ipo_qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            ipo_order = engines_info_[engine_id].worker_.engine_->place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, ipo_price_ticks, ipo_qty_ticks, msgs);
            for (const auto& msg : msgs) {
                if (verbose_) {
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
        }
        
        // Update snapshot so IPO order is visible and cache pointer for get_snapshot_fast
        {
            auto& w = engines_info_[engine_id].worker_;
            w.engine_->update_snapshot();
            w.snapshot_ptr_ = &w.engine_->get_snapshot();
        }
        // Engine-first: ensure this engine's by_user has space for IPO_HOLDER (index 0)
        if (engine_id < user_orders_.size() && user_orders_[engine_id].by_user.size() <= backtest::user::IPO_HOLDER)
            user_orders_[engine_id].by_user.resize(backtest::user::IPO_HOLDER + 1);
        // Track IPO order ownership
        user_orders_[engine_id].by_user[backtest::user::IPO_HOLDER].insert(ipo_order);
        engines_info_[engine_id].worker_.order_to_user_[ipo_order] = backtest::user::IPO_HOLDER;
        
        if (verbose_) notify("[REGISTER] Registered " + it->first + " with IPO: " +
            std::to_string(_ipo_qty) + " shares @ $" + std::to_string(_ipo_price) +
            " (owned by user " + std::to_string(backtest::user::IPO_HOLDER) + ")");
        
        return true;
    } catch(const std::exception& e) {
        if (verbose_) notify("[REGISTER] ERROR: " + std::string(e.what()));
        return false;
    }
}

bool backtest::runtime::EngineRuntime::unregister_stock(const std::string& ticker)
{
    try
    {
        // Verify ticker before processing
        if (ticker.empty()) 
        {
            if (verbose_) notify("[UNREGISTER] ERROR: Empty ticker provided");
            return false;
        }
        
        // Find the ticker-to-engine mapping
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) 
        {
            if (verbose_) notify("[UNREGISTER] ERROR: Stock " + ticker + " does not exist");
            return false;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) 
        {
            if (verbose_) notify("[UNREGISTER] ERROR: Engine not found for " + ticker);
            return false;
        }

        // Unregister all strategies associated with this ticker (iterate over a copy)
        if (engine_id < engines_info_.size())
        {
            std::vector<std::size_t> indices = engines_info_[engine_id].user_indices_;
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
        scheduler_.process_jobs_on(engine_info.submit_.worker_id_);
        
        // Remove from ticker map (engine stays in vector to preserve indices)
        std::string unreg_ticker = ticker_it->first;
        ticker_to_engine_id_.erase(ticker_it);
        
        // Clear snapshot pointer for this engine
        engines_info_[engine_id].worker_.snapshot_ptr_ = nullptr;
        // Clear all user orders for this engine and remove reverse mappings (engine-first)
        if (engine_id < user_orders_.size()) {
            auto& o2u = engines_info_[engine_id].worker_.order_to_user_;
            for (auto& order_set : user_orders_[engine_id].by_user) {
                for (const auto& oid : order_set)
                    o2u.erase(oid);
                order_set.clear();
            }
        }
        
        if (verbose_) notify("[UNREGISTER] Unregistered " + unreg_ticker);
        
        return true;
    }
    catch(const std::exception& e)
    {
        if (verbose_) notify("[UNREGISTER] ERROR: " + std::string(e.what()));
        return false;
    }
}

engine::OrderId backtest::runtime::EngineRuntime::submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty)
{
    return submit_limit_order_async_impl(ticker, side, price, qty, user::INVALID_USER_ID);
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
            if (engine_id < runtime_ptr->user_orders_.size() && 
                user_id < runtime_ptr->user_orders_[engine_id].by_user.size()) 
            {
                engine::Quantity total_owned = 0;
                for (engine::OrderId order_id : runtime_ptr->user_orders_[engine_id].by_user[user_id]) 
                {
                    auto order = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_order(order_id);
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

        // VALIDATE CAPITAL FOR BID (only for registered users): reject if insufficient funds
        if (_side == engine::OrderSide::BID && user_id != backtest::user::INVALID_USER_ID) 
        {
            if (user_id > 0 && user_id <= static_cast<backtest::user::UserId>(runtime_ptr->users_.size())) 
            {
                double notional = backtest::math::internal_to_qty(qty_ticks) * backtest::math::ticks_to_dollars(price_ticks);
                double capital = runtime_ptr->users_[user_id - 1].get_capital();
                if (capital < notional) 
                {
                    if (runtime_ptr->verbose_) runtime_ptr->notify("[LIMIT ORDER] ERROR: Insufficient capital for user " + std::to_string(user_id) +
                        " (capital=" + std::to_string(capital) + " notional=" + std::to_string(notional) + ")");
                    return;
                }
            }
        }
        
        engine::OrderId order_id;
        const bool use_message_path = !runtime_ptr->users_.empty() || runtime_ptr->notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = runtime_ptr->engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = runtime_ptr->users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [rt = runtime_ptr, engine_id](engine::OrderId id) { return rt->engines_info_[engine_id].worker_.order_to_user_.find(id) != rt->engines_info_[engine_id].worker_.order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = runtime_ptr->users_.empty() ? nullptr : &fill_filter_fn;
            order_id = runtime_ptr->engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                runtime_ptr->notify_order_event("[LIMIT ORDER]", order_id, msg.kind);
                if (order_id != engine::INVALID_ORDER_ID && user_id != user::INVALID_USER_ID) {
                    switch (msg.kind) {
                        case engine::EventKind::ACCEPT:
                            runtime_ptr->handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, price_ticks);
                            break;
                        case engine::EventKind::FILL:
                        case engine::EventKind::PARTIAL_FILL:
                            runtime_ptr->handle_fill_event(msg, engine_id);
                            break;
                        default:
                            break;
                    }
                }
            }
        }
        
        if (order_id != engine::INVALID_ORDER_ID) {
            if (user_id != user::INVALID_USER_ID)
                runtime_ptr->track_user_order(order_id, user_id, engine_id);
            if (runtime_ptr->get_quantum() != 0 && user_id == user::INVALID_USER_ID)
                runtime_ptr->increment_order_counter(engine_id);
        }

    }, engine_id); // Use engine_id as owner_id
    
        // Use runtime wrapper to enforce batch-size then submit
        submit_job_on_worker(engine_info.submit_.worker_id_, std::move(job));
        return engine::INVALID_ORDER_ID;  // Async path: order ID not available at return
    } catch (const std::exception& e) {
        if (verbose_) notify("[LIMIT ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        if (verbose_) notify("[LIMIT ORDER] EXCEPTION: Unknown error");
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
            if (engine_id < user_orders_.size() && user_id < user_orders_[engine_id].by_user.size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId oid : user_orders_[engine_id].by_user[user_id]) {
                    auto order = engines_info_[engine_id].worker_.engine_->get_order(oid);
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

        // VALIDATE CAPITAL FOR BID (only for registered users): reject if insufficient funds
        if (_side == engine::OrderSide::BID && user_id != backtest::user::INVALID_USER_ID) {
            if (user_id > 0 && user_id <= users_.size()) {
                double notional = math::internal_to_qty(qty_ticks) * math::ticks_to_dollars(price_ticks);
                double capital = users_[user_id - 1].get_capital();
                if (capital < notional) {
                    if (verbose_) notify("[LIMIT ORDER] ERROR: Insufficient capital for user " + std::to_string(user_id) +
                        " (capital=" + std::to_string(capital) + " notional=" + std::to_string(notional) + ")");
                    return engine::INVALID_ORDER_ID;
                }
            }
        }
        
        engine::OrderId order_id;
        const bool use_message_path = !users_.empty() || notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [this, engine_id](engine::OrderId id) { return engines_info_[engine_id].worker_.order_to_user_.find(id) != engines_info_[engine_id].worker_.order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = users_.empty() ? nullptr : &fill_filter_fn;
            order_id = engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::LIMIT, price_ticks, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                notify_order_event("[LIMIT ORDER]", order_id, msg.kind);
                if (order_id != engine::INVALID_ORDER_ID && user_id != user::INVALID_USER_ID) {
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, price_ticks);
                    }
                    handle_fill_event(msg, engine_id);
                }
            }
        }
        
        if (order_id != engine::INVALID_ORDER_ID) {
            if (user_id != user::INVALID_USER_ID)
                track_user_order(order_id, user_id, engine_id);
            if (get_quantum() != 0 && user_id == user::INVALID_USER_ID)
                increment_order_counter(engine_id);
        }
        
        return order_id;
    } catch (const std::exception& e) {
        if (verbose_) notify("[LIMIT ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        if (verbose_) notify("[LIMIT ORDER] EXCEPTION: Unknown error");
        return engine::INVALID_ORDER_ID;
    }
}

engine::OrderId backtest::runtime::EngineRuntime::submit_market_order(const std::string& ticker, engine::OrderSide side, double qty)
{
    return submit_market_order_async_impl(ticker, side, qty, user::INVALID_USER_ID);
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
                engine_id < runtime_ptr->user_orders_.size() && user_id < runtime_ptr->user_orders_[engine_id].by_user.size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId order_id : runtime_ptr->user_orders_[engine_id].by_user[user_id]) {
                    auto order = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_order(order_id);
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
        
        const engine::MarketSnapshot& snap = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_snapshot();
        engine::Price market_price = (_side == engine::OrderSide::BID) ? snap.best_ask : snap.best_bid;
        
        if (market_price == static_cast<engine::Price>(-1)) {
            if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: No market price available");
            return;
        }

        // VALIDATE CAPITAL FOR BID (registered users only)
        if (_side == engine::OrderSide::BID && user_id != backtest::user::INVALID_USER_ID) {
            if (user_id > 0 && user_id <= runtime_ptr->users_.size()) {
                const double notional = math::internal_to_qty(qty_ticks) * math::ticks_to_dollars(market_price);
                const double capital = runtime_ptr->users_[user_id - 1].get_capital();
                if (capital < notional) {
                    if (runtime_ptr->verbose_) runtime_ptr->notify("[MARKET ORDER] ERROR: Insufficient capital for user " + std::to_string(user_id));
                    return;
                }
            }
        }

        engine::OrderId order_id;
        const bool use_message_path = !runtime_ptr->users_.empty() || runtime_ptr->notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = runtime_ptr->engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = runtime_ptr->users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [rt = runtime_ptr, engine_id](engine::OrderId id) { return rt->engines_info_[engine_id].worker_.order_to_user_.find(id) != rt->engines_info_[engine_id].worker_.order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = runtime_ptr->users_.empty() ? nullptr : &fill_filter_fn;
            order_id = runtime_ptr->engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                runtime_ptr->notify_order_event("[MARKET ORDER]", order_id, msg.kind);
                if (order_id != engine::INVALID_ORDER_ID && user_id != user::INVALID_USER_ID) {
                    switch (msg.kind) {
                        case engine::EventKind::ACCEPT:
                            runtime_ptr->handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, market_price);
                            break;
                        case engine::EventKind::FILL:
                        case engine::EventKind::PARTIAL_FILL:
                            runtime_ptr->handle_fill_event(msg, engine_id);
                            break;
                        default:
                            break;
                    }
                }
            }
        }
        
        if (order_id != engine::INVALID_ORDER_ID) {
            if (user_id != user::INVALID_USER_ID)
                runtime_ptr->track_user_order(order_id, user_id, engine_id);
            if (runtime_ptr->get_quantum() != 0 && user_id == user::INVALID_USER_ID)
                runtime_ptr->increment_order_counter(engine_id);
        }
    }, engine_id);

        submit_job_on_worker(engine_info.submit_.worker_id_, std::move(job));
        return engine::INVALID_ORDER_ID;  // Async path: order ID not available at return
    } catch (const std::exception& e) {
        if (verbose_) notify("[MARKET ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        if (verbose_) notify("[MARKET ORDER] EXCEPTION: Unknown error");
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
            if (engine_id < user_orders_.size() && user_id < user_orders_[engine_id].by_user.size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId oid : user_orders_[engine_id].by_user[user_id]) {
                    auto order = engines_info_[engine_id].worker_.engine_->get_order(oid);
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
        const engine::MarketSnapshot& snap = engines_info_[engine_id].worker_.engine_->get_snapshot();
        engine::Price market_price = (_side == engine::OrderSide::BID) ? snap.best_ask : snap.best_bid;
        
        if (market_price == static_cast<engine::Price>(-1)) {
            if (verbose_) notify("[MARKET ORDER] ERROR: No market price available");
            return engine::INVALID_ORDER_ID;
        }

        // VALIDATE CAPITAL FOR BID (registered users only)
        if (_side == engine::OrderSide::BID && user_id != backtest::user::INVALID_USER_ID) {
            if (user_id > 0 && user_id <= users_.size()) {
                const double notional = math::internal_to_qty(qty_ticks) * math::ticks_to_dollars(market_price);
                const double capital = users_[user_id - 1].get_capital();
                if (capital < notional) {
                    if (verbose_) notify("[MARKET ORDER] ERROR: Insufficient capital for user " + std::to_string(user_id));
                    return engine::INVALID_ORDER_ID;
                }
            }
        }

        engine::OrderId order_id;
        const bool use_message_path = !users_.empty() || notify_order_.load(std::memory_order_acquire);
        if (!use_message_path) {
            order_id = engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks);
        } else {
            std::vector<engine::EngineMsg> msgs;
            bool collect_accept = users_.empty() || (user_id != user::INVALID_USER_ID);
            std::function<bool(engine::OrderId)> fill_filter_fn = [this, engine_id](engine::OrderId id) { return engines_info_[engine_id].worker_.order_to_user_.find(id) != engines_info_[engine_id].worker_.order_to_user_.end(); };
            const std::function<bool(engine::OrderId)>* fill_filter_ptr = users_.empty() ? nullptr : &fill_filter_fn;
            order_id = engines_info_[engine_id].worker_.engine_->place_order(_side, engine::OrderType::MARKET, market_price, qty_ticks, msgs, collect_accept, fill_filter_ptr);
            for (const auto& msg : msgs) {
                notify_order_event("[MARKET ORDER]", order_id, msg.kind);
                if (order_id != engine::INVALID_ORDER_ID && user_id != user::INVALID_USER_ID) {
                    if (msg.kind == engine::EventKind::ACCEPT) {
                        handle_accept_event(order_id, user_id, engine_id, _side, qty_ticks, market_price);
                    }
                    handle_fill_event(msg, engine_id);
                }
            }
        }
        
        if (order_id != engine::INVALID_ORDER_ID) {
            if (user_id != user::INVALID_USER_ID)
                track_user_order(order_id, user_id, engine_id);
            if (get_quantum() != 0 && user_id == user::INVALID_USER_ID)
                increment_order_counter(engine_id);
        }
        
        return order_id;
    } catch (const std::exception& e) {
        if (verbose_) notify("[MARKET ORDER] EXCEPTION: " + std::string(e.what()));
        return engine::INVALID_ORDER_ID;
    } catch (...) {
        if (verbose_) notify("[MARKET ORDER] EXCEPTION: Unknown error");
        return engine::INVALID_ORDER_ID;
    }
}

bool backtest::runtime::EngineRuntime::submit_cancel_order(const std::string& ticker, engine::OrderId order_id)
{
    return submit_cancel_order_async_impl(ticker, order_id, user::INVALID_USER_ID);
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

        const engine::OrderInfo* order = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_order(order_id);
        if (order == nullptr) {
            if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
            return;
        }
        
        engine::EngineMsg msg;
        bool success = runtime_ptr->engines_info_[engine_id].worker_.engine_->cancel_order(order_id, msg);
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

        // If cancel accepted: release any reservation (order had remaining qty/price from before cancel)
        if (msg.kind == engine::EventKind::ACCEPT && order != nullptr) {
            if (user_id != backtest::user::INVALID_USER_ID && msg.order_id != engine::INVALID_ORDER_ID) {
                try {
                    const std::size_t user_idx = static_cast<std::size_t>(user_id) - 1;
                    if (user_id != backtest::user::IPO_HOLDER && user_idx < runtime_ptr->users_.size()) {
                        double rem_qty = backtest::math::internal_to_qty(order->qty_);
                        double price = backtest::math::ticks_to_dollars(order->price_);
                        runtime_ptr->sync_order_api_.release_reservation_for_user(&runtime_ptr->users_[user_idx], order_id, order->side_, rem_qty, price);
                    }
                } catch (...) { }
            }
        }
        
        if (success) {
            switch (msg.kind) {
                case engine::EventKind::ACCEPT:
                    if (runtime_ptr->verbose_) runtime_ptr->notify("[CANCEL ORDER] Order " + std::to_string(order_id) + " cancelled");
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
        if (user_id != backtest::user::INVALID_USER_ID) {
            if (engine_id < runtime_ptr->user_orders_.size() && user_id < runtime_ptr->user_orders_[engine_id].by_user.size()) {
                runtime_ptr->user_orders_[engine_id].by_user[user_id].erase(order_id);
                runtime_ptr->engines_info_[engine_id].worker_.order_to_user_.erase(order_id);
            }
        }
        
        // Increment order counter for quantum tracking (skip when batch mode)
        if (runtime_ptr->get_quantum() != 0) runtime_ptr->increment_order_counter(engine_id);
    }, engine_id);

        submit_job_on_worker(engine_info.submit_.worker_id_, std::move(job));
        return true;
    } catch (const std::exception& e) {
        if (verbose_) notify("[CANCEL ORDER] EXCEPTION: " + std::string(e.what()));
        return false;
    } catch (...) {
        if (verbose_) notify("[CANCEL ORDER] EXCEPTION: Unknown error");
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
        
        const engine::OrderInfo* order = engines_info_[engine_id].worker_.engine_->get_order(order_id);
        if (order == nullptr) {
            if (verbose_) notify("[CANCEL ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
            return false;
        }
        
        // Direct engine call
        engine::EngineMsg msg;
        bool success = engines_info_[engine_id].worker_.engine_->cancel_order(order_id, msg);
        
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

        // Release reservation and untrack order if cancelled successfully (order had remaining qty/price from before cancel)
        if (actually_cancelled && user_id != backtest::user::INVALID_USER_ID) {
            const std::size_t user_idx = static_cast<std::size_t>(user_id) - 1;
            if (user_id != backtest::user::IPO_HOLDER && user_idx < users_.size()) {
                double rem_qty = math::internal_to_qty(order->qty_);
                double price = math::ticks_to_dollars(order->price_);
                sync_order_api_.release_reservation_for_user(&users_[user_idx], order_id, order->side_, rem_qty, price);
            }
            if (engine_id < user_orders_.size() && user_id < user_orders_[engine_id].by_user.size()) {
                user_orders_[engine_id].by_user[user_id].erase(order_id);
                engines_info_[engine_id].worker_.order_to_user_.erase(order_id);
            }
        }
        
        // Increment order counter for quantum tracking (skip when batch mode)
        if (get_quantum() != 0) increment_order_counter(engine_id);
        
        return actually_cancelled;
    } catch (const std::exception& e) {
        if (verbose_) notify("[CANCEL ORDER] EXCEPTION: " + std::string(e.what()));
        return false;
    } catch (...) {
        if (verbose_) notify("[CANCEL ORDER] EXCEPTION: Unknown error");
        return false;
    }
}

bool backtest::runtime::EngineRuntime::submit_replace_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty)
{
    return submit_replace_order_async_impl(ticker, order_id, new_price, new_qty, user::INVALID_USER_ID);
}

bool backtest::runtime::EngineRuntime::submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_qty)
{
    return submit_edit_order_async_impl(ticker, order_id, new_qty, user::INVALID_USER_ID);
}

bool backtest::runtime::EngineRuntime::submit_replace_order_async_impl(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, backtest::user::UserId user_id)
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
    
        const engine::OrderInfo* order = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_order(order_id);
        if (order == nullptr) {
            if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
            return;
        }
        
        // VALIDATE OWNERSHIP BEFORE EDITING (only for registered users)
        if (order->side_ == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
            if (engine_id < runtime_ptr->user_orders_.size() &&
                user_id < runtime_ptr->user_orders_[engine_id].by_user.size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId owned_order_id : runtime_ptr->user_orders_[engine_id].by_user[user_id]) {
                    auto owned_order = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_order(owned_order_id);
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
        double old_qty = backtest::math::internal_to_qty(order->qty_);
        double old_price = backtest::math::ticks_to_dollars(order->price_);
        std::vector<engine::EngineMsg> msgs;
        bool result = runtime_ptr->engines_info_[engine_id].worker_.engine_->replace_order(order_id, order->side_, price_ticks, qty_ticks, msgs);

        for (const auto& msg : msgs) {
            if (runtime_ptr->verbose_) {
                switch (msg.kind) {
                    case engine::EventKind::ACCEPT:
                        runtime_ptr->notify("[REPLACE ORDER] Order " + std::to_string(order_id) + " replaced");
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

            // If order accepted: release old reservation, then reserve new
            if (msg.kind == engine::EventKind::ACCEPT) {
                if (user_id != backtest::user::INVALID_USER_ID && result) {
                    try {
                        if (user_id != backtest::user::IPO_HOLDER && engine_id < runtime_ptr->engines_info_.size() && !runtime_ptr->engines_info_[engine_id].submit_.ticker_.empty()) {
                            runtime_ptr->sync_order_api_.release_reservation_for_user(&runtime_ptr->users_[user_id - 1], order_id, order->side_, old_qty, old_price);
                            double qty = backtest::math::internal_to_qty(qty_ticks);
                            double price = backtest::math::ticks_to_dollars(price_ticks);
                            runtime_ptr->sync_order_api_.reserve_on_accept_to_user(&runtime_ptr->users_[user_id - 1], order_id, order->side_, qty, price);
                        }
                    } catch (...) { }
                }
            }

            if ((msg.kind == engine::EventKind::FILL || msg.kind == engine::EventKind::PARTIAL_FILL) &&
                msg.qty > 0 && msg.price != static_cast<engine::Price>(-1)) {
                try {
                    double qty = backtest::math::internal_to_qty(msg.qty);
                    double price = backtest::math::ticks_to_dollars(msg.price);
                    auto& o2u = runtime_ptr->engines_info_[engine_id].worker_.order_to_user_;
                    auto oit = o2u.find(msg.order_id);
                    if (oit != o2u.end()) {
                        backtest::user::UserId uid = oit->second;
                        if (uid > 0 && engine_id < runtime_ptr->user_orders_.size() && uid < runtime_ptr->user_orders_[engine_id].by_user.size()) {
                            auto& s = runtime_ptr->user_orders_[engine_id].by_user[uid];
                            if (s.find(msg.order_id) != s.end()) {
                                runtime_ptr->sync_order_api_.apply_fill_to_user(&runtime_ptr->users_[uid - 1], msg.order_id, msg.side, qty, price);
                                s.erase(msg.order_id);
                                o2u.erase(oit);
                            }
                        }
                    }
                } catch (...) { }
            }
        }
        
        if (!result) {
            if (runtime_ptr->verbose_) runtime_ptr->notify("[REPLACE ORDER] ERROR: Failed to replace order " + std::to_string(order_id));
        }
        
        // Increment order counter for quantum tracking (skip when batch mode)
        if (runtime_ptr->get_quantum() != 0) runtime_ptr->increment_order_counter(engine_id);
    }, engine_id);

        submit_job_on_worker(engine_info.submit_.worker_id_, std::move(job));
        return true;
    } catch (const std::exception& e) {
        if (verbose_) notify("[REPLACE ORDER] EXCEPTION: " + std::string(e.what()));
        return false;
    } catch (...) {
        if (verbose_) notify("[REPLACE ORDER] EXCEPTION: Unknown error");
        return false;
    }
}

bool backtest::runtime::EngineRuntime::submit_edit_order_async_impl(const std::string& _ticker, engine::OrderId order_id, double new_qty, backtest::user::UserId user_id)
{
    try {
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
        engine::Quantity qty_ticks = math::qty_to_internal(new_qty);
        if (qty_ticks <= 0) {
            throw std::runtime_error("Invalid qty: " + std::to_string(qty_ticks));
            return false;
        }
        auto& engine_info = engines_info_[engine_id];
        auto job = scheduler::make_job([engine_id, order_id, qty_ticks, user_id, runtime_ptr = this]() {
            if (engine_id >= runtime_ptr->engines_info_.size()) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Engine not found");
                return;
            }
            const engine::OrderInfo* order = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_order(order_id);
            if (order == nullptr) {
                if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
                return;
            }
            if (order->side_ == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
                if (engine_id < runtime_ptr->user_orders_.size() && user_id < runtime_ptr->user_orders_[engine_id].by_user.size()) {
                    engine::Quantity total_owned = 0;
                    for (engine::OrderId oid : runtime_ptr->user_orders_[engine_id].by_user[user_id]) {
                        auto o = runtime_ptr->engines_info_[engine_id].worker_.engine_->get_order(oid);
                        if (o != nullptr && o->side_ == engine::OrderSide::ASK && o->status_ == engine::OrderStatus::OPEN)
                            total_owned += o->qty_;
                    }
                    if (total_owned < qty_ticks) {
                        if (runtime_ptr->verbose_) runtime_ptr->notify("[EDIT ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                        return;
                    }
                }
            }
            std::vector<engine::EngineMsg> msgs;
            bool result = runtime_ptr->engines_info_[engine_id].worker_.engine_->edit_order(order_id, qty_ticks, msgs);
            if (result && user_id != backtest::user::INVALID_USER_ID && user_id > 0 && user_id <= runtime_ptr->users_.size()) {
                double old_qty = backtest::math::internal_to_qty(order->qty_);
                double price = backtest::math::ticks_to_dollars(order->price_);
                runtime_ptr->sync_order_api_.release_reservation_for_user(&runtime_ptr->users_[user_id - 1], order_id, order->side_, old_qty, price);
                double qty = backtest::math::internal_to_qty(qty_ticks);
                runtime_ptr->sync_order_api_.reserve_on_accept_to_user(&runtime_ptr->users_[user_id - 1], order_id, order->side_, qty, price);
            }
            for (const auto& msg : msgs) {
                if (runtime_ptr->verbose_ && msg.kind == engine::EventKind::MODIFY)
                    runtime_ptr->notify("[EDIT ORDER] Order " + std::to_string(order_id) + " qty updated");
            }
            if (runtime_ptr->get_quantum() != 0) runtime_ptr->increment_order_counter(engine_id);
        }, engine_id);
        submit_job_on_worker(engine_info.submit_.worker_id_, std::move(job));
        return true;
    } catch (const std::exception& e) {
        if (verbose_) notify("[EDIT ORDER] EXCEPTION: " + std::string(e.what()));
        return false;
    } catch (...) {
        if (verbose_) notify("[EDIT ORDER] EXCEPTION: Unknown error");
        return false;
    }
}

bool backtest::runtime::EngineRuntime::submit_replace_order_sync_impl(const std::string& _ticker, engine::OrderId order_id, double new_price, double new_qty, backtest::user::UserId user_id)
{
    try {
        // Verify ticker exists and get engine_id
        auto ticker_it = ticker_to_engine_id_.find(_ticker);
            if (ticker_it == ticker_to_engine_id_.end()) {
            if (verbose_) notify("[REPLACE ORDER] ERROR: Ticker not found: " + _ticker);
            return false;
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size()) {
            if (verbose_) notify("[REPLACE ORDER] ERROR: Engine not found");
            return false;
        }
        
        // Convert user-facing values to internal format
        engine::Price price_ticks = math::dollars_to_ticks(new_price);
        engine::Quantity qty_ticks = math::qty_to_internal(new_qty);
        if (price_ticks <= 0 || qty_ticks <= 0) {
            if (verbose_) notify("[REPLACE ORDER] ERROR: Invalid price/qty");
            return false;
        }
        
        const engine::OrderInfo* order = engines_info_[engine_id].worker_.engine_->get_order(order_id);
        if (order == nullptr) {
            if (verbose_) notify("[REPLACE ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
            return false;
        }
        
        // VALIDATE OWNERSHIP BEFORE EDITING (only for registered users)
        if (order->side_ == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
            if (engine_id < user_orders_.size() && user_id < user_orders_[engine_id].by_user.size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId owned_order_id : user_orders_[engine_id].by_user[user_id]) {
                    auto owned_order = engines_info_[engine_id].worker_.engine_->get_order(owned_order_id);
                    if (owned_order != nullptr && owned_order->side_ == engine::OrderSide::ASK && 
                        owned_order->status_ == engine::OrderStatus::OPEN) {
                        total_owned += owned_order->qty_;
                    }
                }
                if (total_owned < qty_ticks) {
                    if (verbose_) notify("[REPLACE ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                    return false;
                }
            }
        }
        double old_qty = backtest::math::internal_to_qty(order->qty_);
        double old_price = backtest::math::ticks_to_dollars(order->price_);
        // Direct engine call
        std::vector<engine::EngineMsg> msgs;
        bool result = engines_info_[engine_id].worker_.engine_->replace_order(order_id, order->side_, price_ticks, qty_ticks, msgs);
        
        for (const auto& msg : msgs) {
            if (verbose_) {
                switch (msg.kind) {
                    case engine::EventKind::ACCEPT:
                        notify("[REPLACE ORDER] Order " + std::to_string(order_id) + " replaced");
                        break;
                    case engine::EventKind::FILL:
                        notify("[REPLACE ORDER] Order " + std::to_string(order_id) + " filled");
                        break;
                    case engine::EventKind::REJECT:
                        notify("[REPLACE ORDER] Order " + std::to_string(order_id) + " replace rejected");
                        break;
                    default:
                        break;
                }
            }
            
            // ACCEPT EVENT: release old reservation, then reserve new
            if (msg.kind == engine::EventKind::ACCEPT) {
                if (user_id != backtest::user::INVALID_USER_ID && result) {
                    try {
                        if (user_id != backtest::user::IPO_HOLDER && engine_id < engines_info_.size() && !engines_info_[engine_id].submit_.ticker_.empty()) {
                            sync_order_api_.release_reservation_for_user(&users_[user_id - 1], order_id, order->side_, old_qty, old_price);
                            double qty = backtest::math::internal_to_qty(qty_ticks);
                            double price = backtest::math::ticks_to_dollars(price_ticks);
                            sync_order_api_.reserve_on_accept_to_user(&users_[user_id - 1], order_id, order->side_, qty, price);
                        }
                    } catch (...) { }
                }
            }
            
            // FILL EVENT: update position and remove from set
            if ((msg.kind == engine::EventKind::FILL || msg.kind == engine::EventKind::PARTIAL_FILL) &&
                msg.qty > 0 && msg.price != static_cast<engine::Price>(-1)) {
                try {
                    double qty = backtest::math::internal_to_qty(msg.qty);
                    double price = backtest::math::ticks_to_dollars(msg.price);
                    auto& o2u = engines_info_[engine_id].worker_.order_to_user_;
                    auto oit = o2u.find(msg.order_id);
                    if (oit != o2u.end()) {
                        backtest::user::UserId uid = oit->second;
                        if (uid > 0 && engine_id < user_orders_.size() && uid < user_orders_[engine_id].by_user.size()) {
                            auto& s = user_orders_[engine_id].by_user[uid];
                            if (s.find(msg.order_id) != s.end()) {
                                sync_order_api_.apply_fill_to_user(&users_[uid - 1], msg.order_id, msg.side, qty, price);
                                if (msg.kind == engine::EventKind::FILL) {
                                    s.erase(msg.order_id);
                                    o2u.erase(msg.order_id);
                                }
                            }
                        }
                    }
                } catch (...) { }
            }
        }
        
        if (!result) {
            if (verbose_) notify("[REPLACE ORDER] ERROR: Failed to replace order " + std::to_string(order_id));
            return false;
        }
        
        // Increment order counter for quantum tracking (skip when batch mode)
        if (get_quantum() != 0) increment_order_counter(engine_id);
        
        return true;
    } catch (const std::exception& e) {
        if (verbose_) notify("[REPLACE ORDER] EXCEPTION: " + std::string(e.what()));
        return false;
    } catch (...) {
        if (verbose_) notify("[REPLACE ORDER] EXCEPTION: Unknown error");
        return false;
    }
}

bool backtest::runtime::EngineRuntime::submit_edit_order_sync_impl(const std::string& _ticker, engine::OrderId order_id, double new_qty, backtest::user::UserId user_id)
{
    try {
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
        engine::Quantity qty_ticks = math::qty_to_internal(new_qty);
        if (qty_ticks <= 0) {
            if (verbose_) notify("[EDIT ORDER] ERROR: Invalid qty");
            return false;
        }
        const engine::OrderInfo* order = engines_info_[engine_id].worker_.engine_->get_order(order_id);
        if (order == nullptr) {
            if (verbose_) notify("[EDIT ORDER] ERROR: Order " + std::to_string(order_id) + " not found");
            return false;
        }
        if (order->side_ == engine::OrderSide::ASK && user_id != backtest::user::INVALID_USER_ID) {
            if (engine_id < user_orders_.size() && user_id < user_orders_[engine_id].by_user.size()) {
                engine::Quantity total_owned = 0;
                for (engine::OrderId oid : user_orders_[engine_id].by_user[user_id]) {
                    auto o = engines_info_[engine_id].worker_.engine_->get_order(oid);
                    if (o != nullptr && o->side_ == engine::OrderSide::ASK && o->status_ == engine::OrderStatus::OPEN)
                        total_owned += o->qty_;
                }
                if (total_owned < qty_ticks) {
                    if (verbose_) notify("[EDIT ORDER] ERROR: Insufficient shares for user " + std::to_string(user_id));
                    return false;
                }
            }
        }
        std::vector<engine::EngineMsg> msgs;
        bool result = engines_info_[engine_id].worker_.engine_->edit_order(order_id, qty_ticks, msgs);
        if (result && user_id != backtest::user::INVALID_USER_ID && user_id > 0 && user_id <= users_.size()) {
            double old_qty = math::internal_to_qty(order->qty_);
            double price = math::ticks_to_dollars(order->price_);
            sync_order_api_.release_reservation_for_user(&users_[user_id - 1], order_id, order->side_, old_qty, price);
            double qty = math::internal_to_qty(qty_ticks);
            sync_order_api_.reserve_on_accept_to_user(&users_[user_id - 1], order_id, order->side_, qty, price);
        }
        if (verbose_ && result) notify("[EDIT ORDER] Order " + std::to_string(order_id) + " qty updated");
        if (get_quantum() != 0) increment_order_counter(engine_id);
        return result;
    } catch (const std::exception& e) {
        if (verbose_) notify("[EDIT ORDER] EXCEPTION: " + std::string(e.what()));
        return false;
    } catch (...) {
        if (verbose_) notify("[EDIT ORDER] EXCEPTION: Unknown error");
        return false;
    }
}

// === SYNCHRONOUS READ OPERATIONS ===

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
        
        return engines_info_[engine_id].worker_.engine_->get_order(order_id);
    } catch (...) {
        return nullptr;
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
        
        return engines_info_[engine_id].worker_.engine_.get();
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
            runtime_ptr->engines_info_[engine_id].worker_.engine_->set_auto_match(auto_match);
        }, engine_id);

        submit_job_on_worker(engine_info.submit_.worker_id_, std::move(job));
        return true;
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
        scheduler_.process_jobs_on(engines_info_[engine_id].submit_.worker_id_);
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
        scheduler_.process_jobs_on_async(engines_info_[engine_id].submit_.worker_id_);
    } catch (...) {
        // Silent failure for background processing
    }
}

bool backtest::runtime::EngineRuntime::simulate
(
    const std::string& filepath,
    const std::string& ticker,
    std::size_t target_orders,
    std::size_t price_sample_size,
    double shares_outstanding,
    const std::string& record_path
)
{   
    std::unique_ptr<stream::L2Stream> parser; // L2 stream for replay
    double initial_price = 100.0;  // IPO Price

    // Init Parser
    try 
    {
        parser = std::make_unique<stream::L2Stream>(std::move(filepath));
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
        if (verbose_) notify("[SIMULATE] ERROR: " + std::string(e.what()));
        return false; // Parser Failed
    }
    
    // Register Stock, Toggle Record, and Init Metrics (pass copy of ticker so we keep it for engine_id lookup below)
    if(!register_stock(std::string(ticker), initial_price, shares_outstanding)) return false;
    if (!record_path.empty()) set_record(std::string(ticker), true, std::move(record_path));
    auto engine_id = ticker_to_engine_id_[ticker]; // Engine Id
    auto& engine_info = engines_info_[engine_id]; // Engine Info
    engine_info.get_write_metrics().reset(); // Info Sim Metrics

    // Set and Publish Initial Price (both buffers so worker-published metrics keep it)
    engine_info.get_write_metrics().initial_price = initial_price;
    engine_info.get_write_metrics().simulation_running = true;
    engine_info.publish_metrics();
    engine_info.get_write_metrics().initial_price = initial_price;
    engine_info.get_write_metrics().simulation_running = true;
    
    // Create simulation job
    auto simulation_job = scheduler::make_job([this, parser = std::move(parser), engine_id, target_orders]() mutable {
        
        // Fail if Engine Id does not exist
        if (engine_id >= engines_info_.size()) return;
        
        auto& engine_info = engines_info_[engine_id]; // Capture engine info
        auto* engine = engine_info.worker_.engine_.get(); // Capture engine
        
        // Fail if engine is nullptr
        if (!engine) return;
    
        // Start timing
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Match from the start (default): engine keeps auto_match true, so each place runs matching immediately (no queue/drain).
        // Use this symbol's engine capacity for batch sizing (metrics update interval only)
        std::size_t engine_capacity = engine_info.submit_.capacity_;
        std::size_t batch_size = runtime_batch_size_.load(std::memory_order_relaxed);
        if (batch_size == 0 || batch_size > engine_capacity)
            batch_size = (engine_capacity > 0) ? (engine_capacity / 16) : 50000;

        // Setup simulation variables - reuse moved parser (delta-based: increase = place at back, decrease = remove from back)
        std::unordered_map<uint64_t, double> last_amount_cache;         // last amount applied per level; skip no-op and compute delta
        std::size_t updates = 0;
        
        // Parser is already created and moved into lambda - no need to recreate
        last_amount_cache.reserve(1 << 16);
        
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
                engine::Price price_ticks = math::dollars_to_ticks(update.price);
                const double new_amount = update.amount;

                double last_amount = 0.0;
                auto ait = last_amount_cache.find(key);
                if (ait != last_amount_cache.end()) last_amount = ait->second;
                const double delta = new_amount - last_amount;
                if (std::fabs(delta) < 1e-12) continue;  // no change, skip

                if (new_amount <= 0.0) {
                    // Level to zero: cancel all orders at this (side, price) from the back
                    for (;;) {
                        engine::OrderId id = engine->get_back_order_at_level(side, price_ticks);
                        if (id == engine::INVALID_ORDER_ID) break;
                        engine->cancel_order(id);
                    }
                    last_amount_cache.erase(key);
                } else if (delta > 0.0) {
                    // Increase or new level: place new size at back (new level = place full amount; increase = place delta only)
                    const double place_amount = (ait == last_amount_cache.end()) ? new_amount : delta;
                    const engine::Quantity place_qty_ticks = math::qty_to_internal(place_amount);
                    engine::OrderId order_id = engine::INVALID_ORDER_ID;
                    if (users_.empty()) {
                        order_id = engine->place_order(side, engine::OrderType::LIMIT, price_ticks, place_qty_ticks);
                    } else {
                        msgs.clear();
                        std::function<bool(engine::OrderId)> fill_filter_fn = [this, engine_id](engine::OrderId fid) { return engines_info_[engine_id].worker_.order_to_user_.find(fid) != engines_info_[engine_id].worker_.order_to_user_.end(); };
                        order_id = engine->place_order(side, engine::OrderType::LIMIT, price_ticks, place_qty_ticks, msgs, false, &fill_filter_fn);
                        for (const auto& msg : msgs) {
                            this->notify_order_event("[LIMIT ORDER]", order_id, msg.kind);
                            if (order_id != engine::INVALID_ORDER_ID) {
                                switch (msg.kind) {
                                    case engine::EventKind::FILL:
                                    case engine::EventKind::PARTIAL_FILL:
                                        this->handle_fill_event(msg, engine_id);
                                        break;
                                    default:
                                        break;
                                }
                            }
                        }
                    }
                    if (order_id != engine::INVALID_ORDER_ID && this->get_quantum() != 0) this->increment_order_counter(engine_id);
                    last_amount_cache[key] = new_amount;
                } 
                else {
                    // Decrease: remove from back (conservative / cancellation semantics)
                    double to_remove = last_amount - new_amount;
                    engine::Quantity to_remove_ticks = math::qty_to_internal(to_remove);
                    while (to_remove_ticks > 0) {
                        engine::OrderId id = engine->get_back_order_at_level(side, price_ticks);
                        if (id == engine::INVALID_ORDER_ID) break;
                        const engine::OrderInfo* o = engine->get_order(id);
                        if (!o) break;
                        engine::Quantity qty = o->qty_;
                        if (qty <= to_remove_ticks) {
                            engine->cancel_order(id);
                            to_remove_ticks -= qty;
                        } else {
                            engine->edit_order(id, qty - to_remove_ticks);
                            to_remove_ticks = 0;
                        }
                    }
                    last_amount_cache[key] = new_amount;
                }
            }

            // Process at batch intervals (metrics)
            if (batch_size > 0 && updates % batch_size == 0) 
            {
                // Capture snapshot for compatible metrics design
                engine->update_snapshot();
                const engine::MarketSnapshot& snapshot = engine->get_snapshot();
                
                // Update metrics only during batch processing for efficiency
                auto& metrics = engine_info.get_write_metrics();
                metrics.market_updates_processed = updates;
                metrics.orders_placed = snapshot.placed_count;
                metrics.orders_filled = snapshot.filled_count;
                metrics.orders_cancelled = snapshot.cancelled_count;
                metrics.orders_edited = snapshot.edited_count;
                metrics.orders_replaced = snapshot.replaced_count;
                metrics.final_open_orders = snapshot.open_count;
                metrics.peak_open_orders = snapshot.open_count > metrics.peak_open_orders ? 
                snapshot.open_count : metrics.peak_open_orders;
                metrics.cache_entries = last_amount_cache.size();
                
                // Publish metrics for readers
                engine_info.publish_metrics();
            }
            
            // Check target limit (only if target > 0)
            if (target_orders > 0 && updates >= target_orders) break;
        }

        // Update snapshot for final metrics capture
        engine->update_snapshot(); 
        const engine::MarketSnapshot& final_snapshot = engine->get_snapshot();
        
        // Final metrics update with snapshot-based values for compatibility
        auto& final_metrics = engine_info.get_write_metrics();
        final_metrics.market_updates_processed = updates;  // Ensure final count is written
        final_metrics.orders_placed = final_snapshot.placed_count;
        final_metrics.orders_filled = final_snapshot.filled_count;
        final_metrics.orders_cancelled = final_snapshot.cancelled_count;
        final_metrics.final_open_orders = final_snapshot.open_count;
        final_metrics.peak_open_orders = final_snapshot.open_count > final_metrics.peak_open_orders ?  
        final_snapshot.open_count : final_metrics.peak_open_orders;
        final_metrics.cache_entries = last_amount_cache.size();
        
        auto end_time = std::chrono::high_resolution_clock::now();
        double seconds = std::chrono::duration<double>(end_time - start_time).count();

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
    submit_job_on_worker(engine_info.submit_.worker_id_, std::move(simulation_job));
    
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


std::size_t backtest::runtime::EngineRuntime::get_capacity(const std::string& ticker) const {
    try {
        auto ticker_it = ticker_to_engine_id_.find(ticker);
        if (ticker_it == ticker_to_engine_id_.end()) {
            throw std::invalid_argument("Ticker not found: " + ticker);
        }
        
        EngineId engine_id = ticker_it->second;
        if (engine_id >= engines_info_.size() || !engines_info_[engine_id].worker_.engine_) {
            throw std::runtime_error("Engine not available for ticker: " + ticker);
        }
        
        // Return the per-engine capacity stored in OrderEngineInfo (set at register_stock)
        return engines_info_[engine_id].submit_.capacity_;
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
        
        scheduler::WorkerId worker_id = engines_info_[engine_id].submit_.worker_id_;
        return scheduler_.pending_jobs_on(worker_id);
    } catch (...) {
        return 0;
    }
}

std::size_t backtest::runtime::EngineRuntime::get_placed_count(const std::string& ticker) const {
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return 0;
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap) return 0;
        return snap->placed_count;
    } catch (...) { return 0; }
}

std::size_t backtest::runtime::EngineRuntime::get_filled_count(const std::string& ticker) const {
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return 0;
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap) return 0;
        return snap->filled_count;
    } catch (...) { return 0; }
}

std::size_t backtest::runtime::EngineRuntime::get_cancelled_count(const std::string& ticker) const {
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return 0;
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap) return 0;
        return snap->cancelled_count;
    } catch (...) { return 0; }
}

std::size_t backtest::runtime::EngineRuntime::get_open_count(const std::string& ticker) const {
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return 0;
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap) return 0;
        return snap->open_count;
    } catch (...) { return 0; }
}

backtest::user::UserView* backtest::runtime::EngineRuntime::register_strategy(const std::string& ticker, backtest::user::Strategy strategy, double starting_capital)
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
        users_.emplace_back(std::move(strategy), this, user_id, starting_capital, &sync_order_api_, engine_id);
        user_strategy_engine_id_.push_back(engine_id);
    }
    else
    {
        // Fill the hole: construct User in place at users_[idx]
        if (idx >= user_strategy_engine_id_.size())
        {
            if (verbose_) notify("[RUNTIME] ERROR: Cannot register strategy — capacity reached (max " + std::to_string(max_strategies_) + " strategies)");
            return nullptr;
        }
        users_[idx] = backtest::user::User(std::move(strategy), this, user_id, starting_capital, &sync_order_api_, engine_id);
        user_strategy_engine_id_[idx] = engine_id;
    }

    engines_info_[engine_id].user_indices_.push_back(idx);

    if (verbose_)
    {
        notify("[RUNTIME] Registered strategy for user " + std::to_string(user_id) +
                " on " + ticker + " with capital $" + std::to_string(starting_capital));
    }

    // Engine-first: ensure each engine's by_user has space for this user_id (reserve once to avoid resize realloc)
    if (user_orders_.size() < engines_info_.size())
        user_orders_.resize(engines_info_.size());
    for (EngineId eid = 0; eid < user_orders_.size(); ++eid) {
        if (user_orders_[eid].by_user.size() <= user_id) {
            user_orders_[eid].by_user.reserve(max_strategies_ + 1);
            user_orders_[eid].by_user.resize(user_id + 1);
        }
        user_orders_[eid].by_user[user_id].reserve(USER_ORDERS_PER_USER_BASE_CAPACITY);
    }

    return static_cast<backtest::user::UserView*>(&users_[idx]);
}

bool backtest::runtime::EngineRuntime::unregister_strategy(backtest::user::UserId user_id)
{
    if (user_id < 1 || user_id > users_.size())
        return false;
    const std::size_t idx = user_id - 1;
    if (users_[idx].get_user_id() == backtest::user::INVALID_USER_ID)
        return false; // already unregistered

    EngineId strategy_engine_id = (idx < user_strategy_engine_id_.size()) ? user_strategy_engine_id_[idx] : 0;
    if (strategy_engine_id < engines_info_.size())
    {
        std::vector<std::size_t>& per_engine = engines_info_[strategy_engine_id].user_indices_;
        auto it = std::find(per_engine.begin(), per_engine.end(), idx);
        if (it != per_engine.end())
        {
            *it = per_engine.back();
            per_engine.pop_back();
        }
    }

    // Remove all of this user's orders (engine-first)
    for (EngineId engine_id = 0; engine_id < user_orders_.size(); ++engine_id)
    {
        if (user_id < user_orders_[engine_id].by_user.size())
        {
            for (engine::OrderId order_id : user_orders_[engine_id].by_user[user_id])
                engines_info_[engine_id].worker_.order_to_user_.erase(order_id);
            user_orders_[engine_id].by_user[user_id].clear();
        }
    }

    // Invalidate the user slot (same address, so existing User* becomes a dead/invalid user)
    users_[idx] = backtest::user::User();

    if (verbose_)
        notify("[RUNTIME] Unregistered strategy for user " + std::to_string(user_id));

    return true;
}

std::vector<engine::OrderId> backtest::runtime::EngineRuntime::get_positions(backtest::user::UserId user_id, EngineId engine_id) const
{
    try {
        if (engine_id >= user_orders_.size())
            return {};
        if (user_id >= user_orders_[engine_id].by_user.size())
            return {};
        std::vector<engine::OrderId> positions;
        const auto& order_set = user_orders_[engine_id].by_user[user_id];
        positions.reserve(order_set.size());
        for (engine::OrderId order_id : order_set)
            positions.push_back(order_id);
        return positions;
    } catch (...) {
        return {};
    }
}

std::vector<engine::OrderId> backtest::runtime::EngineRuntime::get_positions(backtest::user::UserId user_id, const std::string& ticker) const
{
    auto ticker_it = ticker_to_engine_id_.find(ticker);
    if (ticker_it == ticker_to_engine_id_.end())
        return {};
    return get_positions(user_id, ticker_it->second);
}

std::vector<engine::OrderId> backtest::runtime::EngineRuntime::get_active_orders(backtest::user::UserId user_id, EngineId engine_id) const
{
    try {
        if (engine_id >= user_orders_.size())
            return {};
        if (user_id >= user_orders_[engine_id].by_user.size())
            return {};
        std::vector<engine::OrderId> active_orders;
        const auto& order_set = user_orders_[engine_id].by_user[user_id];
        active_orders.reserve(order_set.size());
        for (engine::OrderId order_id : order_set)
            active_orders.push_back(order_id);
        return active_orders;
    } catch (...) {
        return {};
    }
}

std::vector<engine::OrderId> backtest::runtime::EngineRuntime::get_active_orders(backtest::user::UserId user_id, const std::string& ticker) const
{
    auto ticker_it = ticker_to_engine_id_.find(ticker);
    if (ticker_it == ticker_to_engine_id_.end())
        return {};
    return get_active_orders(user_id, ticker_it->second);
}

bool backtest::runtime::EngineRuntime::user_has_sufficient_shares(backtest::user::UserId user_id, const std::string& ticker, engine::Quantity qty) const
{
    try {
        const std::size_t user_idx = static_cast<std::size_t>(user_id) - 1;
        if (user_id == user::INVALID_USER_ID || user_idx >= users_.size()) return false;

        const user::User& u = users_[user_idx];
        double net_position = u.get_position();
        double already_committed = u.get_committed_sell_qty();
        double available = net_position - already_committed;
        return available >= math::internal_to_qty(qty);
    } catch (...) {
        return false;
    }
}

backtest::runtime::EngineRuntime::EngineRuntime(std::size_t num_threads, bool _verbose, std::size_t quantum_orders, std::size_t max_capacity, std::size_t max_engine_count, std::size_t max_strategies)
    : scheduler_(num_threads),
    num_workers_(num_threads),
    max_capacity_(max_capacity),
    verbose_(_verbose),
    quantum_orders_(quantum_orders),
    max_engine_count_(max_engine_count),
    max_strategies_(max_strategies),
    log_buffer_(LOG_BUFFER_CAPACITY),
    record_buffer_(RECORD_BUFFER_CAPACITY),
    sync_order_api_(this)
{
    // Initialize runtime batch size to scheduler's batch capacity by default
    runtime_batch_size_.store(scheduler_.get_batch_capacity(), std::memory_order_relaxed);
    engines_info_.reserve(max_engine_count);
    users_.reserve(max_strategies);  // Index 0 (IPO HOLDER) is reserved; keeps UserView* from register_strategy valid
    user_strategy_engine_id_.reserve(max_strategies);
    user_orders_.reserve(max_engine_count);

    if (verbose_)
        std::cout << "[RUNTIME] Starting EngineRuntime with " << num_threads
                << " workers, capacity " << max_capacity << std::endl;

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
            const std::string& ticker = engines_info_[eid].submit_.ticker_;
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

void backtest::runtime::EngineRuntime::notify(const std::string& message) noexcept
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

void backtest::runtime::EngineRuntime::set_record(const std::string& ticker, bool enable) noexcept
{
    auto it = ticker_to_engine_id_.find(ticker);
    if (it == ticker_to_engine_id_.end()) return;
    EngineId eid = it->second;
    if (eid >= record_enabled_.size() || !record_enabled_[eid]) return;
    record_enabled_[eid]->store(enable, std::memory_order_relaxed);
}

void backtest::runtime::EngineRuntime::set_record(const std::string& ticker, bool enable, const std::string& path_override) noexcept
{
    auto it = ticker_to_engine_id_.find(ticker);
    if (it == ticker_to_engine_id_.end()) return;
    EngineId eid = it->second;
    if (eid >= record_enabled_.size() || !record_enabled_[eid]) return;
    record_enabled_[eid]->store(enable, std::memory_order_relaxed);
    if (eid < record_path_override_.size()) 
    {
        record_path_override_[eid] = enable ? std::move(path_override) : std::string();
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
        auto& w = engines_info_[engine_id].worker_;
        w.engine_->update_snapshot();
        w.snapshot_ptr_ = &w.engine_->get_snapshot();
    }
}

void backtest::runtime::EngineRuntime::refresh_user_snapshots_for_engine(EngineId engine_id)
{
    if (engine_id >= engines_info_.size()) return;
    for (std::size_t idx : engines_info_[engine_id].user_indices_) {
        if (idx < users_.size())
            users_[idx].update_snapshot();
    }
}

const engine::MarketSnapshot* backtest::runtime::EngineRuntime::get_snapshot_fast(EngineId engine_id) const noexcept
{
    if (engine_id < engines_info_.size()) {
        const auto* p = engines_info_[engine_id].worker_.snapshot_ptr_;
        if (p) return p;
    }
    return nullptr;
}

const engine::MarketSnapshot* backtest::runtime::EngineRuntime::get_snapshot(const std::string& ticker) const
{
    auto it = ticker_to_engine_id_.find(ticker);
    if (it == ticker_to_engine_id_.end())
        return nullptr;
    return get_snapshot_fast(it->second);
}

const engine::MarketSnapshot* backtest::runtime::EngineRuntime::get_snapshot(EngineId engine_id) const
{
    return get_snapshot_fast(engine_id);
}

const std::string& backtest::runtime::EngineRuntime::get_ticker(EngineId engine_id) const
{
    static const std::string empty;
    if (engine_id >= engines_info_.size())
        return empty;
    return engines_info_[engine_id].submit_.ticker_;
}

const engine::OrderInfo* backtest::runtime::EngineRuntime::get_order(EngineId engine_id, engine::OrderId order_id) const
{
    if (engine_id >= engines_info_.size())
        return nullptr;
    return engines_info_[engine_id].worker_.engine_->get_order(order_id);
}

bool backtest::runtime::EngineRuntime::request_snapshot(const std::string& ticker)
{
    auto it = ticker_to_engine_id_.find(ticker);
    if (it == ticker_to_engine_id_.end()) return false;
    EngineId engine_id = it->second;
    if (engine_id >= engines_info_.size()) return false;
    scheduler::WorkerId worker_id = engines_info_[engine_id].submit_.worker_id_;
    auto job = scheduler::make_job([rt = this, engine_id]() {
        rt->update_snapshot_internal(engine_id);
        rt->refresh_user_snapshots_for_engine(engine_id);
    }, engine_id);
    const_cast<EngineRuntime*>(this)->submit_job_on_worker(worker_id, std::move(job));
    return true;
}

double backtest::runtime::EngineRuntime::get_market_price(const std::string& ticker) const
{
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return -1.0;
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap) return -1.0;
        if (snap->best_bid != static_cast<engine::Price>(-1) && snap->best_ask != static_cast<engine::Price>(-1))
            return (math::ticks_to_dollars(snap->best_bid) + math::ticks_to_dollars(snap->best_ask)) / 2.0;
        if (snap->best_ask != static_cast<engine::Price>(-1)) return math::ticks_to_dollars(snap->best_ask);
        if (snap->best_bid != static_cast<engine::Price>(-1)) return math::ticks_to_dollars(snap->best_bid);
        if (snap->market_price != static_cast<engine::Price>(-1)) return math::ticks_to_dollars(snap->market_price);
        return -1.0;
    } catch (...) { return -1.0; }
}

double backtest::runtime::EngineRuntime::get_best_bid(const std::string& ticker) const
{
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return -1.0;
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap || snap->best_bid == static_cast<engine::Price>(-1)) return -1.0;
        return math::ticks_to_dollars(snap->best_bid);
    } catch (...) { return -1.0; }
}

double backtest::runtime::EngineRuntime::get_best_ask(const std::string& ticker) const
{
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return -1.0;
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap || snap->best_ask == static_cast<engine::Price>(-1)) return -1.0;
        return math::ticks_to_dollars(snap->best_ask);
    } catch (...) { return -1.0; }
}

std::vector<std::pair<double,double>> backtest::runtime::EngineRuntime::get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth) const
{
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return {};
        const engine::MarketSnapshot* snap = get_snapshot_fast(it->second);
        if (!snap) return {};
        std::vector<std::pair<double,double>> out;
        if (side == engine::OrderSide::BID) {
            std::size_t n = std::min(static_cast<std::size_t>(snap->bid_levels), depth);
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
                out.emplace_back(math::ticks_to_dollars(snap->bid_prices[i]), math::internal_to_qty(snap->bid_depth[i]));
            // Corrected Sort for Bids (Decreasing by Price)
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                return a.first > b.first; 
            });
        } else {
            std::size_t n = std::min(static_cast<std::size_t>(snap->ask_levels), depth);
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
                out.emplace_back(math::ticks_to_dollars(snap->ask_prices[i]), math::internal_to_qty(snap->ask_depth[i]));
            // Corrected Sort for Asks (Increasing by Price)
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                return a.first < b.first; 
            });
        }
        return out;
    } catch (...) { return {}; }
}

bool backtest::runtime::EngineRuntime::get_auto_match(const std::string& ticker) const
{
    try {
        auto it = ticker_to_engine_id_.find(ticker);
        if (it == ticker_to_engine_id_.end()) return false;
        EngineId engine_id = it->second;
        if (engine_id >= engines_info_.size()) return false;
        const engine::MarketSnapshot* snap = get_snapshot_fast(engine_id);
        if (!snap) return false;
        return snap->auto_match;
    } catch (...) { return false; }
}

void backtest::runtime::EngineRuntime::increment_order_counter(EngineId engine_id) noexcept
{
    if (engine_id >= engines_info_.size()) return;
    auto& info = engines_info_[engine_id];
    if (++info.worker_.orders_since_quantum_ >= quantum_orders_) {
        info.worker_.orders_since_quantum_ = 0;
        update_snapshot_internal(engine_id);
        if (engine_id < record_enabled_.size() && record_enabled_[engine_id]->load(std::memory_order_relaxed))
            record_book_snapshot(engine_id);
        if (engine_id < engines_info_.size()) {
            for (std::size_t idx : engines_info_[engine_id].user_indices_) {
                if (idx < users_.size()) {
                    users_[idx].on_book_update();
                    users_[idx].update_snapshot();
                }
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
    // Callers only invoke when order_id and user_id are valid.

    // Engine-first: grow by engine then by user on demand
    if (engine_id >= user_orders_.size())
        user_orders_.resize(engine_id + 1);
    if (user_id >= user_orders_[engine_id].by_user.size())
        user_orders_[engine_id].by_user.resize(user_id + 1);

    user_orders_[engine_id].by_user[user_id].insert(order_id);
    engines_info_[engine_id].worker_.order_to_user_[order_id] = user_id;
}

void backtest::runtime::EngineRuntime::handle_accept_event(engine::OrderId order_id, user::UserId user_id, EngineId engine_id, 
                                                           engine::OrderSide side, engine::Quantity qty_ticks, engine::Price price_ticks) noexcept
{
    try {
        if (user_id != user::IPO_HOLDER && engine_id < engines_info_.size() && !engines_info_[engine_id].submit_.ticker_.empty()) {
            double qty = math::internal_to_qty(qty_ticks);
            double price = math::ticks_to_dollars(price_ticks);
            sync_order_api_.reserve_on_accept_to_user(&users_[user_id - 1], order_id, side, qty, price);
        }
    } catch (...) { }
}

void backtest::runtime::EngineRuntime::handle_fill_event(const engine::EngineMsg& msg, EngineId engine_id) noexcept
{
    // Fast-path: only FILL/PARTIAL_FILL with valid payload matter for attribution
    if (msg.kind != engine::EventKind::FILL && msg.kind != engine::EventKind::PARTIAL_FILL) return;
    if (msg.qty == 0 || msg.price == static_cast<engine::Price>(-1)) return;

    auto& o2u = engines_info_[engine_id].worker_.order_to_user_;
    auto oit = o2u.find(msg.order_id);
    if (oit == o2u.end()) return;

    user::UserId uid = oit->second;

    if (uid > 0 && engine_id < user_orders_.size() && uid < user_orders_[engine_id].by_user.size() && user_orders_[engine_id].by_user[uid].count(msg.order_id)) {
        try {
            double qty = math::internal_to_qty(msg.qty);
            double price = math::ticks_to_dollars(msg.price);
            sync_order_api_.apply_fill_to_user(&users_[uid - 1], msg.order_id, msg.side, qty, price);
            if (msg.kind == engine::EventKind::FILL) {
                user_orders_[engine_id].by_user[uid].erase(msg.order_id);
                o2u.erase(msg.order_id);
            }
        } catch (...) { }
    }
}



// Non-inline User member implementations (order_slot_idx is in engine_runtime.h)
backtest::user::User::User(User&& other) noexcept
    : strategy_(std::move(other.strategy_))
    , runtime_(other.runtime_)
    , user_id_(other.user_id_)
    , sync_order_api_(other.sync_order_api_)
    , capital_(other.capital_)
    , realized_pnl_(other.realized_pnl_)
    , total_volume_(other.total_volume_)
    , strategy_engine_id_(other.strategy_engine_id_)
    , position_(other.position_)
    , avg_price_(other.avg_price_)
{
    snapshots_[0] = std::move(other.snapshots_[0]);
    snapshots_[1] = std::move(other.snapshots_[1]);
    active_snapshot_index_.store(other.active_snapshot_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    const UserSnapshot* p = other.published_snapshot_ptr_.load(std::memory_order_relaxed);
    const std::size_t idx = (p == &other.snapshots_[1]) ? 1u : 0u;
    published_snapshot_ptr_.store(&snapshots_[idx], std::memory_order_relaxed);
}

backtest::user::User& backtest::user::User::operator=(User&& other) noexcept
{
    if (this == &other) return *this;
    strategy_ = std::move(other.strategy_);
    runtime_ = other.runtime_;
    user_id_ = other.user_id_;
    sync_order_api_ = other.sync_order_api_;
    capital_ = other.capital_;
    realized_pnl_ = other.realized_pnl_;
    total_volume_ = other.total_volume_;
    strategy_engine_id_ = other.strategy_engine_id_;
    position_ = other.position_;
    avg_price_ = other.avg_price_;
    snapshots_[0] = std::move(other.snapshots_[0]);
    snapshots_[1] = std::move(other.snapshots_[1]);
    active_snapshot_index_.store(other.active_snapshot_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    const UserSnapshot* p = other.published_snapshot_ptr_.load(std::memory_order_relaxed);
    const std::size_t idx = (p == &other.snapshots_[1]) ? 1u : 0u;
    published_snapshot_ptr_.store(&snapshots_[idx], std::memory_order_relaxed);
    return *this;
}

void backtest::user::User::reserve_on_accept(engine::OrderId /*order_id*/, engine::OrderSide side, double qty, double price)
{
    if (side == engine::OrderSide::BID)
        capital_ -= qty * price;
}

void backtest::user::User::release_reservation(engine::OrderId /*order_id*/, engine::OrderSide side, double remaining_qty, double price)
{
    if (side == engine::OrderSide::BID)
        capital_ += remaining_qty * price;
}

void backtest::user::User::apply_fill(engine::OrderId /*order_id*/, engine::OrderSide side, double qty, double price)
{
    double signed_qty = (side == engine::OrderSide::BID) ? qty : -qty;
    update_position(signed_qty, price);
    if (side == engine::OrderSide::ASK)
        capital_ += qty * price;
}

const backtest::user::UserSnapshot& backtest::user::User::get_snapshot() const
{
    const UserSnapshot* p = published_snapshot_ptr_.load(std::memory_order_acquire);
    return p ? *p : snapshots_[0];
}

double backtest::user::User::get_committed_sell_qty() const
{
    double sum = 0.0;
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return sum;
    const std::string ticker = runtime_->get_ticker(strategy_engine_id_);
    if (ticker.empty()) return sum;
    std::vector<engine::OrderId> active = sync_order_api_->get_active_orders(user_id_, ticker);
    for (engine::OrderId oid : active) {
        const engine::OrderInfo* info = get_order_info(oid);
        if (info && info->status_ == engine::OrderStatus::OPEN && info->side_ == engine::OrderSide::ASK)
            sum += backtest::math::internal_to_qty(info->qty_);
    }
    return sum;
}

double backtest::user::User::get_total_reserved_cash() const
{
    double sum = 0.0;
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return sum;
    const std::string ticker = runtime_->get_ticker(strategy_engine_id_);
    if (ticker.empty()) return sum;
    std::vector<engine::OrderId> active = sync_order_api_->get_active_orders(user_id_, ticker);
    for (engine::OrderId oid : active) {
        const engine::OrderInfo* info = get_order_info(oid);
        if (info && info->status_ == engine::OrderStatus::OPEN && info->side_ == engine::OrderSide::BID)
            sum += backtest::math::internal_to_qty(info->qty_) * backtest::math::ticks_to_dollars(info->price_);
    }
    return sum;
}

std::vector<std::pair<double, double>> backtest::user::User::get_open_bids() const
{
    std::vector<std::pair<double, double>> out;
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return out;
    const std::string ticker = runtime_->get_ticker(strategy_engine_id_);
    if (ticker.empty()) return out;
    std::vector<engine::OrderId> active = sync_order_api_->get_active_orders(user_id_, ticker);
    for (engine::OrderId oid : active) {
        const engine::OrderInfo* info = get_order_info(oid);
        if (info && info->status_ == engine::OrderStatus::OPEN && info->side_ == engine::OrderSide::BID && info->qty_ > 0)
            out.emplace_back(backtest::math::internal_to_qty(info->qty_), backtest::math::ticks_to_dollars(info->price_));
    }
    return out;
}

std::vector<std::pair<double, double>> backtest::user::User::get_open_asks() const
{
    std::vector<std::pair<double, double>> out;
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return out;
    const std::string ticker = runtime_->get_ticker(strategy_engine_id_);
    if (ticker.empty()) return out;
    std::vector<engine::OrderId> active = sync_order_api_->get_active_orders(user_id_, ticker);
    for (engine::OrderId oid : active) {
        const engine::OrderInfo* info = get_order_info(oid);
        if (info && info->status_ == engine::OrderStatus::OPEN && info->side_ == engine::OrderSide::ASK && info->qty_ > 0)
            out.emplace_back(backtest::math::internal_to_qty(info->qty_), backtest::math::ticks_to_dollars(info->price_));
    }
    return out;
}

void backtest::user::User::update_snapshot() noexcept
{
    const std::size_t write_idx = 1 - active_snapshot_index_.load(std::memory_order_relaxed);
    UserSnapshot& snap = snapshots_[write_idx];
    snap.user_id = user_id_;
    snap.capital = capital_;
    snap.realized_pnl = realized_pnl_;
    snap.total_volume = total_volume_;
    snap.ticker = (runtime_ && strategy_engine_id_ != backtest::runtime::INVALID_ENGINE_ID) ? runtime_->get_ticker(strategy_engine_id_) : std::string();
    snap.position = position_;
    snap.avg_price = avg_price_;
    double price = (runtime_ && strategy_engine_id_ != backtest::runtime::INVALID_ENGINE_ID) ? runtime_->get_market_price(runtime_->get_ticker(strategy_engine_id_)) : 0.0;
    snap.unrealized_pnl = get_unrealized_pnl(price);
    active_snapshot_index_.store(write_idx, std::memory_order_release);
    published_snapshot_ptr_.store(&snapshots_[write_idx], std::memory_order_release);
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

bool backtest::runtime::UserAPI::submit_replace_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id)
{
    return runtime_ ? runtime_->submit_replace_order_sync_impl(ticker, order_id, new_price, new_qty, user_id) : false;
}

bool backtest::runtime::UserAPI::submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_qty, user::UserId user_id)
{
    return runtime_ ? runtime_->submit_edit_order_sync_impl(ticker, order_id, new_qty, user_id) : false;
}

void backtest::runtime::UserAPI::apply_fill_to_user(user::User* u, engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    if (u) u->apply_fill(order_id, side, qty, price);
}

void backtest::runtime::UserAPI::reserve_on_accept_to_user(user::User* u, engine::OrderId order_id, engine::OrderSide side, double qty, double price)
{
    if (u) u->reserve_on_accept(order_id, side, qty, price);
}

void backtest::runtime::UserAPI::release_reservation_for_user(user::User* u, engine::OrderId order_id, engine::OrderSide side, double remaining_qty, double price)
{
    if (u) u->release_reservation(order_id, side, remaining_qty, price);
}

void backtest::runtime::UserAPI::setup_user_reservations(user::User* /*u*/, std::size_t /*reserve_size*/)
{
    // No-op: reserved state is computed on demand from engine active orders.
}

std::vector<engine::OrderId> backtest::runtime::UserAPI::get_positions(user::UserId user_id, const std::string& ticker) const
{
    return runtime_ ? runtime_->get_positions(user_id, ticker) : std::vector<engine::OrderId>{};
}

std::vector<engine::OrderId> backtest::runtime::UserAPI::get_active_orders(user::UserId user_id, const std::string& ticker) const
{
    return runtime_ ? runtime_->get_active_orders(user_id, ticker) : std::vector<engine::OrderId>{};
}

bool backtest::runtime::UserAPI::has_sufficient_shares(user::UserId user_id, const std::string& ticker, engine::Quantity qty) const
{
    return runtime_ ? runtime_->user_has_sufficient_shares(user_id, ticker, qty) : false;
}

// ===== USER CLASS METHOD IMPLEMENTATIONS =====

engine::OrderId backtest::user::User::submit_limit_order(engine::OrderSide side, double price, double quantity)
{
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return engine::INVALID_ORDER_ID;
    const std::string& ticker = runtime_->get_ticker(strategy_engine_id_);
    return sync_order_api_->submit_limit_order(ticker, side, price, quantity, user_id_);
}

engine::OrderId backtest::user::User::submit_market_order(engine::OrderSide side, double quantity)
{
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return engine::INVALID_ORDER_ID;
    const std::string& ticker = runtime_->get_ticker(strategy_engine_id_);
    return sync_order_api_->submit_market_order(ticker, side, quantity, user_id_);
}

bool backtest::user::User::submit_cancel_order(engine::OrderId order_id)
{
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return false;
    const std::string& ticker = runtime_->get_ticker(strategy_engine_id_);
    return sync_order_api_->submit_cancel_order(ticker, order_id, user_id_);
}

bool backtest::user::User::submit_replace_order(engine::OrderId order_id, double new_price, double new_quantity)
{
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return false;
    const std::string& ticker = runtime_->get_ticker(strategy_engine_id_);
    return sync_order_api_->submit_replace_order(ticker, order_id, new_price, new_quantity, user_id_);
}

bool backtest::user::User::submit_edit_order(engine::OrderId order_id, double new_quantity)
{
    if (!sync_order_api_ || !runtime_ || strategy_engine_id_ == backtest::runtime::INVALID_ENGINE_ID) return false;
    const std::string& ticker = runtime_->get_ticker(strategy_engine_id_);
    return sync_order_api_->submit_edit_order(ticker, order_id, new_quantity, user_id_);
}
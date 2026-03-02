#include "order_engine.h"


// NOTE: Refactor OrderEngine to use boost:: library for map and heap data strcutures for speed

// engine_id used to build external order ids
engine::OrderEngine::OrderEngine(std::size_t capacity, bool verbose, bool auto_match, std::uint16_t engine_id) noexcept
: order_pool_(capacity), order_queue_(),
active_snapshot_(0), placed_count_(0), cancelled_count_(0), filled_count_(0), edited_count_(0), replaced_count_(0),
last_trade_price_(-1), engine_id_(engine_id), verbose_(verbose), auto_match_(auto_match)
{
    // Initialize both snapshots
    snapshots_[0] = MarketSnapshot();
    snapshots_[1] = MarketSnapshot();
    published_snapshot_ptr_.store(&snapshots_[active_snapshot_.load(std::memory_order_relaxed)], std::memory_order_relaxed);
    // Reserve ring buffer capacity to match order pool capacity for predictable behavior
    order_queue_.reserve(capacity);
    (void)verbose_;  // stored for API; runtime uses its own verbose_ for logging
}

// POST: Place Order (price in ticks) - returns all messages in vector
engine::OrderId engine::OrderEngine::place_order(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept
{
    return place_order_impl(side, type, price, qty, &msgs, true, nullptr);
}

// POST: Place Order (price in ticks) - overload without msgs
engine::OrderId engine::OrderEngine::place_order(OrderSide side, OrderType type, Price price, Quantity qty) noexcept
{
    return place_order_impl(side, type, price, qty, nullptr, true, nullptr);
}

// POST: Place Order with optional accept/fill filtering (for runtime when users need reserves/fills only)
engine::OrderId engine::OrderEngine::place_order(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>& msgs, bool collect_accept, const std::function<bool(OrderId)>* fill_filter) noexcept
{
    return place_order_impl(side, type, price, qty, &msgs, collect_accept, fill_filter);
}

// POST: Cancel Order
bool engine::OrderEngine::cancel_order(engine::OrderId id, EngineMsg& msg) noexcept
{
    return cancel_order_impl(id, &msg);
}

// POST: Cancel Order - overload without msg 
bool engine::OrderEngine::cancel_order(engine::OrderId id) noexcept
{
    return cancel_order_impl(id, nullptr);
}

// PATCH: Replace Order (full: side, price, qty) - returns all messages in vector
bool engine::OrderEngine::replace_order(engine::OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept
{
    return replace_order_impl(id, side, price, qty, &msgs);
}

// PATCH: Replace Order (full) - overload without msgs
bool engine::OrderEngine::replace_order(engine::OrderId id, OrderSide side, Price price, Quantity qty) noexcept
{
    return replace_order_impl(id, side, price, qty, nullptr);
}

// PATCH: Edit Order (qty only) - in-place update and depth cache, no pop/push/matching
bool engine::OrderEngine::edit_order(engine::OrderId id, Quantity new_qty) noexcept
{
    return edit_order_qty_impl(id, new_qty, nullptr);
}

bool engine::OrderEngine::edit_order(engine::OrderId id, Quantity new_qty, std::vector<EngineMsg>& msgs) noexcept
{
    return edit_order_qty_impl(id, new_qty, &msgs);
}

// POST: Set Auto Match Flag (no msg emitting)
void engine::OrderEngine::set_auto_match(bool auto_match) noexcept
{
    set_auto_match(auto_match, nullptr);
}

// POST: Set Auto Match Flag with optional msg emitting during drain
void engine::OrderEngine::set_auto_match(bool auto_match, std::vector<EngineMsg>* msgs) noexcept
{
    const bool previous = auto_match_;
    auto_match_ = auto_match;
    // If toggling from off -> on, process queued orders in arrival order
    if (!previous && auto_match_)
    {
        while (!order_queue_.empty())
        {
            const auto q = order_queue_.front();
            order_queue_.pop();

            const engine::OrderId internal_id = q.first;
            if (!order_pool_.is_valid(internal_id)) continue; // Skip freed/cancelled orders

            OrderInfo& order = order_pool_[internal_id];
            // Order already in book (pushed at place time when auto_match was off); run matching only
            matching_engine(internal_id, &order, msgs, nullptr);
        }
        // Publish snapshot reflecting new auto_match state after draining
        //update_snapshot_impl();
    }
    else
    {
        // If toggled (either on->off or on->on without queued orders) still publish snapshot
        //update_snapshot_impl();
    }
}
// GET: Get Auto Match Flag
bool engine::OrderEngine::get_auto_match() const noexcept { return auto_match_; }

// Snapshot updates are managed by the runtime; manual update is still supported.

// Lock-free snapshot access (instant reads, no blocking) — fast path via published pointer
const engine::MarketSnapshot& engine::OrderEngine::get_snapshot() const noexcept
{
    const MarketSnapshot* p = published_snapshot_ptr_.load(std::memory_order_acquire);
    return *p;
}

// POST: Manually update snapshot
void engine::OrderEngine::update_snapshot() noexcept 
{ 
    update_snapshot_impl();
}

// GET: Get Order (returns nullptr if invalid/freed)
const engine::OrderInfo* engine::OrderEngine::get_order(engine::OrderId id) const noexcept { return order_pool_.get(decode_external(id)); }

// GET: Back order at level (for L2 delta sim). Returns INVALID_ORDER_ID if level missing/empty.
engine::OrderId engine::OrderEngine::get_back_order_at_level(OrderSide side, Price price) const noexcept
{
    if (side == OrderSide::BID)
    {
        auto it = bid_levels_.find(price);
        if (it == bid_levels_.end() || it->second.empty()) return INVALID_ORDER_ID;
        const OrderId internal_id = it->second.get_back_id();
        return internal_id == OrderMemoryPool::INVALID_HANDLE ? INVALID_ORDER_ID : encode_external(engine_id_, internal_id);
    }
    else
    {
        auto it = ask_levels_.find(price);
        if (it == ask_levels_.end() || it->second.empty()) return INVALID_ORDER_ID;
        const OrderId internal_id = it->second.get_back_id();
        return internal_id == OrderMemoryPool::INVALID_HANDLE ? INVALID_ORDER_ID : encode_external(engine_id_, internal_id);
    }
}

engine::OrderId engine::OrderEngine::place_order_impl(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>* msgs, bool collect_accept, const std::function<bool(OrderId)>* fill_filter) noexcept
{
    // Allocate slot and get generational handle
    const engine::OrderId internal_id = order_pool_.emplace(side, type, qty, price);
    if (internal_id == INVALID_ORDER_ID)
    {
        if (msgs)
            msgs->emplace_back(EventKind::REJECT, RejectReason::ENGINE_FULL);
        return INVALID_ORDER_ID;  // Memory Pool full
    }

    // Place New Order
    auto& new_order = order_pool_[internal_id];
    placed_count_ += 1;

    // Price adjustment (always): LIMIT cross -> touch; MARKET -> best opposite or reject
    if (type == OrderType::LIMIT)
    {
        if (side == OrderSide::ASK && bid_book_.size() && price < *bid_book_.rbegin())
            new_order.price_ = *bid_book_.rbegin();
        else if (side == OrderSide::BID && ask_book_.size() && price > *ask_book_.begin())
            new_order.price_ = *ask_book_.begin();
    }
    else // MARKET - need opposite book for price
    {
        const bool no_liquidity = (side == OrderSide::ASK) ? bid_book_.empty() : ask_book_.empty();
        if (no_liquidity)
        {
            if (msgs)
                msgs->emplace_back(EventKind::REJECT, RejectReason::NO_MARKET_LIQUIDITY);
            order_pool_.free(internal_id);
            return INVALID_ORDER_ID;
        }
        new_order.price_ = (side == OrderSide::ASK) ? *bid_book_.rbegin() : *ask_book_.begin();
    }

    if (!auto_match_)
        order_queue_.emplace(internal_id, new_order.time_);

    // Invariant: order is always in the book (both auto_match on and off)
    push_into_book(side, new_order.price_, new_order.time_, internal_id);

    // external id has engine prefix
    const engine::OrderId id = encode_external(engine_id_, internal_id);
    if (msgs && collect_accept)
    {
        msgs->emplace_back(EventKind::ACCEPT, id);
    }

    // Only run matching when the order was placed in the book. When auto_match is off the order is only queued; matching happens in set_auto_match drain.
    if (auto_match_) matching_engine(internal_id, &order_pool_[internal_id], msgs, fill_filter);
    return id; // Return external Order ID (engine-prefixed)
}

bool engine::OrderEngine::cancel_order_impl(engine::OrderId id, EngineMsg* msg) noexcept
{
    // Decode external id to internal handle
    const engine::OrderId internal_id = decode_external(id);
    // O(1) validation via generation check
    if (!order_pool_.is_valid(internal_id))
    {
        if (msg)
        {
            msg->kind = EventKind::REJECT;
            msg->reject = RejectReason::ORDER_NOT_FOUND;
        }
        return false; // Order does not exist or already freed
    }

    OrderInfo& order = order_pool_[internal_id];
    pop_from_book(order.side_, order.price_, order.time_, internal_id);

    // Capture fields before freeing the slot
    const Price     snap_price = order.price_;
    const Quantity  snap_qty   = order.qty_;
    const OrderSide snap_side  = order.side_;

    order.status_ = OrderStatus::CANCELLED;
    // Free slot (increments generation, invalidating old handles)
    order_pool_.free(internal_id);
    cancelled_count_ += 1;

    if (msg)
    {
        msg->kind = EventKind::ACCEPT;
        msg->order_id = encode_external(engine_id_, internal_id);
        msg->price = snap_price;
        msg->qty   = snap_qty;
        msg->side  = snap_side;
    }

    return true; // Order successfully canceled
}

bool engine::OrderEngine::replace_order_impl(engine::OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>* msgs) noexcept
{
    // Decode external id to internal handle
    const engine::OrderId internal_id = decode_external(id);
    // O(1) validation via generation check
    if (!order_pool_.is_valid(internal_id))
    {
        if (msgs)
            msgs->emplace_back(EventKind::REJECT, RejectReason::ORDER_NOT_FOUND);
        return false; // Order does not exist
    }

    OrderInfo& order = order_pool_[internal_id];
    pop_from_book(order.side_, order.price_, order.time_, internal_id);

    // Modify order info
    order.side_ = side;
    order.qty_ = qty;
    order.time_ = now_ns(); // Update timestamp
    order.price_ = price;

    // Price adjustment (always): cross -> touch
    if (side == OrderSide::ASK && bid_book_.size() && price < *bid_book_.rbegin())
        order.price_ = *bid_book_.rbegin();
    else if (side == OrderSide::BID && ask_book_.size() && price > *ask_book_.begin())
        order.price_ = *ask_book_.begin();

    if (!auto_match_)
        order_queue_.emplace(internal_id, order.time_);

    // Invariant: order is always in the book (both auto_match on and off)
    push_into_book(side, order.price_, order.time_, internal_id);

    replaced_count_ += 1;
    if (msgs)
    {
        msgs->emplace_back(EventKind::MODIFY, encode_external(engine_id_, internal_id));
    }
    
    if (auto_match_) matching_engine(internal_id, &order_pool_[internal_id], msgs);
    return true; // Return external Order ID
}

bool engine::OrderEngine::edit_order_qty_impl(engine::OrderId id, Quantity new_qty, std::vector<EngineMsg>* msgs) noexcept
{
    const engine::OrderId internal_id = decode_external(id);
    if (!order_pool_.is_valid(internal_id))
    {
        if (msgs) msgs->emplace_back(EventKind::REJECT, RejectReason::ORDER_NOT_FOUND);
        return false;
    }
    OrderInfo& order = order_pool_[internal_id];
    if (new_qty == 0)
    {
        EngineMsg accept_msg;
        bool ok = cancel_order_impl(id, msgs ? &accept_msg : nullptr);
        if (ok && msgs) msgs->push_back(accept_msg);
        return ok;
    }
    const Quantity old_qty = order.qty_;
    const Price price = order.price_;
    const OrderSide side = order.side_;
    order.qty_ = new_qty;
    if (side == OrderSide::BID)
    {
        bid_depth_cache_[price] -= old_qty;
        bid_depth_cache_[price] += new_qty;
    }
    else
    {
        ask_depth_cache_[price] -= old_qty;
        ask_depth_cache_[price] += new_qty;
    }
    edited_count_ += 1;
    if (msgs) msgs->emplace_back(EventKind::MODIFY, encode_external(engine_id_, internal_id));
    return true;
}

// Update snapshot after matching (optimized with TopK heap references)
void engine::OrderEngine::update_snapshot_impl() noexcept
{
    int write_idx = 1 - active_snapshot_.load(std::memory_order_acquire);
    MarketSnapshot& snap = snapshots_[write_idx];

    // Best bid/ask directly from TopK heaps (O(1))
    snap.best_bid = top_bid_heap_.empty() ? static_cast<Price>(-1) : top_bid_heap_.peek();
    snap.best_ask = top_ask_heap_.empty() ? static_cast<Price>(-1) : top_ask_heap_.peek();
    snap.market_price = last_trade_price_;
    snap.auto_match = auto_match_;
    
    // Order counts
    snap.placed_count = placed_count_;
    snap.cancelled_count = cancelled_count_;
    snap.filled_count = filled_count_;
    snap.edited_count = edited_count_;
    snap.replaced_count = replaced_count_;
    snap.open_count = order_pool_.active_count();

    // Build top K bid levels from top-K heap (O(K) iteration + O(1) cache lookups)
    snap.bid_levels = static_cast<std::uint8_t>(top_bid_heap_.size());
    if (!top_bid_heap_.empty())
    {
        // Collect all prices and sort descending
        Price prices[DEPTH_K];
        std::size_t count = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(top_bid_heap_.size()) && count < DEPTH_K; ++i)
        {
            prices[count++] = top_bid_heap_.peek(i);
        }
        
        // Fill snapshot with O(1) cache lookups
        for (std::size_t i = 0; i < count; ++i)
        {
            snap.bid_prices[i] = prices[i];
            auto it = bid_depth_cache_.find(prices[i]);
            snap.bid_depth[i] = (it != bid_depth_cache_.end()) ? it->second : 0;
        }
    }

    // Build top K ask levels from top-K heap (O(K) iteration + O(1) cache lookups)
    snap.ask_levels = static_cast<std::uint8_t>(top_ask_heap_.size());
    if (!top_ask_heap_.empty())
    {
        // Collect all prices and sort ascending
        Price prices[DEPTH_K];
        std::size_t count = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(top_ask_heap_.size()) && count < DEPTH_K; ++i)
        {
            prices[count++] = top_ask_heap_.peek(i);
        }

        // Fill snapshot with O(1) cache lookups
        for (std::size_t i = 0; i < count; ++i)
        {
            snap.ask_prices[i] = prices[i];
            auto it = ask_depth_cache_.find(prices[i]);
            snap.ask_depth[i] = (it != ask_depth_cache_.end()) ? it->second : 0;
        }
    }

    active_snapshot_.store(write_idx, std::memory_order_release);
    // Publish pointer for fastest reader path (single atomic load)
    published_snapshot_ptr_.store(&snapshots_[write_idx], std::memory_order_release);
} 
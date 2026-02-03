#pragma once
#include "order_engine.h"


// NOTE: Refactor OrderEngine to use boost:: library for map and heap data strcutures for speed

// engine_id used to build external order ids
engine::OrderEngine::OrderEngine(std::size_t capacity, bool verbose, bool auto_match, std::uint16_t engine_id) noexcept
: order_pool_(capacity), order_queue_(),
placed_count_(0), cancelled_count_(0), filled_count_(0),
verbose_(verbose), auto_match_(auto_match), last_trade_price_(-1), active_snapshot_(0), engine_id_(engine_id)
{
    // Initialize both snapshots
    snapshots_[0] = MarketSnapshot();
    snapshots_[1] = MarketSnapshot();
    published_snapshot_ptr_.store(&snapshots_[active_snapshot_.load(std::memory_order_relaxed)], std::memory_order_relaxed);
    // Reserve ring buffer capacity to match order pool capacity for predictable behavior
    order_queue_.reserve(capacity);
}

// POST: Place Order (price in ticks) - returns all messages in vector
engine::OrderId engine::OrderEngine::place_order(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept
{
    return place_order_impl(side, type, price, qty, &msgs);
}

// POST: Place Order (price in ticks) - overload without msgs 
engine::OrderId engine::OrderEngine::place_order(OrderSide side, OrderType type, Price price, Quantity qty) noexcept
{
    return place_order_impl(side, type, price, qty, nullptr);
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

// PATCH: Edit Order (price in ticks) - returns all messages in vector
engine::OrderId engine::OrderEngine::edit_order(engine::OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept
{
    return edit_order_impl(id, side, price, qty, &msgs);
}

// PATCH: Edit Order (price in ticks) - overload without msgs 
engine::OrderId engine::OrderEngine::edit_order(engine::OrderId id, OrderSide side, Price price, Quantity qty) noexcept
{
    return edit_order_impl(id, side, price, qty, nullptr);
}

// POST: Set Auto Match Flag
void engine::OrderEngine::set_auto_match(bool auto_match) noexcept
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
            // If the order wasn't in the book, push it in now preserving its stored timestamp
            if (!order.in_book_)
            {
                order.in_book_ = true;
                push_into_book(order.side_, order.price_, order.time_, internal_id);
            }

            // Simulate matching for this now-pushed order; pass id + pointer
            matching_engine(internal_id, &order, nullptr);
        }
        // Publish snapshot reflecting new auto_match state after draining
        update_snapshot_impl();
    }
    else
    {
        // If toggled (either on->off or on->on without queued orders) still publish snapshot
        update_snapshot_impl();
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

engine::OrderId engine::OrderEngine::place_order_impl(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>* msgs) noexcept
{
    // Price adjustment
    if (type == OrderType::LIMIT)
    {
        if (side == OrderSide::ASK && bid_book_.size() && price < bid_book_.peek())
            price = bid_book_.peek();
        else if (side == OrderSide::BID && ask_book_.size() && price > ask_book_.peek())
            price = ask_book_.peek();
    }
    else // MARKET - need opposite book for price
    {
        const bool no_liquidity = (side == OrderSide::ASK) ? bid_book_.empty() : ask_book_.empty();
        if (no_liquidity)
        {
            if (verbose_ && msgs)
            {
                msgs->emplace_back(EventKind::REJECT, RejectReason::NO_MARKET_LIQUIDITY);
            }
            return INVALID_ID;
        }
        price = (side == OrderSide::ASK) ? bid_book_.peek() : ask_book_.peek();
    }

    // Allocate slot and get generational handle
    const engine::OrderId internal_id = order_pool_.emplace(side, type, qty, price);
    if (internal_id == INVALID_ID)
    {
        if (verbose_ && msgs)
        {
            msgs->emplace_back(EventKind::REJECT, RejectReason::ENGINE_FULL);
        }
        return INVALID_ID;  // Memory Pool full
    }
    placed_count_ += 1;
    const auto& new_order = order_pool_[internal_id];

    // external id has engine prefix
    const engine::OrderId id = encode_external(engine_id_, internal_id);
    // Place Order in book only if auto-matching is enabled. Otherwise queue it.
    if (auto_match_)
    {
        order_pool_[internal_id].in_book_ = true;
        push_into_book(side, new_order.price_, new_order.time_, internal_id);
    }
    else
    {
        order_pool_[internal_id].in_book_ = false;
        order_queue_.emplace(internal_id, new_order.time_);
    }

    if (verbose_ && msgs)
    {
        msgs->emplace_back(EventKind::ACCEPT, id);
    }

    // Attempt to match the new order (if auto-matching is enabled)
    if (auto_match_) 
    {
        // pass both id and pointer to avoid an extra lookup
        matching_engine(internal_id, &order_pool_[internal_id], msgs);
    }

    // Auto-update snapshot based on interval (0 = manual only)
    // Snapshot updates are handled by the runtime; do not update here.

    return id; // Return external Order ID (engine-prefixed)
}

bool engine::OrderEngine::cancel_order_impl(engine::OrderId id, EngineMsg* msg) noexcept
{
    // Decode external id to internal handle
    const engine::OrderId internal_id = decode_external(id);
    // O(1) validation via generation check
    if (!order_pool_.is_valid(internal_id))
    {
        if (verbose_ && msg)
        {
            msg->kind = EventKind::REJECT;
            msg->reject = RejectReason::ORDER_NOT_FOUND;
        }
        return false; // Order does not exist or already freed
    }

    OrderInfo& order = order_pool_[internal_id];

    // Pop from book (use internal id) only if it was pushed into the book
    if (order.in_book_)
    {
        pop_from_book(order.side_, order.price_, order.time_, internal_id);
    }

    // Set status before freeing
    order.status_ = OrderStatus::CANCELLED;
    // Notify before freeing
        if (verbose_ && msg)
        {
            msg->kind = EventKind::ACCEPT;
            msg->order_id = encode_external(engine_id_, internal_id);
            msg->price = order.price_;
            msg->qty = order.qty_;
            msg->side = order.side_;
        }

    // Free slot (increments generation, invalidating old handles)
    order_pool_.free(internal_id);
    cancelled_count_ += 1;

    return true; // Order successfully canceled
}

engine::OrderId engine::OrderEngine::edit_order_impl(engine::OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>* msgs) noexcept
{
    // Decode external id to internal handle
    const engine::OrderId internal_id = decode_external(id);
    // O(1) validation via generation check
    if (!order_pool_.is_valid(internal_id))
    {
        if (verbose_ && msgs)
        {
            msgs->emplace_back(EventKind::REJECT, RejectReason::ORDER_NOT_FOUND);
        }
        return INVALID_ID; // Order does not exist
    }

    OrderInfo& order = order_pool_[internal_id];

    // Pop from old position (internal id) only if currently in book
    if (order.in_book_)
    {
        pop_from_book(order.side_, order.price_, order.time_, internal_id);
    }

    // Modify order info
    order.side_ = side;
    order.qty_ = qty;
    order.time_ = now_ns(); // Update timestamp
    order.price_ = price;

    // Price adjustment
    if (side == OrderSide::ASK && bid_book_.size() && price < bid_book_.peek())
        order.price_ = bid_book_.peek();
    else if (side == OrderSide::BID && ask_book_.size() && price > ask_book_.peek())
        order.price_ = ask_book_.peek();

    // Push to new position (internal id) only if auto-matching is enabled
    if (auto_match_)
    {
        order.in_book_ = true;
        push_into_book(side, order.price_, order.time_, internal_id);
    }
    else
    {
        // Keep the order queued (not in the book)
        order.in_book_ = false;
        order_queue_.emplace(internal_id, order.time_);
    }

    if (verbose_ && msgs)
    {
        msgs->emplace_back(EventKind::MODIFY, encode_external(engine_id_, internal_id));
    }

    // Attempt to match the modified order (if auto-matching is enabled)
    if (auto_match_) 
    {
        matching_engine(internal_id, &order_pool_[internal_id], msgs);
    }

    // Auto-update snapshot based on interval
    // Snapshot updates are handled by the runtime; do not update here.

    return encode_external(engine_id_, internal_id); // Return external Order ID
}

// Update snapshot after matching (optimized with TopK heap references)
void engine::OrderEngine::update_snapshot_impl() noexcept
{
    int write_idx = 1 - active_snapshot_.load(std::memory_order_relaxed);
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
    snap.open_count = order_pool_.active_count();

    // Build top K bid levels from top-K heap (O(K) iteration + O(1) cache lookups)
    snap.bid_levels = static_cast<std::uint8_t>(top_bid_heap_.size());
    if (!top_bid_heap_.empty())
    {
        // Collect all prices and sort descending
        Price prices[DEPTH_K];
        int count = 0;
        for (int i = 0; i < top_bid_heap_.size() && count < DEPTH_K; ++i)
        {
            prices[count++] = top_bid_heap_.peek(i);
        }
        
        // Sort descending by price (best bid first)
        std::sort(prices, prices + count, std::greater<Price>());
        // Fill snapshot with O(1) cache lookups
        for (int i = 0; i < count; ++i)
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
        int count = 0;
        for (int i = 0; i < top_ask_heap_.size() && count < DEPTH_K; ++i)
        {
            prices[count++] = top_ask_heap_.peek(i);
        }
        
        // Sort ascending by price (best ask first)
        std::sort(prices, prices + count);
        // Fill snapshot with O(1) cache lookups
        for (int i = 0; i < count; ++i)
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
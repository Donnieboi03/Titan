#pragma once
#include "../tools/heap.cpp"
#include "../tools/memory_pool.cpp"
#include <random>
#include <unordered_set>
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>

namespace engine
{
    // High-precision timestamp (nanoseconds)
    using Timestamp = std::uint64_t;
    inline Timestamp now_ns() noexcept {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    // Order Status
    enum class OrderStatus : std::uint8_t
    {
        OPEN,
        FILLED,
        CANCELLED,
        NONE
    };

    // Order Types
    enum class OrderType : std::uint8_t
    {
        LIMIT,
        MARKET
    };

    // Order Sides
    enum class OrderSide : std::uint8_t
    {
        BID, 
        ASK
    };

    // price and qty in ticks
    using Price = std::uint64_t;   
    using Quantity = std::uint32_t;

    struct OrderInfo
    {
        Timestamp time_; // 8 bytes
        Price price_; // 8 bytes
        Quantity qty_; // 4 bytes
        OrderStatus status_; // 1 byte
        OrderType type_; // 1 byte
        OrderSide side_; // 1 byte

        OrderInfo() = default;
        OrderInfo(OrderSide side, OrderType type, Quantity qty, Price price) noexcept
        : side_(side), type_(type), status_(OrderStatus::OPEN), qty_(qty), price_(price), time_(now_ns())
        {
        }
    };

    // Memory Pool with generational handles
    using OrderMemoryPool = MemoryPool<OrderInfo>;
    using OrderId = OrderMemoryPool::Handle;  // Generational handle: [48-bit slot | 16-bit generation]
    constexpr OrderId INVALID_ID = OrderMemoryPool::INVALID_HANDLE;

    using OrderLevel = Heap<std::pair<Timestamp, OrderId>, HeapType::MIN>;
    using LevelMap = std::unordered_map<Price, OrderLevel>;
    using BidBook = Heap<Price, HeapType::MAX>;
    using AskBook = Heap<Price, HeapType::MIN>;
    
    // Top-K depth tracking (K = number of levels to track)
    constexpr std::size_t DEPTH_K = 10;
    using TopBidHeap = Heap<Price, HeapType::MAX>; // Top K bid prices
    using TopAskHeap = Heap<Price, HeapType::MIN>; // Top K ask prices

    enum class EventKind : uint8_t 
    {
        NONE,
        ACCEPT,
        REJECT,
        MODIFY,
        PARTIAL_FILL,
        FILL,
        CANCEL
    };

    enum class RejectReason : uint8_t 
    {
        NO_MARKET_LIQUIDITY,
        ENGINE_FULL,
        ORDER_NOT_FOUND
    };

    struct EngineMsg 
    {
        EventKind kind = EventKind::NONE;
        union {
            OrderId order_id;
            RejectReason reject;
        };

        // Constructors for different event types
        EngineMsg() = default;
        EngineMsg(EventKind k, OrderId oid) : kind(k), order_id(oid) {}
        EngineMsg(EventKind k, RejectReason rr) : kind(k), reject(rr) {}
    };

    // Lock-free market snapshot for instant reads
    struct MarketSnapshot
    {
        Price best_bid;
        Price best_ask;
        Price market_price;      // Last trade execution price
        Quantity bid_depth[10];  // Top 10 bid levels
        Quantity ask_depth[10];  // Top 10 ask levels
        Price bid_prices[10];
        Price ask_prices[10];
        std::uint8_t bid_levels;
        std::uint8_t ask_levels;
        std::size_t placed_count;   // Total orders placed
        std::size_t cancelled_count; // Total orders cancelled
        std::size_t filled_count;    // Total orders filled
        std::size_t open_count;      // Current open orders

        MarketSnapshot() noexcept
            : best_bid(-1), best_ask(-1), market_price(-1),
              bid_levels(0), ask_levels(0),
              placed_count(0), cancelled_count(0), filled_count(0), open_count(0)
        {
            for (int i = 0; i < 10; ++i) 
            {
                bid_depth[i] = 0;
                ask_depth[i] = 0;
                bid_prices[i] = 0;
                ask_prices[i] = 0;
            }
        }
    };

    class OrderEngine
    {
    public:
        OrderEngine(std::size_t capacity = 1048576, bool verbose = true, bool auto_match = true,
                   std::size_t snapshot_update_interval = 0) noexcept
        : order_pool_(capacity), recent_order_id_(INVALID_ID),
        placed_count_(0), cancelled_count_(0), filled_count_(0),
        verbose_(verbose), auto_match_(auto_match), last_trade_price_(-1), active_snapshot_(0), 
        snapshot_update_interval_(snapshot_update_interval), operations_since_snapshot_(0)
        {
            // Initialize both snapshots
            snapshots_[0] = MarketSnapshot();
            snapshots_[1] = MarketSnapshot();
        }

        // POST: Place Order (price in ticks) - returns all messages in vector
        OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept
        {
            return place_order_impl(side, type, price, qty, &msgs);
        }

        // POST: Place Order (price in ticks) - overload without msgs for backward compatibility
        OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty) noexcept
        {
            return place_order_impl(side, type, price, qty, nullptr);
        }

        // POST: Cancel Order
        bool cancel_order(OrderId id, EngineMsg& msg) noexcept
        {
            return cancel_order_impl(id, &msg);
        }

        // POST: Cancel Order - overload without msg for backward compatibility
        bool cancel_order(OrderId id) noexcept
        {
            return cancel_order_impl(id, nullptr);
        }

        // PATCH: Edit Order (price in ticks) - returns all messages in vector
        OrderId edit_order(OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept
        {
            return edit_order_impl(id, side, price, qty, &msgs);
        }

        // PATCH: Edit Order (price in ticks) - overload without msgs for backward compatibility
        OrderId edit_order(OrderId id, OrderSide side, Price price, Quantity qty) noexcept
        {
            return edit_order_impl(id, side, price, qty, nullptr);
        }

        // POST: Set Auto Match Flag
        void set_auto_match(bool auto_match) noexcept { auto_match_ = auto_match; }
        // GET: Get Auto Match Flag
        bool get_auto_match() const noexcept { return auto_match_; }
        
        // POST: Set Snapshot Update Interval (0 = manual only, N = update every N operations)
        void set_snapshot_update_interval(std::size_t interval) noexcept { snapshot_update_interval_ = interval; }
        // GET: Get Snapshot Update Interval
        std::size_t get_snapshot_update_interval() const noexcept { return snapshot_update_interval_; }
        
        // Lock-free snapshot access (instant reads, no blocking)
        const MarketSnapshot& get_snapshot() const noexcept
        {
            int read_idx = active_snapshot_.load(std::memory_order_acquire);
            return snapshots_[read_idx];
        }
        
        // POST: Manually update snapshot (respects interval to avoid redundant updates)
        void update_snapshot() noexcept 
        { 
            // Only update if interval is 0 (manual mode) or enough operations have occurred
            if (snapshot_update_interval_ == 0 || operations_since_snapshot_ > 0)
            {
                update_snapshot_impl();
                operations_since_snapshot_ = 0;
            }
        }

        // GET: Get Order (returns nullptr if invalid/freed)
        const OrderInfo* get_order(OrderId id) const noexcept { return order_pool_.get(id); }

    private:
        OrderId place_order_impl(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>* msgs) noexcept
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
            const OrderId id = order_pool_.emplace(side, type, qty, price);
            if (id == INVALID_ID)
            {
                if (verbose_ && msgs)
                {
                    msgs->emplace_back(EventKind::REJECT, RejectReason::ENGINE_FULL);
                }
                return INVALID_ID;  // Memory Pool full
            }

            ++placed_count_;
            const auto& new_order = order_pool_[id];

            // Place Order in book
            push_into_book(side, new_order.price_, new_order.time_, id);
            recent_order_id_ = id;

            if (verbose_ && msgs)
            {
                msgs->emplace_back(EventKind::ACCEPT, id);
            }

            // Attempt to match the new order (if auto-matching is enabled)
            if (auto_match_) 
            {
                matching_engine(msgs);
            }

            // Auto-update snapshot based on interval (0 = manual only)
            if (snapshot_update_interval_ > 0) 
            {
                ++operations_since_snapshot_;
                if (operations_since_snapshot_ >= snapshot_update_interval_) 
                {
                    update_snapshot_impl();
                    operations_since_snapshot_ = 0;
                }
            }

            return id; // Return Order ID (generational handle)
        }

        bool cancel_order_impl(OrderId id, EngineMsg* msg) noexcept
        {
            // O(1) validation via generation check
            if (!order_pool_.is_valid(id))
            {
                if (verbose_ && msg)
                {
                    msg->kind = EventKind::REJECT;
                    msg->reject = RejectReason::ORDER_NOT_FOUND;
                }
                return false; // Order does not exist or already freed
            }

            OrderInfo& order = order_pool_[id];

            // Pop from book
            pop_from_book(order.side_, order.price_, order.time_, id);

            // Set status before freeing
            order.status_ = OrderStatus::CANCELLED;
            // Notify before freeing
            if (verbose_ && msg)
            {
                msg->kind = EventKind::ACCEPT;
                msg->order_id = id;
            }

            // Free slot (increments generation, invalidating old handles)
            order_pool_.free(id);
            ++cancelled_count_;

            return true; // Order successfully canceled
        }

        OrderId edit_order_impl(OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>* msgs) noexcept
        {
            // O(1) validation via generation check
            if (!order_pool_.is_valid(id))
            {
                if (verbose_ && msgs)
                {
                    msgs->emplace_back(EventKind::REJECT, RejectReason::ORDER_NOT_FOUND);
                }
                return INVALID_ID; // Order does not exist
            }

            OrderInfo& order = order_pool_[id];

            // Pop from old position
            pop_from_book(order.side_, order.price_, order.time_, id);

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

            // Push to new position
            push_into_book(side, order.price_, order.time_, id);
            recent_order_id_ = id;

            if (verbose_ && msgs)
            {
                msgs->emplace_back(EventKind::MODIFY, id);
            }

            // Attempt to match the modified order (if auto-matching is enabled)
            if (auto_match_) 
            {
                matching_engine(msgs);
            }

            // Auto-update snapshot based on interval
            if (snapshot_update_interval_ > 0) 
            {
                ++operations_since_snapshot_;
                if (operations_since_snapshot_ >= snapshot_update_interval_) 
                {
                    update_snapshot_impl();
                    operations_since_snapshot_ = 0;
                }
            }

            return id; // Return Order ID
        }

        // Push order into book at given price level
        void push_into_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept
        {
            const Quantity qty = order_pool_[id].qty_;
            
            if (side == OrderSide::BID) // BID
            {
                // Use O(1) map lookup 
                const bool new_level = (bid_levels_.find(price) == bid_levels_.end());
                if (new_level)
                {
                    bid_book_.push(price);
                    bid_levels_[price] = OrderLevel();
                    bid_depth_cache_[price] = 0;
                    
                    // Eagerly update top-K bid heap
                    if (top_bid_heap_.size() < DEPTH_K)
                    {
                        top_bid_heap_.push(price);
                    }
                    else
                    {
                        // Find min of top-K (worst bid in top-K)
                        Price worst_price = top_bid_heap_.peek();
                        int worst_idx = 0;
                        for (int i = 1; i < top_bid_heap_.size(); ++i)
                        {
                            if (top_bid_heap_.peek(i) < worst_price)
                            {
                                worst_price = top_bid_heap_.peek(i);
                                worst_idx = i;
                            }
                        }
                        if (price > worst_price)
                        {
                            top_bid_heap_.pop(worst_idx);
                            top_bid_heap_.push(price);
                        }
                    }
                }
                bid_levels_[price].emplace(time, id);
                bid_depth_cache_[price] += qty; // Incremental depth update
            }
            else
            {
                // Use O(1) map lookup 
                const bool new_level = (ask_levels_.find(price) == ask_levels_.end());
                if (new_level)
                {
                    ask_book_.push(price);
                    ask_levels_[price] = OrderLevel();
                    ask_depth_cache_[price] = 0;
                    
                    // Eagerly update top-K ask heap
                    if (top_ask_heap_.size() < DEPTH_K)
                    {
                        top_ask_heap_.push(price);
                    }
                    else
                    {
                        // Find max of top-K (worst ask in top-K)
                        Price worst_price = top_ask_heap_.peek();
                        int worst_idx = 0;
                        for (int i = 1; i < top_ask_heap_.size(); ++i)
                        {
                            if (top_ask_heap_.peek(i) > worst_price)
                            {
                                worst_price = top_ask_heap_.peek(i);
                                worst_idx = i;
                            }
                        }
                        if (price < worst_price)
                        {
                            top_ask_heap_.pop(worst_idx);
                            top_ask_heap_.push(price);
                        }
                    }
                }
                ask_levels_[price].emplace(time, id);
                ask_depth_cache_[price] += qty; // Incremental depth update
            }
        }
        
        // Pop order from book at given price level
        void pop_from_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept
        {
            const Quantity qty = order_pool_[id].qty_;
            
            // Update depth cache
            if (side == OrderSide::BID)
            {
                bid_depth_cache_[price] -= qty;
            }
            else
            {
                ask_depth_cache_[price] -= qty;
            }
            
            OrderLevel& price_level = (side == OrderSide::BID) ? bid_levels_[price] : ask_levels_[price];
            price_level.pop(price_level.find(std::pair<Timestamp, OrderId>(time, id)));
            
            if (price_level.empty())
            {
                if (side == OrderSide::BID)
                {
                    const auto idx = bid_book_.find(price);
                    if (idx != -1) bid_book_.pop(idx);
                    bid_levels_.erase(price);
                    bid_depth_cache_.erase(price);
                    
                    // Remove from top-K heap if present (O(N) with find)
                    int top_idx = top_bid_heap_.find(price);
                    if (top_idx != -1)
                    {
                        top_bid_heap_.pop(top_idx);
                    }
                }
                else // ASK
                {
                    const auto idx = ask_book_.find(price);
                    if (idx != -1) ask_book_.pop(idx);
                    ask_levels_.erase(price);
                    ask_depth_cache_.erase(price);
                    
                    // Remove from top-K heap if present (O(N) with find)
                    int top_idx = top_ask_heap_.find(price);
                    if (top_idx != -1)
                    {
                        top_ask_heap_.pop(top_idx);
                    }
                }
            }
        }
        
        // Matching Engine - takes optional message collector for fill notifications
        void matching_engine(std::vector<EngineMsg>* fill_notifications) noexcept
        {
            if (ask_book_.empty() || bid_book_.empty())
                return;  // Need both sides to match
        
            while (order_pool_[recent_order_id_].qty_ > 0)
            {
                OrderInfo& recent_order = order_pool_[recent_order_id_];
                
                // Get best prices ONCE per iteration
                const Price best_ask_price = ask_book_.peek();
                const Price best_bid_price = bid_book_.peek();
                
                // MARKET orders walk the book - move to current best price level
                if (recent_order.type_ == OrderType::MARKET)
                {
                    const Price new_price = (recent_order.side_ == OrderSide::ASK) 
                        ? best_bid_price : best_ask_price;
                    
                    if (new_price != recent_order.price_)
                    {
                        // Move order to new price level
                        pop_from_book(recent_order.side_, recent_order.price_, recent_order.time_, recent_order_id_);
                        recent_order.price_ = new_price;
                        push_into_book(recent_order.side_, recent_order.price_, recent_order.time_, recent_order_id_);
                    }
                }
                
                // Check if trade is possible (early exit before any lookups)
                const bool can_trade = (recent_order.side_ == OrderSide::ASK && best_bid_price >= recent_order.price_) ||
                                    (recent_order.side_ == OrderSide::BID && best_ask_price <= recent_order.price_);
                if (!can_trade)
                    break;  // No match possible
                
                // Get price levels
                OrderLevel& best_ask_level = ask_levels_[best_ask_price];
                OrderLevel& best_bid_level = bid_levels_[best_bid_price];
                
                if (best_ask_level.empty() || best_bid_level.empty())
                    break;
                
                // Get order IDs from levels
                const OrderId best_ask_id = best_ask_level.peek().second;
                const OrderId best_bid_id = best_bid_level.peek().second;
                
                // Match based on recent order side
                if (recent_order.side_ == OrderSide::ASK)
                    matching(recent_order_id_, best_bid_id, best_ask_level, best_bid_level, fill_notifications);
                else
                    matching(best_ask_id, recent_order_id_, best_ask_level, best_bid_level, fill_notifications);
            }
        }

        void matching(OrderId best_ask_id, OrderId best_bid_id, OrderLevel& best_ask_level,
                    OrderLevel& best_bid_level, std::vector<EngineMsg>* fill_notifications) noexcept
        {   
            OrderInfo& best_ask = order_pool_[best_ask_id];
            OrderInfo& best_bid = order_pool_[best_bid_id];
            
            // Calculate fill quantity
            const Quantity qty_filled = std::min(best_ask.qty_, best_bid.qty_);
            
            // Update depth cache for partial fills
            ask_depth_cache_[best_ask.price_] -= qty_filled;
            bid_depth_cache_[best_bid.price_] -= qty_filled;
            
            // Update quantities
            best_ask.qty_ -= qty_filled;
            best_bid.qty_ -= qty_filled;
            
            // Record trade
            last_trade_price_ = best_ask.price_;
            
            // Track which orders are fully filled
            const bool ask_filled = (best_ask.qty_ == 0);
            const bool bid_filled = (best_bid.qty_ == 0);
            
            // Update book structure for filled orders
            if (ask_filled)
            {
                best_ask_level.pop();
                if (best_ask_level.empty())
                {
                    // Adjust Ask Books
                    ask_book_.pop();
                    ask_levels_.erase(best_ask.price_);
                    // Adjust Top K
                    top_ask_heap_.pop();
                    ask_depth_cache_.erase(best_ask.price_);
                }

                // Set status before freeing
                best_ask.status_ = OrderStatus::FILLED;
                // Notify before freeing
                if (verbose_ && fill_notifications)
                {
                    fill_notifications->emplace_back(EventKind::FILL, best_ask_id);
                }

                order_pool_.free(best_ask_id);
                ++filled_count_;
            }
            else if (verbose_ && fill_notifications)
            {
                fill_notifications->emplace_back(EventKind::PARTIAL_FILL, best_ask_id);
            }
            
            if (bid_filled)
            {
                best_bid_level.pop();
                if (best_bid_level.empty())
                {
                    // Adjust Bid Books
                    bid_book_.pop();
                    bid_levels_.erase(best_bid.price_);
                    // Adjust Top K
                    top_bid_heap_.pop();
                    bid_depth_cache_.erase(best_bid.price_);
                }

                // Set status before freeing
                best_bid.status_ = OrderStatus::FILLED;
                // Notify before freeing
                if (verbose_ && fill_notifications)
                {
                    fill_notifications->emplace_back(EventKind::FILL, best_bid_id);
                }

                order_pool_.free(best_bid_id);
                ++filled_count_;
            }
            else if (verbose_ && fill_notifications)
            {
                fill_notifications->emplace_back(EventKind::PARTIAL_FILL, best_bid_id);
            }
        }

        // Update snapshot after matching (optimized with TopK heap references)
        void update_snapshot_impl() noexcept
        {
            int write_idx = 1 - active_snapshot_.load(std::memory_order_relaxed);
            MarketSnapshot& snap = snapshots_[write_idx];

            // Best bid/ask directly from TopK heaps (O(1))
            snap.best_bid = top_bid_heap_.empty() ? static_cast<Price>(-1) : top_bid_heap_.peek();
            snap.best_ask = top_ask_heap_.empty() ? static_cast<Price>(-1) : top_ask_heap_.peek();
            snap.market_price = last_trade_price_;
            
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
        } 
        
         // OrderBook System
        OrderMemoryPool order_pool_; // Memory Pool for Orders with generational handles
        AskBook ask_book_;      // Asks Order Book
        BidBook bid_book_;      // Bids Order Book
        LevelMap ask_levels_;   // Asks Price Levels
        LevelMap bid_levels_;   // Bids Price Levels
        TopBidHeap top_bid_heap_; // Top K bid prices (eagerly maintained)
        TopAskHeap top_ask_heap_; // Top K ask prices (eagerly maintained)
        OrderId recent_order_id_; // Most recently placed order (generational handle)
        std::size_t placed_count_;    // Total orders successfully placed
        std::size_t cancelled_count_; // Total orders cancelled
        std::size_t filled_count_;    // Total orders filled
        bool verbose_;            // Verbose Mode Flag
        bool auto_match_;       // Auto Matching Flag
        Price last_trade_price_; // Last trade execution price

        // Depth cache for O(1) quantity lookups on top-K levels
        std::unordered_map<Price, Quantity> bid_depth_cache_;
        std::unordered_map<Price, Quantity> ask_depth_cache_;
        // Double-buffered snapshots for lock-free reads
        MarketSnapshot snapshots_[2];
        std::atomic<int> active_snapshot_;
        // Snapshot configuration
        std::size_t snapshot_update_interval_; // 0 = manual only, N = update every N operations
        std::size_t operations_since_snapshot_; // Counter for interval-based updates
    };
}
#pragma once
#include "../tools/heap.cpp"
#include "../tools/memory_pool.cpp"
#include <random>
#include <unordered_set>
#include <iostream>
#include <chrono>
#include <vector>

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

    class OrderEngine
    {
    public:
        OrderEngine(const std::string& ticker, std::size_t capacity = 1048576, bool verbose = true, bool auto_match = true) noexcept
        : order_pool_(capacity), recent_order_id_(INVALID_ID),
        placed_count_(0), cancelled_count_(0), filled_count_(0),
        verbose_(verbose), auto_match_(auto_match), ticker_(ticker), last_trade_price_(-1)
        {
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
            static EngineMsg dummy_msg; // Ignored
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
        
        // GET: Get Order (returns nullptr if invalid/freed)
        const OrderInfo* get_order(OrderId id) const noexcept { return order_pool_.get(id); }

        // GET: Market Price (last trade price)
        Price get_market_price() const noexcept { return last_trade_price_; }

        // GET: Best Ask
        Price get_best_ask() const noexcept
        {
            if (ask_book_.empty()) return -1;
            return ask_book_.peek();
        }

        // GET: Best Bid
        Price get_best_bid() const noexcept
        {
            if (bid_book_.empty()) return -1;
            return bid_book_.peek();
        }
        
        // GET: Market Depth
        std::vector<std::pair<Price, Quantity>> get_market_depth(OrderSide side, std::size_t depth = 10) const
        {
            std::vector<std::pair<Price, Quantity>> depth_result;

            switch(side)
            {
                case OrderSide::BID:
                    {
                        BidBook tmp_book = bid_book_; // Copy BidsBook
                        for (size_t i = 0; i < depth && tmp_book.size(); ++i)
                        {
                            Price best_bid = tmp_book.peek(); // Get Best Bid Price
                            OrderLevel best_level = bid_levels_.at(best_bid); // Get Best Bid Level

                            Quantity total_qty = 0;
                            // Sum up all Quantities on current price level
                            while (best_level.size() > 0)
                            {
                                OrderId oid = best_level.peek().second;
                                if (order_pool_.is_valid(oid))
                                    total_qty += order_pool_[oid].qty_;
                                best_level.pop();
                            }

                            depth_result.emplace_back(best_bid, total_qty);
                            tmp_book.pop();
                        }
                        break;
                    }

                case OrderSide::ASK:
                    {
                        AskBook tmp_book = ask_book_; // Copy AsksBook
                        for (size_t i = 0; i < depth && tmp_book.size(); ++i)
                        {
                            Price best_ask = tmp_book.peek(); // Get Best Ask Price
                            OrderLevel best_level = ask_levels_.at(best_ask); // Get Best Ask Level

                            Quantity total_qty = 0;
                            // Sum up all Quantities on current price level
                            while (best_level.size() > 0)
                            {
                                OrderId oid = best_level.peek().second;
                                if (order_pool_.is_valid(oid))
                                    total_qty += order_pool_[oid].qty_;
                                best_level.pop();
                            }

                            depth_result.emplace_back(best_ask, total_qty);
                            tmp_book.pop();
                        }
                        break;
                    }
            }

            return depth_result;
        }

        // Counters for correctness verification
        std::size_t placed_count() const noexcept { return placed_count_; }
        std::size_t cancelled_count() const noexcept { return cancelled_count_; }
        std::size_t filled_count() const noexcept { return filled_count_; }
        std::size_t open_count() const noexcept { return order_pool_.active_count(); }

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
                matching_engine(msgs);

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
                matching_engine(msgs);

            return id; // Return Order ID
        }

        // Push order into book at given price level
        void push_into_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept
        {
            if (side == OrderSide::BID) // BID
            {
                // Use O(1) map lookup 
                if (bid_levels_.find(price) == bid_levels_.end())
                {
                    bid_book_.push(price);
                    bid_levels_[price] = OrderLevel();
                }
                bid_levels_[price].emplace(time, id);
            }
            else
            {
                // Use O(1) map lookup 
                if (ask_levels_.find(price) == ask_levels_.end())
                {
                    ask_book_.push(price);
                    ask_levels_[price] = OrderLevel();
                }
                ask_levels_[price].emplace(time, id);
            }
        }
        
        // Pop order from book at given price level
        void pop_from_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept
        {
            OrderLevel& price_level = (side == OrderSide::BID) ? bid_levels_[price] : ask_levels_[price];
            price_level.pop(price_level.find(std::pair<Timestamp, OrderId>(time, id)));
            
            if (price_level.empty())
            {
                if (side == OrderSide::BID)
                {
                    const auto idx = bid_book_.find(price);
                    if (idx != -1) bid_book_.pop(idx);
                    bid_levels_.erase(price);
                }
                else // ASK
                {
                    const auto idx = ask_book_.find(price);
                    if (idx != -1) ask_book_.pop(idx);
                    ask_levels_.erase(price);
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
                    ask_book_.pop();
                    ask_levels_.erase(best_ask.price_);
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
                    bid_book_.pop();
                    bid_levels_.erase(best_bid.price_);
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

        OrderMemoryPool order_pool_; // Memory Pool for Orders with generational handles
        AskBook ask_book_;      // Asks Order Book
        BidBook bid_book_;      // Bids Order Book
        LevelMap ask_levels_;   // Asks Price Levels
        LevelMap bid_levels_;   // Bids Price Levels
        OrderId recent_order_id_; // Most recently placed order (generational handle)
        std::size_t placed_count_;    // Total orders successfully placed
        std::size_t cancelled_count_; // Total orders cancelled
        std::size_t filled_count_;    // Total orders filled
        bool verbose_;            // Verbose Mode Flag
        bool auto_match_;       // Auto Matching Flag
        std::string ticker_;    // Ticker
        Price last_trade_price_; // Last trade execution price
    };
}
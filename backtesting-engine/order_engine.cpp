#pragma once
#include "../tools/heap.cpp"
#include "../tools/arena.cpp"
#include <random>
#include <unordered_set>
#include <iostream>
#include <chrono>

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

// Primitive Aliases
using Price = std::uint32_t;    // price in ticks (e.g., cents)
using Quantity = std::double_t;

constexpr double TICK_SIZE = 0.01; // 1 cent
#include <cmath>
inline Price dollars_to_ticks(double dollars) {
    return static_cast<Price>(std::round(dollars / TICK_SIZE));
}
inline double ticks_to_dollars(Price ticks) {
    return ticks * TICK_SIZE;
}

// Order Info - stored in arena slots
struct OrderInfo
{
    Timestamp time_;
    Quantity qty_;
    Price price_;
    OrderStatus status_;
    OrderType type_;
    OrderSide side_;

    OrderInfo() noexcept
    : side_(OrderSide::BID), type_(OrderType::LIMIT), status_(OrderStatus::NONE), qty_(0), price_(0), time_(0)
    {
    }

    OrderInfo(OrderSide side, OrderType type, Quantity qty, Price price) noexcept
    : side_(side), type_(type), status_(OrderStatus::OPEN), qty_(qty), price_(price), time_(now_ns())
    {
    }
};

// Arena with generational handles
using OrderArena = Arena<OrderInfo>;
using OrderId = OrderArena::Handle;  // Generational handle: [48-bit slot | 16-bit generation]
constexpr OrderId INVALID_ID = OrderArena::INVALID_HANDLE;

using OrderLevel = Heap<std::pair<Timestamp, OrderId>, HeapType::MIN>;
using LevelMap = std::unordered_map<Price, OrderLevel>;
using BidBook = Heap<Price, HeapType::MAX>;
using AskBook = Heap<Price, HeapType::MIN>;

class OrderEngine
{
public:
    OrderEngine(const std::string& ticker, std::size_t capacity = 1048576, bool verbose = true, bool auto_match = true) noexcept
    : order_pool_(capacity), recent_order_id_(INVALID_ID),
      placed_count_(0), cancelled_count_(0), filled_count_(0),
      verbose_(verbose), auto_match_(auto_match), ticker_(ticker), last_trade_price_(-1)
    {
    }

    // POST: Place Order (price in ticks)
    OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty) noexcept
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
                if (verbose_)
                    std::cout << "[" << ticker_ << "] | [REJECTED: NO MARKET LIQUIDITY]" << std::endl;
                return INVALID_ID;
            }
            price = (side == OrderSide::ASK) ? bid_book_.peek() : ask_book_.peek();
        }

        // Allocate slot and get generational handle
        const OrderId id = order_pool_.emplace(side, type, qty, price);
        if (id == INVALID_ID) return INVALID_ID;  // Arena full
        
        ++placed_count_;
        const auto& new_order = order_pool_[id];

        // Place Order in book
        push_into_book(side, new_order.price_, new_order.time_, id);

        if (verbose_)
            notify_open(id);
        
        recent_order_id_ = id;

        // Attempt to match the new order (if auto-matching is enabled)
        if (auto_match_)
            matching_engine();

        return id; // Return Order ID (generational handle)
    }

    // POST: Cancel Order
    bool cancel_order(OrderId id) noexcept
    {
        // O(1) validation via generation check
        if (!order_pool_.is_valid(id))
            return false; // Order does not exist or already freed
        
        OrderInfo& order = order_pool_[id];

        // Pop from book
        pop_from_book(order.side_, order.price_, order.time_, id);

        // Set status before freeing
        order.status_ = OrderStatus::CANCELLED;
        
        // Notify before freeing
        if (verbose_)
            notify_cancel(id, order);
        
        // Free slot (increments generation, invalidating old handles)
        order_pool_.free(id);
        ++cancelled_count_;
        
        return true; // Order successfully canceled
    }

    // PATCH: Edit Order (price in ticks)
    OrderId edit_order(OrderId id, OrderSide side, Price price, Quantity qty) noexcept
    {
        // O(1) validation via generation check
        if (!order_pool_.is_valid(id))
            return INVALID_ID; // Order does not exist
        
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

        if (verbose_)
            notify_modify(id);
        
        // Attempt to match the modified order (if auto-matching is enabled)
        if (auto_match_)
            matching_engine();
        
        return id; // Return Order ID
    }

    // POST: Set Auto Match Flag
    void set_auto_match(bool auto_match) noexcept { auto_match_ = auto_match; }

    // GET: Get Auto Match Flag
    bool get_auto_match() const noexcept { return auto_match_; }
    
    // GET: Get Order (returns nullptr if invalid/freed)
    const OrderInfo* get_order(OrderId id) const noexcept
    { 
        return order_pool_.get(id);
    }

    // GET: Market Price (last trade price)
    Price get_market_price() const noexcept
    {
        return last_trade_price_;
    }

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
    // Order Book
    OrderArena order_pool_; // Memory Pool for Orders with generational handles
    AskBook ask_book_;      // Asks Order Book
    BidBook bid_book_;      // Bids Order Book
    LevelMap ask_levels_;   // Asks Price Levels
    LevelMap bid_levels_;   // Bids Price Levels
    OrderId recent_order_id_; // Most recently placed order (generational handle)
    std::size_t placed_count_;    // Total orders successfully placed
    std::size_t cancelled_count_; // Total orders cancelled
    std::size_t filled_count_;    // Total orders filled
    bool verbose_;            // Verbose Mode
    bool auto_match_;       // Auto Matching Flag
    std::string ticker_;    // Ticker
    Price last_trade_price_; // Last trade execution price
    
    // Push order into book at given price level
    void push_into_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept
    {
        if (side == OrderSide::BID) // BID
        {
            // Use O(1) map lookup instead of O(n) heap find
            if (bid_levels_.find(price) == bid_levels_.end())
            {
                bid_book_.push(price);
                bid_levels_[price] = OrderLevel();
            }
            bid_levels_[price].emplace(time, id);
        }
        else
        {
            // Use O(1) map lookup instead of O(n) heap find
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
    
    // Matching Engine
    void matching_engine() noexcept
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
                matching(recent_order_id_, best_bid_id, best_ask_level, best_bid_level);
            else
                matching(best_ask_id, recent_order_id_, best_ask_level, best_bid_level);
        }
    }

    void matching(OrderId best_ask_id, OrderId best_bid_id, 
                  OrderLevel& best_ask_level, OrderLevel& best_bid_level) noexcept
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
            best_ask.status_ = OrderStatus::FILLED;
            best_ask_level.pop();
            if (best_ask_level.empty())
            {
                ask_book_.pop();
                ask_levels_.erase(best_ask.price_);
            }

            order_pool_.free(best_ask_id);
            ++filled_count_;
        }
        
        if (bid_filled)
        {
            best_bid.status_ = OrderStatus::FILLED;
            best_bid_level.pop();
            if (best_bid_level.empty())
            {
                bid_book_.pop();
                bid_levels_.erase(best_bid.price_);
            }

            order_pool_.free(best_bid_id);
            ++filled_count_;
        }
        
        // Notify fills (before freeing)
        if (verbose_)
        {
            notify_fill(best_ask_id, best_ask, qty_filled);
            notify_fill(best_bid_id, best_bid, qty_filled);
        }
    }

    // Notify of what Orders are open
    void notify_open(OrderId id)
    {
        if (!order_pool_.is_valid(id)) return;
        
        const OrderInfo& order = order_pool_[id];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";

        std::cout << "[" << ticker_ << "] | [OPEN] | TYPE: " << type << " | ID: " << id << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << ticks_to_dollars(order.price_) << " | TIME: " << order.time_ << std::endl;
    }

    // Notify of what Orders were filled (takes order ref since called before free)
    void notify_fill(OrderId id, const OrderInfo& order, Quantity qty_filled)
    {
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string status = !order.qty_ ? "[FILLED]" : "[PARTIALLY FILLED]";
        
        std::cout << "[" << ticker_ << "] | " << status << " | TYPE: " << type << " | ID: " << id << " | SIDE: " << side << 
        " | QTY: " << qty_filled << " | PRICE: " << ticks_to_dollars(order.price_) << " | TIME: " << std::time(0) << std::endl;
    }

    // Notify of what Orders were canceled (takes order ref since called before free)
    void notify_cancel(OrderId id, const OrderInfo& order)
    {
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        
        std::cout << "[" << ticker_ << "] | [CANCELED] | TYPE: " << type << " | ID: " << id << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << ticks_to_dollars(order.price_) << " | TIME: " << std::time(0) << std::endl;
    }

    // Notify of what Orders were modified
    void notify_modify(OrderId id)
    {
        if (!order_pool_.is_valid(id)) return;

        const OrderInfo& order = order_pool_[id];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        
        std::cout << "[" << ticker_ << "] | [MODIFIED] | TYPE: " << type << " | ID: " << id << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << ticks_to_dollars(order.price_) << " | TIME: " << std::time(0) << std::endl;
    }
};

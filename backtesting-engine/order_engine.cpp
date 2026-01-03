#pragma once
#include "../tools/heap.cpp"
#include "../tools/arena.cpp"
#include <random>
#include <iostream>

// Order Status
enum class OrderStatus
{
    OPEN,
    FILLED,
    CANCELLED,
    REJECTED
};

// Order Types
enum class OrderType
{
    LIMIT,
    MARKET
};

// Order Sides
enum class OrderSide
{
    BID, 
    ASK
};

// Premative Aliases
using OrderId = std::uint32_t;
using Price = std::double_t;
using Quantity = std::double_t;

// Order Info
struct OrderInfo
{
    std::time_t time_;
    Quantity qty_;
    Price price_;
    OrderId id_;
    OrderStatus status_;
    OrderType type_;
    OrderSide side_;
    
    OrderInfo(OrderSide side, OrderType type, Quantity qty, Price price, OrderId id) noexcept
    : side_(side), type_(type), status_(OrderStatus::OPEN), qty_(qty), price_(price), id_(id), time_(std::time(nullptr))
    {
    }
};

using OrderLevel = Heap<std::pair<std::time_t, OrderId>, HeapType::MIN>;
using LevelMap = std::unordered_map<Price, OrderLevel>;
using OrderArena = Arena<OrderInfo>;
using OrderMap = std::unordered_map<OrderId, OrderArena::Index>;
using BidBook = Heap<Price, HeapType::MAX>;
using AskBook = Heap<Price, HeapType::MIN>;

class OrderEngine
{
public:
    OrderEngine(const std::string& ticker, std::size_t capacity, bool verbose = true, bool auto_match = true) noexcept
    : order_pool_(capacity), recent_order_id_(-1), next_order_id_(0), verbose_(verbose), auto_match_(auto_match), ticker_(ticker), last_trade_price_(-1), num_trades_(0)
    {
    }

    // POST: Place Limit Order
    OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty) noexcept
    {
        const OrderId id = next_order_id_++; // New Order ID
        order_table_[id] = order_pool_.emplace(side, type, qty, price, id); // Map Id to Arena Index
        
        OrderInfo& new_order = order_pool_[order_table_[id]]; // Refrence Order 
        // Price Check
        switch (type)
        {
            case OrderType::LIMIT: // Limit Order
                {
                    // If Limit Order is above (BID) or below (ASK) best opposing price, then adjust
                    if (side == OrderSide::ASK && bid_book_.size() && price < bid_book_.peek())
                        new_order.price_ = bid_book_.peek(); // Adjust price to best bid
                    else if (side == OrderSide::BID && ask_book_.size() && price > ask_book_.peek())
                        new_order.price_ = ask_book_.peek(); // Adjust price to best ask
                    break;
                }

           case OrderType::MARKET: // Market Order
                {
                    // Check Books
                    if (side == OrderSide::ASK && bid_book_.empty())
                    {
                        new_order.status_ = OrderStatus::REJECTED; // Update Order Status
                        // Signal Reject Notification
                        notify_reject(id, "NO MARKET LIQUIDITY (BIDS)");
                        return -1; // No bids to match with
                    }
                    if (side == OrderSide::BID && ask_book_.empty())
                    {
                        new_order.status_ = OrderStatus::REJECTED; // Update Order Status
                        // Signal Reject Notification
                        notify_reject(id, "NO MARKET LIQUIDITY (ASKS)");
                        return -1; // No asks to match with
                    }

                    // If Books, then get best opposing price
                    new_order.price_ = side == OrderSide::ASK ? bid_book_.peek() : ask_book_.peek();
                    break;
                }
                
            default:
                new_order.status_ = OrderStatus::REJECTED; // Update Order Status
                // Signal Reject Notification
                notify_reject(id, "INVALID ORDER TYPE");
                return -1; // Invalid Order Type
        }
        
        // Place Order
        switch (side)
        {
            case OrderSide::ASK:
                {
                    // Create new ask price level if no price level
                    if (ask_book_.find(price) == -1)
                    {
                        ask_book_.push(price);
                        ask_levels_[price] = OrderLevel();
                    }
                    ask_levels_[price].emplace(new_order.time_, new_order.id_);                    
                    break;
                }
            
            case OrderSide::BID:
                {
                    // Create new bid price level if no price level
                    if (bid_book_.find(price) == -1)
                    {
                        bid_book_.push(price);
                        bid_levels_[price] = OrderLevel();
                    }
                    bid_levels_[price].emplace(new_order.time_, new_order.id_);
                    break;
                }
            
            default:
                new_order.status_ = OrderStatus::REJECTED; // Update Order Status
                // Signal Reject Notification
                notify_reject(id, "INVALID ORDER SIDE");
                return -1; // Invalid Order Side
        }

        // Notifiy Open
        notify_open(id);
        recent_order_id_ = id;

        // Attempt to match the new order (if auto-matching is enabled)
        if (auto_match_)
            matching_engine();

        return id; // Return Order ID
    }

    // POST: Cancel Order
    bool cancel_order(OrderId id) noexcept
    {
        if (order_table_.find(id) == order_table_.end()) 
            return false; // Order does not exist;
        
        OrderInfo& order = order_pool_[order_table_[id]];
        if (order.status_ != OrderStatus::OPEN || order.type_ != OrderType::LIMIT)
            return false; // Order is not open and not a limit order

        // Get Order's Price Level
        OrderLevel& price_level = (order.side_ == OrderSide::BID) ? bid_levels_[order.price_] : ask_levels_[order.price_];
        // Pop Order from level
        price_level.pop(price_level.find(std::pair<std::time_t, OrderId>(order.time_, order.id_)));

        // If Price Level is empty pop from Book and erase Price Level
        if (price_level.empty())
        {
            switch(order.side_)
            {
                case OrderSide::BID:
                {
                    const auto& bid = bid_book_.find(order.price_);
                    if (bid != -1)
                    {
                        bid_book_.pop(bid);
                        bid_levels_.erase(order.price_);
                    }
                    break;
                }

                case OrderSide::ASK:
                {
                    const auto& ask = ask_book_.find(order.price_);
                    if (ask != -1)
                    {
                        ask_book_.pop(ask);
                        ask_levels_.erase(order.price_);
                    }
                    break;
                }

                default:
                    return false; // Invalid Order Side
            }
        }


        order.status_ = OrderStatus::CANCELLED; // Update Order Status
        // Signal Cancel Notification
        notify_cancel(id);
        return true; // Order successfully canceled
    }

    // PATCH: Edit Order
    OrderId edit_order(OrderId id, OrderSide side, Price price, Quantity qty) noexcept
    {
        if (!cancel_order(id))
        {
            // If order exists, notify reject for modify attempt; otherwise just return
            if (order_table_.find(id) != order_table_.end())
                // Signal Reject Notification
                notify_reject(id, "MODIFY FAILED: COULD NOT CANCEL ORDER");
            return -1; // If cancel failed
        }

        OrderInfo& modified_order = order_pool_[order_table_[id]]; // Refrence Order

        // Modify Info
        modified_order.side_ = side;
        modified_order.qty_ = qty;
        modified_order.price_ = price;
        modified_order.time_ = std::time(nullptr); // Update timestamp
    
        // Price Check
        if (side == OrderSide::ASK && bid_book_.size() && price < bid_book_.peek())
            modified_order.price_ = bid_book_.peek(); // Adjust price to best bid
        else if (side == OrderSide::BID && ask_book_.size() && price > ask_book_.peek())
            modified_order.price_ = ask_book_.peek(); // Adjust price to best ask
        
        // Place Order
        switch (side)
        {
            case OrderSide::ASK:
                {
                    // Create modified ask price level if no price level
                    if (ask_book_.find(price) == -1)
                    {
                        ask_book_.push(price);
                        ask_levels_[price] = OrderLevel();
                    }
                    ask_levels_[price].emplace(modified_order.time_, modified_order.id_);                    
                    break;
                }
            
            case OrderSide::BID:
                {
                    // Create modified bid price level if no price level
                    if (bid_book_.find(price) == -1)
                    {
                        bid_book_.push(price);
                        bid_levels_[price] = OrderLevel();
                    }
                    bid_levels_[price].emplace(modified_order.time_, modified_order.id_);
                    break;
                }
            
            default:
                modified_order.status_ = OrderStatus::REJECTED; // Update Order Status
                // Signal Reject Notification
                notify_reject(id, "INVALID ORDER SIDE");
                return -1; // Invalid Order Side
        }

        modified_order.status_ = OrderStatus::OPEN; // Update Order Status
        recent_order_id_ = id;

        // Signal Modify Notification
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

    // GET: Get Order
    const OrderInfo* get_order(const unsigned int& id) const noexcept
    { 
            if (order_table_.find(id) == order_table_.end())
                return nullptr; // NULL if no order
            return &order_pool_[order_table_.at(id)]; 
    }

    // GET: Market Price (last trade price)
    Price get_market_price() const noexcept
    {
        return last_trade_price_;
    }

    // GET: Number of trades
    uint64_t get_num_trades() const noexcept { return num_trades_; }

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

    // GET: Orders by Status
    std::vector<OrderInfo> get_orders_by_status(OrderStatus status) const
    {
        std::vector<OrderInfo> result;
        for (const auto& [id, idx] : order_table_) 
        {
            auto order = order_pool_[idx];
            if (order.status_ == status)
                result.push_back(order);
        }
        return result;
    }
    
    // GET: Maket Depth
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
                            total_qty += order_pool_[order_table_.at(best_level.peek().second)].qty_;
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
                            total_qty += order_pool_[order_table_.at(best_level.peek().second)].qty_;
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

private:
    // Order Book
    OrderArena order_pool_; // Memory Pool for Orders (must be first for constructor)
    AskBook ask_book_; // Asks Order Book
    BidBook bid_book_; // Bids Order Book
    LevelMap ask_levels_; // Asks Price Levels
    LevelMap bid_levels_; // Bids Price Levels
    OrderMap order_table_; // Map to all active orders
    OrderId recent_order_id_; // Recent Order ID
    OrderId next_order_id_; // Next Order ID
    bool verbose_; // Verbose Mode
    bool auto_match_; // Auto Matching Flag
    std::string ticker_; // Ticker
    Price last_trade_price_; // Last trade execution price
    std::atomic<uint64_t> num_trades_; // Total number of trades

    // Matching Engine
    void matching_engine() noexcept
    {
        // Early exit checks
        if (order_table_.find(recent_order_id_) == order_table_.end())
            return;  // Recent order doesn't exist
        
        if (ask_book_.empty() || bid_book_.empty())
            return;  // Need both sides to match
        
        // Get recent order ONCE
        OrderInfo& recent_order = order_pool_[order_table_[recent_order_id_]];
        
        // Match loop
        while (recent_order.status_ == OrderStatus::OPEN && recent_order.qty_ > 0)
        {
            // Get best prices ONCE per iteration
            const Price best_ask_price = ask_book_.peek();
            const Price best_bid_price = bid_book_.peek();
            
            // Check if trade is possible (early exit before any lookups)
            const bool can_trade = (recent_order.side_ == OrderSide::ASK && best_bid_price >= recent_order.price_) ||
                                   (recent_order.side_ == OrderSide::BID && best_ask_price <= recent_order.price_);
            if (!can_trade)
                break;  // No match possible
            
            // Get price levels ONCE (no redundant existence checks - if price is in book, level MUST exist)
            OrderLevel& best_ask_level = ask_levels_[best_ask_price];
            OrderLevel& best_bid_level = bid_levels_[best_bid_price];
            
            // Safety check (should never happen, but prevents crash)
            if (best_ask_level.empty() || best_bid_level.empty())
                break;
            
            // Get order IDs from levels
            const OrderId best_ask_id = best_ask_level.peek().second;
            const OrderId best_bid_id = best_bid_level.peek().second;
            
            // Get orders ONCE (use operator[] which is faster than at() for existing keys)
            OrderInfo& best_ask = order_pool_[order_table_[best_ask_id]];
            OrderInfo& best_bid = order_pool_[order_table_[best_bid_id]];
            
            // Match based on recent order side
            if (recent_order.side_ == OrderSide::ASK)
                matching(recent_order, best_bid, best_ask_level, best_bid_level);
            else
                matching(best_ask, recent_order, best_ask_level, best_bid_level);
        }
    }

    void matching(OrderInfo& best_ask, OrderInfo& best_bid, 
                  OrderLevel& best_ask_level, OrderLevel& best_bid_level) noexcept
    {   
        // Calculate fill quantity
        const Quantity qty_filled = std::min(best_ask.qty_, best_bid.qty_);
        
        // Update quantities
        best_ask.qty_ -= qty_filled;
        best_bid.qty_ -= qty_filled;
        
        // Record trade
        last_trade_price_ = best_ask.price_;  // Use passive order price
        num_trades_.fetch_add(1, std::memory_order_relaxed);  // Atomic increment
        
        // Update statuses BEFORE notifications (so notifications see correct state)
        if (best_ask.qty_ == 0)
            best_ask.status_ = OrderStatus::FILLED;
        
        if (best_bid.qty_ == 0)
            best_bid.status_ = OrderStatus::FILLED;
    
        // Submit notifications AFTER status updates
        notify_fill(best_ask.id_, qty_filled);
        notify_fill(best_bid.id_, qty_filled);

        // Clean up filled orders from book
        if (best_ask.qty_ == 0)
        {
            best_ask_level.pop();
            if (best_ask_level.empty())
            {
                ask_book_.pop();
                ask_levels_.erase(best_ask.price_);
            }
        }

        if (best_bid.qty_ == 0)
        {
            best_bid_level.pop();
            if (best_bid_level.empty())
            {
                bid_book_.pop();
                bid_levels_.erase(best_bid.price_);
            }
        }
    }

    // Notify of what Orders are open
    void notify_open(OrderId id)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Open Order");

        if (!verbose_) return; // If not verbose, do not notify

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";

        // Notification
        std::cout << ticker_msg << " | " << "[OPEN] | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << order.time_ << std::endl;
    }

    // Notify of what Orders were filled
    void notify_fill(OrderId id, Quantity qty_filled)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Filled Order");

        if (!verbose_) return; // If not verbose, do not notify

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string status = !order.qty_ ? "[FILLED]" : "[PARTIALLY FILLED]";
        const std::string ticker_msg = "[" + ticker_ + "]";
        const std::time_t current_time = std::time(0); // Time of Fill
        
        // Notification
        std::cout << ticker_msg << " | " << status << " | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << qty_filled << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were canceled
    void notify_cancel(OrderId id)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Cancelled Order");

        if (!verbose_) return; // If not verbose, do not notify

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";
        const std::time_t current_time = std::time(0); // Time of Cancel
        
        // Notification
        std::cout << ticker_msg << " | " << "[CANCELED] | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were canceled
    void notify_reject(OrderId id, const std::string err)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Rejected Order");
        
        if (!verbose_) return; // If not verbose, do not notify

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string reject_msg = "[REJECTED: " + err +  "]";
        const std::string ticker_msg = "[" + ticker_ + "]";
        const std::time_t current_time = std::time(0); // Time of Rejection
        
        // Notification
        std::cout << ticker_msg << " | " << reject_msg << " | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were modified
    void notify_modify(OrderId id)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Modified Order");

        if (!verbose_) return; // If not verbose, do not notify

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";
        const std::time_t current_time = std::time(0); // Time of Modification
        
        // Notification
        std::cout << ticker_msg << " | " << "[MODIFIED] | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }
};
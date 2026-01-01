#pragma once
#include "../tools/Heap.cpp"
#include "../tools/Arena.cpp"
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

enum class OrderLevelType
{
    TIME,
    ID
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
    
    OrderInfo(const OrderSide side, const OrderType type, double qty, double price, const unsigned int id) noexcept
    : side_(side), type_(type), status_(OrderStatus::OPEN), qty_(qty), price_(price), id_(id), time_(std::time(nullptr))
    {
    }
};

// Data Structure Aliases
using OrderLevel = Heap<std::pair<std::time_t, OrderId>, HeapType::MIN>;
using LevelMap = std::unordered_map<Price, OrderLevel>;
using OrderPool = Arena<OrderInfo>;
using OrderMap = std::unordered_map<OrderId, OrderPool::Index>;
using BidBook = Heap<Price, HeapType::MAX>;
using AskBook = Heap<Price, HeapType::MIN>;

class OrderEngine
{
public:
    OrderEngine(const std::string& ticker, std::size_t capacity, bool verbose = true) noexcept
    : book_updated_(false), recent_order_id_(0), next_order_id_(1), order_pool_(capacity), verbose_(verbose), ticker_(ticker)
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
                        notify_reject(id, "NO MARKET LIQUIDITY (BIDS)");
                        return -1; // No bids to match with
                    }
                    if (side == OrderSide::BID && bid_book_.empty())
                    {
                        notify_reject(id, "NO MARKET LIQUIDITY (ASKS)");
                        return -1; // No asks to match with
                    }

                    // If Books, then get best opposing price
                    new_order.price_ = side == OrderSide::ASK ? bid_book_.peek() : bid_book_.peek();
                    break;
                }
                
            default:
                return 0; // Invalid Order Type
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
                return -1; // Invalid Order Side
        }

        // Notifiy Open
        notify_open(id);
        recent_order_id_ = id;

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

        // Notify Cancel
        notify_cancel(id);
        return true; // Order successfully canceled
    }

    // PATCH: Edit Order
    OrderId edit_order(OrderId id, OrderSide side, Price price, Quantity qty) noexcept
    {
        if (!cancel_order(id))
            return -1; // If cancel failed
        
        OrderInfo& new_order = order_pool_[order_table_[id]]; // Refrence Order

        // Modify Info
        new_order.side_ = side;
        new_order.qty_ = qty;
        new_order.price_ = price;
    
        // Price Check
        if (side == OrderSide::ASK && bid_book_.size() && price < bid_book_.peek())
            new_order.price_ = bid_book_.peek(); // Adjust price to best bid
        else if (side == OrderSide::BID && ask_book_.size() && price > ask_book_.peek())
            new_order.price_ = ask_book_.peek(); // Adjust price to best ask
        
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
                return -1; // Invalid Order Side
        }

        // Notifiy Modify
        notify_modify(id);
        return id; // Return Order ID
    }

    // GET: Get Order
    const OrderInfo* get_order(const unsigned int& id) const noexcept
    { 
            if (order_table_.find(id) == order_table_.end())
                return nullptr; // NULL if no order
            return &order_pool_[order_table_.at(id)]; 
    }

    // GET: Average Price
    Price get_price() const noexcept
    {
        if (bid_book_.empty() && ask_book_.empty())
            return -1; // If both books are empty
        else if (bid_book_.empty())
            return ask_book_.peek(); // If BidBook is empty
        else if (ask_book_.empty())
            return bid_book_.peek(); // If AskBook is Empty
        return (ask_book_.peek() + bid_book_.peek()) / 2; // Average of best ask and bid
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
        return std::move(result);
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

        return std::move(depth_result);
    }

private:
    // Order Book
    AskBook ask_book_; // Ask Order Book
    BidBook bid_book_; // Bid Order Book
    LevelMap ask_levels_; // Ask Price Levels
    LevelMap bid_levels_; // Bids Price Levels
    OrderMap order_table_; // Map to all active orders
    OrderPool order_pool_; // Pool of activate orders
    
    unsigned int recent_order_id_; // New Orders ID
    unsigned int next_order_id_; // Next Order ID

    bool book_updated_;

    bool verbose_; // Verbose Mode
    std::string ticker_; // Ticker

    // Matching Engine
    void matching_engine() noexcept
    {
        auto is_recent_order = order_table_.find(recent_order_id_) != order_table_.end();
        auto is_books = ask_book_.size() && bid_book_.size();
        // Match if there is a recent order placed and there are orders on both sides
        if (is_recent_order && is_books)
        {
            // Get Recent Order
            OrderInfo& recent_order = order_pool_[order_table_[recent_order_id_]];
            // Match order while order status is Open and there is a qty
            while (recent_order.status_ == OrderStatus::OPEN && recent_order.qty_)
            {
                // Get best asks and bids
                Price best_asks_price = ask_book_.peek();
                Price best_bids_price = bid_book_.peek();
                if (ask_levels_.find(best_asks_price) == ask_levels_.end() ||
                    bid_levels_.find(best_bids_price) == bid_levels_.end())
                    break; // No best price level to match with
                    
                // Get price level for best asks and bids
                OrderLevel& best_level_asks = ask_levels_[best_asks_price];
                OrderLevel& best_level_bids = bid_levels_[best_bids_price];
                if (best_level_asks.empty() || best_level_bids.empty())
                    break; // No best level to match with
                
                // Get OrderInfo for best ask and bid
                OrderInfo& best_ask = order_pool_[order_table_[best_level_asks.peek().second]];
                OrderInfo& best_bid = order_pool_[order_table_[best_level_bids.peek().second]];
                
                // Break If you can't trade
                const bool can_trade_asks = recent_order.side_ == OrderSide::ASK && 
                !best_level_bids.empty() && best_bid.price_ >= recent_order.price_;
                const bool can_trade_bids = recent_order.side_ == OrderSide::BID && 
                !best_level_asks.empty() && best_ask.price_ <= recent_order.price_;
                if (!(can_trade_asks || can_trade_bids)) 
                    break; // No match possible

                switch (recent_order.side_)
                {
                    case OrderSide::ASK:
                        {
                            matching(recent_order, best_bid, best_level_asks, best_level_bids);
                            break;
                        }
                    
                    case OrderSide::BID:
                        {
                            matching(best_ask, recent_order, best_level_asks, best_level_bids);
                            break;
                        }
                }                
            }
        } 
    }

    // Match Orders
    void matching(OrderInfo& best_ask, OrderInfo& best_bid, OrderLevel& best_level_asks, OrderLevel& best_level_bids)
    {   
        // Get qty filled and apply difference
        const Quantity qty_filled = std::min(best_ask.qty_, best_bid.qty_);
        best_ask.qty_ -= qty_filled;
        best_bid.qty_ -= qty_filled;
        
        notify_fill(best_ask.id_, qty_filled);
        notify_fill(best_bid.id_, qty_filled);

        // If ask qty is 0 then notify fill and erase
        if (!best_ask.qty_)
        {
            best_level_asks.pop();
            // If ask level is now empty then erase level
            if (!best_level_asks.size())
            {
                ask_book_.pop();
                ask_levels_.erase(best_ask.price_);
            }
        }

        // If ask qty is 0 then notify fill and erase
        if (!best_bid.qty_)
        {
            best_level_bids.pop();
            // If ask level is now empty then erase level
            if (!best_level_bids.size())
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

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";
        order.status_ = OrderStatus::OPEN; // Update Order Status
        
        // Notification
        if (!verbose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << "[OPEN] | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << order.time_ << std::endl;
    }

    // Notify of what Orders were filled
    void notify_fill(OrderId id, Quantity qty_filled)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Filled Order");

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string status = !order.qty_ ? "[FILLED]" : "[PARTIALLY FILLED]";
        const std::string ticker_msg = "[" + ticker_ + "]";
        if (!order.qty_)
            order.status_ = OrderStatus::FILLED; // Update Order Status

        // Time Order was Filled
        const std::time_t current_time = std::time(0);
        
        // Notification
        if (!verbose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << status << " | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << qty_filled << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were canceled
    void notify_cancel(OrderId id)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Cancelled Order");

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";
        order.status_ = OrderStatus::CANCELLED; // Update Order Status

        // Time Order was Canceled
        const std::time_t current_time = std::time(0);
        
        // Notification
        if (!verbose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << "[CANCELED] | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were canceled
    void notify_reject(OrderId id, const std::string err)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Rejected Order");

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string reject_msg = "[REJECTED: " + err +  "]";
        const std::string ticker_msg = "[" + ticker_ + "]";
        order.status_ = OrderStatus::REJECTED; // Update Order Status

        // Time Order was Rejected
        const std::time_t current_time = std::time(0);
        
        // Notification
        if (!verbose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << reject_msg << " | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were modified
    void notify_modify(OrderId id)
    {
        if (order_table_.find(id) == order_table_.end()) 
            throw std::runtime_error("Could Not Find Modified Order");

        OrderInfo& order = order_pool_[order_table_.at(id)];
        const std::string side = order.side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order.type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";
        order.status_ = OrderStatus::OPEN; // Update Order Status
        
        // Time Order was Modified
        const std::time_t current_time = std::time(0);
        
        // Notification
        if (!verbose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << "[MODIFIED] | " << "TYPE: " << type << " | ID: " << order.id_ << " | SIDE: " << side << 
        " | QTY: " << order.qty_ << " | PRICE: " << order.price_ << " | TIME: "  << current_time << std::endl;
    }
};
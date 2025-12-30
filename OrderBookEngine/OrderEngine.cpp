#pragma once
#include "PriceHeap.cpp"
#include "RingBuffer.cpp"
#include "Arena.cpp"
#include <memory>
#include <deque>
#include <random>
#include <thread>
#include <mutex>
#include <atomic>
#include <iostream>
#include <set>
#include <map>

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
using OrderId = const std::uint32_t;
using Price = std::double_t;
using Quantity = std::double_t;

// Order Info
struct OrderInfo
{
    const std::time_t time_;
    Quantity qty_;
    Price price_;
    OrderId id_;
    OrderStatus status_;
    const OrderType type_;
    const OrderSide side_;
    
    OrderInfo(const OrderSide side, const OrderType type, double qty, double price, const unsigned int id) noexcept
    : side_(side), type_(type), status_(OrderStatus::OPEN), qty_(qty), price_(price), id_(id), time_(std::time(nullptr))
    {
    }
};

// Data Structure Aliases
using OrderLevel = RingBuffer<OrderId>;
using LevelMap = std::unordered_map<Price, OrderLevel>;
using OrderMap = std::unordered_map<OrderId, Arena<OrderInfo>::Index>;
using OrderBook = PriceHeap;
class OrderEngine
{
public:
    OrderEngine
    (const std::string& ticker) noexcept
    : engine_running_(true), book_updated_(false), recent_order_id_(0), next_order_id_(1), AsksBook_(true), BidsBook_(false), vebose_(true), ticker_(ticker)
    {
    }

private:
     // Order Book
    PriceHeap AsksBook_; // Asks Order Book
    PriceHeap BidsBook_; // Bids Order Book
    LevelMap AskLevels_; // Asks Price Levels
    LevelMap BidLevels_; // Bids Price Levels
    OrderMap OrderTable_; // Map to all active orders
    unsigned int recent_order_id_; // New Orders ID
    unsigned int next_order_id_; // Next Order ID

    // Concurreny
    std::thread engine_;
    std::mutex order_lock_;
    std::condition_variable order_cv_;
    std::atomic<bool> engine_running_;
    bool book_updated_;

    bool vebose_; // Verbose Mode
    std::string ticker_; // Ticker


};


// Order Matching Engine
class OrderEngine
{
public:
    // Default Constructor
    OrderEngine
    (const std::string& ticker) noexcept
    : engine_running_(true), book_updated_(false), recent_order_id_(0), next_order_id_(1), AsksBook_(true), BidsBook_(false), vebose_(true), ticker_(ticker)
    {
        engine_ = std::thread(&OrderEngine::matching_engine, this);
    } 

    // Verbose Specifier
    OrderEngine
    (const std::string& ticker, bool verbose) noexcept
    : engine_running_(true), book_updated_(false), recent_order_id_(0), next_order_id_(1), AsksBook_(true), BidsBook_(false), vebose_(verbose), ticker_(ticker)
    {
        engine_ = std::thread(&OrderEngine::matching_engine, this);
    } 
   
    ~OrderEngine
    () noexcept
    {
        std::unique_lock<std::mutex> lock(order_lock_); 
        engine_running_ = false;
        book_updated_ = true;
        order_cv_.notify_all();
        order_cv_.wait(lock);
        if (engine_.joinable()) 
            engine_.join(); 
    }

    // POST: Place Limit Order
    unsigned int place_order
    (OrderSide side, OrderType type, double price, double qty) noexcept
    {
        // Mutex
        std::unique_lock<std::mutex> lock(order_lock_);
        
        const unsigned int id = next_order_id_++; // New Order ID

        // New Order
        std::shared_ptr<OrderInfo> new_order;
        switch (type)
        {
            case OrderType::LIMIT: // Limit Order
                {
                    // If Limit Order is above (BID) or below (ASK) best opposing price, then adjust
                    if (side == OrderSide::ASK && BidsBook_.size() && price < BidsBook_.peek())
                        price = BidsBook_.peek(); // Adjust price to best bid
                    else if (side == OrderSide::BID && AsksBook_.size() && price > AsksBook_.peek())
                        price = AsksBook_.peek(); // Adjust price to best ask
                    new_order = std::make_shared<OrderInfo>(side, OrderType::LIMIT, qty, price, id);
                    break;
                }

           case OrderType::MARKET: // Market Order
                {
                    // If Market Order, then get best opposing price
                    price = side == OrderSide::ASK ? BidsBook_.peek() : AsksBook_.peek();
                    new_order = std::make_shared<OrderInfo>(side, OrderType::MARKET, qty, price, id);
                    break;
                }
                
            default:
                return 0; // Invalid Order Type
        }
        
        OrderTable_[id] = new_order; // Key New Order

        // Valid Market
        if (type == OrderType::MARKET) 
        {
            if (side == OrderSide::ASK && !BidsBook_.size())
            {
                notify_reject(id, "NO MARKET LIQUIDITY (BIDS)");
                return 0; // No bids to match with
            }
            if (side == OrderSide::BID && !AsksBook_.size())
            {
                notify_reject(id, "NO MARKET LIQUIDITY (ASKS)");
                return 0; // No asks to match with
            }
        }
        
        // Place Order
        switch (side)
        {
            case OrderSide::ASK:
                {
                    // Create new ask price level if no price level
                    if (AsksBook_.find(price) == -1)
                    {
                        AsksBook_.push(price);
                        AskLevels_[price] = OrderLevel();
                    }
                    AskLevels_[price].push_back(new_order);
                    break;
                }
            
            case OrderSide::BID:
                {
                    // Create new bid price level if no price level
                    if (BidsBook_.find(price) == -1)
                    {
                        BidsBook_.push(price);
                        BidLevels_[price] = OrderLevel();
                    }
                    BidLevels_[price].push_back(new_order);
                    break;
                }
            
            default:
                return 0; // Invalid Order Side
        }

        // Notifiy Open
        notify_open(id);
        recent_order_id_ = id;

        // Notify Matching Engine
        book_updated_ = true;
        order_cv_.notify_all();
        order_cv_.wait(lock, [this]{ 
            return !book_updated_; 
        }); // Wait for matching engine
        return id; // Return Order ID
    }

    // POST: Cancel Order
    bool cancel_order
    (const unsigned int id) noexcept
    {
        // Mutex
        std::unique_lock<std::mutex> lock(order_lock_);
 
        if (OrderTable_.find(id) == OrderTable_.end()) 
            return false; // Order does not exist;
        
        auto& order = OrderTable_[id];
        if (order->status_ != OrderStatus::OPEN || order->type_ != OrderType::LIMIT)
            return false; // Order is not open and not a limit order

        // Get Order Level based on Order Side
        auto& order_level = (order->side_ == OrderSide::BID) ?
        BidLevels_[order->price_] : AskLevels_[order->price_];

        // Iterate thruogh order level filtering out ORDER ID
        OrderLevel new_level;
        for (auto& cur_order : order_level)
        {
            if (cur_order->id_ != id) new_level.push_back(cur_order);
        }
        order_level = std::move(new_level);

        // If Order Level is empty pop from Book and erase Order Level
        if (order_level.empty())
        {
            switch(order->side_)
            {
                case OrderSide::BID:
                {
                    const auto& bid = BidsBook_.find(order->price_);
                    if (bid != -1)
                    {
                        BidsBook_.pop(bid);
                        BidLevels_.erase(order->price_);
                    }
                    break;
                }

                case OrderSide::ASK:
                {
                    const auto& ask = AsksBook_.find(order->price_);
                    if (ask != -1)
                    {
                        AsksBook_.pop(ask);
                        AskLevels_.erase(order->price_);
                    }
                    break;
                }

                default:
                    return false; // Invalid Order Side
            }
        }

        // Notify Cancel
        notify_cancel(id);

        // Notify Matching Engine
        book_updated_ = true;
        order_cv_.notify_all();
        order_cv_.wait(lock, [this]() { return !book_updated_; });
        return true; // Order successfully canceled
    }

    // PATCH: Edit Order
    unsigned int edit_order
    (const unsigned int id, const OrderSide side, const double price, const double qty) noexcept
    {
        if (!cancel_order(id))
            return 0; // If cancel failed
        return place_order(side, OrderType::LIMIT, qty, price);
    }

    // GET: Get Order
    std::shared_ptr<OrderInfo> get_order
    (const unsigned int& id) const noexcept
    { 
            const auto& order = OrderTable_.find(id);
            if (order == OrderTable_.end())
                return nullptr; // Return nullptr if order not found
            return order->second; 
    }

    // GET: Average Price
    double get_price() const noexcept
    {
        if (!BidsBook_.size() && !AsksBook_.size())
            return -1; // If both books are empty
        else if (!BidsBook_.size())
            return AsksBook_.peek(); // If BidBook is empty
        else if (!AsksBook_.size())
            return BidsBook_.peek(); // If AskBook is Empty
        return (AsksBook_.peek() + BidsBook_.peek()) / 2; // Average of best ask and bid
    }

    // GET: Best Ask
    double get_best_ask() const noexcept
    {
        if (!AsksBook_.size()) return -1;
        return AsksBook_.peek();
    }

    // GET: Best Bid
    double get_best_bid() const noexcept
    {
        if (!BidsBook_.size()) return -1;
        return BidsBook_.peek();
    }

    // GET: Orders by Status
    std::vector<std::shared_ptr<OrderInfo>> get_orders_by_status(OrderStatus status) const 
    {
        std::vector<std::shared_ptr<OrderInfo>> result;
        for (const auto& [id, order] : OrderTable_) 
        {
            if (order->status_ == status)
                result.push_back(order);
        }
        return std::move(result);
    }
    
    // GET: Maket Depth
    std::vector<std::pair<double, std::size_t>> get_market_depth(OrderSide side, std::size_t depth = 10) const
    {
        std::vector<std::pair<double, std::size_t>> depth_result;

        switch(side)
        {
            case OrderSide::BID:
                {
                    auto tmp_book = BidsBook_; // Copy BidsBook
                    for (size_t i = 0; i < depth && tmp_book.size(); ++i)
                    {
                        double best_bid = tmp_book.peek(); // Get Best Bid Price
                        const auto& best_level = BidLevels_.at(best_bid); // Get Best Bid Level

                        double total_qty = 0;
                        // Sum up all Quantities on current price level
                        for (auto& order : best_level)
                            total_qty += order->qty_;

                        depth_result.emplace_back(best_bid, total_qty);
                        tmp_book.pop();
                    }
                    break;
                }

            case OrderSide::ASK:
                {
                    auto tmp_book = AsksBook_; // Copy AsksBook
                    for (size_t i = 0; i < depth && tmp_book.size(); ++i)
                    {
                        double best_ask = tmp_book.peek(); // Get Best Ask Price
                        const auto& best_level = AskLevels_.at(best_ask); // Get Best Ask Level

                        double total_qty = 0;
                        // Sum up all Quantities on current price level
                        for (auto& order : best_level)
                            total_qty += order->qty_;

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
    PriceHeap AsksBook_; // Asks Order Book
    PriceHeap BidsBook_; // Bids Order Book
    LevelMap AskLevels_; // Asks Price Levels
    LevelMap BidLevels_; // Bids Price Levels
    OrderMap OrderTable_; // Map to all active orders
    unsigned int recent_order_id_; // New Orders ID
    unsigned int next_order_id_; // Next Order ID

    // Concurreny
    std::thread engine_;
    std::mutex order_lock_;
    std::condition_variable order_cv_;
    std::atomic<bool> engine_running_;
    bool book_updated_;

    bool vebose_; // Verbose Mode
    std::string ticker_; // Ticker

    // Matching Engine
    void matching_engine()
    {
        while (engine_running_.load())
        {
            // Sleep Lock
            std::unique_lock<std::mutex> lock(order_lock_);
            order_cv_.wait(lock, [this]{ 
                    return  !engine_running_ || book_updated_; 
            });
            
            auto is_recent_order = OrderTable_.find(recent_order_id_) != OrderTable_.end();
            auto is_books = AsksBook_.size() && BidsBook_.size();
            // Attempt to Match only if engine is running, there is a recent order placed, and there are orders on both sides
            if (engine_running_ && is_recent_order && is_books)
            {
                // Get Recent Order
                std::shared_ptr<OrderInfo>& recent_order = OrderTable_[recent_order_id_];
                // Match order while order status is Open and there is a qty
                while (recent_order->status_ == OrderStatus::OPEN && recent_order->qty_)
                {
                    // Get best asks and bids
                    const double& best_asks_price = AsksBook_.peek();
                    const double& best_bids_price = BidsBook_.peek();
                    if (AskLevels_.find(best_asks_price) == AskLevels_.end() ||
                        BidLevels_.find(best_bids_price) == BidLevels_.end())
                        break; // No best price level to match with
                        
                    // Get price level for best asks and bids
                    OrderLevel& best_level_asks = AskLevels_[best_asks_price];
                    OrderLevel& best_level_bids = BidLevels_[best_bids_price];
                    if (best_level_asks.empty() || best_level_bids.empty())
                        break; // No best level to match with
                    
                    // Get OrderInfo for best ask and bid
                    std::shared_ptr<OrderInfo>& best_ask = best_level_asks.front();
                    std::shared_ptr<OrderInfo>& best_bid = best_level_bids.front();
                    
                    // Break If you can't trade
                    const bool can_trade_asks = recent_order->side_ == OrderSide::ASK && 
                    !best_level_bids.empty() && best_bid->price_ >= recent_order->price_;
                    const bool can_trade_bids = recent_order->side_ == OrderSide::BID && 
                    !best_level_asks.empty() && best_ask->price_ <= recent_order->price_;
                    if (!(can_trade_asks || can_trade_bids)) 
                        break; // No match possible

                    switch (recent_order->side_)
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
            
            // Notify Order Engine
            book_updated_ = false;
            order_cv_.notify_all();
        }
    }

    // Match Orders
    void matching(std::shared_ptr<OrderInfo>& best_ask, std::shared_ptr<OrderInfo>& best_bid, OrderLevel& best_level_asks, OrderLevel& best_level_bids)
    {   
        // Get qty filled and apply difference
        const double qty_filled = std::min(best_ask->qty_, best_bid->qty_);
        best_ask->qty_ -= qty_filled;
        best_bid->qty_ -= qty_filled;
        
        notify_fill(best_ask->id_, qty_filled);
        notify_fill(best_bid->id_, qty_filled);

        // If ask qty is 0 then notify fill and erase
        if (!best_ask->qty_)
        {
            best_level_asks.pop_front();
            // If ask level is now empty then erase level
            if (!best_level_asks.size())
            {
                AsksBook_.pop();
                AskLevels_.erase(best_ask->price_);
            }
        }

        // If ask qty is 0 then notify fill and erase
        if (!best_bid->qty_)
        {
            best_level_bids.pop_front();
            // If ask level is now empty then erase level
            if (!best_level_bids.size())
            {
                BidsBook_.pop();
                BidLevels_.erase(best_bid->price_);
            }
        }
    }

    // Notify of what Orders are open
    void notify_open(const unsigned int& id)
    {
        if (OrderTable_.find(id) == OrderTable_.end()) 
            throw std::runtime_error("Could Not Find Open Order");

        const std::shared_ptr<OrderInfo>& order = OrderTable_[id];
        const std::string side = order->side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order->type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";
        order->status_ = OrderStatus::OPEN; // Update Order Status
        
        // Notification
        if (!vebose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << "[OPEN] | " << "TYPE: " << type << " | ID: " << order->id_ << " | SIDE: " << side << 
        " | QTY: " << order->qty_ << " | PRICE: " << order->price_ << " | TIME: "  << order->time_ << std::endl;
    }

    // Notify of what Orders were filled
    void notify_fill(const unsigned int& id, const double& qty_filled)
    {
        if (OrderTable_.find(id) == OrderTable_.end()) 
            throw std::runtime_error("Could Not Find Filled Order");

        const std::shared_ptr<OrderInfo>& order = OrderTable_[id];
        const std::string side = order->side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order->type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string status = !order->qty_ ? "[FILLED]" : "[PARTIALLY FILLED]";
        const std::string ticker_msg = "[" + ticker_ + "]";
        if (!order->qty_)
            order->status_ = OrderStatus::FILLED; // Update Order Status

        // Time Order was Filled
        const std::time_t current_time = std::time(0);
        
        // Notification
        if (!vebose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << status << " | " << "TYPE: " << type << " | ID: " << order->id_ << " | SIDE: " << side << 
        " | QTY: " << qty_filled << " | PRICE: " << order->price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were canceled
    void notify_cancel(const unsigned int& id)
    {
        if (OrderTable_.find(id) == OrderTable_.end()) 
            throw std::runtime_error("Could Not Find Cancelled Order");

        const std::shared_ptr<OrderInfo>& order = OrderTable_[id];
        const std::string side = order->side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order->type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string ticker_msg = "[" + ticker_ + "]";
        order->status_ = OrderStatus::CANCELLED; // Update Order Status

        // Time Order was Canceled
        const std::time_t current_time = std::time(0);
        
        // Notification
        if (!vebose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << "[CANCELED] | " << "TYPE: " << type << " | ID: " << order->id_ << " | SIDE: " << side << 
        " | QTY: " << order->qty_ << " | PRICE: " << order->price_ << " | TIME: "  << current_time << std::endl;
    }

    // Notify of what Orders were canceled
    void notify_reject(const unsigned int& id, const std::string err)
    {
        if (OrderTable_.find(id) == OrderTable_.end()) 
            throw std::runtime_error("Could Not Find Rejected Order");

        const std::shared_ptr<OrderInfo>& order = OrderTable_[id];
        const std::string side = order->side_ == OrderSide::BID ? "BUY" : "SELL";
        const std::string type = order->type_ == OrderType::LIMIT ? "LIMIT" : "MARKET";
        const std::string reject_msg = "[REJECTED: " + err +  "]";
        const std::string ticker_msg = "[" + ticker_ + "]";
        order->status_ = OrderStatus::REJECTED; // Update Order Status

        // Time Order was Rejected
        const std::time_t current_time = std::time(0);
        
        // Notification
        if (!vebose_) return; // If not verbose, do not notify
        std::cout << ticker_msg << " | " << reject_msg << " | " << "TYPE: " << type << " | ID: " << order->id_ << " | SIDE: " << side << 
        " | QTY: " << order->qty_ << " | PRICE: " << order->price_ << " | TIME: "  << current_time << std::endl;
    }
};

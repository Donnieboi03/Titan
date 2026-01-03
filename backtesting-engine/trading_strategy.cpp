#pragma once
#include "order_engine.cpp"

// Forward declaration
class EngineRuntime;

// Strategy callback interface
// Inherit from this to create custom trading strategies that react to order book updates
struct TradingStrategy
{
    virtual ~TradingStrategy() = default;
    
    // Called on each book update with read-only access to engine
    // @param ticker - The stock ticker symbol
    // @param engine - Const pointer to OrderEngine for reading book data (thread-safe reads)
    // @param runtime - Pointer to EngineRuntime for submitting new orders
    virtual void on_book_update(const std::string& ticker, const OrderEngine* engine, EngineRuntime* runtime) = 0;
    
    // Called when one of your orders gets filled
    // @param ticker - The stock ticker symbol
    // @param order_id - The order that was filled
    // @param price - The execution price
    // @param qty - The quantity filled
    virtual void on_fill(const std::string& ticker, OrderId order_id, Price price, Quantity qty) = 0;
    
    // Optional: Called when an order is canceled
    virtual void on_cancel(const std::string& ticker, OrderId order_id)
    {
        // Default implementation: do nothing
    }
    
    // Optional: Called when an order is rejected
    virtual void on_reject(const std::string& ticker, OrderId order_id, const std::string& reason)
    {
        // Default implementation: do nothing
    }
};

// Example Strategy: Simple Market Maker
// Creates buy and sell orders around the mid price
class MarketMakerStrategy : public TradingStrategy
{
public:
    MarketMakerStrategy(double spread = 0.10, Quantity quote_size = 100, int max_position = 1000)
        : spread_(spread), quote_size_(quote_size), max_position_(max_position), position_(0) {}
    
    void on_book_update(const std::string& ticker, const OrderEngine* engine, EngineRuntime* runtime) override
    {
        // Read current book state (const access is thread-safe)
        Price best_bid = engine->get_best_bid();
        Price best_ask = engine->get_best_ask();
        
        // Check if book has liquidity
        if (best_bid <= 0 || best_ask <= 0)
            return;
        
        // Calculate mid price
        Price mid = (best_bid + best_ask) / 2.0;
        
        // Calculate our quote prices (spread around mid)
        Price our_bid = mid - spread_ / 2.0;
        Price our_ask = mid + spread_ / 2.0;
        
        // Position management: don't exceed max position
        if (position_ < max_position_)
        {
            // Submit buy order
            OrderId bid_result = -1;
            runtime->limit_order(ticker, OrderSide::BID, our_bid, quote_size_, &bid_result);
            active_bids_.push_back(bid_result);
        }
        
        if (position_ > -max_position_)
        {
            // Submit sell order
            OrderId ask_result = -1;
            runtime->limit_order(ticker, OrderSide::ASK, our_ask, quote_size_, &ask_result);
            active_asks_.push_back(ask_result);
        }
        
        // Execute batch asynchronously
        runtime->execute_batch();
    }
    
    void on_fill(const std::string& ticker, OrderId order_id, Price price, Quantity qty) override
    {
        // Update position based on fill
        // Check if it was a buy or sell
        bool is_bid = std::find(active_bids_.begin(), active_bids_.end(), order_id) != active_bids_.end();
        
        if (is_bid)
        {
            position_ += qty;  // Bought: increase position
            std::cout << "[MM] Bought " << qty << " @ " << price << ", Position: " << position_ << std::endl;
        }
        else
        {
            position_ -= qty;  // Sold: decrease position
            std::cout << "[MM] Sold " << qty << " @ " << price << ", Position: " << position_ << std::endl;
        }
        
        // Calculate PnL
        total_volume_ += qty;
        realized_pnl_ += (price - avg_fill_price_) * qty;
        
        std::cout << "[MM] Total Volume: " << total_volume_ << ", PnL: $" << realized_pnl_ << std::endl;
    }
    
    void on_cancel(const std::string& ticker, OrderId order_id) override
    {
        // Remove from active orders
        active_bids_.erase(std::remove(active_bids_.begin(), active_bids_.end(), order_id), active_bids_.end());
        active_asks_.erase(std::remove(active_asks_.begin(), active_asks_.end(), order_id), active_asks_.end());
    }
    
    // Getters for monitoring
    int get_position() const { return position_; }
    double get_pnl() const { return realized_pnl_; }
    double get_total_volume() const { return total_volume_; }
    
private:
    double spread_;              // Bid-ask spread to quote
    Quantity quote_size_;        // Size of each quote
    int max_position_;           // Maximum position (long or short)
    int position_;               // Current position (positive = long, negative = short)
    double realized_pnl_;        // Realized profit/loss
    double total_volume_;        // Total volume traded
    double avg_fill_price_;      // Average fill price for PnL calculation
    
    std::vector<OrderId> active_bids_;  // Track active buy orders
    std::vector<OrderId> active_asks_;  // Track active sell orders
};

// Example Strategy: Momentum Trader
// Buys when price is rising, sells when falling
class MomentumStrategy : public TradingStrategy
{
public:
    MomentumStrategy(int lookback = 10, double threshold = 0.5, Quantity order_size = 50)
        : lookback_(lookback), threshold_(threshold), order_size_(order_size), position_(0) {}
    
    void on_book_update(const std::string& ticker, const OrderEngine* engine, EngineRuntime* runtime) override
    {
        Price mid = (engine->get_best_bid() + engine->get_best_ask()) / 2.0;
        
        if (mid <= 0)
            return;
        
        // Store price history
        price_history_.push_back(mid);
        if (price_history_.size() > lookback_)
            price_history_.erase(price_history_.begin());
        
        // Need enough history
        if (price_history_.size() < lookback_)
            return;
        
        // Calculate momentum (price change over lookback period)
        double momentum = (price_history_.back() - price_history_.front()) / price_history_.front() * 100.0;
        
        OrderId result = -1;
        
        // Positive momentum: buy
        if (momentum > threshold_ && position_ <= 0)
        {
            runtime->limit_order(ticker, OrderSide::BID, mid, order_size_, &result);
            std::cout << "[Momentum] BUY signal, momentum: " << momentum << "%" << std::endl;
        }
        // Negative momentum: sell
        else if (momentum < -threshold_ && position_ >= 0)
        {
            runtime->limit_order(ticker, OrderSide::ASK, mid, order_size_, &result);
            std::cout << "[Momentum] SELL signal, momentum: " << momentum << "%" << std::endl;
        }
        
        runtime->execute_batch();
    }
    
    void on_fill(const std::string& ticker, OrderId order_id, Price price, Quantity qty) override
    {
        std::cout << "[Momentum] Filled " << qty << " @ " << price << std::endl;
        position_ += qty;  // Simplified: assume all buys are positive
    }
    
private:
    int lookback_;                    // Number of price samples for momentum
    double threshold_;                // Momentum threshold to trigger trade
    Quantity order_size_;             // Size of each order
    int position_;                    // Current position
    std::vector<Price> price_history_; // Historical mid prices
};

// Example Strategy: Arbitrage (for multiple exchanges/symbols)
class ArbitrageStrategy : public TradingStrategy
{
public:
    ArbitrageStrategy(const std::string& symbol_a, const std::string& symbol_b, double spread_threshold = 0.5)
        : symbol_a_(symbol_a), symbol_b_(symbol_b), spread_threshold_(spread_threshold) {}
    
    void on_book_update(const std::string& ticker, const OrderEngine* engine, EngineRuntime* runtime) override
    {
        // Store prices for both symbols
        Price mid = (engine->get_best_bid() + engine->get_best_ask()) / 2.0;
        
        if (ticker == symbol_a_)
            price_a_ = mid;
        else if (ticker == symbol_b_)
            price_b_ = mid;
        
        // Need prices for both symbols
        if (price_a_ <= 0 || price_b_ <= 0)
            return;
        
        // Calculate spread
        double spread = std::abs(price_a_ - price_b_) / ((price_a_ + price_b_) / 2.0) * 100.0;
        
        // Trade if spread exceeds threshold
        if (spread > spread_threshold_)
        {
            OrderId result_a = -1, result_b = -1;
            
            if (price_a_ > price_b_)
            {
                // Sell A, Buy B
                runtime->limit_order(symbol_a_, OrderSide::ASK, price_a_, 10, &result_a);
                runtime->limit_order(symbol_b_, OrderSide::BID, price_b_, 10, &result_b);
                std::cout << "[Arbitrage] Sell " << symbol_a_ << " @ " << price_a_ 
                          << ", Buy " << symbol_b_ << " @ " << price_b_ << std::endl;
            }
            else
            {
                // Buy A, Sell B
                runtime->limit_order(symbol_a_, OrderSide::BID, price_a_, 10, &result_a);
                runtime->limit_order(symbol_b_, OrderSide::ASK, price_b_, 10, &result_b);
                std::cout << "[Arbitrage] Buy " << symbol_a_ << " @ " << price_a_ 
                          << ", Sell " << symbol_b_ << " @ " << price_b_ << std::endl;
            }
            
            runtime->execute_batch();
        }
    }
    
    void on_fill(const std::string& ticker, OrderId order_id, Price price, Quantity qty) override
    {
        std::cout << "[Arbitrage] Filled " << ticker << ": " << qty << " @ " << price << std::endl;
    }
    
private:
    std::string symbol_a_;
    std::string symbol_b_;
    double spread_threshold_;
    Price price_a_ = 0.0;
    Price price_b_ = 0.0;
};

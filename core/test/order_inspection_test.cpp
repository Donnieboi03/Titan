#include <iostream>
#include <iomanip>
#include <cassert>
#include "../order_engine.cpp"
#include "../engine_runtime.cpp"

using namespace backtest::runtime;
using namespace backtest::user;
using namespace engine;

// Example: Strategy that inspects and manages its own orders
void order_managing_strategy(User* user) {
    auto tickers = user->list_tickers();
    
    for (const auto& ticker : tickers) {
        double bid = user->get_best_bid(ticker);
        double ask = user->get_best_ask(ticker);
        
        if (bid <= 0 || ask <= 0) continue;
        
        double mid = (bid + ask) / 2.0;
        double spread = ask - bid;
        
        // Get all our open orders for this ticker
        auto order_ids = user->get_positions(ticker);
        
        // Cancel orders that are too far from mid price
        for (const auto& order_id : order_ids) {
            const OrderInfo* info = user->get_order_info(ticker, order_id);
            
            // Check if order exists and is open
            if (!info || info->status_ != OrderStatus::OPEN) continue;
            
            // Convert price from ticks to dollars
            double order_price = backtest::math::ticks_to_dollars(info->price_);
            double distance_from_mid = std::abs(order_price - mid);
            
            // Cancel if order is more than 1% away from mid
            if (distance_from_mid > mid * 0.01) {
                user->submit_cancel_order(ticker, order_id);
                std::cout << "Cancelled order #" << order_id 
                          << " at $" << order_price 
                          << " (mid: $" << mid << ")\n";
            }
        }
        
        // Place new orders if we don't have too many
        if (order_ids.size() < 10) {
            // Place bid slightly below mid
            user->submit_limit_order(ticker, OrderSide::BID, mid - spread * 0.25, 1.0);
            // Place ask slightly above mid
            user->submit_limit_order(ticker, OrderSide::ASK, mid + spread * 0.25, 1.0);
        }
    }
}

// Example: Strategy that only trades when it has no open orders
void cautious_strategy(User* user) {
    auto tickers = user->list_tickers();
    
    for (const auto& ticker : tickers) {
        auto order_ids = user->get_positions(ticker);
        
        // Count open orders
        int open_orders = 0;
        for (const auto& order_id : order_ids) {
            const OrderInfo* info = user->get_order_info(ticker, order_id);
            if (info && info->status_ == OrderStatus::OPEN) {
                open_orders++;
            }
        }
        
        // Only place new orders if we have no open orders
        if (open_orders == 0) {
            double bid = user->get_best_bid(ticker);
            double ask = user->get_best_ask(ticker);
            
            if (bid > 0 && ask > 0) {
                double mid = (bid + ask) / 2.0;
                user->submit_limit_order(ticker, OrderSide::BID, mid * 0.99, 1.0);
                std::cout << "Placed order at $" << (mid * 0.99) << "\n";
            }
        }
    }
}

void test_order_inspection() {
    std::cout << "=== Order Inspection Test ===\n\n";
    
    // Setup runtime
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(1, 1048576, false, 1000);
    
    // Register stock
    std::string ticker = "TEST";
    runtime.register_stock(std::string(ticker), 100.0, 1000.0);
    
    // Register strategy for this ticker
    User* manager = runtime.register_strategy(std::string(ticker), order_managing_strategy, 100000.0);
    
    std::cout << "Registered strategy: Manager (User " << manager->get_user_id() << ")\n\n";
    
    // Enable matching so orders can interact
    runtime.set_auto_match(ticker, true);
    
    // Manager places some initial orders
    manager->submit_limit_order(ticker, OrderSide::BID, 99.0, 10.0);
    manager->submit_limit_order(ticker, OrderSide::BID, 98.5, 10.0);
    manager->submit_limit_order(ticker, OrderSide::ASK, 101.0, 10.0);
    manager->submit_limit_order(ticker, OrderSide::ASK, 102.0, 10.0);
    
    runtime.process_pending_orders();
    
    std::cout << "Manager placed 4 initial orders\n";
    std::cout << "Best Bid: $" << runtime.get_best_bid(ticker) << "\n";
    std::cout << "Best Ask: $" << runtime.get_best_ask(ticker) << "\n\n";
    
    // Inspect the orders
    auto order_ids = manager->get_positions(ticker);
    std::cout << "Manager has " << order_ids.size() << " orders:\n";
    
    for (const auto& order_id : order_ids) {
        const OrderInfo* info = manager->get_order_info(ticker, order_id);
        if (info) {
            double price = backtest::math::ticks_to_dollars(info->price_);
            std::cout << "  Order #" << order_id << ": "
                      << (info->side_ == OrderSide::BID ? "BID" : "ASK")
                      << " $" << std::fixed << std::setprecision(2) << price
                      << " qty=" << info->qty_
                      << " status=";
            
            switch (info->status_) {
                case OrderStatus::OPEN: std::cout << "OPEN"; break;
                case OrderStatus::FILLED: std::cout << "FILLED"; break;
                case OrderStatus::CANCELLED: std::cout << "CANCELLED"; break;
                default: std::cout << "UNKNOWN";
            }
            std::cout << "\n";
        }
    }
    std::cout << "\n";
    
    // Trigger manager strategy (should cancel far orders and place new ones)
    std::cout << "--- Running Manager Strategy ---\n";
    manager->on_book_update();
    runtime.process_pending_orders();
    
    // Inspect orders again
    order_ids = manager->get_positions(ticker);
    std::cout << "\nAfter strategy execution, Manager has " << order_ids.size() << " orders:\n";
    
    for (const auto& order_id : order_ids) {
        const OrderInfo* info = manager->get_order_info(ticker, order_id);
        if (info) {
            double price = backtest::math::ticks_to_dollars(info->price_);
            std::cout << "  Order #" << order_id << ": "
                      << (info->side_ == OrderSide::BID ? "BID" : "ASK")
                      << " $" << std::fixed << std::setprecision(2) << price
                      << " qty=" << info->qty_;
            
            switch (info->status_) {
                case OrderStatus::OPEN: std::cout << " [OPEN]"; break;
                case OrderStatus::FILLED: std::cout << " [FILLED]"; break;
                case OrderStatus::CANCELLED: std::cout << " [CANCELLED]"; break;
                default: std::cout << " [UNKNOWN]";
            }
            std::cout << "\n";
        }
    }
    
    // Final statistics
    std::cout << "\n=== Final Statistics ===\n";
    std::cout << "Manager:\n";
    std::cout << "  Capital: $" << manager->get_capital() << "\n";
    std::cout << "  Position: " << manager->get_position(ticker) << " shares\n";
    std::cout << "  Realized P&L: $" << manager->get_realized_pnl() << "\n";
    std::cout << "  Total Volume: $" << manager->get_total_volume() << "\n\n";
    
    std::cout << "✓ Order inspection test completed!\n";
    std::cout << "✓ Successfully demonstrated get_order_info() for order state inspection\n";
}

int main() {
    try {
        test_order_inspection();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

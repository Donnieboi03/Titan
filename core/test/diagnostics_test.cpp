#include <iostream>
#include <iomanip>
#include <cassert>
#include "../order_engine.cpp"
#include "../engine_runtime.cpp"

using namespace backtest::runtime;
using namespace backtest::user;
using namespace engine;

void test_diagnostics() {
    std::cout << "=== Engine Diagnostics Test ===\n\n";
    
    // Setup runtime with specific capacity
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(1, 1048576, false, 1000);
    
    // Register stock
    std::string ticker = "AAPL";
    runtime.register_stock(std::string(ticker), 150.0, 1000.0);
    
    // Register strategy with capital for this ticker
    User* trader = runtime.register_strategy(std::string(ticker), [](User* user) {
        auto tickers = user->list_tickers();
        for (const auto& t : tickers) {
            double bid = user->get_best_bid(t);
            if (bid > 0) {
                user->submit_limit_order(t, OrderSide::BID, bid - 1.0, 10.0);
            }
        }
    }, 100000.0);
    
    std::cout << "Initial Diagnostics:\n";
    std::cout << "  Capacity: " << runtime.get_capacity(ticker) << " orders\n";
    std::cout << "  Utilization: " << runtime.get_utilization(ticker) << " active\n";
    std::cout << "  Pending: " << runtime.get_pending_count(ticker) << " queued\n";
    std::cout << "  Placed: " << runtime.get_placed_count(ticker) << "\n";
    std::cout << "  Open: " << runtime.get_open_count(ticker) << "\n\n";
    
    // Submit orders
    std::cout << "Submitting 10 limit orders...\n";
    for (int i = 0; i < 10; i++) {
        trader->submit_limit_order(ticker, OrderSide::BID, 148.0 + i * 0.1, 5.0);
    }
    
    std::cout << "After submission (before processing):\n";
    std::cout << "  Pending: " << runtime.get_pending_count(ticker) << " queued\n\n";
    
    // Process orders
    runtime.process_pending_orders();
    
    std::cout << "After processing:\n";
    std::cout << "  Utilization: " << runtime.get_utilization(ticker) << " active\n";
    std::cout << "  Pending: " << runtime.get_pending_count(ticker) << " queued\n";
    std::cout << "  Placed: " << runtime.get_placed_count(ticker) << "\n";
    std::cout << "  Open: " << runtime.get_open_count(ticker) << "\n\n";
    
    // Get order IDs and test order_exists
    auto order_ids = trader->get_active_orders(ticker);
    if (!order_ids.empty()) {
        OrderId first_order = order_ids[0];
        std::cout << "Testing order_exists():\n";
        std::cout << "  Order #" << first_order << " exists: " 
                  << (runtime.order_exists(ticker, first_order) ? "YES" : "NO") << "\n";
        std::cout << "  Order #999999 exists: " 
                  << (runtime.order_exists(ticker, 999999) ? "YES" : "NO") << "\n\n";
        
        // Cancel an order
        std::cout << "Cancelling order #" << first_order << "...\n";
        trader->submit_cancel_order(ticker, first_order);
        runtime.process_pending_orders();
        
        std::cout << "After cancellation:\n";
        std::cout << "  Order #" << first_order << " exists: " 
                  << (runtime.order_exists(ticker, first_order) ? "YES" : "NO") << "\n";
        std::cout << "  Open: " << runtime.get_open_count(ticker) << "\n";
        std::cout << "  Cancelled: " << runtime.get_cancelled_count(ticker) << "\n\n";
    }
    
    // Calculate utilization percentage
    double utilization_pct = (runtime.get_utilization(ticker) * 100.0) / runtime.get_capacity(ticker);
    std::cout << "=== Final Metrics ===\n";
    std::cout << "  Capacity: " << runtime.get_capacity(ticker) << "\n";
    std::cout << "  Utilization: " << runtime.get_utilization(ticker) 
              << " (" << std::fixed << std::setprecision(4) << utilization_pct << "%)\n";
    std::cout << "  Placed: " << runtime.get_placed_count(ticker) << "\n";
    std::cout << "  Cancelled: " << runtime.get_cancelled_count(ticker) << "\n";
    std::cout << "  Filled: " << runtime.get_filled_count(ticker) << "\n";
    std::cout << "  Open: " << runtime.get_open_count(ticker) << "\n\n";
    
    std::cout << "✓ Diagnostics test completed!\n";
}

int main() {
    try {
        test_diagnostics();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

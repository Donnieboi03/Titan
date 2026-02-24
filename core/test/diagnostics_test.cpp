#include <iostream>
#include <iomanip>
#include <cassert>
#include <algorithm>
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
    
    runtime.request_snapshot(ticker);
    runtime.process_pending_orders();
    const auto* snap = runtime.get_snapshot(ticker);
    std::cout << "Initial Diagnostics:\n";
    std::cout << "  Capacity: " << runtime.get_capacity(ticker) << " orders\n";
    std::cout << "  Utilization: " << (snap ? snap->open_count : 0) << " active\n";
    std::cout << "  Pending: " << runtime.get_pending_count(ticker) << " queued\n";
    std::cout << "  Placed: " << (snap ? snap->placed_count : 0) << "\n";
    std::cout << "  Open: " << (snap ? snap->open_count : 0) << "\n\n";
    
    // Submit orders
    std::cout << "Submitting 10 limit orders...\n";
    for (int i = 0; i < 10; i++) {
        trader->submit_limit_order(ticker, OrderSide::BID, 148.0 + i * 0.1, 5.0);
    }
    
    std::cout << "After submission (before processing):\n";
    std::cout << "  Pending: " << runtime.get_pending_count(ticker) << " queued\n\n";
    
    // Process orders
    runtime.request_snapshot(ticker);
    runtime.process_pending_orders();
    
    snap = runtime.get_snapshot(ticker);
    std::cout << "After processing:\n";
    std::cout << "  Utilization: " << (snap ? snap->open_count : 0) << " active\n";
    std::cout << "  Pending: " << runtime.get_pending_count(ticker) << " queued\n";
    std::cout << "  Placed: " << (snap ? snap->placed_count : 0) << "\n";
    std::cout << "  Open: " << (snap ? snap->open_count : 0) << "\n\n";
    
    // Get order IDs and test presence via get_active_orders
    auto order_ids = trader->get_active_orders(ticker);
    if (!order_ids.empty()) {
        OrderId first_order = order_ids[0];
        std::cout << "Testing order presence (get_active_orders):\n";
        auto active = trader->get_active_orders(ticker);
        bool first_in_list = std::find(active.begin(), active.end(), first_order) != active.end();
        std::cout << "  Order #" << first_order << " in active list: "
                  << (first_in_list ? "YES" : "NO") << "\n";
        bool fake_in_list = std::find(active.begin(), active.end(), 999999) != active.end();
        std::cout << "  Order #999999 in active list: "
                  << (fake_in_list ? "YES" : "NO") << "\n\n";
        
        // Cancel an order
        std::cout << "Cancelling order #" << first_order << "...\n";
        trader->submit_cancel_order(ticker, first_order);
        runtime.request_snapshot(ticker);
        runtime.process_pending_orders();
        
        snap = runtime.get_snapshot(ticker);
        active = trader->get_active_orders(ticker);
        bool still_in_list = std::find(active.begin(), active.end(), first_order) != active.end();
        std::cout << "After cancellation:\n";
        std::cout << "  Order #" << first_order << " in active list: "
                  << (still_in_list ? "YES" : "NO") << "\n";
        std::cout << "  Open: " << (snap ? snap->open_count : 0) << "\n";
        std::cout << "  Cancelled: " << (snap ? snap->cancelled_count : 0) << "\n\n";
    }
    
    runtime.request_snapshot(ticker);
    runtime.process_pending_orders();
    snap = runtime.get_snapshot(ticker);
    std::size_t open_count = snap ? snap->open_count : 0;
    double utilization_pct = runtime.get_capacity(ticker) != 0 ? (open_count * 100.0) / runtime.get_capacity(ticker) : 0.0;
    std::cout << "=== Final Metrics ===\n";
    std::cout << "  Capacity: " << runtime.get_capacity(ticker) << "\n";
    std::cout << "  Utilization: " << open_count
              << " (" << std::fixed << std::setprecision(4) << utilization_pct << "%)\n";
    std::cout << "  Placed: " << (snap ? snap->placed_count : 0) << "\n";
    std::cout << "  Cancelled: " << (snap ? snap->cancelled_count : 0) << "\n";
    std::cout << "  Filled: " << (snap ? snap->filled_count : 0) << "\n";
    std::cout << "  Open: " << (snap ? snap->open_count : 0) << "\n\n";
    
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

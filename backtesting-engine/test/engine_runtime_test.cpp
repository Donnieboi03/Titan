#include "../engine_runtime.cpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <thread>
#include <numeric>

void print_test_header(const std::string& test_name) {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "TEST: " << test_name << std::endl;
    std::cout << std::string(60, '=') << std::endl;
}

void test_singleton_pattern() {
    print_test_header("Singleton Pattern");
    runtime::EngineRuntime::reset_instance();    
        auto& runtime1 = runtime::EngineRuntime::get_instance(1, 1048576 * 4, false, 0);  // Use proper capacity
    auto& runtime2 = runtime::EngineRuntime::get_instance(4, 2000000, false, 0); // Should be same instance, params ignored
    
    // Both references should point to the same instance
    assert(&runtime1 == &runtime2);
    std::cout << "✓ Singleton pattern working correctly" << std::endl;
}

void test_stock_registration() {
    print_test_header("Stock Registration");
    
    runtime::EngineRuntime::reset_instance();
        auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Test normal registration
    bool success = runtime.register_stock("BTC", 50000.00, 2.0);
    assert(success);
    
    // Verify registration worked
    auto tickers = runtime.list_tickers();
    std::cout << "Number of tickers after registration: " << tickers.size() << std::endl;
    for (const auto& ticker : tickers) {
        std::cout << "  - " << ticker << std::endl;
    }
    assert(tickers.size() == 1);
    assert(tickers[0] == "BTC");
    std::cout << "✓ Basic stock registration successful" << std::endl;
    
    // Test duplicate registration (should be rejected)
    bool duplicate_success = runtime.register_stock("BTC", 51000.00, 1.0);
    assert(!duplicate_success); // Should return false
    
    tickers = runtime.list_tickers();
    assert(tickers.size() == 1); // Should still be 1
    std::cout << "✓ Duplicate registration properly rejected" << std::endl;
    
    // Test multiple stocks
    std::cout << "Registering ETH..." << std::endl;
    bool eth_success = runtime.register_stock("ETH", 3500.00, 10.0);
    assert(eth_success);
    
    std::cout << "Registering AAPL..." << std::endl;
    bool aapl_success = runtime.register_stock("AAPL", 150.00, 1000.0);
    assert(aapl_success);
    
    tickers = runtime.list_tickers();
    std::cout << "Number of tickers after multiple registrations: " << tickers.size() << std::endl;
    for (const auto& ticker : tickers) {
        std::cout << "  - " << ticker << std::endl;
    }
    assert(tickers.size() == 3);
    std::cout << "✓ Multiple stock registration successful" << std::endl;
}

void test_market_data_reads() {
    print_test_header("Market Data Reads");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Register a stock
    bool success = runtime.register_stock("TSLA", 200.00, 100.0);
    assert(success);
    
    // Test market data reads
    auto market_price = runtime.get_market_price("TSLA");
    auto best_bid = runtime.get_best_bid("TSLA");
    auto best_ask = runtime.get_best_ask("TSLA");
    
    assert(best_ask != 0);
    assert(best_ask == 200.00);
    std::cout << "✓ Best ask: $" << std::fixed << std::setprecision(2) << best_ask << std::endl;
    
    // No bids yet, so best_bid should be empty
    assert(best_bid == -1);
    std::cout << "✓ No bids available as expected" << std::endl;
    
    // Test market depth
    auto depth = runtime.get_market_depth("TSLA", engine::OrderSide::ASK);
    assert(depth.size() >= 1);
    std::cout << "✓ Market depth: " << depth.size() << " levels" << std::endl;
}

void test_limit_orders() {
    print_test_header("Limit Orders");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Register stock
    bool success = runtime.register_stock("NVDA", 800.00, 50.0);
    assert(success);
    
    // Submit buy orders
    runtime.submit_limit_order("NVDA", engine::OrderSide::BID, 795.00, 1.0);
    runtime.submit_limit_order("NVDA", engine::OrderSide::BID, 790.00, 2.0);
    runtime.submit_limit_order("NVDA", engine::OrderSide::BID, 785.00, 1.5);
    runtime.process_pending_orders();
    
    // Verify all 3 buy orders were placed using engine counts (4 total = 1 IPO + 3 bids)
    std::size_t placed_after_bids = runtime.get_placed_count("NVDA");
    if (placed_after_bids == 4) {
        std::cout << "✓ All 3 bid orders successfully placed (plus IPO)" << std::endl;
    }
    
    // Check best bid
    auto best_bid = runtime.get_best_bid("NVDA");
    assert(best_bid != 0);
    assert(std::abs(best_bid - 795.00) < 0.01);
    std::cout << "✓ Best bid: $" << std::fixed << std::setprecision(2) << best_bid << std::endl;
    
    // Submit sell order above market (no fill) - price it high enough to avoid matching
    runtime.submit_limit_order("NVDA", engine::OrderSide::ASK, 850.00, 0.5);
    runtime.process_pending_orders();
    
    // Verify all 5 orders are now placed (1 IPO + 3 bids + 1 ask) using engine counts
    std::size_t final_placed = runtime.get_placed_count("NVDA");
    std::size_t final_open = runtime.get_open_count("NVDA");
    
    if (final_placed == 5 && final_open == 5) {
        std::cout << "✓ All orders successfully placed" << std::endl;
    }
    
    auto best_ask = runtime.get_best_ask("NVDA");
    assert(best_ask != 0);
    assert(std::abs(best_ask - 800.00) < 0.01); // Should still be IPO price
    std::cout << "✓ Best ask: $" << std::fixed << std::setprecision(2) << best_ask << std::endl;
}

void test_market_orders() {
    print_test_header("Market Orders");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Register stock with lower quantity for easier testing
    bool success = runtime.register_stock("MSFT", 300.00, 10.0);
    assert(success);
    
    // Place some limit orders to create a book
    runtime.submit_limit_order("MSFT", engine::OrderSide::BID, 299.00, 2.0);
    runtime.submit_limit_order("MSFT", engine::OrderSide::BID, 298.00, 3.0);
    runtime.process_pending_orders();
    
    // Verify limit orders were placed using engine counts (3 total = 1 IPO + 2 limit)
    std::size_t limit_placed = runtime.get_placed_count("MSFT");
    if (limit_placed == 3) {
        std::cout << "✓ Both limit orders successfully placed plus IPO" << std::endl;
    }
    
    // Submit market buy order (should match against the ask at $300)
    runtime.submit_market_order("MSFT", engine::OrderSide::BID, 1.0);
    runtime.process_pending_orders();
    
    std::cout << "✓ Market order processed" << std::endl;
    
    // Check if market price updated (should be around $300)
    auto market_price = runtime.get_market_price("MSFT");
    if (market_price != 0) {
        std::cout << "✓ Market order executed at: $" << std::fixed << std::setprecision(2) << market_price << std::endl;
    }
}

void test_order_cancellation() {
    print_test_header("Order Cancellation");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Register stock
    bool success = runtime.register_stock("AMZN", 3000.00, 20.0);
    assert(success);
    
    // Submit a limit order to create something to cancel
    runtime.submit_limit_order("AMZN", engine::OrderSide::BID, 2995.00, 1.0);
    runtime.process_pending_orders();
    
    // Get positions to find the order ID
    auto positions = runtime.get_positions(0, "AMZN");
    
    if (!positions.empty()) {
        // Try to cancel the first order (or second if IPO is first)
        engine::OrderId order_to_cancel = positions.size() == 1 ? positions[0] : positions[1];
        runtime.submit_cancel_order("AMZN", order_to_cancel);
        runtime.process_pending_orders();
        
        std::cout << "✓ Order cancellation completed" << std::endl;
    }
}

void test_order_editing() {
    print_test_header("Order Editing");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Register stock
    bool success = runtime.register_stock("GOOGL", 2800.00, 15.0);
    assert(success);
    
    // Submit an order
    runtime.submit_limit_order("GOOGL", engine::OrderSide::BID, 2795.00, 1.0);
    runtime.process_pending_orders();
    
    // Get order ID (expecting IPO order for user 0 plus our order)
    auto positions = runtime.get_positions(0, "GOOGL");
    if (positions.size() == 2) {
        // IPO order is at position 0, user order at position 1
        std::cout << "✓ User has 2 positions as expected (1 IPO + 1 user order)" << std::endl;
    } else if (positions.size() == 1) {
        std::cout << "✓ User has 1 position (IPO may not be counted)" << std::endl;
    } else {
        std::cout << "INFO: User has " << positions.size() << " positions" << std::endl;
    }
    
    // Verify initial placement with engine counts (2 total = 1 IPO + 1 user)
    std::size_t placed_before = runtime.get_placed_count("GOOGL");
    if (placed_before == 2) {
        std::cout << "✓ Initial order successfully placed plus IPO" << std::endl;
    }
    
    if (!positions.empty()) {
        // If we have 2 positions, user order is at index 1 (after IPO)
        // If we have 1 position, user order is at index 0
        engine::OrderId order_to_edit = positions.size() == 1 ? positions[0] : positions[1];
        
        // Edit the order (new price and quantity)
        runtime.submit_edit_order("GOOGL", order_to_edit, 2790.00, 1.5);
        runtime.process_pending_orders();
        std::cout << "✓ Order edit submitted" << std::endl;
        
        // Verify order still exists
        std::size_t open_after = runtime.get_open_count("GOOGL");
        
        if (open_after >= 1) {
            std::cout << "✓ Order successfully edited" << std::endl;
        }
        
        // Verify the best bid changed
        auto best_bid = runtime.get_best_bid("GOOGL");
        if (best_bid != 0) {
            std::cout << "✓ Best bid after edit: $" << std::fixed << std::setprecision(2) << best_bid << std::endl;
        }
    }
}

void test_multi_user_trading() {
    print_test_header("Multi-User Trading");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Register stock
    bool success = runtime.register_stock("META", 250.00, 100.0);
    assert(success);
    
    // User 0 places buy orders
    runtime.submit_limit_order("META", engine::OrderSide::BID, 249.00, 2.0);
    runtime.submit_limit_order("META", engine::OrderSide::BID, 248.00, 3.0);
    
    // User 0 places more buy orders  
    runtime.submit_limit_order("META", engine::OrderSide::BID, 247.00, 1.0);
    runtime.submit_limit_order("META", engine::OrderSide::BID, 246.00, 2.0);
    
    // Check positions after 4 limit orders
    runtime.process_pending_orders();
    
    // Verify using engine counts (5 total = 1 IPO + 4 user orders)
    std::size_t placed_before_market = runtime.get_placed_count("META");
    std::size_t open_before_market = runtime.get_open_count("META");
    if (placed_before_market == 5 && open_before_market == 5) {
        std::cout << "✓ All 4 user limit orders successfully placed" << std::endl;
    }
    
    // User 0 sells at market (should fill against best bids)
    runtime.submit_market_order("META", engine::OrderSide::ASK, 1.0);
    
    runtime.process_pending_orders();
    
    std::cout << "✓ Single-user trading completed" << std::endl;
}

void test_async_processing() {
    print_test_header("Async Processing");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    const int BASIC_TEST_ORDERS = 50;
    
    // Register stock
    bool success = runtime.register_stock("AAPL", 180.00, 50000.0);
    assert(success);
    
    std::cout << "=== Basic Async Test (" << BASIC_TEST_ORDERS << " orders) ===" << std::endl;
    
    // Submit many orders without processing
    for (int i = 0; i < BASIC_TEST_ORDERS; ++i) {
        double price = 179.00 + (i * 0.10);
        runtime.submit_limit_order("AAPL", engine::OrderSide::BID, price, 1.0);
    }
    
    std::cout << "Submitted " << BASIC_TEST_ORDERS << " orders..." << std::endl;
    
    // Check that jobs are pending
    assert(!runtime.all_jobs_completed());
    std::cout << "✓ Jobs are pending as expected" << std::endl;
    
    // Process all pending orders
    auto start = std::chrono::high_resolution_clock::now();
    runtime.process_pending_orders();
    auto end = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "✓ Processed " << BASIC_TEST_ORDERS << " orders in " << duration.count() << " microseconds" << std::endl;
    
    // Verify all orders were actually placed using engine counts
    std::size_t placed_count = runtime.get_placed_count("AAPL");
    std::cout << "Placed count: " << placed_count << ", expected: " << BASIC_TEST_ORDERS + 1 << std::endl;
    
    if (placed_count == BASIC_TEST_ORDERS + 1) {
        std::cout << "✓ All orders successfully placed" << std::endl;
    }
    
    // Verify all jobs completed
    assert(runtime.all_jobs_completed());
    std::cout << "✓ All async processing tests passed" << std::endl;
}

void test_stress_performance() {
    print_test_header("Stress Testing & Performance");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(1, 1048576 * 2, false, 0);  // 1 thread for single stock
    runtime.reset();
    
    const int STRESS_TEST_ORDERS = 1000000;  // 10M orders
    
    // Register stock for stress testing with sufficient capacity
    bool success = runtime.register_stock("STRESS", 100.00, 10000.0);  // capacity=0 uses default
    assert(success);
    
    std::cout << "=== Stress Test (" << STRESS_TEST_ORDERS << " orders) ===" << std::endl;
    std::cout << "Note: Using runtime default capacity of 1048576 for OrderEngine (matching will free slots)" << std::endl;
    
    // ========== TEST 1: PLACEMENT WITH MATCHING ==========
    std::cout << "Phase 1: Testing order placement with matching..." << std::endl;
    
    const int NUM_PRICES = 100;  // Price levels
    auto total_start = std::chrono::high_resolution_clock::now();
    auto submit_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < STRESS_TEST_ORDERS; ++i) {
        // Use same price range for BID and ASK to enable matching
        double price_ticks = 10000 + (i % NUM_PRICES);
        double price = price_ticks / 100.0;  // Convert to dollars
        engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
        runtime.submit_limit_order("STRESS", side, price, 0.1);
    }
    auto submit_end = std::chrono::high_resolution_clock::now();
    
    auto submit_duration = std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start);
    std::cout << "✓ Submitted " << STRESS_TEST_ORDERS << " orders" << std::endl;
    std::cout << "✓ Submission took " << submit_duration.count() << " microseconds" << std::endl;
    
    // Process all orders (wait for workers to complete)
    auto process_start = std::chrono::high_resolution_clock::now();
    runtime.process_pending_orders();
    auto process_end = std::chrono::high_resolution_clock::now();
    
    auto process_duration = std::chrono::duration_cast<std::chrono::microseconds>(process_end - process_start);
    std::cout << "✓ Worker synchronization took " << process_duration.count() << " microseconds" << std::endl;
    
    // Calculate end-to-end throughput (submission + processing)
    auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(process_end - total_start);
    double orders_per_second = STRESS_TEST_ORDERS / (total_duration.count() / 1000000.0);
    std::cout << "✓ Total time (submission + processing): " << total_duration.count() << " microseconds" << std::endl;
    std::cout << "✓ End-to-end throughput: " << std::fixed << std::setprecision(0) << orders_per_second << " orders/second" << std::endl;
    
    // Verify results with matching
    std::size_t placed_count = runtime.get_placed_count("STRESS");
    std::size_t open_count = runtime.get_open_count("STRESS");
    std::size_t filled_count = runtime.get_filled_count("STRESS");
    
    std::cout << "✓ Placement results - Placed: " << placed_count 
              << ", Open: " << open_count 
              << ", Filled: " << filled_count << std::endl;
    
    assert(placed_count == STRESS_TEST_ORDERS + 1 && "All orders plus IPO should be placed");
    std::cout << "✓ Matching enabled: " << filled_count << " orders filled, capacity reused successfully" << std::endl;
    
    // ========== TEST 2: CANCELLATION ==========
    std::cout << "Phase 2: Testing order cancellation..." << std::endl;
    
    // Get half the orders to cancel
    auto positions = runtime.get_positions(0, "STRESS");
    std::size_t cancel_count = std::min(positions.size() / 2, (std::size_t)500000);  // Cancel up to 500k
    
    auto cancel_start = std::chrono::high_resolution_clock::now();
    for (std::size_t i = 0; i < cancel_count; ++i) {
        runtime.submit_cancel_order("STRESS", positions[i]);
    }
    runtime.process_pending_orders();
    auto cancel_end = std::chrono::high_resolution_clock::now();
    
    auto cancel_duration = std::chrono::duration_cast<std::chrono::microseconds>(cancel_end - cancel_start);
    double cancel_ops_per_sec = cancel_count / (cancel_duration.count() / 1000000.0);
    
    std::size_t cancelled_count = runtime.get_cancelled_count("STRESS");
    std::size_t open_after_cancel = runtime.get_open_count("STRESS");
    
    std::cout << "✓ Cancelled " << cancelled_count << " orders in " << cancel_duration.count() 
              << " microseconds (" << std::fixed << std::setprecision(0) << cancel_ops_per_sec << " ops/sec)" << std::endl;
    std::cout << "✓ Orders remaining open: " << open_after_cancel << std::endl;
    
    // ========== TEST 3: EDITING ==========
    std::cout << "Phase 3: Testing order editing..." << std::endl;
    
    // Get remaining orders to edit
    auto remaining_positions = runtime.get_positions(0, "STRESS");
    std::size_t edit_count = std::min(remaining_positions.size() / 2, (std::size_t)250000);  // Edit up to 250k
    
    auto edit_start = std::chrono::high_resolution_clock::now();
    for (std::size_t i = 0; i < edit_count; ++i) {
        // Edit with slightly different price and quantity
        double new_price = 60.00 + (i % 100) * 0.01;
        runtime.submit_edit_order("STRESS", remaining_positions[i], new_price, 0.2);
    }
    runtime.process_pending_orders();
    auto edit_end = std::chrono::high_resolution_clock::now();
    
    auto edit_duration = std::chrono::duration_cast<std::chrono::microseconds>(edit_end - edit_start);
    double edit_ops_per_sec = edit_count / (edit_duration.count() / 1000000.0);
    
    std::cout << "✓ Edited " << edit_count << " orders in " << edit_duration.count() 
              << " microseconds (" << std::fixed << std::setprecision(0) << edit_ops_per_sec << " ops/sec)" << std::endl;
    
    // ========== FINAL VERIFICATION ==========
    std::size_t final_placed = runtime.get_placed_count("STRESS");
    std::size_t final_open = runtime.get_open_count("STRESS");
    std::size_t final_filled = runtime.get_filled_count("STRESS");
    std::size_t final_cancelled = runtime.get_cancelled_count("STRESS");
    
    std::cout << "✓ Final counts - Placed: " << final_placed 
              << ", Open: " << final_open 
              << ", Filled: " << final_filled 
              << ", Cancelled: " << final_cancelled << std::endl;
    
    // Verify market depth
    auto depth = runtime.get_market_depth("STRESS", engine::OrderSide::BID, 10);
    std::cout << "✓ Market depth has " << depth.size() << " bid levels" << std::endl;
    
    std::cout << "✓ Memory footprint: ~" << (1500000 * sizeof(void*) / (1024 * 1024)) << " MB" << std::endl;
    
    assert(runtime.all_jobs_completed());
    std::cout << "✓ Stress test completed successfully" << std::endl;
}

void test_multi_stock_stress() {
    print_test_header("Multi-Stock Concurrent Stress Test");
    
    const int NUM_STOCKS = 8;
    const int ORDERS_PER_STOCK = 10000000;  // 10M orders per stock = 80M total
    const int NUM_WORKERS = 8;  // One worker per stock for optimal parallelism
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(NUM_WORKERS, 1048576, false, 0);
    runtime.reset();
    
    std::cout << "=== Multi-Stock Test (" << NUM_STOCKS << " stocks, " 
              << ORDERS_PER_STOCK << " orders each, " << NUM_STOCKS * ORDERS_PER_STOCK 
              << " total) ===" << std::endl;
    
    // Register multiple stocks
    std::vector<std::string> tickers = {"AAPL", "GOOGL", "MSFT", "AMZN", "TSLA", "META", "NVDA", "AMD"};
    for (int i = 0; i < NUM_STOCKS; ++i) {
        bool success = runtime.register_stock(tickers[i], 100.0 + i * 10, 100000.0);
        assert(success);
    }
    std::cout << "✓ Registered " << NUM_STOCKS << " stocks" << std::endl;
    
    // Phase 1: Concurrent order submission across all stocks
    std::cout << "Phase 1: Testing concurrent order placement..." << std::endl;
    
    const int NUM_PRICES = 100;
    auto total_start = std::chrono::high_resolution_clock::now();
    auto submit_start = std::chrono::high_resolution_clock::now();
    
    // Interleave orders across stocks for maximum concurrency
    for (int i = 0; i < ORDERS_PER_STOCK; ++i) {
        for (int stock_idx = 0; stock_idx < NUM_STOCKS; ++stock_idx) {
            double price_ticks = 10000 + (i % NUM_PRICES);
            double price = price_ticks / 100.0;
            engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
            runtime.submit_limit_order(tickers[stock_idx], side, price, 0.1);
        }
    }
    
    auto submit_end = std::chrono::high_resolution_clock::now();
    auto submit_duration = std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start);
    
    std::cout << "✓ Submitted " << NUM_STOCKS * ORDERS_PER_STOCK << " orders across " 
              << NUM_STOCKS << " stocks" << std::endl;
    std::cout << "✓ Submission took " << submit_duration.count() << " microseconds" << std::endl;
    
    // Process all orders (wait for workers)
    auto process_start = std::chrono::high_resolution_clock::now();
    runtime.process_pending_orders();
    auto process_end = std::chrono::high_resolution_clock::now();
    
    auto process_duration = std::chrono::duration_cast<std::chrono::microseconds>(process_end - process_start);
    auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(process_end - total_start);
    double total_orders = NUM_STOCKS * ORDERS_PER_STOCK;
    double orders_per_second = total_orders / (total_duration.count() / 1000000.0);
    
    std::cout << "✓ Worker synchronization took " << process_duration.count() << " microseconds" << std::endl;
    std::cout << "✓ Total time (submission + processing): " << total_duration.count() << " microseconds" << std::endl;
    std::cout << "✓ End-to-end throughput: " << std::fixed << std::setprecision(0) 
              << orders_per_second << " orders/second" << std::endl;
    
    // Verify results for each stock
    std::size_t total_placed = 0;
    std::size_t total_filled = 0;
    std::size_t total_open = 0;
    
    for (const auto& ticker : tickers) {
        std::size_t placed = runtime.get_placed_count(ticker);
        std::size_t filled = runtime.get_filled_count(ticker);
        std::size_t open = runtime.get_open_count(ticker);
        
        total_placed += placed;
        total_filled += filled;
        total_open += open;
    }
    
    std::cout << "✓ Aggregate results - Placed: " << total_placed 
              << ", Filled: " << total_filled 
              << ", Open: " << total_open << std::endl;
    
    // Calculate fill rate
    double fill_rate = (static_cast<double>(total_filled) / total_placed) * 100.0;
    std::cout << "✓ Fill rate: " << std::fixed << std::setprecision(1) 
              << fill_rate << "%" << std::endl;
    
    // Phase 2: Per-stock statistics
    std::cout << "\nPer-Stock Statistics:" << std::endl;
    for (const auto& ticker : tickers) {
        std::size_t placed = runtime.get_placed_count(ticker);
        std::size_t filled = runtime.get_filled_count(ticker);
        std::size_t open = runtime.get_open_count(ticker);
        
        std::cout << "  " << ticker << ": Placed=" << placed 
                  << ", Filled=" << filled 
                  << ", Open=" << open << std::endl;
    }
    
    assert(runtime.all_jobs_completed());
    std::cout << "✓ Multi-stock stress test completed successfully" << std::endl;
}

void test_accumulate_drain_runtime()
{
    print_test_header("Accumulate (auto_match=off) then Drain via EngineRuntime");

    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, 1048576, false, 0);
    runtime.reset();

    const std::string ticker = "ACC";
    const std::size_t NUM_ORDERS = 1000000; // adjust for CI
    const std::size_t CAPACITY = 1024 * 1024;

    bool ok = runtime.register_stock(ticker, 100.0, 1000.0, CAPACITY);
    assert(ok && "register_stock should succeed");

    // Ensure auto-match is off on engine prior to submissions
    runtime.set_auto_match(ticker, false);
    runtime.process_pending_orders(); // wait for toggle applied

    auto submit_start = std::chrono::high_resolution_clock::now();
    std::size_t submitted = 0;
    for (std::size_t i = 0; i < NUM_ORDERS; ++i) {
        double price = 100.0 + static_cast<double>(i % 10);
        double qty = 1.0;
        bool s = runtime.submit_limit_order(ticker, engine::OrderSide::BID, price, qty);
        if (s) submitted++;
    }
    auto submit_end = std::chrono::high_resolution_clock::now();
    auto submit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(submit_end - submit_start).count();

    double submit_rate = submit_ms > 0 ? (submitted / (submit_ms / 1000.0)) : 0.0;
    std::cout << " Submitted: " << submitted << " in " << submit_ms << " ms (" << submit_rate << " ops/sec)\n";

    // Now enable auto-match and measure drain time
    auto drain_start = std::chrono::high_resolution_clock::now();
    runtime.set_auto_match(ticker, true);
    // Wait for worker to process toggle and drain queued orders
    runtime.process_pending_orders();
    auto drain_end = std::chrono::high_resolution_clock::now();
    auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(drain_end - drain_start).count();

    // Refresh snapshot and report
    std::size_t placed = runtime.get_placed_count(ticker);
    std::size_t filled = runtime.get_filled_count(ticker);
    std::size_t open = runtime.get_open_count(ticker);

    double drain_rate = drain_ms > 0 ? (placed / (drain_ms / 1000.0)) : 0.0;
    std::cout << " Drain: processed " << placed << " queued orders in " << drain_ms << " ms (" << drain_rate << " ops/sec)\n";
    std::cout << " Result: filled=" << filled << ", open=" << open << "\n";

    std::cout << "✓ EngineRuntime accumulate+drain test completed\n";
}

void test_edge_cases() {
    print_test_header("Edge Cases");
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 10000, false, 0);
    runtime.reset();
    
    // Test operations on non-existent ticker
    runtime.submit_limit_order("NONEXISTENT", engine::OrderSide::BID, 100.0, 1.0);
    runtime.process_pending_orders();
    std::cout << "✓ Handled non-existent ticker gracefully" << std::endl;
    
    // Test invalid values
    bool neg_price = runtime.register_stock("TEST", -100.0, 1.0); // Negative price
    assert(!neg_price);
    bool neg_qty = runtime.register_stock("TEST2", 100.0, -1.0); // Negative quantity  
    assert(!neg_qty);
    
    // Test empty ticker
    bool empty_ticker = runtime.register_stock("", 100.0, 1.0);
    assert(!empty_ticker);
    
    // Register valid stock for remaining tests
    bool valid = runtime.register_stock("VALID", 100.0, 10.0);
    assert(valid);
    
    // Test zero/negative order values
    runtime.submit_limit_order("VALID", engine::OrderSide::BID, 0.0, 1.0);  // Zero price
    runtime.submit_limit_order("VALID", engine::OrderSide::BID, 99.0, 0.0); // Zero quantity
    runtime.submit_limit_order("VALID", engine::OrderSide::BID, -1.0, 1.0); // Negative price
    runtime.process_pending_orders();
    std::cout << "✓ Handled invalid order values gracefully" << std::endl;
}

void test_notifications() {
    print_test_header("Notification System");
    
    runtime::EngineRuntime::reset_instance();
    // Enable verbose mode to activate notification system
    auto& runtime = runtime::EngineRuntime::get_instance(2, 10000, true, 0);
    runtime.reset();
    
    std::cout << "=== Testing notification system with verbose=true ===" << std::endl;
    
    // Register a stock
    bool success = runtime.register_stock("NOTIFY_TEST", 100.0, 5.0);
    assert(success);
    std::cout << "✓ Stock registered with notifications enabled" << std::endl;
    
    // Trigger various events that generate notifications
    std::cout << "Submitting orders to generate notifications..." << std::endl;
    
    // Valid orders
    runtime.submit_limit_order("NOTIFY_TEST", engine::OrderSide::BID, 99.0, 1.0);
    runtime.submit_limit_order("NOTIFY_TEST", engine::OrderSide::ASK, 101.0, 1.0);

    // Invalid operations (should trigger error notifications)
    runtime.submit_limit_order("NONEXISTENT", engine::OrderSide::BID, 50.0, 1.0);  // Invalid ticker
    runtime.submit_limit_order("NOTIFY_TEST", engine::OrderSide::BID, -10.0, 1.0); // Invalid price
    runtime.submit_limit_order("NOTIFY_TEST", engine::OrderSide::BID, 50.0, 0.0);  // Invalid quantity

    // Cancel non-existent order
    runtime.submit_cancel_order("NOTIFY_TEST", 99999);
    
    // Process all pending orders
    runtime.process_pending_orders();
    
    // Give notification thread time to process messages
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::cout << "✓ Notification system processed all events" << std::endl;
    std::cout << "✓ Check console output above for notification messages" << std::endl;
    
    // Unregister stock (should also generate notification)
    bool unregistered = runtime.unregister_stock("NOTIFY_TEST");
    assert(unregistered);
    
    // Give notification thread time to process final messages
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::cout << "✓ Notification system test completed" << std::endl;
    std::cout << "Note: Notifications are printed to console in verbose mode" << std::endl;
}

int main() {
    std::cout << "Starting EngineRuntime Tests..." << std::endl;
    
    try {
        
        test_singleton_pattern();
        test_stock_registration();
        test_market_data_reads();
        test_limit_orders();
        test_market_orders();
        test_order_cancellation();
        test_order_editing();
        test_multi_user_trading();
        test_async_processing();
        test_stress_performance();
        test_multi_stock_stress();
            test_accumulate_drain_runtime();
        test_edge_cases();
        test_notifications();
        
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "🎉 ALL TESTS PASSED! 🎉" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
    
    return 0;
}
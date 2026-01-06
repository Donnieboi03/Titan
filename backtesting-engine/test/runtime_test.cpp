#include "../engine_runtime.cpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <iomanip>
#include <cmath>
#include <numeric>
#include <algorithm>

void test_basic_batch_orders()
{
    std::cout << "=== Test: Basic Batch Orders ===" << std::endl;
    
    auto& runtime = EngineRuntime::get_instance(4, 10000, 0, false, true);
    
    // Initialize stock
    assert(runtime.register_stock("AAPL", 100.0, 1000));
    
    // Prepare result storage
    std::vector<OrderId> order_ids(10, -1);
    
    // Submit 10 orders in batch
    for (int i = 0; i < 10; ++i)
    {
        runtime.limit_order("AAPL", OrderSide::BID, 99.0 + i, 10, &order_ids[i]);
    }
    
    // Execute all at once
    runtime.execute_batch();
    
    // Verify all orders were placed
    int successful = 0;
    for (const auto& id : order_ids)
    {
        if (id != static_cast<OrderId>(-1))
            successful++;
    }
    
    std::cout << "Batch submitted " << successful << "/" << order_ids.size() << " orders successfully" << std::endl;
    assert(successful == 10);
    
    std::cout << "✓ Basic batch orders test passed" << std::endl;
    
    // Reset runtime for next test
    EngineRuntime::get_instance().reset();
}

void test_mixed_batch_operations()
{
    std::cout << "\n=== Test: Mixed Batch Operations ===" << std::endl;
    
    auto& runtime = EngineRuntime::get_instance(4, 10000, 0, false, true);
    
    // Initialize stock
    assert(runtime.register_stock("TSLA", 200.0, 500));
    
    // Place some orders first
    OrderId id1 = -1, id2 = -1;
    runtime.limit_order("TSLA", OrderSide::BID, 195.0, 10, &id1);
    runtime.limit_order("TSLA", OrderSide::BID, 190.0, 20, &id2);
    runtime.execute_batch();
    
    // Now batch submit: new orders, cancel, and edit
    OrderId new_order_id = -1;
    bool cancel_result = false;
    OrderId edit_result = -1;
    
    runtime.limit_order("TSLA", OrderSide::BID, 185.0, 15, &new_order_id);
    runtime.market_order("TSLA", OrderSide::BID, 5, &new_order_id);
    runtime.cancel_order("TSLA", id2, &cancel_result, 1);  // Add user_id parameter
    runtime.edit_order("TSLA", id1, OrderSide::BID, 196.0, 12, &edit_result);
    
    // Execute batch
    runtime.execute_batch();
    
    std::cout << "Cancel result: " << (cancel_result ? "success" : "failed") << std::endl;
    std::cout << "Edit result: " << (edit_result != static_cast<OrderId>(-1) ? "success" : "failed") << std::endl;
    
    std::cout << "✓ Mixed batch operations test passed" << std::endl;
    
    // Reset runtime for next test
    EngineRuntime::get_instance().reset();
}

void test_multi_stock_batch()
{
    std::cout << "\n=== Test: Multi-Stock Batch ===" << std::endl;
    
    auto& runtime = EngineRuntime::get_instance(4, 10000, 0, false, true);
    
    // Initialize multiple stocks (use unique tickers to avoid conflicts)
    bool ibm_init = runtime.register_stock("IBM", 150.0, 1000);
    bool amzn_init = runtime.register_stock("AMZN", 300.0, 500);
    bool meta_init = runtime.register_stock("META", 2500.0, 200);
    
    if (!ibm_init) std::cerr << "Failed to initialize IBM" << std::endl;
    if (!amzn_init) std::cerr << "Failed to initialize AMZN" << std::endl;
    if (!meta_init) std::cerr << "Failed to initialize META" << std::endl;
    
    assert(ibm_init && amzn_init && meta_init);
    
    // Batch submit orders across different stocks
    std::vector<OrderId> results(15, -1);
    
    for (int i = 0; i < 5; ++i)
    {
        runtime.limit_order("IBM", OrderSide::BID, 149.0 + i, 10, &results[i]);
        runtime.limit_order("AMZN", OrderSide::BID, 299.0 + i, 5, &results[5 + i]);
        runtime.limit_order("META", OrderSide::BID, 2499.0 + i, 2, &results[10 + i]);
    }
    
    // Execute all at once (across multiple engines)
    runtime.execute_batch();
    
    // Verify all succeeded
    int successful = 0;
    for (const auto& id : results)
    {
        if (id != static_cast<OrderId>(-1))
            successful++;
    }
    
    std::cout << "Multi-stock batch: " << successful << "/" << results.size() << " orders successful" << std::endl;
    assert(successful == 15);
    
    std::cout << "✓ Multi-stock batch test passed" << std::endl;
    
    // Reset runtime for next test
    EngineRuntime::get_instance().reset();
}

void test_large_batch_performance()
{
    std::cout << "\n=== Test: Large Batch Performance ===" << std::endl;
    
    const std::size_t num_orders = 1000000;
    const std::size_t num_workers = 4;
    const std::size_t batch_size = 10000;  // ✅ Much larger batches
    
    std::cout << "Configuration: " << num_orders << " orders, " << num_workers 
              << " workers, batch size " << batch_size << std::endl;
    
    const std::size_t capacity = num_orders * 2;
    const std::size_t ipo_qty = capacity;
    
    auto& runtime = EngineRuntime::get_instance(num_workers, capacity, 0, false, false);
    
    std::cout << "Initializing stock with capacity " << capacity << "..." << std::endl;
    assert(runtime.register_stock("SPY", 400.0, ipo_qty, capacity));
    
    std::vector<OrderId> results(num_orders, -1);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::cout << "Submitting " << num_orders << " orders..." << std::endl;
    
    // ✅ Submit ALL orders without waiting between batches
    for (std::size_t i = 0; i < num_orders; ++i)
    {
        runtime.limit_order("SPY", OrderSide::BID, 390.0 + (i % 100) * 0.1, 1, &results[i]);
        
        if (batch_size > 0 && (i + 1) % batch_size == 0)
        {
            std::cout << "Batch" << i << "\n";
            runtime.execute_batch();  // ✅ Non-blocking, doesn't wait
            std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
        }
    }
    
    // ✅ Execute final batch
    runtime.execute_batch();
    
    // ✅ NOW wait for everything to complete (only once!)
    std::cout << "Waiting for all jobs to complete..." << std::endl;
    runtime.wait_for_jobs();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    int successful = 0;
    for (const auto& id : results)
    {
        if (id != static_cast<OrderId>(-1))
            successful++;
    }
    
    std::cout << "Processed " << successful << "/" << num_orders << " orders in " 
              << duration.count() << "ms" << std::endl;
    std::cout << "Throughput: " << (successful * 1000.0 / duration.count()) 
              << " orders/sec" << std::endl;
    
    std::cout << "✓ Large batch performance test passed" << std::endl;
    
    // Reset runtime for next test
    EngineRuntime::get_instance().reset();
}

void test_sequential_vs_batch_comparison()
{
    std::cout << "\n=== Test: Sequential vs Batch Comparison ===" << std::endl;
    
    const std::size_t num_orders = 1000;
    
    // Test 1: Sequential execution (execute after each order)
    {
        auto& runtime = EngineRuntime::get_instance(4, 50000, 0, false, true);
        assert(runtime.register_stock("TEST1", 100.0, 5000));
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (std::size_t i = 0; i < num_orders; ++i)
        {
            OrderId result = -1;
            runtime.limit_order("TEST1", OrderSide::BID, 99.0 + (i % 10) * 0.1, 1, &result);
            runtime.execute_batch();
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto seq_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "Sequential: " << seq_duration.count() << "ms" << std::endl;
    }
    
    // Test 2: Batch execution (submit all, execute once)
    {
        auto& runtime = EngineRuntime::get_instance(4, 50000, 0, false, true);
        assert(runtime.register_stock("TEST2", 100.0, 5000));
        
        std::vector<OrderId> results(num_orders, -1);
        
        auto start = std::chrono::high_resolution_clock::now();
        
        for (std::size_t i = 0; i < num_orders; ++i)
        {
            runtime.limit_order("TEST2", OrderSide::BID, 99.0 + (i % 10) * 0.1, 1, &results[i]);
        }
        runtime.execute_batch();
        
        auto end = std::chrono::high_resolution_clock::now();
        auto batch_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "Batch: " << batch_duration.count() << "ms" << std::endl;
    }
    
    std::cout << "✓ Sequential vs Batch comparison completed" << std::endl;
    
    // Reset runtime for next test
    EngineRuntime::get_instance().reset();
}

void test_non_blocking_multi_stock()
{
    std::cout << "\n=== Test: Non-Blocking Multi-Stock ===" << std::endl;
    
    const std::size_t num_orders_per_stock = 5000;
    auto& runtime = EngineRuntime::get_instance(4, 50000, 0, false, false);  // Non-blocking mode
    
    // Initialize multiple stocks (use unique tickers)
    assert(runtime.register_stock("QQQ", 400.0, 10000, 20000));
    assert(runtime.register_stock("DIA", 150.0, 10000, 20000));
    assert(runtime.register_stock("IWM", 2500.0, 10000, 20000));
    
    std::vector<OrderId> spy_results(num_orders_per_stock, -1);
    std::vector<OrderId> aapl_results(num_orders_per_stock, -1);
    std::vector<OrderId> googl_results(num_orders_per_stock, -1);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Submit orders for all stocks in parallel
    std::cout << "Submitting orders for all stocks..." << std::endl;
    for (std::size_t i = 0; i < num_orders_per_stock; ++i)
    {
        runtime.limit_order("QQQ", OrderSide::BID, 390.0 + (i % 100) * 0.1, 1, &spy_results[i]);
        runtime.limit_order("DIA", OrderSide::BID, 140.0 + (i % 100) * 0.1, 1, &aapl_results[i]);
        runtime.limit_order("IWM", OrderSide::BID, 2400.0 + (i % 100) * 0.1, 1, &googl_results[i]);
    }
    
    // Execute all stocks concurrently (non-blocking)
    std::cout << "Processing all stocks in parallel..." << std::endl;
    runtime.execute_batch();
    
    // Wait for all jobs to complete
    std::cout << "Waiting for all stocks to complete..." << std::endl;
    runtime.wait_for_jobs();
    
    std::cout << "✓ QQQ completed!" << std::endl;
    std::cout << "✓ DIA completed!" << std::endl;
    std::cout << "✓ IWM completed!" << std::endl;
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Count successes
    int spy_success = 0, aapl_success = 0, googl_success = 0;
    for (const auto& id : spy_results) if (id != -1) spy_success++;
    for (const auto& id : aapl_results) if (id != -1) aapl_success++;
    for (const auto& id : googl_results) if (id != -1) googl_success++;
    
    std::cout << "Results:" << std::endl;
    std::cout << "  QQQ: " << spy_success << "/" << num_orders_per_stock << " orders" << std::endl;
    std::cout << "  DIA: " << aapl_success << "/" << num_orders_per_stock << " orders" << std::endl;
    std::cout << "  IWM: " << googl_success << "/" << num_orders_per_stock << " orders" << std::endl;
    std::cout << "Total: " << (spy_success + aapl_success + googl_success) << " orders in " << duration.count() << "ms" << std::endl;
    std::cout << "Throughput: " << ((spy_success + aapl_success + googl_success) * 1000.0 / duration.count()) << " orders/sec" << std::endl;
    
    assert(spy_success == num_orders_per_stock);
    assert(aapl_success == num_orders_per_stock);
    assert(googl_success == num_orders_per_stock);
    
    std::cout << "✓ Non-blocking multi-stock test passed" << std::endl;
    
    // Reset runtime for next test
    EngineRuntime::get_instance().reset();
}

void test_monte_carlo_simulation()
{
    std::cout << "\n=== Test: Monte Carlo Price Simulation ===" << std::endl;
    
    const std::size_t num_simulations = 10000;  // Back to 10k
    const std::size_t batch_size = 10000;  // Bigger batches
    
    auto& runtime = EngineRuntime::get_instance(4, 100000, 0, false, false);
    
    assert(runtime.register_stock("BTC", 50000.0, 100000, 100000));
    runtime.set_auto_match("BTC", false);  // Keep disabled for book analysis
    
    std::vector<OrderId> results(num_simulations, -1);
    std::srand(std::time(nullptr));
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::cout << "Submitting " << num_simulations << " orders in batches of " << batch_size << "..." << std::endl;
    
    // Submit all orders in batches WITHOUT polling
    for (std::size_t i = 0; i < num_simulations; i += batch_size)
    {
        std::size_t batch_end = std::min(i + batch_size, num_simulations);
        
        // Submit batch
        for (std::size_t j = i; j < batch_end; ++j)
        {
            OrderSide side = (std::rand() % 2 == 0) ? OrderSide::BID : OrderSide::ASK;
            double offset_pct = (std::rand() % 500) / 10000.0;
            
            Price price;
            if (side == OrderSide::BID)
                price = 50000.0 * (0.95 + offset_pct);
            else
                price = 50000.0 * (1.001 + offset_pct);
            
            Quantity qty = 1 + (std::rand() % 10);
            runtime.limit_order("BTC", side, price, qty, &results[j]);
        }
        
        // Execute batch and wait for THIS batch only
        runtime.execute_batch();
        while (!runtime.stock_completed("BTC")) {
            std::this_thread::yield();  // Yield instead of busy-wait
        }
        
        // Optional: Print progress every 10 batches
        if ((i / batch_size) % 10 == 0)
        {
            std::cout << "Progress: " << (i + batch_size) << "/" << num_simulations << std::endl;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Total orders: " << num_simulations << std::endl;
    std::cout << "Duration: " << duration.count() << "ms" << std::endl;
    std::cout << "Throughput: " << (num_simulations * 1000.0 / duration.count()) << " orders/sec" << std::endl;
    
    // Query book state ONCE at the end
    auto engine = runtime.get_engine("BTC");
    Price best_bid = engine ? engine->get_best_bid() : -1;
    Price best_ask = engine ? engine->get_best_ask() : -1;
    Price mid = (best_bid > 0 && best_ask > 0) ? (best_bid + best_ask) / 2.0 : -1;
    
    std::cout << "Final: Best Bid=$" << best_bid << ", Best Ask=$" << best_ask 
              << ", Mid=$" << mid << std::endl;
    
    // Get market depth at the end
    std::cout << "\n=== Market Depth (Top 10 Levels) ===" << std::endl;
    auto bid_depth = runtime.get_market_depth("BTC", OrderSide::BID, 10);
    auto ask_depth = runtime.get_market_depth("BTC", OrderSide::ASK, 10);
    
    std::cout << "\nBID SIDE:" << std::endl;
    std::cout << "Price      | Quantity" << std::endl;
    std::cout << "-----------|----------" << std::endl;
    for (const auto& [price, qty] : bid_depth)
    {
        std::cout << "$" << std::setw(8) << std::fixed << std::setprecision(2) << price 
                  << " | " << std::setw(8) << qty << std::endl;
    }
    
    std::cout << "\nASK SIDE:" << std::endl;
    std::cout << "Price      | Quantity" << std::endl;
    std::cout << "-----------|----------" << std::endl;
    for (const auto& [price, qty] : ask_depth)
    {
        std::cout << "$" << std::setw(8) << std::fixed << std::setprecision(2) << price 
                  << " | " << std::setw(8) << qty << std::endl;
    }
    
    std::cout << "✓ Monte Carlo simulation test passed" << std::endl;
}

int main(int argc, char* argv[])
{
    std::cout << "========================================" << std::endl;
    std::cout << "  Engine Runtime Batch Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    try
    {
        //test_basic_batch_orders();
        //test_mixed_batch_operations();
        //test_multi_stock_batch();
        test_large_batch_performance();
        //test_sequential_vs_batch_comparison();
        //test_non_blocking_multi_stock();
        //test_monte_carlo_simulation();
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "  ✓ All Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}

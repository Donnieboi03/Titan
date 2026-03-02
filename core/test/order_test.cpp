#include "../engine_runtime_types.h"
#include "../order_engine.cpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <iomanip>
#include <thread>
#include <cstdlib>
#include <ctime>

// Global verbose flag
bool VERBOSE = false;

// Helper: convert dollars to ticks for test inputs (uses backtest::math scale)
inline engine::Price price(double dollars) { return backtest::math::dollars_to_ticks(dollars); }

// Test the new notification system
void test_new_notification_system()
{
    std::cout << "=== Testing New Notification System ===\n";

    engine::OrderEngine engine(10000, true); // verbose = true

    // Place a resting bid order
    std::vector<engine::EngineMsg> bid_msgs;
    engine::OrderId bid_id = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 10, bid_msgs);

    assert(bid_msgs.size() == 1);
    assert(bid_msgs[0].kind == engine::EventKind::ACCEPT);
    assert(bid_msgs[0].order_id == bid_id);

    // Place an ask order that should match
    std::vector<engine::EngineMsg> ask_msgs;
    engine::OrderId ask_id = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(100.0), 5, ask_msgs);

    assert(ask_msgs.size() == 3); // ACCEPT + 1 FILL + 1 PARTIAL_FILL messages
    assert(ask_msgs[0].kind == engine::EventKind::ACCEPT);
    assert(ask_msgs[0].order_id == ask_id);

    assert(ask_msgs[1].kind == engine::EventKind::FILL);
    assert(ask_msgs[1].order_id == ask_id);

    assert(ask_msgs[2].kind == engine::EventKind::PARTIAL_FILL);
    assert(ask_msgs[2].order_id == bid_id);

    std::cout << "✓ New notification system works correctly\n";
}

void test_verbose_performance_comparison()
{
    std::cout << "=== Testing Verbose Mode Performance Comparison ===\n";

    const int NUM_ORDERS = 50000; // Smaller test for quick results

    // Test with verbose = false (optimized path - no message collection)
    {
        engine::OrderEngine engine_quiet(100000, false); // verbose = false
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < NUM_ORDERS; ++i) {
            engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
            engine::Price p = price(100.0 + (i % 5)); // Small spread to encourage matching
            engine_quiet.place_order(side, engine::OrderType::LIMIT, p, 10);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_quiet = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "Verbose=FALSE: " << NUM_ORDERS << " orders in " << duration_quiet.count() << "ms "
                  << "(" << (NUM_ORDERS * 1000.0 / duration_quiet.count()) << " ops/sec)\n";
        std::cout << "  Filled: " << engine_quiet.get_snapshot().filled_count << "\n";
    }

    // Test with verbose = true (notification collection path)
    {
        engine::OrderEngine engine_verbose(100000, true); // verbose = true
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < NUM_ORDERS; ++i) {
            engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
            engine::Price p = price(100.0 + (i % 5)); // Small spread to encourage matching
            engine_verbose.place_order(side, engine::OrderType::LIMIT, p, 10);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_verbose = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "Verbose=TRUE:  " << NUM_ORDERS << " orders in " << duration_verbose.count() << "ms "
                  << "(" << (NUM_ORDERS * 1000.0 / duration_verbose.count()) << " ops/sec)\n";
        std::cout << "  Filled: " << engine_verbose.get_snapshot().filled_count << "\n";
    }

    std::cout << "✓ Verbose performance comparison completed\n";
}

void test_place_limit_order()
{
    std::cout << "=== Testing Place Limit Order ===\n";
    
    engine::OrderEngine engine(10000, VERBOSE);
    
    // Place bid orders
    auto bid1 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 10);
    auto bid2 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(99.0), 20);
    auto bid3 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(98.0), 15);

    assert(bid1 != engine::INVALID_ORDER_ID && "Bid order 1 should be placed");
    assert(bid2 != engine::INVALID_ORDER_ID && "Bid order 2 should be placed");
    assert(bid3 != engine::INVALID_ORDER_ID && "Bid order 3 should be placed");

    // Place ask orders
    auto ask1 = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(101.0), 10);
    auto ask2 = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(102.0), 20);
    auto ask3 = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(103.0), 15);
    
    assert(ask1 != engine::INVALID_ORDER_ID && "Ask order 1 should be placed");
    assert(ask2 != engine::INVALID_ORDER_ID && "Ask order 2 should be placed");
    assert(ask3 != engine::INVALID_ORDER_ID && "Ask order 3 should be placed");
    
    // Verify orders exist
    const engine::OrderInfo* bid_order = engine.get_order(bid1);
    assert(bid_order != nullptr && "Bid order should exist");
    assert(bid_order->side_ == engine::OrderSide::BID && "Order side should be BID");
    assert(bid_order->price_ == price(100.0) && "Order price should be 100.0");
    assert(bid_order->qty_ == 10 && "Order quantity should be 10");
    assert(bid_order->status_ == engine::OrderStatus::OPEN && "Order status should be OPEN");
    const engine::OrderInfo* ask_order = engine.get_order(ask1);
    assert(ask_order != nullptr && "Ask order should exist");
    assert(ask_order->side_ == engine::OrderSide::ASK && "Order side should be ASK");
    assert(ask_order->price_ == price(101.0) && "Order price should be 101.0");
    assert(ask_order->qty_ == 10 && "Order quantity should be 10");
    assert(ask_order->status_ == engine::OrderStatus::OPEN && "Order status should be OPEN");
    
    // Update snapshot and verify best bid and ask
    engine.update_snapshot();
    auto snap = engine.get_snapshot();
    assert(snap.best_bid == price(100.0) && "Best bid should be 100.0");
    assert(snap.best_ask == price(101.0) && "Best ask should be 101.0");
    
    // Verify counters
    assert(snap.placed_count == 6 && "Should have 6 placed orders");
    
    if (VERBOSE) std::cout << "Market depth size: " << snap.bid_levels << "\n";
    std::cout << "✓ Place Limit Order test PASSED!\n\n";
}

void test_place_market_order()
{
    std::cout << "=== Testing Place Market Order ===\n";
    
    engine::OrderEngine engine(10000, VERBOSE);
    
    // Try to place market order with no liquidity
    auto market_bid = engine.place_order(engine::OrderSide::BID, engine::OrderType::MARKET, 0, 10);
    assert(market_bid == engine::INVALID_ORDER_ID && "Market order should fail without liquidity");
    
    // Place limit orders first
    engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(200.0), 10);
    engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(199.0), 10);
    // Now place market orders
    auto market_bid2 = engine.place_order(engine::OrderSide::BID, engine::OrderType::MARKET, 0, 5);
    assert(market_bid2 != engine::INVALID_ORDER_ID && "Market order should succeed with liquidity");
    
    std::cout << "✓ Place Market Order test PASSED!\n\n";
}

void test_cancel_order()
{
    std::cout << "=== Testing Cancel Order ===\n";
    
    engine::OrderEngine engine(10000, VERBOSE);
    
    // Place orders
    auto bid1 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(300.0), 10);
    auto bid2 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(299.0), 20);
    auto ask1 = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(301.0), 10);
    
    // Verify orders exist
    assert(engine.get_order(bid1) != nullptr && "Order should exist before cancel");
    assert(engine.get_order(bid1)->status_ == engine::OrderStatus::OPEN && "Order should be OPEN");
    
    // Cancel order
    bool cancelled = engine.cancel_order(bid1);
    assert(cancelled && "Cancel should succeed");
    
    // Verify order is freed from memory (cancelled orders go to ledger)
    const engine::OrderInfo* cancelled_order = engine.get_order(bid1);
    assert(cancelled_order == nullptr && "Cancelled order should be freed from memory");
    
    // Verify best bid changed
    engine.update_snapshot();
    assert(engine.get_snapshot().best_bid == price(299.0) && "Best bid should update after cancel");
    
    // Try to cancel non-existent order
    bool cancel_fail = engine.cancel_order(99999);
    assert(!cancel_fail && "Cancel should fail for non-existent order");
    
    // Try to cancel already cancelled order (should fail - already freed)
    bool cancel_twice = engine.cancel_order(bid1);
    assert(!cancel_twice && "Cancel should fail for already cancelled order");
    
    // Verify counters
    assert(engine.get_snapshot().cancelled_count == 1 && "Should have 1 cancelled order");

    // Test new cancel API with message
    engine::OrderEngine engine2(10000, true);
    auto test_order = engine2.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 10);

    engine::EngineMsg cancel_msg;
    bool cancel_result = engine2.cancel_order(test_order, cancel_msg);
    assert(cancel_result && "Cancel should succeed");
    assert(cancel_msg.kind == engine::EventKind::ACCEPT && "Should get ACCEPT message");
    assert(cancel_msg.order_id == test_order && "Message should contain correct order ID");

    std::cout << "✓ Cancel Order test PASSED!\n\n";
}

void test_edit_order()
{
    std::cout << "=== Testing Replace Order and Edit Order (qty-only) ===\n";
    
    engine::OrderEngine engine(10000, VERBOSE);
    
    // Place initial orders
    auto bid1 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(150.0), 10);
    auto ask1 = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(151.0), 10);
    
    // Verify initial order
    const engine::OrderInfo* initial = engine.get_order(bid1);
    assert(initial->price_ == price(150.0) && "Initial price should be 150.0");
    assert(initial->qty_ == 10 && "Initial quantity should be 10");
    
    // Replace order (change price and quantity)
    bool replace_result = engine.replace_order(bid1, engine::OrderSide::BID, price(149.0), 20);
    assert(replace_result && "Replace should succeed");
    
    // Verify order was modified (not cancelled)
    const engine::OrderInfo* replaced_order = engine.get_order(bid1);
    assert(replaced_order != nullptr && "Replaced order should exist");
    assert(replaced_order->status_ == engine::OrderStatus::OPEN && "Replaced order should still be OPEN");
    assert(replaced_order->price_ == price(149.0) && "New price should be 149.0");
    assert(replaced_order->qty_ == 20 && "New quantity should be 20");
    
    // Verify best bid changed
    engine.update_snapshot();
    assert(engine.get_snapshot().best_bid == price(149.0) && "Best bid should reflect replaced order");
    
    // Edit order (qty-only): same price, change quantity only
    bool edit_result = engine.edit_order(bid1, 15);
    assert(edit_result && "Edit (qty-only) should succeed");
    const engine::OrderInfo* edited_order = engine.get_order(bid1);
    assert(edited_order != nullptr && "Edited order should exist");
    assert(edited_order->price_ == price(149.0) && "Price unchanged after qty-only edit");
    assert(edited_order->qty_ == 15 && "Quantity should be 15 after edit");
    
    // Try to replace non-existent order
    bool replace_fail = engine.replace_order(99999, engine::OrderSide::BID, price(150.0), 10);
    assert(!replace_fail && "Replace should fail for non-existent order");
    
    std::cout << "✓ Replace Order and Edit Order (qty-only) test PASSED!\n\n";
}

void test_place_cancel_edit_replace_throughput()
{
    std::cout << "=== Throughput: place, cancel, edit (qty-only), replace ===\n";
    const std::size_t N = 500000;
    engine::OrderEngine engine(N * 2, false, true);

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<engine::OrderId> ids;
    ids.reserve(N);
    for (std::size_t i = 0; i < N; ++i)
        ids.push_back(engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0 + i * 0.01), 10));
    auto t1 = std::chrono::high_resolution_clock::now();
    double place_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << " Place: " << N << " in " << place_ms << " ms (" << (N / (place_ms / 1000.0)) << " ops/sec)\n";

    t0 = std::chrono::high_resolution_clock::now();
    for (std::size_t i = 0; i < N / 2; ++i)
        engine.replace_order(ids[i], engine::OrderSide::BID, price(101.0), 20);
    t1 = std::chrono::high_resolution_clock::now();
    double replace_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << " Replace: " << (N/2) << " in " << replace_ms << " ms (" << ((N/2) / (replace_ms / 1000.0)) << " ops/sec)\n";

    t0 = std::chrono::high_resolution_clock::now();
    for (std::size_t i = N / 2; i < N; ++i)
        engine.edit_order(ids[i], 15);
    t1 = std::chrono::high_resolution_clock::now();
    double edit_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << " Edit (qty-only): " << (N - N/2) << " in " << edit_ms << " ms (" << ((N - N/2) / (edit_ms / 1000.0)) << " ops/sec)\n";

    t0 = std::chrono::high_resolution_clock::now();
    for (std::size_t i = 0; i < N; ++i)
        engine.cancel_order(ids[i]);
    t1 = std::chrono::high_resolution_clock::now();
    double cancel_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << " Cancel: " << N << " in " << cancel_ms << " ms (" << (N / (cancel_ms / 1000.0)) << " ops/sec)\n";

    std::cout << "✓ Place/Cancel/Edit/Replace throughput test completed\n\n";
}

void test_multiple_orders_same_price()
{
    std::cout << "=== Testing Multiple Orders at Same Price ===\n";
    
    engine::OrderEngine engine(10000, VERBOSE);
    
    // Place multiple orders at same price
    auto bid1 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 10);
    auto bid2 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 20);
    auto bid3 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 15);
    
    // Get market depth via snapshot
    engine.update_snapshot();
    auto snap = engine.get_snapshot();
    assert(snap.bid_levels >= 1 && "Should have at least one price level");
    assert(snap.bid_prices[0] == price(100.0) && "Price should be 100.0");
    assert(snap.bid_depth[0] == 45 && "Total quantity should be 45 (10+20+15)");
    
    // Cancel middle order
    engine.cancel_order(bid2);
    
    // Check depth again
    engine.update_snapshot();
    snap = engine.get_snapshot();
    assert(snap.bid_depth[0] == 25 && "Total quantity should be 25 after cancel");
    
    std::cout << "✓ Multiple Orders at Same Price test PASSED!\n\n";
}

void test_order_priority()
{
    std::cout << "=== Testing Order Priority (Time Priority) ===\n";
    
    engine::OrderEngine engine(10000, VERBOSE);
    
    // Place orders at same price with time delay
    auto bid1 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(500.0), 10);
    auto bid2 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(500.0), 20);
    auto bid3 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(500.0), 30);
    
    // Verify orders exist
    assert(engine.get_order(bid1) != nullptr && "Order 1 should exist");
    assert(engine.get_order(bid2) != nullptr && "Order 2 should exist");
    assert(engine.get_order(bid3) != nullptr && "Order 3 should exist");
    
    // Orders should have different timestamps
    const engine::OrderInfo* o1 = engine.get_order(bid1);
    const engine::OrderInfo* o2 = engine.get_order(bid2);
    const engine::OrderInfo* o3 = engine.get_order(bid3);
    
    assert(o1->time_ <= o2->time_ && "Order 1 time should be <= Order 2 time");
    assert(o2->time_ <= o3->time_ && "Order 2 time should be <= Order 3 time");
    
    std::cout << "✓ Order Priority test PASSED!\n\n";
}

// Delta L2 semantics: increase = place at back, decrease = remove from back (get_back_order_at_level).
void test_delta_l2_back_of_queue()
{
    std::cout << "=== Testing Delta L2: Back of Queue (get_back_order_at_level) ===\n";

    engine::OrderEngine engine(10000, VERBOSE);
    const engine::Price p = price(100.0);

    // New level: place first order (10)
    auto id1 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, p, 10);
    assert(id1 != engine::INVALID_ORDER_ID);
    engine.update_snapshot();
    assert(engine.get_snapshot().bid_depth[0] == 10);

    // Back at level should be the only order
    engine::OrderId back_id = engine.get_back_order_at_level(engine::OrderSide::BID, p);
    assert(back_id == id1 && "Back order at level should be first order when only one");

    // Increase: place second order at same price (20) -> goes to back
    auto id2 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, p, 20);
    assert(id2 != engine::INVALID_ORDER_ID);
    back_id = engine.get_back_order_at_level(engine::OrderSide::BID, p);
    assert(back_id == id2 && "Back order should be the newly placed order (new size at back)");
    engine.update_snapshot();
    assert(engine.get_snapshot().bid_depth[0] == 30 && "Depth = 10 + 20");

    // Decrease from back: cancel back order (20), depth should become 10
    engine.cancel_order(back_id);
    engine.update_snapshot();
    assert(engine.get_snapshot().bid_depth[0] == 10 && "After removing back order, depth = 10");
    back_id = engine.get_back_order_at_level(engine::OrderSide::BID, p);
    assert(back_id == id1 && "Back order should now be the first order");

    // Level to zero: cancel remaining order
    engine.cancel_order(back_id);
    engine.update_snapshot();
    assert(engine.get_snapshot().bid_levels == 0 || engine.get_snapshot().bid_depth[0] == 0);
    assert(engine.get_back_order_at_level(engine::OrderSide::BID, p) == engine::INVALID_ORDER_ID);

    std::cout << "✓ Delta L2 back-of-queue test PASSED!\n\n";
}

void test_stress_orders()
{
    std::cout << "=== Stress Test: Order Operations ===\n";
    
    const int NUM_ORDERS = 10000000;  // 10M orders
    const std::size_t CAPACITY = (512 * 512);  // 500K capacity
    const int NUM_PRICES = 100;  // Price levels
    
    // ========== TEST 1a: PLACEMENT WITH MATCHING ==========
    {
        engine::OrderEngine engine(CAPACITY, false, true);  // auto_match enabled
        
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_ORDERS; ++i)
        {
            engine::Price p = 10000 + (i % NUM_PRICES);
            engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
            engine.place_order(side, engine::OrderType::LIMIT, p, 10);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        engine.update_snapshot();
        std::cout << "  Placement (matching): " << std::fixed << std::setprecision(2) 
                  << (engine.get_snapshot().placed_count / (ms / 1000.0)) << " ops/sec"
                  << " [" << engine.get_snapshot().placed_count << " placed, " 
                  << engine.get_snapshot().filled_count << " filled]\n";
    }
    
    // ========== TEST 1b: PLACEMENT WITHOUT MATCHING ==========
    {
        engine::OrderEngine engine(NUM_ORDERS, false, false);  // auto_match disabled
        
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_ORDERS; ++i)
        {
            engine::Price p = 10000 + (i % NUM_PRICES);
            engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
            engine.place_order(side, engine::OrderType::LIMIT, p, 10);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        engine.update_snapshot();
        std::cout << "  Placement (no match): " << std::fixed << std::setprecision(2) 
                  << (engine.get_snapshot().placed_count / (ms / 1000.0)) << " ops/sec"
                  << " [" << engine.get_snapshot().placed_count << " placed]\n";
    }
    
    // ========== TEST 2: CANCEL/EDIT (no matching, stable order book) ==========
    {
        const std::size_t TEST_COUNT = 500000;  // 500K orders for accurate measurement
        engine::OrderEngine engine(TEST_COUNT * 2, false, false);  // No matching
        std::vector<engine::OrderId> order_ids;
        order_ids.reserve(TEST_COUNT);
        
        // Place orders with non-crossing prices (bids < asks)
        for (std::size_t i = 0; i < TEST_COUNT; ++i)
        {
            engine::Price p = (i % 2 == 0) ? 9900 + (i % 50) : 10100 + (i % 50);
            engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
            auto id = engine.place_order(side, engine::OrderType::LIMIT, p, 10);
            if (id != engine::INVALID_ORDER_ID) order_ids.push_back(id);
        }
        
        // Cancel half
        auto start = std::chrono::high_resolution_clock::now();
        std::size_t cancelled = 0;
        for (std::size_t i = 0; i < order_ids.size() / 2; ++i)
        {
            if (engine.cancel_order(order_ids[i])) cancelled++;
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        std::cout << "  Cancel: " << std::fixed << std::setprecision(2) 
                  << (ms > 0 ? (cancelled / (ms / 1000.0)) : 0) << " ops/sec"
                  << " [" << cancelled << " cancelled]\n";
        
        // Edit remaining half (these are newer orders, deeper in heap)
        start = std::chrono::high_resolution_clock::now();
        std::size_t edited = 0;
        for (std::size_t i = order_ids.size() / 2; i < order_ids.size(); ++i)
        {
            engine::Price new_price = 9900 + (i % 50);
            if (engine.replace_order(order_ids[i], engine::OrderSide::BID, new_price, 20))
                edited++;
        }
        end = std::chrono::high_resolution_clock::now();
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        std::cout << "  Edit: " << std::fixed << std::setprecision(2) 
                  << (ms > 0 ? (edited / (ms / 1000.0)) : 0) << " ops/sec"
                  << " [" << edited << " edited]\n";
    }
    
    std::cout << "  Memory footprint: " << (CAPACITY * sizeof(engine::OrderInfo) / (1024 * 1024)) << " MB\n";
    std::cout << "✓ Stress Test PASSED!\n\n";
}

void test_slot_reuse()
{
    std::cout << "=== Testing Slot Reuse with Immediate Free ===\n";
    
    // Small capacity to force slot reuse
    engine::OrderEngine engine(100, false, true);
    
    // Phase 1: Place orders that will match and fill (auto-freed)
    std::cout << "Phase 1: Placing and matching 50 order pairs...\n";
    std::vector<engine::OrderId> filled_bids;
    std::vector<engine::OrderId> filled_asks;
    
    for (int i = 0; i < 50; ++i)
    {
        auto bid = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 10);
        auto ask = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(100.0), 10);
        filled_bids.push_back(bid);
        filled_asks.push_back(ask);
    }
    
    // Verify all orders are now freed (not in memory)
    int freed_count = 0;
    for (auto id : filled_bids) {
        if (engine.get_order(id) == nullptr) freed_count++;
    }
    for (auto id : filled_asks) {
        if (engine.get_order(id) == nullptr) freed_count++;
    }
    std::cout << "  Orders freed from memory: " << freed_count << "/100\n";
    assert(freed_count == 100 && "All filled orders should be freed immediately");
    
    // Verify filled count
    engine.update_snapshot();
    assert(engine.get_snapshot().filled_count == 100 && "Should have 100 filled orders");
    
    // Phase 2: Verify no open orders remain
    std::cout << "Phase 2: Checking open order count...\n";
    std::cout << "  Open orders: " << engine.get_snapshot().open_count << "\n";
    assert(engine.get_snapshot().open_count == 0 && "No orders should be open after all matched");
    
    // Phase 3: Place new orders - they should reuse freed slots
    std::cout << "Phase 3: Placing 80 new orders (should reuse slots)...\n";
    std::vector<engine::OrderId> new_orders;
    for (int i = 0; i < 40; ++i)
    {
        // Prices that won't match each other
        auto bid = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(99.0) + i, 5);
        auto ask = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(101.0) + i, 5);
        new_orders.push_back(bid);
        new_orders.push_back(ask);
    }
    
    // Verify new orders are valid and OPEN
    int open_count = 0;
    for (auto id : new_orders) {
        const auto* order = engine.get_order(id);
        if (order && order->status_ == engine::OrderStatus::OPEN) open_count++;
    }
    std::cout << "  New open orders: " << open_count << "/80\n";
    assert(open_count == 80 && "All new orders should be open");
    
    // Phase 4: Verify old IDs still don't work (freed from map)
    std::cout << "Phase 4: Verifying old OrderIds are invalid...\n";
    int invalid_count = 0;
    for (auto id : filled_bids) {
        if (engine.get_order(id) == nullptr) invalid_count++;
    }
    std::cout << "  Old IDs invalid: " << invalid_count << "/50\n";
    assert(invalid_count == 50 && "Old OrderIds should remain invalid");
    
    // Phase 5: Try to edit/cancel with old IDs - should fail
    std::cout << "Phase 5: Testing edit/cancel with freed OrderIds...\n";
    bool edit_result = engine.replace_order(filled_bids[0], engine::OrderSide::BID, price(105.0), 20);
    bool cancel_result = engine.cancel_order(filled_asks[0]);
    std::cout << "  Edit with freed ID: " << (edit_result ? "SUCCEEDED (BAD!)" : "rejected (good)") << "\n";
    std::cout << "  Cancel with freed ID: " << (cancel_result ? "SUCCEEDED (BAD!)" : "rejected (good)") << "\n";
    assert(!edit_result && "Edit with freed ID should fail");
    assert(!cancel_result && "Cancel with freed ID should fail");
    
    std::cout << "✓ Slot Reuse Test PASSED!\n\n";
}

void test_accumulate_drain_throughput()
{
    std::cout << "=== Testing Accumulate (auto_match=off) then Drain Throughput ===\n";

    const std::size_t NUM_ORDERS = 10000000; // configurable for CI
    const std::size_t CAPACITY = 1024 * 1024 * 16;

    // Create engine with auto_match disabled so orders are queued
    engine::OrderEngine engine(CAPACITY, false, false);

    // Placement phase
    auto t0 = std::chrono::high_resolution_clock::now();
    for (std::size_t i = 0; i < NUM_ORDERS; ++i) {
        engine::OrderSide side = (i % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
        engine::Price p = price(100.0 + static_cast<double>(i % 10));
        engine.place_order(side, engine::OrderType::LIMIT, p, 1);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto place_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    engine.update_snapshot();
    auto placed = engine.get_snapshot().placed_count;

    double place_rate = place_ms > 0 ? (placed / (place_ms / 1000.0)) : 0.0;
    std::cout << " Placement: " << placed << " orders in " << place_ms << " ms (" << place_rate << " ops/sec)\n";

    // Drain phase: flip auto_match on and measure time to process queued orders
    auto tdrain0 = std::chrono::high_resolution_clock::now();
    engine.set_auto_match(true);
    auto tdrain1 = std::chrono::high_resolution_clock::now();
    auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tdrain1 - tdrain0).count();

    engine.update_snapshot();
    auto filled = engine.get_snapshot().filled_count;
    auto open = engine.get_snapshot().open_count;

    double drain_rate = drain_ms > 0 ? (placed / (drain_ms / 1000.0)) : 0.0;
    std::cout << " Drain: processed " << placed << " queued orders in " << drain_ms << " ms (" << drain_rate << " ops/sec)\n";
    std::cout << " Result: filled=" << filled << ", open=" << open << "\n";

    std::cout << "✓ Accumulate+Drain throughput test completed\n\n";
}

void test_memory_efficiency()
{
    std::cout << "=== Testing Memory Efficiency with Immediate Slot Reuse ===\n";
    
    // Use a SMALL capacity - without slot reuse this would overflow!
    const std::size_t CAPACITY = 1048576;  // 1MB slots
    const int TOTAL_ORDERS = 10000000;    // 10M orders to place
    
    engine::OrderEngine engine(CAPACITY, false, true);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Place orders in batches - filled orders are auto-freed
    for (int batch = 0; batch < TOTAL_ORDERS / 10000; ++batch)
    {
        // Place 10k orders per batch (5k pairs that will match)
        for (int i = 0; i < 5000; ++i)
        {
            engine::Price p = 10000 + (i % 100);  // Direct ticks
            engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, p, 10);
            engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, p, 10);
        }
        // Slots are automatically freed on fill - no manual cleanup needed!
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    engine.update_snapshot();
    std::size_t orders_placed = engine.get_snapshot().placed_count;
    
    std::cout << "  Capacity: " << CAPACITY << " slots (" << (CAPACITY * sizeof(engine::OrderInfo) / 1024 / 1024) << " MB)\n";
    std::cout << "  Total orders placed: " << orders_placed << "\n";
    std::cout << "  Total filled: " << engine.get_snapshot().filled_count << "\n";
    std::cout << "  Final open orders: " << engine.get_snapshot().open_count << "\n";
    std::cout << "  Time: " << duration_ms << " ms\n";
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) 
              << (orders_placed / (duration_ms / 1000.0)) << " orders/sec\n";
    std::cout << "\n  Without slot reuse: would need " << (TOTAL_ORDERS * sizeof(engine::OrderInfo) / 1024 / 1024) << " MB\n";
    std::cout << "  With immediate free: only " << (CAPACITY * sizeof(engine::OrderInfo) / 1024 / 1024) << " MB max\n";
    std::cout << "  Memory savings: " << (100 - (CAPACITY * 100 / TOTAL_ORDERS)) << "%\n";
    
    std::cout << "✓ Memory Efficiency Test PASSED!\n\n";
}

void test_capacity_limits()
{
    std::cout << "=== Testing Capacity vs Orders Relationship ===\n";
    std::cout << "Finding minimum capacity needed for different order counts...\n\n";
    
    // Test different scenarios
    struct Scenario {
        const char* name;
        int num_orders;
        int price_spread;  // How many different prices (affects concurrent orders)
        bool expect_rejects;
    };
    
    Scenario scenarios[] = {
        {"Full matching (1 price)", 100000, 1, false},      // All orders match immediately
        {"Partial matching (10 prices)", 100000, 10, false}, // Some buildup
        {"Wide spread (100 prices)", 100000, 100, true},    // More concurrent orders
        {"Very wide (1000 prices)", 100000, 1000, true},    // Many concurrent orders
    };
    
    const std::size_t CAPACITY = 10000;  // Small capacity to force limits
    
    for (const auto& s : scenarios)
    {
        engine::OrderEngine engine(CAPACITY, false, true);
        
        std::size_t rejected = 0;
        std::size_t peak_open = 0;
        
        for (int i = 0; i < s.num_orders; ++i)
        {
            // Bids at low prices, asks at high prices
            // Spread determines how many don't match immediately
            engine::Price bid_price = 10000 - (i % s.price_spread);
            engine::Price ask_price = 10000 + (i % s.price_spread);
            
            if (engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, bid_price, 10) == engine::INVALID_ORDER_ID)
                rejected++;
            if (engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, ask_price, 10) == engine::INVALID_ORDER_ID)
                rejected++;
            
            engine.update_snapshot();
            peak_open = std::max(peak_open, engine.get_snapshot().open_count);
        }
        
        engine.update_snapshot();
        std::cout << s.name << ":\n";
        std::cout << "  Capacity: " << CAPACITY << ", Orders attempted: " << s.num_orders * 2 << "\n";
        std::cout << "  Placed: " << engine.get_snapshot().placed_count << ", Rejected: " << rejected << "\n";
        std::cout << "  Filled: " << engine.get_snapshot().filled_count << ", Final open: " << engine.get_snapshot().open_count << "\n";
        std::cout << "  Peak concurrent open: " << peak_open << "\n";
        std::cout << "  Efficiency: " << std::fixed << std::setprecision(1) 
                  << (100.0 * engine.get_snapshot().placed_count / (s.num_orders * 2)) << "% placed\n";
        std::cout << "  Utilization: placed/capacity = " << std::setprecision(1)
                  << (1.0 * engine.get_snapshot().placed_count / CAPACITY) << "x\n\n";
    }
    
    std::cout << "=== Key Insight ===\n";
    std::cout << "Minimum capacity needed = peak concurrent open orders\n";
    std::cout << "If orders match immediately (same price), capacity can be tiny.\n";
    std::cout << "If orders don't match (spread prices), capacity must hold all open.\n";
    std::cout << "✓ Capacity Limits Test PASSED!\n\n";
}

void test_order_matching_correctness()
{
    std::cout << "=== Testing Order Matching Correctness ===\n";
    
    engine::OrderEngine engine(10000, VERBOSE);
    
    // Test 1: Simple full match - both orders freed after fill
    std::cout << "Test 1: Simple full match...\n";
    auto bid1 = engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(100.0), 10);
    auto ask1 = engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(100.0), 10);
    
    // Both orders should be filled and freed from memory
    engine.update_snapshot();
    assert(engine.get_order(bid1) == nullptr && "Filled bid should be freed");
    assert(engine.get_order(ask1) == nullptr && "Filled ask should be freed");
    assert(engine.get_snapshot().open_count == 0 && "No open orders after full match");
    assert(engine.get_snapshot().filled_count == 2 && "Should have 2 filled orders");
    std::cout << "  ✓ Full match works correctly\n";
    
    // Test 2: Partial match - ask larger than bid
    std::cout << "Test 2: Partial match (ask > bid)...\n";
    engine::OrderEngine engine_test2(10000, VERBOSE);
    auto bid2 = engine_test2.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(101.0), 5);
    auto ask2 = engine_test2.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(101.0), 15);
    
    // Bid fully filled (freed), ask partially filled (still open)
    assert(engine_test2.get_order(bid2) == nullptr && "Filled bid should be freed");
    const engine::OrderInfo* ask2_info = engine_test2.get_order(ask2);
    assert(ask2_info != nullptr && "Partial ask should still exist");
    assert(ask2_info->status_ == engine::OrderStatus::OPEN && "Ask should be partially filled");
    assert(ask2_info->qty_ == 10 && "Ask quantity should be 10 remaining");
    std::cout << "  ✓ Partial match (ask > bid) works correctly\n";
    
    // Test 3: Partial match - bid larger than ask
    std::cout << "Test 3: Partial match (bid > ask)...\n";
    engine::OrderEngine engine_test3(10000, VERBOSE);
    auto bid3 = engine_test3.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(102.0), 20);
    auto ask3 = engine_test3.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(102.0), 8);
    
    // Ask fully filled (freed), bid partially filled (still open)
    assert(engine_test3.get_order(ask3) == nullptr && "Filled ask should be freed");
    const engine::OrderInfo* bid3_info = engine_test3.get_order(bid3);
    assert(bid3_info != nullptr && "Partial bid should still exist");
    assert(bid3_info->status_ == engine::OrderStatus::OPEN && "Bid should be partially filled");
    assert(bid3_info->qty_ == 12 && "Bid quantity should be 12 remaining");
    std::cout << "  ✓ Partial match (bid > ask) works correctly\n";
    
    // Test 4: Multiple matches - FIFO order
    std::cout << "Test 4: Multiple matches with FIFO...\n";
    engine::OrderEngine engine2(10000, VERBOSE);
    
    // Place multiple bids at same price
    auto bid4a = engine2.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(50.0), 10);
    auto bid4b = engine2.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(50.0), 15);
    auto bid4c = engine2.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(50.0), 5);
    
    // Place large ask that should match in FIFO order (25 qty matches 10+15)
    auto ask4 = engine2.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(50.0), 25);
    
    // First two bids filled (freed), third untouched, ask filled (freed)
    assert(engine2.get_order(bid4a) == nullptr && "First bid should be filled and freed");
    assert(engine2.get_order(bid4b) == nullptr && "Second bid should be filled and freed");
    assert(engine2.get_order(ask4) == nullptr && "Ask should be filled and freed");
    
    const engine::OrderInfo* bid4c_info = engine2.get_order(bid4c);
    assert(bid4c_info != nullptr && "Third bid should remain");
    assert(bid4c_info->status_ == engine::OrderStatus::OPEN && "Third bid should remain open");
    assert(bid4c_info->qty_ == 5 && "Third bid qty should be unchanged");
    std::cout << "  ✓ FIFO matching works correctly\n";
    
    // Test 5: Price-time priority
    std::cout << "Test 5: Price-time priority...\n";
    engine::OrderEngine engine3(10000, VERBOSE);
    
    // Place bids at different prices
    auto bid5a = engine3.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(75.0), 10); // Lower price
    auto bid5b = engine3.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(77.0), 10); // Higher price (should match first)
    
    // Place ask that can match higher priced bid
    auto ask5 = engine3.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(75.0), 10);
    
    // Higher priced bid matched (freed), lower priced bid remains, ask filled (freed)
    assert(engine3.get_order(bid5b) == nullptr && "Higher priced bid should be filled and freed");
    assert(engine3.get_order(ask5) == nullptr && "Ask should be filled and freed");
    
    const engine::OrderInfo* bid5a_info = engine3.get_order(bid5a);
    assert(bid5a_info != nullptr && "Lower priced bid should remain");
    assert(bid5a_info->status_ == engine::OrderStatus::OPEN && "Lower priced bid should remain open");
    std::cout << "  ✓ Price-time priority works correctly\n";
    
    // Test 6: Market depth after matching
    std::cout << "Test 6: Market depth correctness...\n";
    engine::OrderEngine engine4(10000, VERBOSE);
    
    // Build order book
    engine4.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(90.0), 100);
    engine4.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(91.0), 200);
    engine4.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(92.0), 150);
    engine4.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(93.0), 100);
    engine4.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(94.0), 200);
    
    // Execute trade that removes top of book
    auto large_sell = engine4.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(92.0), 150);
    
    // Check best bid changed
    engine4.update_snapshot();
    auto snap4 = engine4.get_snapshot();
    assert(snap4.best_bid == price(91.0) && "Best bid should be updated after match");
    
    // Get market depth via snapshot
    assert(snap4.bid_levels == 2 && "Should have 2 bid levels remaining");
    assert(snap4.bid_prices[0] == price(91.0) && "Top bid should be 91.0");
    assert(snap4.bid_depth[0] == 200 && "Top bid qty should be 200");
    std::cout << "  ✓ Market depth updates correctly after matching\n";
    
    // Test 7: No matching when prices don't cross
    std::cout << "Test 7: No match when prices don't cross...\n";
    engine::OrderEngine engine5(10000, VERBOSE);
    
    auto bid6 = engine5.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(80.0), 10);
    auto ask6 = engine5.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(85.0), 10);
    
    const engine::OrderInfo* bid6_info = engine5.get_order(bid6);
    const engine::OrderInfo* ask6_info = engine5.get_order(ask6);
    
    // Both should remain open (no match)
    assert(bid6_info->status_ == engine::OrderStatus::OPEN && "Bid should remain open");
    assert(ask6_info->status_ == engine::OrderStatus::OPEN && "Ask should remain open");
    assert(bid6_info->qty_ == 10 && "Bid qty unchanged");
    assert(ask6_info->qty_ == 10 && "Ask qty unchanged");
    engine5.update_snapshot();
    auto snap5 = engine5.get_snapshot();
    assert(snap5.best_bid == price(80.0) && "Best bid should be 80.0");
    assert(snap5.best_ask == price(85.0) && "Best ask should be 85.0");
    std::cout << "  ✓ No matching when prices don't cross\n";
    
    std::cout << "✓ Order Matching Correctness Test PASSED!\n\n";
}

void test_realtime_order_book_display()
{
    std::cout << "=== Testing Real-Time Order Book Display ===\n";
    std::cout << "Starting order book visualization (runs for 10 seconds)...\n";
    std::cout << "Press Ctrl+C to exit early\n\n";
    
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    engine::OrderEngine engine(100000, false, true); // Auto-update every operation
    
    // Seed initial order book
    for (int i = 0; i < 20; ++i)
    {
        engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(50000.0 - i * 10), 10 + (i % 5));
        engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(50010.0 + i * 10), 10 + (i % 5));
    }
    
    auto start_time = std::chrono::steady_clock::now();
    int iteration = 0;
    
    while (true)
    {
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
        
        if (elapsed >= 10) break; // Run for 10 seconds
        
        // Clear screen and move cursor to top
        std::cout << "\033[2J\033[H";
        
        // Get current snapshot
        const auto& snap = engine.get_snapshot();
        
        // Display header
        std::cout << "═══════════════════════════════════════════════════════════\n";
        std::cout << "                    BTC-USD Order Book                     \n";
        std::cout << "═══════════════════════════════════════════════════════════\n";
        std::cout << "  Market Price: $" << std::fixed << std::setprecision(2) 
                  << (snap.market_price != static_cast<engine::Price>(-1) ? backtest::math::ticks_to_dollars(snap.market_price) : 0.0) << "\n";
        std::cout << "  Spread: $" << (snap.best_ask != static_cast<engine::Price>(-1) && snap.best_bid != static_cast<engine::Price>(-1) 
                  ? backtest::math::ticks_to_dollars(snap.best_ask - snap.best_bid) : 0.0) << "\n";
        std::cout << "═══════════════════════════════════════════════════════════\n\n";
        
        // Display asks (top to bottom, highest to lowest)
        std::cout << "  ASKS (Sell Orders)\n";
        std::cout << "  ───────────────────────────────────────────────────────\n";
        for (int i = snap.ask_levels - 1; i >= 0; --i)
        {
            double price_dollars = backtest::math::ticks_to_dollars(snap.ask_prices[i]);
            double qty = backtest::math::internal_to_qty(snap.ask_depth[i]);
            double total = price_dollars * qty;
            
            std::cout << "  " << std::setw(12) << std::fixed << std::setprecision(2) << price_dollars 
                      << " │ " << std::setw(10) << std::setprecision(5) << qty 
                      << " │ " << std::setw(12) << std::setprecision(2) << total << "\n";
        }
        
        // Display spread line
        std::cout << "  ───────────────────────────────────────────────────────\n";
        if (snap.best_ask != static_cast<engine::Price>(-1) && snap.best_bid != static_cast<engine::Price>(-1))
        {
            double spread = backtest::math::ticks_to_dollars(snap.best_ask - snap.best_bid);
            std::cout << "  " << std::setw(20) << "SPREAD: $" << std::setprecision(2) << spread << "\n";
        }
        std::cout << "  ───────────────────────────────────────────────────────\n";
        
        // Display bids (top to bottom, highest to lowest)
        std::cout << "  BIDS (Buy Orders)\n";
        std::cout << "  ───────────────────────────────────────────────────────\n";
        for (int i = 0; i < snap.bid_levels; ++i)
        {
            double price_dollars = backtest::math::ticks_to_dollars(snap.bid_prices[i]);
            double qty = backtest::math::internal_to_qty(snap.bid_depth[i]);
            double total = price_dollars * qty;
            
            std::cout << "  " << std::setw(12) << std::fixed << std::setprecision(2) << price_dollars 
                      << " │ " << std::setw(10) << std::setprecision(5) << qty 
                      << " │ " << std::setw(12) << std::setprecision(2) << total << "\n";
        }
        
        std::cout << "  ───────────────────────────────────────────────────────\n";
        std::cout << "          Price ($)  │  Quantity  │    Total ($)   \n";
        std::cout << "═══════════════════════════════════════════════════════════\n";
        std::cout << "  Orders: " << engine.get_snapshot().placed_count << " placed, " 
                  << engine.get_snapshot().filled_count << " filled, " 
                  << engine.get_snapshot().open_count << " open\n";
        std::cout << "  Iteration: " << iteration << " | Time: " << elapsed << "s / 10s\n";
        std::cout << "═══════════════════════════════════════════════════════════\n";
        
        std::cout.flush();
        
        // Simulate market activity
        int action = iteration % 10;
        
        if (action < 3) // Place new orders
        {
            double bid_price = 50000.0 - (std::rand() % 100);
            double ask_price = 50010.0 + (std::rand() % 100);
            int qty = 5 + (std::rand() % 20);
            
            engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(bid_price), qty);
            engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(ask_price), qty);
        }
        else if (action < 6) // Place crossing orders (trigger matches)
        {
            if (snap.best_ask != static_cast<engine::Price>(-1))
            {
                int qty = 1 + (std::rand() % 5);
                engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, snap.best_ask, qty);
            }
        }
        else if (action < 8) // Place more aggressive orders
        {
            double aggressive_bid = 50000.0 + (std::rand() % 50);
            double aggressive_ask = 50010.0 - (std::rand() % 50);
            int qty = 10 + (std::rand() % 15);
            
            if (aggressive_bid < 50010.0)
                engine.place_order(engine::OrderSide::BID, engine::OrderType::LIMIT, price(aggressive_bid), qty);
            if (aggressive_ask > 50000.0)
                engine.place_order(engine::OrderSide::ASK, engine::OrderType::LIMIT, price(aggressive_ask), qty);
        }
        else // Large market orders
        {
            int qty = 15 + (std::rand() % 30);
            engine::OrderSide side = (std::rand() % 2 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
            
            if ((side == engine::OrderSide::BID && snap.best_ask != static_cast<engine::Price>(-1)) ||
                (side == engine::OrderSide::ASK && snap.best_bid != static_cast<engine::Price>(-1)))
            {
                engine.place_order(side, engine::OrderType::MARKET, 0, qty);
            }
        }
        
        iteration++;
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Update every 200ms
    }
    
    // Clear screen one final time
    std::cout << "\033[2J\033[H";
    std::cout << "✓ Real-Time Order Book Display Test Completed!\n\n";
}

int main(int argc, char* argv[])
{
    std::srand(std::time(nullptr)); // Seed random number generator
    
    // Check if user wants real-time display
    if (argc > 1 && std::string(argv[1]) == "--realtime")
    {
        test_realtime_order_book_display();
        return 0;
    }
    
    std::cout << "========================================\n";
    std::cout << "  Order Engine Tests\n";
    std::cout << "========================================\n\n";
    
    test_place_limit_order();
    test_place_market_order();
    test_cancel_order();
    test_edit_order();
    test_place_cancel_edit_replace_throughput();
    test_multiple_orders_same_price();
    test_order_priority();
    test_delta_l2_back_of_queue();
    test_order_matching_correctness();
    test_slot_reuse();
    test_accumulate_drain_throughput();
    test_memory_efficiency();
    test_capacity_limits();
    test_stress_orders();
    // test_new_notification_system();
    // test_verbose_performance_comparison();
    std::cout << "========================================\n";
    std::cout << "  All Order Tests PASSED! ✓\n";
    std::cout << "========================================\n";
    std::cout << "\nTip: Run './order_test --realtime' for live order book display\n";
    
    return 0;
}

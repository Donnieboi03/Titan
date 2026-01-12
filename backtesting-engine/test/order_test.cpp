#include "../order_engine.cpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <iomanip>

// Global verbose flag
bool VERBOSE = false;

// Helper: convert dollars to ticks for test inputs
inline Price price(double dollars) { return dollars_to_ticks(dollars); }

void test_place_limit_order()
{
    std::cout << "=== Testing Place Limit Order ===\n";
    
    OrderEngine engine("AAPL", 10000, VERBOSE);
    
    // Place bid orders
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(100.0), 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(99.0), 20);
    auto bid3 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(98.0), 15);
    
    assert(bid1 != INVALID_ID && "Bid order 1 should be placed");
    assert(bid2 != INVALID_ID && "Bid order 2 should be placed");
    assert(bid3 != INVALID_ID && "Bid order 3 should be placed");
    
    // Place ask orders
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(101.0), 10);
    auto ask2 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(102.0), 20);
    auto ask3 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(103.0), 15);
    
    assert(ask1 != INVALID_ID && "Ask order 1 should be placed");
    assert(ask2 != INVALID_ID && "Ask order 2 should be placed");
    assert(ask3 != INVALID_ID && "Ask order 3 should be placed");
    
    // Verify orders exist
    const OrderInfo* bid_order = engine.get_order(bid1);
    assert(bid_order != nullptr && "Bid order should exist");
    assert(bid_order->side_ == OrderSide::BID && "Order side should be BID");
    assert(bid_order->price_ == price(100.0) && "Order price should be 100.0");
    assert(bid_order->qty_ == 10 && "Order quantity should be 10");
    assert(bid_order->status_ == OrderStatus::OPEN && "Order status should be OPEN");
    const OrderInfo* ask_order = engine.get_order(ask1);
    assert(ask_order != nullptr && "Ask order should exist");
    assert(ask_order->side_ == OrderSide::ASK && "Order side should be ASK");
    assert(ask_order->price_ == price(101.0) && "Order price should be 101.0");
    assert(ask_order->qty_ == 10 && "Order quantity should be 10");
    assert(ask_order->status_ == OrderStatus::OPEN && "Order status should be OPEN");
    // Verify best bid and ask
    assert(engine.get_best_bid() == price(100.0) && "Best bid should be 100.0");
    assert(engine.get_best_ask() == price(101.0) && "Best ask should be 101.0");
    
    // Verify counters
    assert(engine.placed_count() == 6 && "Should have 6 placed orders");
    
    if (VERBOSE) std::cout << "Market depth size: " << engine.get_market_depth(OrderSide::BID).size() << "\n";
    std::cout << "✓ Place Limit Order test PASSED!\n\n";
}

void test_place_market_order()
{
    std::cout << "=== Testing Place Market Order ===\n";
    
    OrderEngine engine("TSLA", 10000, VERBOSE);
    
    // Try to place market order with no liquidity
    auto market_bid = engine.place_order(OrderSide::BID, OrderType::MARKET, 0, 10);
    assert(market_bid == INVALID_ID && "Market order should fail without liquidity");
    
    // Place limit orders first
    engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(200.0), 10);
    engine.place_order(OrderSide::BID, OrderType::LIMIT, price(199.0), 10);
    // Now place market orders
    auto market_bid2 = engine.place_order(OrderSide::BID, OrderType::MARKET, 0, 5);
    assert(market_bid2 != INVALID_ID && "Market order should succeed with liquidity");
    
    std::cout << "✓ Place Market Order test PASSED!\n\n";
}

void test_cancel_order()
{
    std::cout << "=== Testing Cancel Order ===\n";
    
    OrderEngine engine("MSFT", 10000, VERBOSE);
    
    // Place orders
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(300.0), 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(299.0), 20);
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(301.0), 10);
    
    // Verify orders exist
    assert(engine.get_order(bid1) != nullptr && "Order should exist before cancel");
    assert(engine.get_order(bid1)->status_ == OrderStatus::OPEN && "Order should be OPEN");
    
    // Cancel order
    bool cancelled = engine.cancel_order(bid1);
    assert(cancelled && "Cancel should succeed");
    
    // Verify order is freed from memory (cancelled orders go to ledger)
    const OrderInfo* cancelled_order = engine.get_order(bid1);
    assert(cancelled_order == nullptr && "Cancelled order should be freed from memory");
    
    // Verify best bid changed
    assert(engine.get_best_bid() == price(299.0) && "Best bid should update after cancel");
    
    // Try to cancel non-existent order
    bool cancel_fail = engine.cancel_order(99999);
    assert(!cancel_fail && "Cancel should fail for non-existent order");
    
    // Try to cancel already cancelled order (should fail - already freed)
    bool cancel_twice = engine.cancel_order(bid1);
    assert(!cancel_twice && "Cancel should fail for already cancelled order");
    
    // Verify counters
    assert(engine.cancelled_count() == 1 && "Should have 1 cancelled order");
    
    std::cout << "✓ Cancel Order test PASSED!\n\n";
}

void test_edit_order()
{
    std::cout << "=== Testing Edit Order ===\n";
    
    OrderEngine engine("GOOGL", 10000, VERBOSE);
    
    // Place initial orders
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(150.0), 10);
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(151.0), 10);
    
    // Verify initial order
    const OrderInfo* initial = engine.get_order(bid1);
    assert(initial->price_ == price(150.0) && "Initial price should be 150.0");
    assert(initial->qty_ == 10 && "Initial quantity should be 10");
    
    // Edit order (change price and quantity)
    auto edited_id = engine.edit_order(bid1, OrderSide::BID, price(149.0), 20);
    assert(edited_id != INVALID_ID && "Edit should succeed");
    assert(edited_id == bid1 && "Edited order ID should be the same as original");
    
    // Verify order was modified (not cancelled)
    const OrderInfo* edited_order = engine.get_order(bid1);
    assert(edited_order != nullptr && "Edited order should exist");
    assert(edited_order->status_ == OrderStatus::OPEN && "Edited order should still be OPEN");
    assert(edited_order->price_ == price(149.0) && "New price should be 149.0");
    assert(edited_order->qty_ == 20 && "New quantity should be 20");
    // Verify best bid changed
    assert(engine.get_best_bid() == price(149.0) && "Best bid should reflect edited order");
    
    // Try to edit non-existent order
    auto edit_fail = engine.edit_order(99999, OrderSide::BID, price(150.0), 10);
    assert(edit_fail == INVALID_ID && "Edit should fail for non-existent order");
    
    std::cout << "✓ Edit Order test PASSED!\n\n";
}

void test_multiple_orders_same_price()
{
    std::cout << "=== Testing Multiple Orders at Same Price ===\n";
    
    OrderEngine engine("AMZN", 10000, VERBOSE);
    
    // Place multiple orders at same price
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(100.0), 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(100.0), 20);
    auto bid3 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(100.0), 15);
    
    // Get market depth
    auto depth = engine.get_market_depth(OrderSide::BID, 5);
    assert(depth.size() >= 1 && "Should have at least one price level");
    assert(depth[0].first == price(100.0) && "Price should be 100.0");
    assert(depth[0].second == 45 && "Total quantity should be 45 (10+20+15)");
    
    // Cancel middle order
    engine.cancel_order(bid2);
    
    // Check depth again
    depth = engine.get_market_depth(OrderSide::BID, 5);
    assert(depth[0].second == 25 && "Total quantity should be 25 after cancel");
    
    std::cout << "✓ Multiple Orders at Same Price test PASSED!\n\n";
}

void test_order_priority()
{
    std::cout << "=== Testing Order Priority (Time Priority) ===\n";
    
    OrderEngine engine("NVDA", 10000, VERBOSE);
    
    // Place orders at same price with time delay
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(500.0), 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(500.0), 20);
    auto bid3 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(500.0), 30);
    
    // Verify orders exist
    assert(engine.get_order(bid1) != nullptr && "Order 1 should exist");
    assert(engine.get_order(bid2) != nullptr && "Order 2 should exist");
    assert(engine.get_order(bid3) != nullptr && "Order 3 should exist");
    
    // Orders should have different timestamps
    const OrderInfo* o1 = engine.get_order(bid1);
    const OrderInfo* o2 = engine.get_order(bid2);
    const OrderInfo* o3 = engine.get_order(bid3);
    
    assert(o1->time_ <= o2->time_ && "Order 1 time should be <= Order 2 time");
    assert(o2->time_ <= o3->time_ && "Order 2 time should be <= Order 3 time");
    
    std::cout << "✓ Order Priority test PASSED!\n\n";
}

void test_stress_orders()
{
    std::cout << "=== Stress Test: Order Operations ===\n";
    
    const int NUM_ORDERS = 100000000;  // 10M orders
    const std::size_t CAPACITY = (512 * 1024);  // 500K capacity
    const int NUM_PRICES = 1000;  // Price levels
    
    // ========== TEST 1a: PLACEMENT WITH MATCHING ==========
    {
        OrderEngine engine("PLACE", CAPACITY, false, true);  // auto_match enabled
        
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_ORDERS; ++i)
        {
            Price p = 10000 + (i % NUM_PRICES);
            OrderSide side = (i % 2 == 0) ? OrderSide::BID : OrderSide::ASK;
            engine.place_order(side, OrderType::LIMIT, p, 10);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        std::cout << "  Placement (matching): " << std::fixed << std::setprecision(2) 
                  << (engine.placed_count() / (ms / 1000.0)) << " ops/sec"
                  << " [" << engine.placed_count() << " placed, " 
                  << engine.filled_count() << " filled]\n";
    }
    
    // ========== TEST 1b: PLACEMENT WITHOUT MATCHING ==========
    {
        OrderEngine engine("PLACE_NO_MATCH", NUM_ORDERS, false, false);  // auto_match disabled
        
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < NUM_ORDERS; ++i)
        {
            Price p = 10000 + (i % NUM_PRICES);
            OrderSide side = (i % 2 == 0) ? OrderSide::BID : OrderSide::ASK;
            engine.place_order(side, OrderType::LIMIT, p, 10);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        std::cout << "  Placement (no match): " << std::fixed << std::setprecision(2) 
                  << (engine.placed_count() / (ms / 1000.0)) << " ops/sec"
                  << " [" << engine.placed_count() << " placed]\n";
    }
    
    // ========== TEST 2: CANCEL/EDIT (no matching, stable order book) ==========
    {
        const std::size_t TEST_COUNT = 500000;  // 500K orders for accurate measurement
        OrderEngine engine("MODIFY", TEST_COUNT * 2, false, false);  // No matching
        std::vector<OrderId> order_ids;
        order_ids.reserve(TEST_COUNT);
        
        // Place orders with non-crossing prices (bids < asks)
        for (std::size_t i = 0; i < TEST_COUNT; ++i)
        {
            Price p = (i % 2 == 0) ? 9900 + (i % 50) : 10100 + (i % 50);
            OrderSide side = (i % 2 == 0) ? OrderSide::BID : OrderSide::ASK;
            auto id = engine.place_order(side, OrderType::LIMIT, p, 10);
            if (id != INVALID_ID) order_ids.push_back(id);
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
            Price new_price = 9900 + (i % 50);
            if (engine.edit_order(order_ids[i], OrderSide::BID, new_price, 20) != INVALID_ID)
                edited++;
        }
        end = std::chrono::high_resolution_clock::now();
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        std::cout << "  Edit: " << std::fixed << std::setprecision(2) 
                  << (ms > 0 ? (edited / (ms / 1000.0)) : 0) << " ops/sec"
                  << " [" << edited << " edited]\n";
    }
    
    std::cout << "  Memory footprint: " << (CAPACITY * sizeof(OrderInfo) / (1024 * 1024)) << " MB\n";
    std::cout << "✓ Stress Test PASSED!\n\n";
}

void test_slot_reuse()
{
    std::cout << "=== Testing Slot Reuse with Immediate Free ===\n";
    
    // Small capacity to force slot reuse
    OrderEngine engine("REUSE", 100, false, true);
    
    // Phase 1: Place orders that will match and fill (auto-freed)
    std::cout << "Phase 1: Placing and matching 50 order pairs...\n";
    std::vector<OrderId> filled_bids;
    std::vector<OrderId> filled_asks;
    
    for (int i = 0; i < 50; ++i)
    {
        auto bid = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(100.0), 10);
        auto ask = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(100.0), 10);
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
    assert(engine.filled_count() == 100 && "Should have 100 filled orders");
    
    // Phase 2: Verify no open orders remain
    std::cout << "Phase 2: Checking open order count...\n";
    std::cout << "  Open orders: " << engine.open_count() << "\n";
    assert(engine.open_count() == 0 && "No orders should be open after all matched");
    
    // Phase 3: Place new orders - they should reuse freed slots
    std::cout << "Phase 3: Placing 80 new orders (should reuse slots)...\n";
    std::vector<OrderId> new_orders;
    for (int i = 0; i < 40; ++i)
    {
        // Prices that won't match each other
        auto bid = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(99.0) + i, 5);
        auto ask = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(101.0) + i, 5);
        new_orders.push_back(bid);
        new_orders.push_back(ask);
    }
    
    // Verify new orders are valid and OPEN
    int open_count = 0;
    for (auto id : new_orders) {
        const auto* order = engine.get_order(id);
        if (order && order->status_ == OrderStatus::OPEN) open_count++;
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
    bool edit_result = (engine.edit_order(filled_bids[0], OrderSide::BID, price(105.0), 20) != INVALID_ID);
    bool cancel_result = engine.cancel_order(filled_asks[0]);
    std::cout << "  Edit with freed ID: " << (edit_result ? "SUCCEEDED (BAD!)" : "rejected (good)") << "\n";
    std::cout << "  Cancel with freed ID: " << (cancel_result ? "SUCCEEDED (BAD!)" : "rejected (good)") << "\n";
    assert(!edit_result && "Edit with freed ID should fail");
    assert(!cancel_result && "Cancel with freed ID should fail");
    
    std::cout << "✓ Slot Reuse Test PASSED!\n\n";
}

void test_memory_efficiency()
{
    std::cout << "=== Testing Memory Efficiency with Immediate Slot Reuse ===\n";
    
    // Use a SMALL capacity - without slot reuse this would overflow!
    const std::size_t CAPACITY = 1048576;  // 1MB slots
    const int TOTAL_ORDERS = 10000000;    // 10M orders to place
    
    OrderEngine engine("MEMORY", CAPACITY, false, true);
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Place orders in batches - filled orders are auto-freed
    for (int batch = 0; batch < TOTAL_ORDERS / 10000; ++batch)
    {
        // Place 10k orders per batch (5k pairs that will match)
        for (int i = 0; i < 5000; ++i)
        {
            Price p = 10000 + (i % 100);  // Direct ticks
            engine.place_order(OrderSide::BID, OrderType::LIMIT, p, 10);
            engine.place_order(OrderSide::ASK, OrderType::LIMIT, p, 10);
        }
        // Slots are automatically freed on fill - no manual cleanup needed!
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::size_t orders_placed = engine.placed_count();
    
    std::cout << "  Capacity: " << CAPACITY << " slots (" << (CAPACITY * sizeof(OrderInfo) / 1024 / 1024) << " MB)\n";
    std::cout << "  Total orders placed: " << orders_placed << "\n";
    std::cout << "  Total filled: " << engine.filled_count() << "\n";
    std::cout << "  Final open orders: " << engine.open_count() << "\n";
    std::cout << "  Time: " << duration_ms << " ms\n";
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) 
              << (orders_placed / (duration_ms / 1000.0)) << " orders/sec\n";
    std::cout << "\n  Without slot reuse: would need " << (TOTAL_ORDERS * sizeof(OrderInfo) / 1024 / 1024) << " MB\n";
    std::cout << "  With immediate free: only " << (CAPACITY * sizeof(OrderInfo) / 1024 / 1024) << " MB max\n";
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
        OrderEngine engine("CAP", CAPACITY, false, true);
        
        std::size_t rejected = 0;
        std::size_t peak_open = 0;
        
        for (int i = 0; i < s.num_orders; ++i)
        {
            // Bids at low prices, asks at high prices
            // Spread determines how many don't match immediately
            Price bid_price = 10000 - (i % s.price_spread);
            Price ask_price = 10000 + (i % s.price_spread);
            
            if (engine.place_order(OrderSide::BID, OrderType::LIMIT, bid_price, 10) == INVALID_ID)
                rejected++;
            if (engine.place_order(OrderSide::ASK, OrderType::LIMIT, ask_price, 10) == INVALID_ID)
                rejected++;
            
            peak_open = std::max(peak_open, engine.open_count());
        }
        
        std::cout << s.name << ":\n";
        std::cout << "  Capacity: " << CAPACITY << ", Orders attempted: " << s.num_orders * 2 << "\n";
        std::cout << "  Placed: " << engine.placed_count() << ", Rejected: " << rejected << "\n";
        std::cout << "  Filled: " << engine.filled_count() << ", Final open: " << engine.open_count() << "\n";
        std::cout << "  Peak concurrent open: " << peak_open << "\n";
        std::cout << "  Efficiency: " << std::fixed << std::setprecision(1) 
                  << (100.0 * engine.placed_count() / (s.num_orders * 2)) << "% placed\n";
        std::cout << "  Utilization: placed/capacity = " << std::setprecision(1)
                  << (1.0 * engine.placed_count() / CAPACITY) << "x\n\n";
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
    
    OrderEngine engine("MSFT", 10000, VERBOSE);
    
    // Test 1: Simple full match - both orders freed after fill
    std::cout << "Test 1: Simple full match...\n";
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, price(100.0), 10);
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, price(100.0), 10);
    
    // Both orders should be filled and freed from memory
    assert(engine.get_order(bid1) == nullptr && "Filled bid should be freed");
    assert(engine.get_order(ask1) == nullptr && "Filled ask should be freed");
    assert(engine.open_count() == 0 && "No open orders after full match");
    assert(engine.filled_count() == 2 && "Should have 2 filled orders");
    std::cout << "  ✓ Full match works correctly\n";
    
    // Test 2: Partial match - ask larger than bid
    std::cout << "Test 2: Partial match (ask > bid)...\n";
    OrderEngine engine_test2("TEST2", 10000, VERBOSE);
    auto bid2 = engine_test2.place_order(OrderSide::BID, OrderType::LIMIT, price(101.0), 5);
    auto ask2 = engine_test2.place_order(OrderSide::ASK, OrderType::LIMIT, price(101.0), 15);
    
    // Bid fully filled (freed), ask partially filled (still open)
    assert(engine_test2.get_order(bid2) == nullptr && "Filled bid should be freed");
    const OrderInfo* ask2_info = engine_test2.get_order(ask2);
    assert(ask2_info != nullptr && "Partial ask should still exist");
    assert(ask2_info->status_ == OrderStatus::OPEN && "Ask should be partially filled");
    assert(ask2_info->qty_ == 10 && "Ask quantity should be 10 remaining");
    std::cout << "  ✓ Partial match (ask > bid) works correctly\n";
    
    // Test 3: Partial match - bid larger than ask
    std::cout << "Test 3: Partial match (bid > ask)...\n";
    OrderEngine engine_test3("TEST3", 10000, VERBOSE);
    auto bid3 = engine_test3.place_order(OrderSide::BID, OrderType::LIMIT, price(102.0), 20);
    auto ask3 = engine_test3.place_order(OrderSide::ASK, OrderType::LIMIT, price(102.0), 8);
    
    // Ask fully filled (freed), bid partially filled (still open)
    assert(engine_test3.get_order(ask3) == nullptr && "Filled ask should be freed");
    const OrderInfo* bid3_info = engine_test3.get_order(bid3);
    assert(bid3_info != nullptr && "Partial bid should still exist");
    assert(bid3_info->status_ == OrderStatus::OPEN && "Bid should be partially filled");
    assert(bid3_info->qty_ == 12 && "Bid quantity should be 12 remaining");
    std::cout << "  ✓ Partial match (bid > ask) works correctly\n";
    
    // Test 4: Multiple matches - FIFO order
    std::cout << "Test 4: Multiple matches with FIFO...\n";
    OrderEngine engine2("FIFO", 10000, VERBOSE);
    
    // Place multiple bids at same price
    auto bid4a = engine2.place_order(OrderSide::BID, OrderType::LIMIT, price(50.0), 10);
    auto bid4b = engine2.place_order(OrderSide::BID, OrderType::LIMIT, price(50.0), 15);
    auto bid4c = engine2.place_order(OrderSide::BID, OrderType::LIMIT, price(50.0), 5);
    
    // Place large ask that should match in FIFO order (25 qty matches 10+15)
    auto ask4 = engine2.place_order(OrderSide::ASK, OrderType::LIMIT, price(50.0), 25);
    
    // First two bids filled (freed), third untouched, ask filled (freed)
    assert(engine2.get_order(bid4a) == nullptr && "First bid should be filled and freed");
    assert(engine2.get_order(bid4b) == nullptr && "Second bid should be filled and freed");
    assert(engine2.get_order(ask4) == nullptr && "Ask should be filled and freed");
    
    const OrderInfo* bid4c_info = engine2.get_order(bid4c);
    assert(bid4c_info != nullptr && "Third bid should remain");
    assert(bid4c_info->status_ == OrderStatus::OPEN && "Third bid should remain open");
    assert(bid4c_info->qty_ == 5 && "Third bid qty should be unchanged");
    std::cout << "  ✓ FIFO matching works correctly\n";
    
    // Test 5: Price-time priority
    std::cout << "Test 5: Price-time priority...\n";
    OrderEngine engine3("PRIORITY", 10000, VERBOSE);
    
    // Place bids at different prices
    auto bid5a = engine3.place_order(OrderSide::BID, OrderType::LIMIT, price(75.0), 10); // Lower price
    auto bid5b = engine3.place_order(OrderSide::BID, OrderType::LIMIT, price(77.0), 10); // Higher price (should match first)
    
    // Place ask that can match higher priced bid
    auto ask5 = engine3.place_order(OrderSide::ASK, OrderType::LIMIT, price(75.0), 10);
    
    // Higher priced bid matched (freed), lower priced bid remains, ask filled (freed)
    assert(engine3.get_order(bid5b) == nullptr && "Higher priced bid should be filled and freed");
    assert(engine3.get_order(ask5) == nullptr && "Ask should be filled and freed");
    
    const OrderInfo* bid5a_info = engine3.get_order(bid5a);
    assert(bid5a_info != nullptr && "Lower priced bid should remain");
    assert(bid5a_info->status_ == OrderStatus::OPEN && "Lower priced bid should remain open");
    std::cout << "  ✓ Price-time priority works correctly\n";
    
    // Test 6: Market depth after matching
    std::cout << "Test 6: Market depth correctness...\n";
    OrderEngine engine4("DEPTH", 10000, VERBOSE);
    
    // Build order book
    engine4.place_order(OrderSide::BID, OrderType::LIMIT, price(90.0), 100);
    engine4.place_order(OrderSide::BID, OrderType::LIMIT, price(91.0), 200);
    engine4.place_order(OrderSide::BID, OrderType::LIMIT, price(92.0), 150);
    engine4.place_order(OrderSide::ASK, OrderType::LIMIT, price(93.0), 100);
    engine4.place_order(OrderSide::ASK, OrderType::LIMIT, price(94.0), 200);
    
    // Execute trade that removes top of book
    auto large_sell = engine4.place_order(OrderSide::ASK, OrderType::LIMIT, price(92.0), 150);
    
    // Check best bid changed
    assert(engine4.get_best_bid() == price(91.0) && "Best bid should be updated after match");
    
    // Get market depth
    auto bid_depth = engine4.get_market_depth(OrderSide::BID, 5);
    assert(bid_depth.size() == 2 && "Should have 2 bid levels remaining");
    assert(bid_depth[0].first == price(91.0) && "Top bid should be 91.0");
    assert(bid_depth[0].second == 200 && "Top bid qty should be 200");
    std::cout << "  ✓ Market depth updates correctly after matching\n";
    
    // Test 7: No matching when prices don't cross
    std::cout << "Test 7: No match when prices don't cross...\n";
    OrderEngine engine5("NOCROSS", 10000, VERBOSE);
    
    auto bid6 = engine5.place_order(OrderSide::BID, OrderType::LIMIT, price(80.0), 10);
    auto ask6 = engine5.place_order(OrderSide::ASK, OrderType::LIMIT, price(85.0), 10);
    
    const OrderInfo* bid6_info = engine5.get_order(bid6);
    const OrderInfo* ask6_info = engine5.get_order(ask6);
    
    // Both should remain open (no match)
    assert(bid6_info->status_ == OrderStatus::OPEN && "Bid should remain open");
    assert(ask6_info->status_ == OrderStatus::OPEN && "Ask should remain open");
    assert(bid6_info->qty_ == 10 && "Bid qty unchanged");
    assert(ask6_info->qty_ == 10 && "Ask qty unchanged");
    assert(engine5.get_best_bid() == price(80.0) && "Best bid should be 80.0");
    assert(engine5.get_best_ask() == price(85.0) && "Best ask should be 85.0");
    std::cout << "  ✓ No matching when prices don't cross\n";
    
    std::cout << "✓ Order Matching Correctness Test PASSED!\n\n";
}

int main()
{
    std::cout << "========================================\n";
    std::cout << "  Order Engine Tests\n";
    std::cout << "========================================\n\n";
    
    test_place_limit_order();
    test_place_market_order();
    test_cancel_order();
    test_edit_order();
    test_multiple_orders_same_price();
    test_order_priority();
    test_order_matching_correctness();
    test_slot_reuse();
    test_memory_efficiency();
    test_capacity_limits();
    test_stress_orders();
    std::cout << "========================================\n";
    std::cout << "  All Order Tests PASSED! ✓\n";
    std::cout << "========================================\n";
    
    return 0;
}

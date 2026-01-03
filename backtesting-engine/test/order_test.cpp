#include "../order_engine.cpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <iomanip>

// Global verbose flag
bool VERBOSE = false;

void test_place_limit_order()
{
    std::cout << "=== Testing Place Limit Order ===\n";
    
    OrderEngine engine("AAPL", 10000, VERBOSE);
    
    // Place bid orders
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 100.0, 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 99.0, 20);
    auto bid3 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 98.0, 15);
    
    assert(bid1 != static_cast<OrderId>(-1) && "Bid order 1 should be placed");
    assert(bid2 != static_cast<OrderId>(-1) && "Bid order 2 should be placed");
    assert(bid3 != static_cast<OrderId>(-1) && "Bid order 3 should be placed");
    
    // Place ask orders
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, 101.0, 10);
    auto ask2 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, 102.0, 20);
    auto ask3 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, 103.0, 15);
    
    assert(ask1 != static_cast<OrderId>(-1) && "Ask order 1 should be placed");
    assert(ask2 != static_cast<OrderId>(-1) && "Ask order 2 should be placed");
    assert(ask3 != static_cast<OrderId>(-1) && "Ask order 3 should be placed");
    
    // Verify orders exist
    const OrderInfo* bid_order = engine.get_order(bid1);
    assert(bid_order != nullptr && "Bid order should exist");
    assert(bid_order->side_ == OrderSide::BID && "Order side should be BID");
    assert(bid_order->price_ == 100.0 && "Order price should be 100.0");
    assert(bid_order->qty_ == 10 && "Order quantity should be 10");
    assert(bid_order->status_ == OrderStatus::OPEN && "Order status should be OPEN");
    
    const OrderInfo* ask_order = engine.get_order(ask1);
    assert(ask_order != nullptr && "Ask order should exist");
    assert(ask_order->side_ == OrderSide::ASK && "Order side should be ASK");
    assert(ask_order->price_ == 101.0 && "Order price should be 101.0");
    assert(ask_order->qty_ == 10 && "Order quantity should be 10");
    assert(ask_order->status_ == OrderStatus::OPEN && "Order status should be OPEN");
    
    // Verify best bid and ask
    assert(engine.get_best_bid() == 100.0 && "Best bid should be 100.0");
    assert(engine.get_best_ask() == 101.0 && "Best ask should be 101.0");
    
    if (VERBOSE) std::cout << "Market depth size: " << engine.get_market_depth(OrderSide::BID).size() << "\n";
    std::cout << "✓ Place Limit Order test PASSED!\n\n";
}

void test_place_market_order()
{
    std::cout << "=== Testing Place Market Order ===\n";
    
    OrderEngine engine("TSLA", 10000, VERBOSE);
    
    // Try to place market order with no liquidity
    auto market_bid = engine.place_order(OrderSide::BID, OrderType::MARKET, 0, 10);
    assert(market_bid == static_cast<OrderId>(-1) && "Market order should fail without liquidity");
    
    // Place limit orders first
    engine.place_order(OrderSide::ASK, OrderType::LIMIT, 200.0, 10);
    engine.place_order(OrderSide::BID, OrderType::LIMIT, 199.0, 10);
    // Now place market orders
    auto market_bid2 = engine.place_order(OrderSide::BID, OrderType::MARKET, 0, 5);
    assert(market_bid2 != static_cast<OrderId>(-1) && "Market order should succeed with liquidity");
    
    std::cout << "✓ Place Market Order test PASSED!\n\n";
}

void test_cancel_order()
{
    std::cout << "=== Testing Cancel Order ===\n";
    
    OrderEngine engine("MSFT", 10000, VERBOSE);
    
    // Place orders
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 300.0, 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 299.0, 20);
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, 301.0, 10);
    
    // Verify orders exist
    assert(engine.get_order(bid1) != nullptr && "Order should exist before cancel");
    assert(engine.get_order(bid1)->status_ == OrderStatus::OPEN && "Order should be OPEN");
    
    // Cancel order
    bool cancelled = engine.cancel_order(bid1);
    assert(cancelled && "Cancel should succeed");
    
    // Verify order is cancelled
    const OrderInfo* cancelled_order = engine.get_order(bid1);
    assert(cancelled_order != nullptr && "Order should still exist");
    assert(cancelled_order->status_ == OrderStatus::CANCELLED && "Order status should be CANCELLED");
    
    // Verify best bid changed
    assert(engine.get_best_bid() == 299.0 && "Best bid should update after cancel");
    
    // Try to cancel non-existent order
    bool cancel_fail = engine.cancel_order(99999);
    assert(!cancel_fail && "Cancel should fail for non-existent order");
    
    // Try to cancel already cancelled order
    bool cancel_twice = engine.cancel_order(bid1);
    assert(!cancel_twice && "Cancel should fail for already cancelled order");
    
    std::cout << "✓ Cancel Order test PASSED!\n\n";
}

void test_edit_order()
{
    std::cout << "=== Testing Edit Order ===\n";
    
    OrderEngine engine("GOOGL", 10000, VERBOSE);
    
    // Place initial orders
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 150.0, 10);
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, 151.0, 10);
    
    // Verify initial order
    const OrderInfo* initial = engine.get_order(bid1);
    assert(initial->price_ == 150.0 && "Initial price should be 150.0");
    assert(initial->qty_ == 10 && "Initial quantity should be 10");
    
    // Edit order (change price and quantity)
    auto edited_id = engine.edit_order(bid1, OrderSide::BID, 149.0, 20);
    assert(edited_id != 0 && "Edit should succeed");
    assert(edited_id == bid1 && "Edited order ID should be the same as original");
    
    // Verify order was modified (not cancelled)
    const OrderInfo* edited_order = engine.get_order(bid1);
    assert(edited_order != nullptr && "Edited order should exist");
    assert(edited_order->status_ == OrderStatus::OPEN && "Edited order should still be OPEN");
    assert(edited_order->price_ == 149.0 && "New price should be 149.0");
    assert(edited_order->qty_ == 20 && "New quantity should be 20");
    
    // Verify best bid changed
    assert(engine.get_best_bid() == 149.0 && "Best bid should reflect edited order");
    
    // Try to edit non-existent order
    auto edit_fail = engine.edit_order(99999, OrderSide::BID, 150.0, 10);
    assert(edit_fail == static_cast<OrderId>(-1) && "Edit should fail for non-existent order");
    
    std::cout << "✓ Edit Order test PASSED!\n\n";
}

void test_multiple_orders_same_price()
{
    std::cout << "=== Testing Multiple Orders at Same Price ===\n";
    
    OrderEngine engine("AMZN", 10000, VERBOSE);
    
    // Place multiple orders at same price
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 100.0, 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 100.0, 20);
    auto bid3 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 100.0, 15);
    
    // Get market depth
    auto depth = engine.get_market_depth(OrderSide::BID, 5);
    assert(depth.size() >= 1 && "Should have at least one price level");
    assert(depth[0].first == 100.0 && "Price should be 100.0");
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
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 500.0, 10);
    auto bid2 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 500.0, 20);
    auto bid3 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 500.0, 30);
    
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
    std::cout << "=== Stress Testing Order Operations ===\n";
    
    const int NUM_ORDERS = 10000000;
    OrderEngine engine("SPY", NUM_ORDERS + 1, false, false);
    
    std::vector<OrderId> order_ids;
    
    // ========== PLACEMENT THROUGHPUT ==========
    std::cout << "Placing " << NUM_ORDERS << " orders...\n";
    auto place_start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < NUM_ORDERS; ++i)
    {
        double price = 100.0 + (i % 100) * 0.1;
        OrderSide side = (i % 2 == 0) ? OrderSide::BID : OrderSide::ASK;
        auto id = engine.place_order(side, OrderType::LIMIT, price, 10);
        if (id != static_cast<OrderId>(-1))
            order_ids.push_back(id);
    }
    
    auto place_end = std::chrono::high_resolution_clock::now();
    auto place_duration = std::chrono::duration_cast<std::chrono::milliseconds>(place_end - place_start).count();
    double place_throughput = (order_ids.size() / (place_duration / 1000.0));
    
    std::cout << "Placed " << order_ids.size() << " orders successfully.\n";
    std::cout << "Time: " << place_duration << " ms\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
              << place_throughput << " orders/sec\n";
    std::cout << "Latency: " << std::fixed << std::setprecision(3) 
              << (place_duration * 1000.0 / order_ids.size()) << " μs/order\n\n";
    
    // ========== CANCELLATION THROUGHPUT ==========
    std::cout << "Cancelling " << order_ids.size() / 2 << " orders...\n";
    auto cancel_start = std::chrono::high_resolution_clock::now();
    
    int cancelled_count = 0;
    for (size_t i = 0; i < order_ids.size() / 2; ++i)
    {
        if (engine.cancel_order(order_ids[i]))
            cancelled_count++;
    }
    
    auto cancel_end = std::chrono::high_resolution_clock::now();
    auto cancel_duration = std::chrono::duration_cast<std::chrono::milliseconds>(cancel_end - cancel_start).count();
    double cancel_throughput = (cancelled_count / (cancel_duration / 1000.0));
    
    std::cout << "Cancelled " << cancelled_count << " orders successfully.\n";
    std::cout << "Time: " << cancel_duration << " ms\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
              << cancel_throughput << " cancels/sec\n";
    std::cout << "Latency: " << std::fixed << std::setprecision(3) 
              << (cancel_duration * 1000.0 / cancelled_count) << " μs/cancel\n\n";
    
    // ========== EDIT THROUGHPUT ==========
    const int NUM_EDITS = 1000;
    std::cout << "Editing " << NUM_EDITS << " orders...\n";
    auto edit_start = std::chrono::high_resolution_clock::now();
    
    int edited_count = 0;
    for (size_t i = order_ids.size() / 2; i < order_ids.size() / 2 + NUM_EDITS && i < order_ids.size(); ++i)
    {
        auto new_id = engine.edit_order(order_ids[i], OrderSide::BID, 105.0, 20);
        if (new_id != 0)
            edited_count++;
    }
    
    auto edit_end = std::chrono::high_resolution_clock::now();
    auto edit_duration = std::chrono::duration_cast<std::chrono::milliseconds>(edit_end - edit_start).count();
    double edit_throughput = (edited_count / (edit_duration / 1000.0));
    
    std::cout << "Edited " << edited_count << " orders successfully.\n";
    std::cout << "Time: " << edit_duration << " ms\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
              << edit_throughput << " edits/sec\n";
    std::cout << "Latency: " << std::fixed << std::setprecision(3) 
              << (edit_duration * 1000.0 / edited_count) << " μs/edit\n\n";
    
    // ========== QUERY THROUGHPUT ==========
    std::cout << "Querying orders by status...\n";
    auto query_start = std::chrono::high_resolution_clock::now();
    
    auto open_orders = engine.get_orders_by_status(OrderStatus::OPEN);
    auto cancelled_orders = engine.get_orders_by_status(OrderStatus::CANCELLED);
    
    auto query_end = std::chrono::high_resolution_clock::now();
    auto query_duration = std::chrono::duration_cast<std::chrono::microseconds>(query_end - query_start).count();
    
    std::cout << "Open orders: " << open_orders.size() << "\n";
    std::cout << "Cancelled orders: " << cancelled_orders.size() << "\n";
    std::cout << "Query time: " << query_duration << " μs\n\n";
    
    // ========== SUMMARY ==========
    std::cout << "========== PERFORMANCE SUMMARY ==========\n";
    std::cout << "Place Orders:  " << std::fixed << std::setprecision(2) 
              << place_throughput << " ops/sec (" 
              << std::fixed << std::setprecision(3) 
              << (place_duration * 1000.0 / order_ids.size()) << " μs/op)\n";
    std::cout << "Cancel Orders: " << std::fixed << std::setprecision(2) 
              << cancel_throughput << " ops/sec (" 
              << std::fixed << std::setprecision(3) 
              << (cancel_duration * 1000.0 / cancelled_count) << " μs/op)\n";
    std::cout << "Edit Orders:   " << std::fixed << std::setprecision(2) 
              << edit_throughput << " ops/sec (" 
              << std::fixed << std::setprecision(3) 
              << (edit_duration * 1000.0 / edited_count) << " μs/op)\n";
    std::cout << "Query Orders:  " << query_duration << " μs total\n";
    std::cout << "=========================================\n";
    
    std::cout << "✓ Stress Test PASSED!\n\n";
}

void test_order_matching_correctness()
{
    std::cout << "=== Testing Order Matching Correctness ===\n";
    
    OrderEngine engine("MSFT", 10000, VERBOSE);
    
    // Test 1: Simple full match
    std::cout << "Test 1: Simple full match...\n";
    auto bid1 = engine.place_order(OrderSide::BID, OrderType::LIMIT, 100.0, 10);
    auto ask1 = engine.place_order(OrderSide::ASK, OrderType::LIMIT, 100.0, 10);
    
    // Both orders should be filled
    const OrderInfo* bid1_info = engine.get_order(bid1);
    const OrderInfo* ask1_info = engine.get_order(ask1);
    assert(bid1_info->status_ == OrderStatus::FILLED && "Bid should be filled");
    assert(ask1_info->status_ == OrderStatus::FILLED && "Ask should be filled");
    assert(bid1_info->qty_ == 0 && "Bid quantity should be 0");
    assert(ask1_info->qty_ == 0 && "Ask quantity should be 0");
    std::cout << "  ✓ Full match works correctly\n";
    
    // Test 2: Partial match - ask larger than bid
    std::cout << "Test 2: Partial match (ask > bid)...\n";
    OrderEngine engine_test2("TEST2", 10000, VERBOSE);
    auto bid2 = engine_test2.place_order(OrderSide::BID, OrderType::LIMIT, 101.0, 5);
    auto ask2 = engine_test2.place_order(OrderSide::ASK, OrderType::LIMIT, 101.0, 15);
    
    const OrderInfo* bid2_info = engine_test2.get_order(bid2);
    const OrderInfo* ask2_info = engine_test2.get_order(ask2);
    assert(bid2_info->status_ == OrderStatus::FILLED && "Bid should be filled");
    assert(ask2_info->status_ == OrderStatus::OPEN && "Ask should be partially filled");
    assert(bid2_info->qty_ == 0 && "Bid quantity should be 0");
    assert(ask2_info->qty_ == 10 && "Ask quantity should be 10 remaining");
    std::cout << "  ✓ Partial match (ask > bid) works correctly\n";
    
    // Test 3: Partial match - bid larger than ask
    std::cout << "Test 3: Partial match (bid > ask)...\n";
    OrderEngine engine_test3("TEST3", 10000, VERBOSE);
    auto bid3 = engine_test3.place_order(OrderSide::BID, OrderType::LIMIT, 102.0, 20);
    auto ask3 = engine_test3.place_order(OrderSide::ASK, OrderType::LIMIT, 102.0, 8);
    
    const OrderInfo* bid3_info = engine_test3.get_order(bid3);
    const OrderInfo* ask3_info = engine_test3.get_order(ask3);
    assert(ask3_info->status_ == OrderStatus::FILLED && "Ask should be filled");
    assert(bid3_info->status_ == OrderStatus::OPEN && "Bid should be partially filled");
    assert(ask3_info->qty_ == 0 && "Ask quantity should be 0");
    assert(bid3_info->qty_ == 12 && "Bid quantity should be 12 remaining");
    std::cout << "  ✓ Partial match (bid > ask) works correctly\n";
    
    // Test 4: Multiple matches - FIFO order
    std::cout << "Test 4: Multiple matches with FIFO...\n";
    OrderEngine engine2("FIFO", 10000, VERBOSE);
    
    // Place multiple bids at same price
    auto bid4a = engine2.place_order(OrderSide::BID, OrderType::LIMIT, 50.0, 10);
    auto bid4b = engine2.place_order(OrderSide::BID, OrderType::LIMIT, 50.0, 15);
    auto bid4c = engine2.place_order(OrderSide::BID, OrderType::LIMIT, 50.0, 5);
    
    // Place large ask that should match in FIFO order
    auto ask4 = engine2.place_order(OrderSide::ASK, OrderType::LIMIT, 50.0, 25);
    
    const OrderInfo* bid4a_info = engine2.get_order(bid4a);
    const OrderInfo* bid4b_info = engine2.get_order(bid4b);
    const OrderInfo* bid4c_info = engine2.get_order(bid4c);
    const OrderInfo* ask4_info = engine2.get_order(ask4);
    
    // First order should be fully filled
    assert(bid4a_info->status_ == OrderStatus::FILLED && "First bid should be filled");
    assert(bid4a_info->qty_ == 0 && "First bid qty should be 0");
    
    // Second order should be fully filled
    assert(bid4b_info->status_ == OrderStatus::FILLED && "Second bid should be filled");
    assert(bid4b_info->qty_ == 0 && "Second bid qty should be 0");
    
    // Third order should be untouched (only 25 qty available)
    assert(bid4c_info->status_ == OrderStatus::OPEN && "Third bid should remain open");
    assert(bid4c_info->qty_ == 5 && "Third bid qty should be unchanged");
    
    // Ask should be fully filled
    assert(ask4_info->status_ == OrderStatus::FILLED && "Ask should be filled");
    assert(ask4_info->qty_ == 0 && "Ask qty should be 0");
    std::cout << "  ✓ FIFO matching works correctly\n";
    
    // Test 5: Price-time priority
    std::cout << "Test 5: Price-time priority...\n";
    OrderEngine engine3("PRIORITY", 10000, VERBOSE);
    
    // Place bids at different prices
    auto bid5a = engine3.place_order(OrderSide::BID, OrderType::LIMIT, 75.0, 10); // Lower price
    auto bid5b = engine3.place_order(OrderSide::BID, OrderType::LIMIT, 77.0, 10); // Higher price (should match first)
    
    // Place ask that can match both
    auto ask5 = engine3.place_order(OrderSide::ASK, OrderType::LIMIT, 75.0, 10);
    
    const OrderInfo* bid5a_info = engine3.get_order(bid5a);
    const OrderInfo* bid5b_info = engine3.get_order(bid5b);
    const OrderInfo* ask5_info = engine3.get_order(ask5);
    
    // Higher priced bid should match first
    assert(bid5b_info->status_ == OrderStatus::FILLED && "Higher priced bid should match");
    assert(bid5a_info->status_ == OrderStatus::OPEN && "Lower priced bid should remain");
    assert(ask5_info->status_ == OrderStatus::FILLED && "Ask should be filled");
    std::cout << "  ✓ Price-time priority works correctly\n";
    
    // Test 6: Market depth after matching
    std::cout << "Test 6: Market depth correctness...\n";
    OrderEngine engine4("DEPTH", 10000, VERBOSE);
    
    // Build order book
    engine4.place_order(OrderSide::BID, OrderType::LIMIT, 90.0, 100);
    engine4.place_order(OrderSide::BID, OrderType::LIMIT, 91.0, 200);
    engine4.place_order(OrderSide::BID, OrderType::LIMIT, 92.0, 150);
    engine4.place_order(OrderSide::ASK, OrderType::LIMIT, 93.0, 100);
    engine4.place_order(OrderSide::ASK, OrderType::LIMIT, 94.0, 200);
    
    // Execute trade that removes top of book
    auto large_sell = engine4.place_order(OrderSide::ASK, OrderType::LIMIT, 92.0, 150);
    
    // Check best bid changed
    assert(engine4.get_best_bid() == 91.0 && "Best bid should be updated after match");
    
    // Get market depth
    auto bid_depth = engine4.get_market_depth(OrderSide::BID, 5);
    assert(bid_depth.size() == 2 && "Should have 2 bid levels remaining");
    assert(bid_depth[0].first == 91.0 && "Top bid should be 91.0");
    assert(bid_depth[0].second == 200 && "Top bid qty should be 200");
    std::cout << "  ✓ Market depth updates correctly after matching\n";
    
    // Test 7: No matching when prices don't cross
    std::cout << "Test 7: No match when prices don't cross...\n";
    OrderEngine engine5("NOCROSS", 10000, VERBOSE);
    
    auto bid6 = engine5.place_order(OrderSide::BID, OrderType::LIMIT, 80.0, 10);
    auto ask6 = engine5.place_order(OrderSide::ASK, OrderType::LIMIT, 85.0, 10);
    
    const OrderInfo* bid6_info = engine5.get_order(bid6);
    const OrderInfo* ask6_info = engine5.get_order(ask6);
    
    // Both should remain open (no match)
    assert(bid6_info->status_ == OrderStatus::OPEN && "Bid should remain open");
    assert(ask6_info->status_ == OrderStatus::OPEN && "Ask should remain open");
    assert(bid6_info->qty_ == 10 && "Bid qty unchanged");
    assert(ask6_info->qty_ == 10 && "Ask qty unchanged");
    assert(engine5.get_best_bid() == 80.0 && "Best bid should be 80.0");
    assert(engine5.get_best_ask() == 85.0 && "Best ask should be 85.0");
    std::cout << "  ✓ No matching when prices don't cross\n";
    
    std::cout << "✓ Order Matching Correctness Test PASSED!\n\n";
}

int main()
{
    std::cout << "========================================\n";
    std::cout << "  Order Engine Tests\n";
    std::cout << "========================================\n\n";
    
    // Set verbose mode from environment or default to false
    // You can set VERBOSE = true; here to enable verbose output
    
    test_place_limit_order();
    test_place_market_order();
    test_cancel_order();
    //test_edit_order();
    test_multiple_orders_same_price();
    test_order_priority();
    test_order_matching_correctness();
    test_stress_orders();
    
    std::cout << "========================================\n";
    std::cout << "  All Order Tests PASSED! ✓\n";
    std::cout << "========================================\n";
    
    return 0;
}

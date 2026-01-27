#include "../engine_runtime.cpp"
#include <iostream>
#include <cassert>
#include <vector>

using namespace runtime;
using namespace engine;

// Test strategy: Simple market maker
void market_maker_strategy(runtime::User* user) {
    auto tickers = user->get_runtime()->list_tickers();
    
    for (const auto& ticker : tickers) {
        double bid = user->get_best_bid(ticker);
        double ask = user->get_best_ask(ticker);
        
        if (bid > 0 && ask > 0) {
            double mid = (bid + ask) / 2.0;
            double spread = 0.01; // 1 cent spread
            
            // Place buy order below mid
            user->submit_limit_order(ticker, OrderSide::BID, mid - spread, 0.1);
            
            // If we have positions, place sell order above mid
            auto positions = user->get_open_positions(ticker);
            if (!positions.empty()) {
                user->submit_limit_order(ticker, OrderSide::ASK, mid + spread, 0.05);
            }
        }
    }
}

// Test strategy: Aggressive buyer
void aggressive_buyer_strategy(runtime::User* user) {
    auto tickers = user->get_runtime()->list_tickers();
    
    for (const auto& ticker : tickers) {
        double ask = user->get_best_ask(ticker);
        
        if (ask > 0) {
            // Buy at current ask price
            user->submit_limit_order(ticker, OrderSide::BID, ask, 0.5);
        }
    }
}

void test_basic_strategy_registration() {
    std::cout << "\n=== Test 1: Basic Strategy Registration ===" << std::endl;
    
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(2, 1048576, true, 2);
    
    // Register stock
    bool registered = runtime.register_stock("BTC", 100000.0, 10.0);
    assert(registered && "Stock registration failed");
    
    // Register strategy
    runtime::User* user1 = runtime.register_strategy(market_maker_strategy, 50000.0);
    assert(user1 != nullptr && "Strategy registration failed");
    assert(user1->get_user_id() == 1 && "First user should have ID 1 (0 is IPO_HOLDER)");
    
    runtime::User* user2 = runtime.register_strategy(aggressive_buyer_strategy, 75000.0);
    assert(user2 != nullptr && "Second strategy registration failed");
    assert(user2->get_user_id() == 2 && "Second user should have ID 2");
    
    std::cout << "✓ User 1 ID: " << user1->get_user_id() << " (Capital: $" << user1->get_capital() << ")" << std::endl;
    std::cout << "✓ User 2 ID: " << user2->get_user_id() << " (Capital: $" << user2->get_capital() << ")" << std::endl;
    std::cout << "✓ Strategy registration test passed!" << std::endl;
}

void test_ipo_holder_positions() {
    std::cout << "\n=== Test 2: IPO Holder Positions ===" << std::endl;
    
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(2, 1048576, true);
    
    // Register stock - IPO_HOLDER (user_id=0) should own initial shares
    runtime.register_stock("ETH", 5000.0, 100.0);
    runtime.process_pending_orders();
    
    // Check IPO holder positions
    auto ipo_positions = runtime.get_positions(IPO_HOLDER, "ETH");
    assert(!ipo_positions.empty() && "IPO_HOLDER should have positions");
    std::cout << "✓ IPO_HOLDER has " << ipo_positions.size() << " order(s)" << std::endl;
    
    // Verify IPO order is an ASK (sell order)
    const OrderInfo* ipo_order = runtime.get_order("ETH", ipo_positions[0]);
    assert(ipo_order != nullptr && "IPO order should exist");
    assert(ipo_order->side_ == OrderSide::ASK && "IPO order should be a sell order");
    std::cout << "✓ IPO order is ASK side with qty: " << math::internal_to_qty(ipo_order->qty_) << std::endl;
    std::cout << "✓ IPO holder test passed!" << std::endl;
}

void test_user_wrapper_methods() {
    std::cout << "\n=== Test 3: User Wrapper Methods ===" << std::endl;
    
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(2, 1048576, false);
    
    runtime.register_stock("SOL", 200.0, 50.0);
    runtime.process_pending_orders();
    
    runtime::User* trader = runtime.register_strategy([](runtime::User* u) {}, 100000.0);
    
    // Test price queries
    double bid = trader->get_best_bid("SOL");
    double ask = trader->get_best_ask("SOL");
    std::cout << "✓ Best bid: $" << bid << ", Best ask: $" << ask << std::endl;
    assert(ask == 200.0 && "IPO ask price should be $200");
    
    // Test order submission (should succeed - buying from IPO)
    bool buy_submitted = trader->submit_limit_order("SOL", OrderSide::BID, 200.0, 1.0);
    assert(buy_submitted && "Buy order submission failed");
    runtime.process_pending_orders();
    
    // Check if order filled
    auto positions = trader->get_open_positions("SOL");
    std::cout << "✓ User positions after buy: " << positions.size() << std::endl;
    
    // Test market depth
    auto depth = trader->get_market_depth("SOL", OrderSide::ASK, 5);
    std::cout << "✓ Market depth (ASK): " << depth.size() << " levels" << std::endl;
    
    std::cout << "✓ Wrapper methods test passed!" << std::endl;
}

void test_position_tracking() {
    std::cout << "\n=== Test 4: Position Tracking with Vector Structure ===" << std::endl;
    
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(2, 1048576, false);
    
    // Register multiple stocks
    runtime.register_stock("AAPL", 180.0, 100.0);
    runtime.register_stock("MSFT", 400.0, 50.0);
    runtime.process_pending_orders();
    
    runtime::User* trader = runtime.register_strategy([](runtime::User* u) {}, 100000.0);
    
    // Buy from both stocks
    trader->submit_limit_order("AAPL", OrderSide::BID, 180.0, 5.0);
    trader->submit_limit_order("MSFT", OrderSide::BID, 400.0, 2.0);
    runtime.process_pending_orders();
    
    // Check positions in both stocks
    auto aapl_positions = trader->get_open_positions("AAPL");
    auto msft_positions = trader->get_open_positions("MSFT");
    
    std::cout << "✓ AAPL positions: " << aapl_positions.size() << std::endl;
    std::cout << "✓ MSFT positions: " << msft_positions.size() << std::endl;
    
    // Test selling (should work since we own shares now)
    if (!aapl_positions.empty()) {
        bool sell_submitted = trader->submit_limit_order("AAPL", OrderSide::ASK, 185.0, 1.0);
        assert(sell_submitted && "Sell order should be accepted");
        std::cout << "✓ Sell order submitted successfully" << std::endl;
    }
    
    std::cout << "✓ Position tracking test passed!" << std::endl;
}

void test_quantum_execution() {
    std::cout << "\n=== Test 5: Quantum Execution System ===" << std::endl;
    
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(2, 1048576, true);
    
    runtime.register_stock("BTC", 100000.0, 10.0);
    runtime.process_pending_orders();
    
    runtime::User* trader = runtime.register_strategy([](runtime::User* u) {
        std::cout << "  Strategy called for user " << u->get_user_id() << std::endl;

        // Simple strategy: buy if we can
        double ask = u->get_best_ask("BTC");
        if (ask > 0) {
            std::cout << "    Best ask: $" << ask << std::endl;
            // u->submit_limit_order("BTC", OrderSide::BID, ask, 0.1);
        }
    }, 500000.0);
    
    // Quantum execution is configured at runtime construction
    
    std::cout << "Submitting orders to trigger quantum..." << std::endl;
    
    // Submit orders - every 2 orders should trigger strategy
    trader->submit_limit_order("BTC", OrderSide::BID, 99000.0, 0.05);
    trader->submit_limit_order("BTC", OrderSide::BID, 99000.0, 0.05);
    // Strategy should be called here (after 2 orders)
    
    trader->submit_limit_order("BTC", OrderSide::BID, 99000.0, 0.05);
    trader->submit_limit_order("BTC", OrderSide::BID, 99000.0, 0.05);
    // Strategy should be called again (after 4 total orders)
    
    runtime.process_pending_orders();

    std::cout << "✓ Quantum execution test passed!" << std::endl;
}

void test_multi_user_interaction() {
    std::cout << "\n=== Test 6: Multi-User Interaction ===" << std::endl;
    
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(4, 1048576, true);
    
    runtime.register_stock("TSLA", 250.0, 100.0);
    runtime.process_pending_orders();
    
    // Register multiple users
    runtime::User* buyer = runtime.register_strategy([](runtime::User* u) {}, 100000.0);
    runtime::User* seller = runtime.register_strategy([](runtime::User* u) {}, 50000.0);
    
    std::cout << "✓ Buyer ID: " << buyer->get_user_id() << std::endl;
    std::cout << "✓ Seller ID: " << seller->get_user_id() << std::endl;
    
    // Buyer buys from IPO
    buyer->submit_limit_order("TSLA", OrderSide::BID, 250.0, 10.0);
    runtime.process_pending_orders();
    
    auto buyer_positions = buyer->get_open_positions("TSLA");
    std::cout << "✓ Buyer positions: " << buyer_positions.size() << std::endl;
    
    // Buyer places sell order
    if (!buyer_positions.empty()) {
        buyer->submit_limit_order("TSLA", OrderSide::ASK, 260.0, 5.0);
        runtime.process_pending_orders();
        
        // Seller buys from buyer
        seller->submit_limit_order("TSLA", OrderSide::BID, 260.0, 3.0);
        runtime.process_pending_orders();
        
        auto seller_positions = seller->get_open_positions("TSLA");
        std::cout << "✓ Seller acquired positions: " << seller_positions.size() << std::endl;
    }
    
    std::cout << "✓ Multi-user interaction test passed!" << std::endl;
}

void test_error_handling() {
    std::cout << "\n=== Test 7: Error Handling ===" << std::endl;
    
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(2, 1048576, false);
    
    runtime.register_stock("NVDA", 500.0, 10.0);
    runtime.process_pending_orders();
    
    runtime::User* trader = runtime.register_strategy([](runtime::User* u) {}, 10000.0);
    
    // Try to sell without owning shares (should fail)
    bool result = trader->submit_limit_order("NVDA", OrderSide::ASK, 510.0, 5.0);
    runtime.process_pending_orders();
    
    std::cout << "✓ Sell without shares result: " << (result ? "submitted" : "rejected") << std::endl;
    
    // Try invalid ticker
    result = trader->submit_limit_order("INVALID", OrderSide::BID, 100.0, 1.0);
    std::cout << "✓ Invalid ticker result: " << (result ? "submitted" : "rejected") << std::endl;
    assert(!result && "Invalid ticker should be rejected");
    
    // Try to access non-existent position
    auto positions = trader->get_open_positions("NVDA");
    std::cout << "✓ Empty positions size: " << positions.size() << std::endl;
    
    std::cout << "✓ Error handling test passed!" << std::endl;
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "   Trading Strategy Test Suite" << std::endl;
    std::cout << "======================================" << std::endl;
    
    try {
        test_basic_strategy_registration();
        test_ipo_holder_positions();
        test_user_wrapper_methods();
        test_position_tracking();
        test_quantum_execution();
        test_multi_user_interaction();
        test_error_handling();
        
        std::cout << "\n======================================" << std::endl;
        std::cout << "   ✓ ALL TESTS PASSED!" << std::endl;
        std::cout << "======================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}

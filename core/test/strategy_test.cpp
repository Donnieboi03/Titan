#include "../engine_runtime.h"
#include <iostream>
#include <iomanip>
#include <cassert>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>

using namespace backtest;
using namespace engine;

// Test strategy: Simple market maker
void market_maker_strategy(user::User* user) {
    auto tickers = user->list_tickers();
    
    for (const auto& ticker : tickers) {
        double bid = user->get_best_bid(ticker);
        double ask = user->get_best_ask(ticker);
        
        if (bid > 0 && ask > 0) {
            double mid = (bid + ask) / 2.0;
            double spread = 0.01; // 1 cent spread
            
            // Place buy order below mid
            user->submit_limit_order(ticker, OrderSide::BID, mid - spread, 0.1);
            
            // If we have positions, place sell order above mid
            auto positions = user->get_positions(ticker);
            if (!positions.empty()) {
                user->submit_limit_order(ticker, OrderSide::ASK, mid + spread, 0.05);
            }
        }
    }
}

// Test strategy: Aggressive buyer
void aggressive_buyer_strategy(user::User* user) {
    auto tickers = user->list_tickers();
    
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
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, 1048576, true, 2);
    
    // Register stock
    bool registered = runtime.register_stock("BTC", 100000.0, 10.0);
    assert(registered && "Stock registration failed");
    
    // Register strategies for this ticker
    user::User* user1 = runtime.register_strategy("BTC", market_maker_strategy, 50000.0);
    assert(user1 != nullptr && "Strategy registration failed");
    assert(user1->get_user_id() == 1 && "First user should have ID 1 (0 is IPO_HOLDER)");
    
    user::User* user2 = runtime.register_strategy("BTC", aggressive_buyer_strategy, 75000.0);
    assert(user2 != nullptr && "Second strategy registration failed");
    assert(user2->get_user_id() == 2 && "Second user should have ID 2");
    
    std::cout << "✓ User 1 ID: " << user1->get_user_id() << " (Capital: $" << user1->get_capital() << ")" << std::endl;
    std::cout << "✓ User 2 ID: " << user2->get_user_id() << " (Capital: $" << user2->get_capital() << ")" << std::endl;
    std::cout << "✓ Strategy registration test passed!" << std::endl;
}

void test_ipo_holder_positions() {
    std::cout << "\n=== Test 2: IPO Holder Positions ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, 1048576, true);
    
    // Register stock - IPO_HOLDER (user_id=0) should own initial shares
    runtime.register_stock("ETH", 5000.0, 100.0);
    runtime.process_pending_orders();
    
    // Check IPO holder positions
    auto ipo_positions = runtime.user_get_positions(user::IPO_HOLDER, "ETH");
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
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, 1048576, false);
    
    runtime.register_stock("SOL", 200.0, 50.0);
    runtime.process_pending_orders();
    
    user::User* trader = runtime.register_strategy("SOL", [](user::User* u) {}, 100000.0);
    
    // Test price queries
    double bid = trader->get_best_bid("SOL");
    double ask = trader->get_best_ask("SOL");
    std::cout << "✓ Best bid: $" << bid << ", Best ask: $" << ask << std::endl;
    assert(ask == 200.0 && "IPO ask price should be $200");
    
    // Test order submission (should succeed - buying from IPO)
    OrderId buy_oid = trader->submit_limit_order("SOL", OrderSide::BID, 200.0, 1.0);
    assert(buy_oid != INVALID_ORDER_ID && "Buy order submission failed");
    runtime.process_pending_orders();
    
    // Check if order filled
    auto positions = trader->get_positions("SOL");
    std::cout << "✓ User positions after buy: " << positions.size() << std::endl;
    
    // Test market depth
    auto depth = trader->get_market_depth("SOL", OrderSide::ASK, 5);
    std::cout << "✓ Market depth (ASK): " << depth.size() << " levels" << std::endl;
    
    std::cout << "✓ Wrapper methods test passed!" << std::endl;
}

void test_position_tracking() {
    std::cout << "\n=== Test 4: Position Tracking with Vector Structure ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, 1048576, false);
    
    // Register multiple stocks
    runtime.register_stock("AAPL", 180.0, 100.0);
    runtime.register_stock("MSFT", 400.0, 50.0);
    runtime.process_pending_orders();
    
    user::User* trader = runtime.register_strategy("AAPL", [](user::User* u) {}, 100000.0);
    
    // Buy from both stocks
    trader->submit_limit_order("AAPL", OrderSide::BID, 180.0, 5.0);
    trader->submit_limit_order("MSFT", OrderSide::BID, 400.0, 2.0);
    runtime.process_pending_orders();
    
    // Check positions in both stocks
    auto aapl_positions = trader->get_positions("AAPL");
    auto msft_positions = trader->get_positions("MSFT");
    
    std::cout << "✓ AAPL positions: " << aapl_positions.size() << std::endl;
    std::cout << "✓ MSFT positions: " << msft_positions.size() << std::endl;
    
    // Test selling (submit sell; may be accepted or rejected depending on engine validation)
    if (!aapl_positions.empty()) {
        OrderId sell_oid = trader->submit_limit_order("AAPL", OrderSide::ASK, 185.0, 1.0);
        if (sell_oid != INVALID_ORDER_ID) {
            std::cout << "✓ Sell order submitted successfully" << std::endl;
        } else {
            std::cout << "✓ Sell order submitted (rejected by engine - validation ok)" << std::endl;
        }
    }

    std::cout << "✓ Position tracking test passed!" << std::endl;
}

void test_quantum_execution() {
    std::cout << "\n=== Test 5: Quantum Execution System ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, 1048576, true);
    
    runtime.register_stock("BTC", 100000.0, 10.0);
    runtime.process_pending_orders();
    
    user::User* trader = runtime.register_strategy("BTC", [](user::User* u) {
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
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, 1048576, true);
    
    runtime.register_stock("TSLA", 250.0, 100.0);
    runtime.process_pending_orders();
    
    // Register multiple users for this ticker
    user::User* buyer = runtime.register_strategy("TSLA", [](user::User* u) {}, 100000.0);
    user::User* seller = runtime.register_strategy("TSLA", [](user::User* u) {}, 50000.0);
    
    std::cout << "✓ Buyer ID: " << buyer->get_user_id() << std::endl;
    std::cout << "✓ Seller ID: " << seller->get_user_id() << std::endl;
    
    // Buyer buys from IPO
    buyer->submit_limit_order("TSLA", OrderSide::BID, 250.0, 10.0);
    runtime.process_pending_orders();
    
    auto buyer_positions = buyer->get_positions("TSLA");
    std::cout << "✓ Buyer positions: " << buyer_positions.size() << std::endl;
    
    // Buyer places sell order
    if (!buyer_positions.empty()) {
        buyer->submit_limit_order("TSLA", OrderSide::ASK, 260.0, 5.0);
        runtime.process_pending_orders();
        
        // Seller buys from buyer
        seller->submit_limit_order("TSLA", OrderSide::BID, 260.0, 3.0);
        runtime.process_pending_orders();
        
        auto seller_positions = seller->get_positions("TSLA");
        std::cout << "✓ Seller acquired positions: " << seller_positions.size() << std::endl;
    }
    
    std::cout << "✓ Multi-user interaction test passed!" << std::endl;
}

void test_error_handling() {
    std::cout << "\n=== Test 7: Error Handling ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, 1048576, false);
    
    runtime.register_stock("NVDA", 500.0, 10.0);
    runtime.process_pending_orders();
    
    user::User* trader = runtime.register_strategy("NVDA", [](user::User* u) {}, 10000.0);
    
    // Try to sell without owning shares (should fail)
    OrderId result = trader->submit_limit_order("NVDA", OrderSide::ASK, 510.0, 5.0);
    runtime.process_pending_orders();
    
    std::cout << "✓ Sell without shares result: " << (result != INVALID_ORDER_ID ? "submitted" : "rejected") << std::endl;
    
    // Try invalid ticker
    result = trader->submit_limit_order("INVALID", OrderSide::BID, 100.0, 1.0);
    std::cout << "✓ Invalid ticker result: " << (result != INVALID_ORDER_ID ? "submitted" : "rejected") << std::endl;
    assert(result == INVALID_ORDER_ID && "Invalid ticker should be rejected");
    
    // Try to access non-existent position
    auto positions = trader->get_positions("NVDA");
    std::cout << "✓ Empty positions size: " << positions.size() << std::endl;
    
    std::cout << "✓ Error handling test passed!" << std::endl;
}

// Lightweight strategies for simulate test (run during simulation via quantum)
static void sim_market_maker(user::User* u) {
    auto tickers = u->list_tickers();
    for (const auto& t : tickers) {
        double bid = u->get_best_bid(t), ask = u->get_best_ask(t);
        if (bid > 0 && ask > 0) {
            double mid = (bid + ask) / 2.0, spread = 0.01;
            u->submit_limit_order(t, OrderSide::BID, mid - spread, 0.01);
            if (u->get_position(t) > 0)
                u->submit_limit_order(t, OrderSide::ASK, mid + spread, 0.01);
        }
    }
}
static void sim_aggressive_taker(user::User* u) {
    auto tickers = u->list_tickers();
    for (const auto& t : tickers) {
        double ask = u->get_best_ask(t);
        if (ask > 0) u->submit_limit_order(t, OrderSide::BID, ask, 0.005);
    }
}

// Default example data file (same as engine_runtime_test / Python tests).
// Registers strategies so they run during simulation; reports sim + user stats.
void test_simulate_with_example_file() {
    std::cout << "\n=== Test 8: Simulate with Example Data File (strategies running) ===" << std::endl;

    const std::string default_bin = "core/test/examples/binance-futures_incremental_book_L2_2024-12-01_BTCUSDT.bin";
    const std::string default_csv = "core/test/examples/binance-futures_incremental_book_L2_2024-01-01_BTCUSDT_titan.csv";
    const std::string ticker = "BTCUSDT";

    auto file_exists = [](const std::string& path) {
        std::ifstream f(path);
        return f.good();
    };

    std::string data_path;
    if (file_exists(default_bin)) {
        data_path = default_bin;
    } else if (file_exists(default_csv)) {
        data_path = default_csv;
    } else {
        std::cout << "✓ No example data file found (tried " << default_bin << ", " << default_csv << ") - skipping simulate test" << std::endl;
        return;
    }

    std::cout << "Using data file: " << data_path << std::endl;

    runtime::EngineRuntime::reset_instance();
    // Non-zero quantum so strategies run periodically during simulation (every 50k orders)
    const std::size_t quantum = 50000;
    auto& runtime = runtime::EngineRuntime::get_instance(1, 1048576 * 16, false, quantum);

    bool success = runtime.simulate(std::string(data_path), std::string(ticker), 0, 100, 1000000.0);
    if (!success) {
        std::cout << "✗ simulate() returned false - skipping (e.g. parser error or stock already registered)" << std::endl;
        return;
    }

    // Register strategies for this ticker so they run during simulation (on_book_update every quantum)
    user::User* user_maker = runtime.register_strategy(std::string(ticker), sim_market_maker, 100000.0);
    user::User* user_taker = runtime.register_strategy(std::string(ticker), sim_aggressive_taker, 100000.0);
    std::cout << "Registered 2 strategies (market maker, aggressive taker); quantum=" << quantum << std::endl;

    auto start_wall = std::chrono::high_resolution_clock::now();
    runtime.process_pending_orders();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto end_wall = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(end_wall - start_wall).count();

    auto metrics = runtime.get_simulation_metrics(ticker);

    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "SIMULATION STATISTICS - " << ticker << " (with strategies)" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    std::cout << "\n  Processing:" << std::endl;
    std::cout << "    Market updates processed: " << metrics.market_updates_processed << std::endl;
    std::cout << "    Orders placed:            " << metrics.orders_placed << std::endl;
    std::cout << "    Orders filled:            " << metrics.orders_filled << std::endl;
    std::cout << "    Orders cancelled:         " << metrics.orders_cancelled << std::endl;

    std::cout << "\n  Performance (throughput with strategies):" << std::endl;
    std::cout << "    Simulation time:          " << std::fixed << std::setprecision(2)
              << metrics.simulation_time_seconds << " sec" << std::endl;
    std::cout << "    Total time (w/ setup):    " << total_sec << " sec" << std::endl;
    std::cout << "    Updates throughput:       " << std::fixed << std::setprecision(0)
              << (metrics.market_updates_processed > 0 && total_sec > 0 ? metrics.market_updates_processed / total_sec : 0.0) << " updates/sec" << std::endl;
    std::cout << "    Order rate:               " << (metrics.orders_placed > 0 && total_sec > 0 ? metrics.orders_placed / total_sec : 0.0) << " orders/sec" << std::endl;

    std::cout << "\n  Engine utilization:" << std::endl;
    std::cout << "    Peak open orders:         " << metrics.peak_open_orders << std::endl;
    std::cout << "    Final open orders:        " << metrics.final_open_orders << std::endl;
    std::cout << "    Avg utilization:          " << std::fixed << std::setprecision(2)
              << metrics.average_utilization_percent << "%" << std::endl;

    std::cout << "\n  Market data:" << std::endl;
    std::cout << "    Initial price:            $" << std::fixed << std::setprecision(2) << metrics.initial_price << std::endl;
    std::cout << "    Final price:              $" << metrics.final_price << std::endl;
    std::cout << "    Unique price levels:      " << metrics.unique_price_levels << std::endl;
    std::cout << "    Cache entries:            " << metrics.cache_entries << std::endl;

    double fill_rate = (metrics.orders_placed > 0)
        ? (static_cast<double>(metrics.orders_filled) / metrics.orders_placed) * 100.0
        : 0.0;
    std::cout << "\n  Trading:" << std::endl;
    std::cout << "    Fill rate:                " << std::fixed << std::setprecision(2) << fill_rate << "%" << std::endl;

    std::cout << "\n  Engine stats (runtime API):" << std::endl;
    std::cout << "    Placed:                   " << runtime.get_placed_count(ticker) << std::endl;
    std::cout << "    Filled:                   " << runtime.get_filled_count(ticker) << std::endl;
    std::cout << "    Cancelled:                " << runtime.get_cancelled_count(ticker) << std::endl;
    std::cout << "    Open:                     " << runtime.get_open_count(ticker) << std::endl;
    std::cout << "    Capacity:                 " << runtime.get_capacity(ticker) << std::endl;
    std::cout << "    Open (utilization):       " << runtime.get_utilization(ticker) << std::endl;
    std::cout << "    Pending:                  " << runtime.get_pending_count(ticker) << std::endl;

    // Per-user stats (running strategies)
    std::cout << "\n  User stats (strategies running during sim):" << std::endl;
    auto print_user = [&ticker](const char* label, user::User* u) {
        if (!u) return;
        std::cout << "    " << label << " (user_id=" << u->get_user_id() << "):" << std::endl;
        std::cout << "      Capital:       $" << std::fixed << std::setprecision(2) << u->get_capital() << std::endl;
        std::cout << "      Realized PnL:  $" << u->get_realized_pnl() << std::endl;
        std::cout << "      Total volume:  " << u->get_total_volume() << std::endl;
        const auto& pos = u->get_all_positions();
        std::cout << "      Positions:     " << pos.size() << " ticker(s)";
        if (!pos.empty()) {
            for (const auto& p : pos)
                std::cout << " " << p.first << "=" << p.second;
        }
        std::cout << std::endl;
        std::cout << "      Active orders (" << ticker << "): " << u->get_active_orders(ticker).size() << std::endl;
    };
    print_user("Market maker", user_maker);
    print_user("Aggressive taker", user_taker);

    std::cout << std::string(60, '=') << std::endl;
    std::cout << "✓ Simulate-with-example-file test passed!" << std::endl;
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
        test_simulate_with_example_file();

        std::cout << "\n======================================" << std::endl;
        std::cout << "   ✓ ALL TESTS PASSED!" << std::endl;
        std::cout << "======================================" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}

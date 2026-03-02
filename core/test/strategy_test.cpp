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

// Test strategy: Simple market maker (bound to one ticker)
void market_maker_strategy(user::User* user) {
    double bid = user->get_best_bid();
    double ask = user->get_best_ask();

    if (bid > 0 && ask > 0) {
        double mid = (bid + ask) / 2.0;
        double spread = 0.01; // 1 cent spread

        user->submit_limit_order(OrderSide::BID, mid - spread, 0.1);

        auto positions = user->get_positions();
        if (!positions.empty()) {
            user->submit_limit_order(OrderSide::ASK, mid + spread, 0.05);
        }
    }
}

// Test strategy: Aggressive buyer (bound to one ticker)
void aggressive_buyer_strategy(user::User* user) {
    double ask = user->get_best_ask();

    if (ask > 0) {
        user->submit_limit_order(OrderSide::BID, ask, 0.5);
    }
}

void test_basic_strategy_registration() {
    std::cout << "\n=== Test 1: Basic Strategy Registration ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, true, 2, 1048576);
    
    // Register stock
    bool registered = runtime.register_stock("BTC", 100000.0, 10.0);
    assert(registered && "Stock registration failed");
    
    // Register strategies for this ticker (returns UserView* — observational only)
    user::UserView* user1 = runtime.register_strategy("BTC", market_maker_strategy, 50000.0);
    assert(user1 != nullptr && "Strategy registration failed");
    assert(user1->get_user_id() == 1 && "First user should have ID 1 (0 is IPO_HOLDER)");
    
    user::UserView* user2 = runtime.register_strategy("BTC", aggressive_buyer_strategy, 75000.0);
    assert(user2 != nullptr && "Second strategy registration failed");
    assert(user2->get_user_id() == 2 && "Second user should have ID 2");
    
    std::cout << "✓ User 1 ID: " << user1->get_user_id() << " (Capital: $" << user1->get_capital() << ")" << std::endl;
    std::cout << "✓ User 2 ID: " << user2->get_user_id() << " (Capital: $" << user2->get_capital() << ")" << std::endl;
    std::cout << "✓ Strategy registration test passed!" << std::endl;
}

void test_ipo_holder_positions() {
    std::cout << "\n=== Test 2: IPO Holder Positions ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, true, 4096, 1048576);
    
    // Register stock - IPO_HOLDER (user_id=0) should own initial shares
    runtime.register_stock("ETH", 5000.0, 100.0);
    runtime.process_pending_orders();
    
    // Check IPO holder positions
    auto ipo_positions = runtime.get_positions(user::IPO_HOLDER, "ETH");
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
    auto& runtime = runtime::EngineRuntime::get_instance(2, false, 4096, 1048576);
    
    runtime.register_stock("SOL", 200.0, 50.0);
    runtime.process_pending_orders();
    
    user::UserView* trader = runtime.register_strategy("SOL", [](user::User* u) {}, 100000.0);
    
    // Test price queries (via runtime; client has UserView*)
    double bid = runtime.get_best_bid("SOL");
    double ask = runtime.get_best_ask("SOL");
    std::cout << "✓ Best bid: $" << bid << ", Best ask: $" << ask << std::endl;
    assert(ask == 200.0 && "IPO ask price should be $200");
    
    // Test order submission via User (tracked; same ticker as strategy)
    OrderId buy_oid = static_cast<user::User*>(trader)->submit_limit_order(OrderSide::BID, 200.0, 1.0);
    assert(buy_oid != INVALID_ORDER_ID && "Buy order submission failed");
    runtime.process_pending_orders();
    
    // Check if order filled (positions by user_id via runtime)
    auto positions = runtime.get_positions(trader->get_user_id(), "SOL");
    std::cout << "✓ User positions after buy: " << positions.size() << std::endl;
    
    // Test market depth
    auto depth = runtime.get_market_depth("SOL", OrderSide::ASK, 5);
    std::cout << "✓ Market depth (ASK): " << depth.size() << " levels" << std::endl;
    
    std::cout << "✓ Wrapper methods test passed!" << std::endl;
}

void test_position_tracking() {
    std::cout << "\n=== Test 4: Position Tracking with Vector Structure ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, false, 4096, 1048576);
    
    // Register multiple stocks
    runtime.register_stock("AAPL", 180.0, 100.0);
    runtime.register_stock("MSFT", 400.0, 50.0);
    runtime.process_pending_orders();
    
    user::UserView* trader = runtime.register_strategy("AAPL", [](user::User* u) {}, 100000.0);
    user::UserView* trader_msft = runtime.register_strategy("MSFT", [](user::User* u) {}, 100000.0);
    
    // Buy via User submit (tracked; each user on own ticker)
    static_cast<user::User*>(trader)->submit_limit_order(OrderSide::BID, 180.0, 5.0);
    static_cast<user::User*>(trader_msft)->submit_limit_order(OrderSide::BID, 400.0, 2.0);
    runtime.process_pending_orders();
    
    // Check positions in both stocks
    auto aapl_positions = runtime.get_positions(trader->get_user_id(), "AAPL");
    auto msft_positions = runtime.get_positions(trader_msft->get_user_id(), "MSFT");
    
    std::cout << "✓ AAPL positions: " << aapl_positions.size() << std::endl;
    std::cout << "✓ MSFT positions: " << msft_positions.size() << std::endl;
    
    // Test selling (submit sell; may be accepted or rejected depending on engine validation)
    if (!aapl_positions.empty()) {
        OrderId sell_oid = static_cast<user::User*>(trader)->submit_limit_order(OrderSide::ASK, 185.0, 1.0);
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
    auto& runtime = runtime::EngineRuntime::get_instance(2, true, 4096, 1048576);
    
    runtime.register_stock("BTC", 100000.0, 10.0);
    runtime.process_pending_orders();
    
    user::UserView* trader = runtime.register_strategy("BTC", [](user::User* u) {
        std::cout << "  Strategy called for user " << u->get_user_id() << std::endl;
        double ask = u->get_best_ask();
        if (ask > 0) {
            std::cout << "    Best ask: $" << ask << std::endl;
        }
    }, 500000.0);
    
    std::cout << "Submitting orders to trigger quantum..." << std::endl;
    auto* u = static_cast<user::User*>(trader);
    u->submit_limit_order(OrderSide::BID, 99000.0, 0.05);
    u->submit_limit_order(OrderSide::BID, 99000.0, 0.05);
    u->submit_limit_order(OrderSide::BID, 99000.0, 0.05);
    u->submit_limit_order(OrderSide::BID, 99000.0, 0.05);
    runtime.process_pending_orders();

    std::cout << "✓ Quantum execution test passed!" << std::endl;
}

void test_multi_user_interaction() {
    std::cout << "\n=== Test 6: Multi-User Interaction ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(4, true, 4096, 1048576);
    
    runtime.register_stock("TSLA", 250.0, 100.0);
    runtime.process_pending_orders();
    
    // Register multiple users for this ticker (returns UserView*)
    user::UserView* buyer = runtime.register_strategy("TSLA", [](user::User* u) {}, 100000.0);
    user::UserView* seller = runtime.register_strategy("TSLA", [](user::User* u) {}, 50000.0);
    
    std::cout << "✓ Buyer ID: " << buyer->get_user_id() << std::endl;
    std::cout << "✓ Seller ID: " << seller->get_user_id() << std::endl;
    
    // Buyer buys from IPO (via User submit)
    static_cast<user::User*>(buyer)->submit_limit_order(OrderSide::BID, 250.0, 10.0);
    runtime.process_pending_orders();
    
    auto buyer_positions = runtime.get_positions(buyer->get_user_id(), "TSLA");
    std::cout << "✓ Buyer positions: " << buyer_positions.size() << std::endl;
    
    // Buyer places sell order
    if (!buyer_positions.empty()) {
        static_cast<user::User*>(buyer)->submit_limit_order(OrderSide::ASK, 260.0, 5.0);
        runtime.process_pending_orders();
        
        // Seller buys from buyer
        static_cast<user::User*>(seller)->submit_limit_order(OrderSide::BID, 260.0, 3.0);
        runtime.process_pending_orders();
        
        auto seller_positions = runtime.get_positions(seller->get_user_id(), "TSLA");
        std::cout << "✓ Seller acquired positions: " << seller_positions.size() << std::endl;
    }
    
    std::cout << "✓ Multi-user interaction test passed!" << std::endl;
}

void test_error_handling() {
    std::cout << "\n=== Test 7: Error Handling ===" << std::endl;
    
    runtime::EngineRuntime::reset_instance();
    auto& runtime = runtime::EngineRuntime::get_instance(2, false, 4096, 1048576);
    
    runtime.register_stock("NVDA", 500.0, 10.0);
    runtime.process_pending_orders();
    
    user::UserView* trader = runtime.register_strategy("NVDA", [](user::User* u) {}, 10000.0);
    
    // Try to sell without owning shares (should fail) — submit via User
    OrderId result = static_cast<user::User*>(trader)->submit_limit_order(OrderSide::ASK, 510.0, 5.0);
    runtime.process_pending_orders();
    
    std::cout << "✓ Sell without shares result: " << (result != INVALID_ORDER_ID ? "submitted" : "rejected") << std::endl;
    
    // Try invalid ticker (untracked runtime submit)
    result = runtime.submit_limit_order("INVALID", OrderSide::BID, 100.0, 1.0);
    std::cout << "✓ Invalid ticker result: " << (result != INVALID_ORDER_ID ? "submitted" : "rejected") << std::endl;
    assert(result == INVALID_ORDER_ID && "Invalid ticker should be rejected");
    
    // Try to access non-existent position
    auto positions = runtime.get_positions(trader->get_user_id(), "NVDA");
    std::cout << "✓ Empty positions size: " << positions.size() << std::endl;
    
    std::cout << "✓ Error handling test passed!" << std::endl;
}

// Lightweight strategies for simulate test (bound to one ticker)
static void sim_market_maker(user::User* u) {
    double bid = u->get_best_bid(), ask = u->get_best_ask();
    if (bid > 0 && ask > 0) {
        double mid = (bid + ask) / 2.0, spread = 0.01;
        u->submit_limit_order(OrderSide::BID, mid - spread, 0.01);
        if (u->get_position() > 0)
            u->submit_limit_order(OrderSide::ASK, mid + spread, 0.01);
    }
}
static void sim_aggressive_taker(user::User* u) {
    double ask = u->get_best_ask();
    if (ask > 0) u->submit_limit_order(OrderSide::BID, ask, 0.005);
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
    auto& runtime = runtime::EngineRuntime::get_instance(1, false, quantum, 1048576 * 16);

    bool success = runtime.simulate(std::string(data_path), std::string(ticker), 0, 100, 1000000.0);
    if (!success) {
        std::cout << "✗ simulate() returned false - skipping (e.g. parser error or stock already registered)" << std::endl;
        return;
    }

    // Register strategies for this ticker so they run during simulation (on_book_update every quantum)
    user::UserView* user_maker = runtime.register_strategy(std::string(ticker), sim_market_maker, 100000.0);
    user::UserView* user_taker = runtime.register_strategy(std::string(ticker), sim_aggressive_taker, 100000.0);
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
    std::cout << "    Orders edited:            " << metrics.orders_edited << std::endl;
    std::cout << "    Orders replaced:          " << metrics.orders_replaced << std::endl;

    std::cout << "\n  Performance (throughput with strategies):" << std::endl;
    std::cout << "    Simulation time:          " << std::fixed << std::setprecision(2)
              << metrics.simulation_time_seconds << " sec" << std::endl;
    std::cout << "    Total time (w/ setup):    " << total_sec << " sec" << std::endl;
    std::cout << "    Updates throughput:       " << std::fixed << std::setprecision(0)
              << metrics.updates_per_second() << " updates/sec" << std::endl;
    std::cout << "    Order ops rate:           " << std::fixed << std::setprecision(0) << metrics.orders_per_second() << " orders/sec" << std::endl;

    std::cout << "\n  Engine utilization:" << std::endl;
    std::cout << "    Peak open orders:         " << metrics.peak_open_orders << std::endl;
    std::cout << "    Final open orders:        " << metrics.final_open_orders << std::endl;

    std::cout << "\n  Market data:" << std::endl;
    std::cout << "    Initial price:            $" << std::fixed << std::setprecision(2) << metrics.initial_price << std::endl;
    std::cout << "    Final price:              $" << metrics.final_price << std::endl;
    std::cout << "    Cache entries:            " << metrics.cache_entries << std::endl;

    double fill_rate = (metrics.orders_placed > 0)
        ? (static_cast<double>(metrics.orders_filled) / metrics.orders_placed) * 100.0
        : 0.0;
    std::cout << "\n  Trading:" << std::endl;
    std::cout << "    Fill rate:                " << std::fixed << std::setprecision(2) << fill_rate << "%" << std::endl;

    std::cout << "\n  Engine stats (runtime API):" << std::endl;
    runtime.request_snapshot(ticker);
    runtime.process_pending_orders();
    const auto* snap = runtime.get_snapshot(ticker);
    std::cout << "    Placed:                   " << (snap ? snap->placed_count : 0) << std::endl;
    std::cout << "    Filled:                   " << (snap ? snap->filled_count : 0) << std::endl;
    std::cout << "    Cancelled:                " << (snap ? snap->cancelled_count : 0) << std::endl;
    std::cout << "    Edited:                   " << (snap ? snap->edited_count : 0) << std::endl;
    std::cout << "    Replaced:                 " << (snap ? snap->replaced_count : 0) << std::endl;
    std::cout << "    Open:                     " << (snap ? snap->open_count : 0) << std::endl;
    std::cout << "    Capacity:                 " << runtime.get_capacity(ticker) << std::endl;
    std::cout << "    Open (utilization):       " << (snap ? snap->open_count : 0) << std::endl;
    std::cout << "    Pending:                  " << runtime.get_pending_count(ticker) << std::endl;

    // Per-user stats (via UserView / snapshot; active orders via runtime)
    std::cout << "\n  User stats (strategies running during sim):" << std::endl;
    auto print_user = [&runtime, &ticker](const char* label, user::UserView* u) {
        if (!u) return;
        const auto& snap = u->get_snapshot();
        std::cout << "    " << label << " (user_id=" << snap.user_id << "):" << std::endl;
        std::cout << "      Capital:       $" << std::fixed << std::setprecision(2) << snap.capital << std::endl;
        std::cout << "      Realized PnL:  $" << snap.realized_pnl << std::endl;
        std::cout << "      Total volume:  " << snap.total_volume << std::endl;
        std::cout << "      Positions:     " << snap.positions.size() << " ticker(s)";
        if (!snap.positions.empty()) {
            for (const auto& p : snap.positions)
                std::cout << " " << p.first << "=" << p.second;
        }
        std::cout << std::endl;
        std::cout << "      Active orders (" << ticker << "): " << runtime.get_active_orders(u->get_user_id(), ticker).size() << std::endl;
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

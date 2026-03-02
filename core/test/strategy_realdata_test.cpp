// Compile and run:
// clang++ -std=c++20 -O3 -DNDEBUG -I/opt/homebrew/include -L/opt/homebrew/lib strategy_realdata_test.cpp ../market_data_stream.cpp -lhwy -lz -o strategy_realdata_test && ./strategy_realdata_test

#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <string>
#include <cassert>
#include "../order_engine.cpp"
#include "../engine_runtime.cpp"
#include "../market_data_stream.h"

using namespace backtest::runtime;
using namespace backtest::user;
using namespace engine;
using namespace stream;

// Statistics tracking
struct StrategyStats {
    uint64_t orders_placed = 0;
    uint64_t orders_filled = 0;
    uint64_t orders_cancelled = 0;
    double total_volume = 0.0;
    double best_price_seen = 0.0;
    uint64_t market_updates = 0;
};

StrategyStats strategy_stats;

// Simple market maker strategy (bound to one ticker)
void market_maker_strategy(User* user) {
    double bid = user->get_best_bid();
    double ask = user->get_best_ask();

    if (ask > 0 && (strategy_stats.best_price_seen == 0.0 || ask < strategy_stats.best_price_seen)) {
        strategy_stats.best_price_seen = ask;
    }

    strategy_stats.market_updates++;

    if (bid > 0 && ask > 0) {
        double mid = (bid + ask) / 2.0;
        double spread = (ask - bid);

        if (spread > 0.01 && spread < (mid * 0.01)) {
            if (user->submit_limit_order(OrderSide::BID, mid - (spread / 4.0), 0.001) != engine::INVALID_ORDER_ID) {
                strategy_stats.orders_placed++;
            }
            if (user->submit_limit_order(OrderSide::ASK, mid + (spread / 4.0), 0.001) != engine::INVALID_ORDER_ID) {
                strategy_stats.orders_placed++;
            }
        }
    }
}

// Aggressive taker strategy - crosses the spread (bound to one ticker)
void aggressive_taker_strategy(User* user) {
    double bid = user->get_best_bid();
    double ask = user->get_best_ask();

    strategy_stats.market_updates++;

    if (ask > 0 && strategy_stats.market_updates % 50 == 0) {
        if (user->submit_limit_order(OrderSide::BID, ask, 0.0001) != engine::INVALID_ORDER_ID) {
            strategy_stats.orders_placed++;
        }
    }

    if (bid > 0 && strategy_stats.market_updates % 75 == 0) {
        if (user->submit_limit_order(OrderSide::ASK, bid, 0.0001) != engine::INVALID_ORDER_ID) {
            strategy_stats.orders_placed++;
        }
    }
}

void test_strategy_with_real_data(std::string& data_file, const std::string& ticker) {
    std::cout << "\n=== Strategy Test with Real Market Data ===\n";
    std::cout << "Data File: " << data_file << "\n";
    std::cout << "Ticker: " << ticker << "\n\n";
    
    // Configurable parameters
    constexpr uint64_t MAX_EVENTS = 0;  // Adjust this to process more/fewer events
    constexpr uint64_t BATCH_INTERVAL = 25000;   // More frequent processing
    constexpr uint64_t STRATEGY_INTERVAL = 5000; // More frequent strategy calls
    
    // Reset runtime
    EngineRuntime::reset_instance();

    // Init Runtime (max_strategies >= 10002 so 10k registrations below don't reallocate users_ and invalidate maker/taker)
    auto& runtime = EngineRuntime::get_instance(1, false, STRATEGY_INTERVAL, 8 * 1024 * 1024, 16, 100250);
    //runtime.set_batch_size(BATCH_INTERVAL);

    // Stage Simulate
    runtime.simulate
    (
        std::move(data_file), // Real Data Path
        std::string(ticker),  // Name of Market
        MAX_EVENTS            // Max Amount of Events 
    );
    
    // Register strategies for this ticker
    UserView* maker = runtime.register_strategy(std::string(ticker), market_maker_strategy, 100000.0);
    UserView* taker = runtime.register_strategy(std::string(ticker), aggressive_taker_strategy, 100000.0);
    
    for (int i = 0; i < 10000; i++)
    {
        runtime.register_strategy(std::string(ticker), aggressive_taker_strategy, 100000.0);
    }
    
    std::cout << "Market Maker (User " << maker->get_user_id() << "): $" << maker->get_capital() << " capital\n";
    std::cout << "Aggressive Taker (User " << taker->get_user_id() << "): $" << taker->get_capital() << " capital\n\n";
    
    // Final processing
    std::cout << "\nProcessing Simulation...\n";
    runtime.process_pending_orders();
    
    const auto& sim_metrics = runtime.get_simulation_metrics(ticker);

    // Print results
    std::cout << "\n=== Replay Results ===\n";
    std::cout << "Events Processed: " << sim_metrics.market_updates_processed << "\n";
    std::cout << "L2 Orders Submitted: " << sim_metrics.orders_placed << "\n";
    std::cout << "L2 Orders Filled: " << sim_metrics.orders_filled << "\n";
    std::cout << "L2 Orders Cancelled: " << sim_metrics.orders_cancelled << "\n";
    std::cout << "L2 Orders Edited: " << sim_metrics.orders_edited << "\n";
    std::cout << "L2 Orders Replaced: " << sim_metrics.orders_replaced << "\n";
    std::cout << "Duration: " << sim_metrics.simulation_time_seconds << " seconds\n";
    std::cout << "Throughput: " << sim_metrics.orders_per_second() << " order ops/sec\n\n";
    
    std::cout << "=== Strategy Statistics ===\n";
    std::cout << "Strategy Orders Placed: " << strategy_stats.orders_placed << "\n";
    std::cout << "Market Updates Received: " << strategy_stats.market_updates << "\n";
    std::cout << "Best Price Seen: $" << strategy_stats.best_price_seen << "\n\n";
    
    // Get final market state (request → process → get for fresh snapshot)
    runtime.request_snapshot(ticker);
    runtime.process_pending_orders();
    auto final_bid = runtime.get_best_bid(ticker);
    auto final_ask = runtime.get_best_ask(ticker);
    auto market_price = runtime.get_market_price(ticker);

    std::cout << "=== Final Market State ===\n";
    std::cout << "Market Price: $" << market_price << "\n";
    std::cout << "Best Bid: $" << final_bid << "\n";
    std::cout << "Best Ask: $" << final_ask << "\n";
    std::cout << "Spread: $" << (final_ask - final_bid) << "\n\n";
    
    // Show market depth
    const auto& top_bids = runtime.get_market_depth(ticker, engine::OrderSide::BID);
    std::cout << "Top " + std::to_string(top_bids.size()) + " Bids:\n";
    for(const auto& s : top_bids){
        std::cout << "  $" << s.first << " (" << s.second << ")\n";
    }

    const auto& top_asks = runtime.get_market_depth(ticker, engine::OrderSide::ASK);
     std::cout << "Top " + std::to_string(top_asks.size()) + " Asks:\n";
    for(const auto& s : top_asks){
        std::cout << "  $" << s.first << " (" << s.second << ")\n";
    }
    
    // Show strategy performance (Realized P&L = only from closed trades; capital drop can be reserved + open position cost)
    std::cout << "\n=== Strategy Performance ===\n";
    auto fmt_price = [](double x) { std::ostringstream o; o << std::fixed << std::setprecision(2) << x; return o.str(); };
    std::cout << "Market Maker:\n";
    std::cout << "  Capital: $" << fmt_price(maker->get_capital()) << "\n";
    std::cout << "  Reserved cash (open BIDs): $" << fmt_price(maker->get_total_reserved_cash()) << "\n";
    std::cout << "  Open BIDs: ";
    { const auto bids = maker->get_open_bids(); if (bids.empty()) std::cout << "(none)\n"; else { std::cout << "\n"; for (const auto& b : bids) std::cout << "    " << b.first << " @ $" << fmt_price(b.second) << "\n"; } }
    std::cout << "  Committed sell qty (open ASKs): " << maker->get_committed_sell_qty() << " shares\n";
    std::cout << "  Open ASKs: ";
    { const auto asks = maker->get_open_asks(); if (asks.empty()) std::cout << "(none)\n"; else { std::cout << "\n"; for (const auto& a : asks) std::cout << "    " << a.first << " @ $" << fmt_price(a.second) << "\n"; } }
    std::cout << "  Realized P&L: $" << fmt_price(maker->get_realized_pnl()) << " (from closed trades only)\n";
    std::cout << "  Total Volume (shares): " << maker->get_total_volume() << "\n";
    std::cout << "  Position: " << maker->get_position() << " shares\n";
    std::cout << "  Unrealized P&L: $" << fmt_price(maker->get_unrealized_pnl()) << "\n";
    
    std::cout << "\nAggressive Taker:\n";
    std::cout << "  Capital: $" << fmt_price(taker->get_capital()) << "\n";
    std::cout << "  Reserved cash (open BIDs): $" << fmt_price(taker->get_total_reserved_cash()) << "\n";
    std::cout << "  Open BIDs: ";
    { const auto bids = taker->get_open_bids(); if (bids.empty()) std::cout << "(none)\n"; else { std::cout << "\n"; for (const auto& b : bids) std::cout << "    " << b.first << " @ $" << fmt_price(b.second) << "\n"; } }
    std::cout << "  Committed sell qty (open ASKs): " << taker->get_committed_sell_qty() << " shares\n";
    std::cout << "  Open ASKs: ";
    { const auto asks = taker->get_open_asks(); if (asks.empty()) std::cout << "(none)\n"; else { std::cout << "\n"; for (const auto& a : asks) std::cout << "    " << a.first << " @ $" << fmt_price(a.second) << "\n"; } }
    std::cout << "  Realized P&L: $" << fmt_price(taker->get_realized_pnl()) << " (from closed trades only)\n";
    std::cout << "  Total Volume (shares): " << taker->get_total_volume() << "\n";
    std::cout << "  Position: " << taker->get_position() << " shares\n";
    std::cout << "  Unrealized P&L: $" << fmt_price(taker->get_unrealized_pnl()) << "\n";
    
    // Get engine statistics (reuse snap_final from request → process → get above)
    const auto* snap = runtime.get_snapshot(ticker);
    uint64_t placed = snap ? static_cast<uint64_t>(snap->placed_count) : 0;
    uint64_t filled = snap ? static_cast<uint64_t>(snap->filled_count) : 0;
    uint64_t cancelled = snap ? static_cast<uint64_t>(snap->cancelled_count) : 0;
    uint64_t edited = snap ? static_cast<uint64_t>(snap->edited_count) : 0;
    uint64_t replaced = snap ? static_cast<uint64_t>(snap->replaced_count) : 0;
    
    std::cout << "\n=== Engine Statistics ===\n";
    std::cout << "Total Orders Placed: " << placed << "\n";
    std::cout << "Total Orders Filled: " << filled << "\n";
    std::cout << "Total Orders Cancelled: " << cancelled << "\n";
    std::cout << "Total Orders Edited: " << edited << "\n";
    std::cout << "Total Orders Replaced: " << replaced << "\n";
    std::cout << "Fill Rate: " << std::fixed << std::setprecision(2) 
              << (placed > 0 ? (100.0 * filled / placed) : 0.0) << "%\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== Titan Strategy Test with Real Data ===\n";
    std::cout << "Testing strategy execution with market data parser\n";
    
    // Default to binary file if available
    std::string data_file = "core/test/examples/binance-futures_incremental_book_L2_2024-12-01_BTCUSDT.bin";
    std::string ticker = "BTCUSDT";
    
    if (argc >= 2) {
        data_file = argv[1];
    }
    if (argc >= 3) {
        ticker = argv[2];
    }
    
    try {
        test_strategy_with_real_data(data_file, ticker);
        std::cout << "\n✓ Strategy test completed successfully!\n";
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

// Compile and run:
// clang++ -std=c++20 -O3 -DNDEBUG -I/opt/homebrew/include -L/opt/homebrew/lib strategy_realdata_test.cpp ../market_data_parser.cpp -lhwy -lz -o strategy_realdata_test && ./strategy_realdata_test

#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
#include <cassert>
#include "../order_engine.cpp"
#include "../engine_runtime.cpp"
#include "../market_data_parser.h"

using namespace backtest::runtime;
using namespace backtest::user;
using namespace engine;
using namespace parser;

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

// Simple market maker strategy
void market_maker_strategy(User* user) {
    auto tickers = user->list_tickers();
    
    for (const auto& ticker : tickers) {
        double bid = user->get_best_bid(ticker);
        double ask = user->get_best_ask(ticker);
        
        // Track best prices
        if (ask > 0 && (strategy_stats.best_price_seen == 0.0 || ask < strategy_stats.best_price_seen)) {
            strategy_stats.best_price_seen = ask;
        }
        
        strategy_stats.market_updates++;
        
        // Only trade if we have valid market data
        if (bid > 0 && ask > 0) {
            double mid = (bid + ask) / 2.0;
            double spread = (ask - bid);
            
            // Place passive orders on both sides (market making)
            // Only if spread is reasonable (less than 1% of mid price)
            if (spread > 0.01 && spread < (mid * 0.01)) {
                // Buy below mid
                if (user->submit_limit_order(ticker, OrderSide::BID, mid - (spread / 4.0), 0.001) != engine::INVALID_ORDER_ID) {
                    strategy_stats.orders_placed++;
                }
                
                // Sell above mid  
                if (user->submit_limit_order(ticker, OrderSide::ASK, mid + (spread / 4.0), 0.001) != engine::INVALID_ORDER_ID) {
                    strategy_stats.orders_placed++;
                }
            }
        }
    }
}

// Aggressive taker strategy - crosses the spread
void aggressive_taker_strategy(User* user) {
    auto tickers = user->list_tickers();
    
    for (const auto& ticker : tickers) {
        double bid = user->get_best_bid(ticker);
        double ask = user->get_best_ask(ticker);
        
        strategy_stats.market_updates++;
        
        // Randomly buy or sell at market prices (more frequently)
        if (ask > 0 && strategy_stats.market_updates % 50 == 0) {
            // Buy at ask (take liquidity)
            if (user->submit_limit_order(ticker, OrderSide::BID, ask, 0.0001) != engine::INVALID_ORDER_ID) {
                strategy_stats.orders_placed++;
            }
        }
        
        if (bid > 0 && strategy_stats.market_updates % 75 == 0) {
            // Sell at bid (take liquidity)
            if (user->submit_limit_order(ticker, OrderSide::ASK, bid, 0.0001) != engine::INVALID_ORDER_ID) {
                strategy_stats.orders_placed++;
            }
        }
    }
}

void test_strategy_with_real_data(const std::string& data_file, const std::string& ticker) {
    std::cout << "\n=== Strategy Test with Real Market Data ===\n";
    std::cout << "Data File: " << data_file << "\n";
    std::cout << "Ticker: " << ticker << "\n\n";
    
    // Configurable parameters
    constexpr uint64_t MAX_EVENTS = 100000000;  // Adjust this to process more/fewer events
    constexpr int PRICE_SAMPLE_SIZE = 5;     // Number of orders to average for initial price
    
    // Reset runtime
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(1, 16 * 1024 * 1024, false);
    
    // Parse first 5 orders to get average initial price
    MarketDataParser price_parser(data_file);
    L2Update price_update;
    double initial_price = 95000.0;  // Realistic BTCUSDT price as fallback
    std::vector<double> first_prices;
    
    // Get exactly first 5 orders for price averaging
    int count = 0;
    while (count < PRICE_SAMPLE_SIZE && price_parser.parse_next(price_update)) {
        if (!price_update.is_snapshot && price_update.amount > 0) {
            first_prices.push_back(price_update.price);
            count++;
        }
    }
    
    if (!first_prices.empty()) {
        // Calculate average of first 5 orders
        double sum = 0.0;
        for (double price : first_prices) {
            sum += price;
        }
        initial_price = sum / first_prices.size();
    }
    
    std::cout << "Initial Price (avg of first " << first_prices.size() << " orders): $" 
              << std::fixed << std::setprecision(2) << initial_price << "\n";
    std::cout << "Event Limit: " << MAX_EVENTS << "\n\n";
    
    // Register stock with reasonable IPO quantity (1 BTC worth)
    runtime.register_stock(ticker, initial_price, 1.0);
    
    // Register strategies
    User* maker = runtime.register_strategy(market_maker_strategy, 100000.0);
    User* taker = runtime.register_strategy(aggressive_taker_strategy, 100000.0);
    
    std::cout << "Market Maker (User " << maker->get_user_id() << "): $" << maker->get_capital() << " capital\n";
    std::cout << "Aggressive Taker (User " << taker->get_user_id() << "): $" << taker->get_capital() << " capital\n\n";
    
    // Disable auto-matching for performance
    runtime.set_auto_match(ticker, false);
    constexpr uint64_t BATCH_INTERVAL = 25000;   // More frequent processing
    constexpr uint64_t STRATEGY_INTERVAL = 5000; // More frequent strategy calls
    
    // Parse and replay market data
    std::cout << "Parsing market data...\n";
    MarketDataParser parser(data_file);
    L2Update update;
    uint64_t event_count = 0;
    uint64_t orders_submitted = 0;
    uint64_t snapshots_skipped = 0;
    uint64_t zero_deltas_skipped = 0;
    uint64_t invalid_price_skipped = 0;
    uint64_t deletion_events = 0;  // Track amount=0 events
    
    // Cache for tracking price levels
    std::unordered_map<uint64_t, double> price_cache;
    
    auto make_key = [](double price, char side) -> uint64_t {
        Price price_ticks = backtest::math::dollars_to_ticks(price);
        uint64_t side_bit = (side == 'b' || side == 'B') ? 0 : 1;
        return (price_ticks << 1) | side_bit;
    };
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    while (parser.parse_next(update)) {
        event_count++;
        
        // Skip snapshots
        if (update.is_snapshot) {
            snapshots_skipped++;
            continue;
        }
        
        // Track deletions (amount=0) separately
        if (update.amount == 0.0) {
            deletion_events++;
        }
        
        // Process L2 update
        OrderSide side = (update.side == 'b' || update.side == 'B') ? OrderSide::BID : OrderSide::ASK;
        uint64_t key = make_key(update.price, update.side);
        double old_amount = price_cache[key];
        double delta = update.amount - old_amount;
        price_cache[key] = update.amount;
        
        // Skip invalid prices
        if (update.price <= 0.0) {
            invalid_price_skipped++;
            continue;
        }
        
        // Skip zero deltas
        if (delta == 0.0) {
            zero_deltas_skipped++;
            continue;
        }
        
        // Submit order for delta
        if (delta > 0.0) {
            runtime.submit_limit_order(ticker, side, update.price, delta);
            orders_submitted++;
        } else {
            OrderSide opposite = (side == OrderSide::BID) ? OrderSide::ASK : OrderSide::BID;
            runtime.submit_limit_order(ticker, opposite, update.price, -delta);
            orders_submitted++;
        }
        
        // Run strategies periodically
        if (event_count % STRATEGY_INTERVAL == 0) {
            maker->on_book_update();
            taker->on_book_update();
        }
        
        // Batch process orders
        if (event_count % BATCH_INTERVAL == 0) {
            runtime.process_pending_orders_async();
        }
        
        // Limit test duration for reasonable execution time  
        if (event_count >= MAX_EVENTS) break;
    }
    
    // Final processing
    std::cout << "\nProcessing final batch...\n";
    runtime.set_auto_match(ticker, true);
    runtime.process_pending_orders();
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double duration_sec = duration.count() / 1000.0;
    
    // Print results
    std::cout << "\n=== Replay Results ===\n";
    std::cout << "Events Processed: " << event_count << "\n";
    std::cout << "  - Snapshots (skipped): " << snapshots_skipped << "\n";
    std::cout << "  - Zero Deltas (skipped): " << zero_deltas_skipped << "\n";
    std::cout << "  - Invalid Prices (skipped): " << invalid_price_skipped << "\n";
    std::cout << "L2 Orders Submitted: " << orders_submitted << "\n";
    std::cout << "Verification: " << snapshots_skipped << " + " << zero_deltas_skipped 
              << " + " << invalid_price_skipped << " + " << orders_submitted 
              << " = " << (snapshots_skipped + zero_deltas_skipped + invalid_price_skipped + orders_submitted) << "\n";
    std::cout << "Duration: " << duration_sec << " seconds\n";
    std::cout << "Throughput: " << (event_count / duration_sec) << " events/sec\n\n";
    
    std::cout << "=== Strategy Statistics ===\n";
    std::cout << "Strategy Orders Placed: " << strategy_stats.orders_placed << "\n";
    std::cout << "Market Updates Received: " << strategy_stats.market_updates << "\n";
    std::cout << "Best Price Seen: $" << strategy_stats.best_price_seen << "\n\n";
    
    // Get final market state
    double final_bid = runtime.get_best_bid(ticker);
    double final_ask = runtime.get_best_ask(ticker);
    double market_price = runtime.get_market_price(ticker);
    
    std::cout << "=== Final Market State ===\n";
    std::cout << "Market Price: $" << market_price << "\n";
    std::cout << "Best Bid: $" << final_bid << "\n";
    std::cout << "Best Ask: $" << final_ask << "\n";
    std::cout << "Spread: $" << (final_ask - final_bid) << "\n\n";
    
    // Show market depth
    auto bid_depth = runtime.get_market_depth(ticker, OrderSide::BID, 5);
    auto ask_depth = runtime.get_market_depth(ticker, OrderSide::ASK, 5);
    
    std::cout << "Top 5 Bids:\n";
    for (size_t i = 0; i < bid_depth.size(); ++i) {
        std::cout << "  $" << bid_depth[i].first << " (" << bid_depth[i].second << ")\n";
    }
    
    std::cout << "\nTop 5 Asks:\n";
    for (size_t i = 0; i < ask_depth.size(); ++i) {
        std::cout << "  $" << ask_depth[i].first << " (" << ask_depth[i].second << ")\n";
    }
    
    // Show strategy performance
    std::cout << "\n=== Strategy Performance ===\n";
    std::cout << "Market Maker:\n";
    std::cout << "  Capital: $" << maker->get_capital() << "\n";
    std::cout << "  Realized P&L: $" << maker->get_realized_pnl() << "\n";
    std::cout << "  Total Volume: $" << maker->get_total_volume() << "\n";
    std::cout << "  Position: " << maker->get_position(ticker) << " shares\n";
    std::cout << "  Unrealized P&L: $" << maker->get_unrealized_pnl(ticker, market_price) << "\n";
    
    std::cout << "\nAggressive Taker:\n";
    std::cout << "  Capital: $" << taker->get_capital() << "\n";
    std::cout << "  Realized P&L: $" << taker->get_realized_pnl() << "\n";
    std::cout << "  Total Volume: $" << taker->get_total_volume() << "\n";
    std::cout << "  Position: " << taker->get_position(ticker) << " shares\n";
    std::cout << "  Unrealized P&L: $" << taker->get_unrealized_pnl(ticker, market_price) << "\n";
    
    // Get engine statistics
    uint64_t placed = runtime.get_placed_count(ticker);
    uint64_t filled = runtime.get_filled_count(ticker);
    uint64_t cancelled = runtime.get_cancelled_count(ticker);
    
    std::cout << "\n=== Engine Statistics ===\n";
    std::cout << "Total Orders Placed: " << placed << "\n";
    std::cout << "Total Orders Filled: " << filled << "\n";
    std::cout << "Total Orders Cancelled: " << cancelled << "\n";
    std::cout << "Fill Rate: " << std::fixed << std::setprecision(2) 
              << (placed > 0 ? (100.0 * filled / placed) : 0.0) << "%\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== Titan Strategy Test with Real Data ===\n";
    std::cout << "Testing strategy execution with market data parser\n";
    
    // Default to binary file if available
    std::string data_file = "../../core/test/examples/binance-futures_incremental_book_L2_2024-12-01_BTCUSDT.bin";
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

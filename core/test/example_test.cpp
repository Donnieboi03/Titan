#include "../engine_runtime.cpp"  // Includes everything in ONE compilation unit
#include "../order_engine.cpp"
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

// Simple example showing how a user of the API would drive the runtime
//  - Create runtime with configurable workers and capacity
//  - Register stocks
//  - Register a simple strategy (lambda)
//  - Submit orders via the User wrapper and process them
//  - Inspect results (positions, best bid/ask, PnL)

static void usage(const char* prog) {
    std::cout << "Usage: " << prog << " [num_workers] [engine_capacity]" << std::endl;
    std::cout << "  num_workers    : number of worker threads (default 4)" << std::endl;
    std::cout << "  engine_capacity: per-engine capacity (default 10000)" << std::endl;
}

int main(int argc, char** argv)
{
    std::size_t num_workers = 2;
    std::size_t capacity = 10000;

    if (argc > 1) {
        if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
            usage(argv[0]);
            return 0;
        }
        num_workers = static_cast<std::size_t>(std::stoul(argv[1]));
    }
    if (argc > 2) {
        capacity = static_cast<std::size_t>(std::stoul(argv[2]));
    }

    std::cout << "Starting example runtime with " << num_workers << " workers and capacity " << capacity << "\n";

    // Reset any existing singleton instance and create a fresh runtime
    backtest::runtime::EngineRuntime::reset_instance();
    // NOTE: quantum must be > 0 for strategies to be triggered. Use quantum = 4 here.
    auto& runtime = backtest::runtime::EngineRuntime::get_instance(num_workers, capacity, false);

    // Register a couple of stocks that the strategy can trade
    runtime.register_stock("EXM", 100.00, 1000.0); // example stock
    runtime.register_stock("ACME", 25.50, 5000.0);

    // Process IPO placement so books are initialized and visible
    runtime.process_pending_orders();

    // Register a more complex, stateful strategy that alternates actions
    auto state = std::make_shared<std::unordered_map<backtest::user::UserId, std::atomic<int>>>();
    auto complex_strategy = [state](backtest::user::User* u) {
        backtest::user::UserId id = u->get_user_id();
        std::atomic<int>& cnt = (*state)[id];
        auto strat = cnt.fetch_add(1, std::memory_order_relaxed);

        // Read market data for both tickers
        double exm_bid = u->get_best_bid("EXM");
        double exm_ask = u->get_best_ask("EXM");
        double acme_bid = u->get_best_bid("ACME");
        double acme_ask = u->get_best_ask("ACME");

        // Behavior rotates every call: 1=aggressive buy EXM, 2=place passive sell if we have position, 3=opportunistic market buy ACME
        if (strat % 3 == 1) {
            if (exm_ask > 0) {
                engine::OrderId ok = u->submit_limit_order("EXM", engine::OrderSide::BID, exm_ask, 1.0);
                if (ok != engine::INVALID_ORDER_ID) std::cout << "[strategy] user " << id << " placed limit BUY EXM @ " << exm_ask << "\n";
            }
        } else if (strat % 3 == 2) {
            auto pos = u->get_positions("EXM");
            if (!pos.empty() && exm_bid > 0) {
                // Try to take profit with a slightly higher ask
                double sell_price = exm_bid + 0.02;
                engine::OrderId ok = u->submit_limit_order("EXM", engine::OrderSide::ASK, sell_price, 1.0);
                if (ok != engine::INVALID_ORDER_ID) std::cout << "[strategy] user " << id << " placed limit SELL EXM @ " << sell_price << "\n";
            }
        } else {
            // Opportunistic market buy on ACME if there's available ask
            if (acme_ask > 0) {
                engine::OrderId ok = u->submit_market_order("ACME", engine::OrderSide::BID, 0.5);
                if (ok != engine::INVALID_ORDER_ID) std::cout << "[strategy] user " << id << " submitted MARKET BUY ACME @ " << acme_ask << "\n";
            }
        }
    };

    // Register the strategy for EXM (it can still trade ACME via the User API)
    backtest::user::User* trader = runtime.register_strategy("EXM", complex_strategy);
    if (!trader) {
        std::cerr << "Failed to register strategy/user" << std::endl;
        return 1;
    }

    std::cout << "Registered trader with user_id=" << trader->get_user_id() << " and capital=$" << trader->get_capital() << "\n";

    runtime.set_batch_size(32);
    // Seed varied orders across the two tickers to create realistic book state
    for (int i = 0; i < 256; ++i) {
        const char* ticker = (i % 2 == 0) ? "EXM" : "ACME";
        engine::OrderSide side = (i % 3 == 0) ? engine::OrderSide::BID : engine::OrderSide::ASK;
        double base = (std::string(ticker) == "EXM") ? 100.00 : 25.50;
        double price = base + ((i % 5) - 2) * 0.25; // vary around IPO
        double qty = 0.5 + (i % 4) * 0.5;
        runtime.submit_limit_order(ticker, side, price, qty);
    }

    // Add a couple of more aggressive orders
    runtime.submit_limit_order("EXM", engine::OrderSide::ASK, 101.50, 2.0);
    runtime.submit_limit_order("ACME", engine::OrderSide::BID, 25.40, 10.0);

    // Process jobs so that seeded orders are handled and strategies will be invoked by quantum
    runtime.process_pending_orders();

    // Now inspect results: positions, best bid/ask and PnL
    auto exm_positions = trader->get_positions("EXM");
    std::cout << "Trader EXM open positions count: " << exm_positions.size() << "\n";

    double best_bid = trader->get_best_bid("EXM");
    double best_ask = trader->get_best_ask("EXM");
    std::cout << "Market EXM best bid: $" << best_bid << " best ask: $" << best_ask << "\n";

    std::cout << "Trader realized PnL: $" << trader->get_realized_pnl()
              << "  capital: $" << trader->get_capital()
              << "  total volume: " << trader->get_total_volume() << "\n";

    // Print aggregate stats for both tickers
    for (const auto& t : {std::string("EXM"), std::string("ACME")}) {
        std::cout << "Ticker " << t << ": Placed=" << runtime.get_placed_count(t)
                  << " Open=" << runtime.get_open_count(t)
                  << " Filled=" << runtime.get_filled_count(t)
                  << " Cancelled=" << runtime.get_cancelled_count(t) << "\n";
    }

    // Demonstrate how a client would cancel or edit orders via User wrapper
    if (!exm_positions.empty()) {
        engine::OrderId oid = exm_positions[0];
        std::cout << "Trader has order id " << oid << ", attempting to cancel it...\n";
        bool cancelled = trader->submit_cancel_order("EXM", oid);
        runtime.process_pending_orders();
        std::cout << "Cancel returned: " << (cancelled ? "submitted" : "rejected") << "\n";
    }
    std::cout << "Example run complete. Use this file as a template for integrating strategies with the EngineRuntime API.\n";
    return 0;
}

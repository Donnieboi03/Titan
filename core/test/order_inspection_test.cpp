#include <iostream>
#include <iomanip>
#include <cassert>
#include "../order_engine.cpp"
#include "../engine_runtime.cpp"

using namespace backtest::runtime;
using namespace backtest::user;
using namespace engine;

// Example: Strategy that inspects and manages its own orders (bound to one ticker)
void order_managing_strategy(User* user) {
    double bid = user->get_best_bid();
    double ask = user->get_best_ask();

    if (bid <= 0 || ask <= 0) return;

    double mid = (bid + ask) / 2.0;
    double spread = ask - bid;

    auto order_ids = user->get_positions();

    for (const auto& order_id : order_ids) {
        const OrderInfo* info = user->get_order_info(order_id);
        if (!info || info->status_ != OrderStatus::OPEN) continue;

        double order_price = backtest::math::ticks_to_dollars(info->price_);
        double distance_from_mid = std::abs(order_price - mid);

        if (distance_from_mid > mid * 0.01) {
            user->submit_cancel_order(order_id);
            std::cout << "Cancelled order #" << order_id
                      << " at $" << order_price
                      << " (mid: $" << mid << ")\n";
        }
    }

    if (order_ids.size() < 10) {
        user->submit_limit_order(OrderSide::BID, mid - spread * 0.25, 1.0);
        user->submit_limit_order(OrderSide::ASK, mid + spread * 0.25, 1.0);
    }
}

// Example: Strategy that only trades when it has no open orders (bound to one ticker)
void cautious_strategy(User* user) {
    auto order_ids = user->get_positions();

    int open_orders = 0;
    for (const auto& order_id : order_ids) {
        const OrderInfo* info = user->get_order_info(order_id);
        if (info && info->status_ == OrderStatus::OPEN)
            open_orders++;
    }

    if (open_orders == 0) {
        double bid = user->get_best_bid();
        double ask = user->get_best_ask();
        if (bid > 0 && ask > 0) {
            double mid = (bid + ask) / 2.0;
            user->submit_limit_order(OrderSide::BID, mid * 0.99, 1.0);
            std::cout << "Placed order at $" << (mid * 0.99) << "\n";
        }
    }
}

void test_order_inspection() {
    std::cout << "=== Order Inspection Test ===\n\n";
    
    // Setup runtime
    EngineRuntime::reset_instance();
    auto& runtime = EngineRuntime::get_instance(1, false, 1000, 1048576);
    
    // Register stock
    std::string ticker = "TEST";
    runtime.register_stock(std::string(ticker), 100.0, 1000.0);
    
    // Register strategy for this ticker (returns UserView*; client submits via runtime)
    UserView* manager = runtime.register_strategy(std::string(ticker), order_managing_strategy, 100000.0);
    
    std::cout << "Registered strategy: Manager (User " << manager->get_user_id() << ")\n\n";
    
    // Enable matching so orders can interact
    runtime.set_auto_match(ticker, true);
    
    // Manager places some initial orders via User (tracked; same ticker as strategy)
    auto* mgr = static_cast<user::User*>(manager);
    mgr->submit_limit_order(OrderSide::BID, 99.0, 10.0);
    mgr->submit_limit_order(OrderSide::BID, 98.5, 10.0);
    mgr->submit_limit_order(OrderSide::ASK, 101.0, 10.0);
    mgr->submit_limit_order(OrderSide::ASK, 102.0, 10.0);
    
    runtime.request_snapshot(ticker);
    runtime.process_pending_orders();
    
    std::cout << "Manager placed 4 initial orders\n";
    const auto* snap = runtime.get_snapshot(ticker);
    double best_bid = snap && snap->best_bid != static_cast<engine::Price>(-1) ? backtest::math::ticks_to_dollars(snap->best_bid) : -1.0;
    double best_ask = snap && snap->best_ask != static_cast<engine::Price>(-1) ? backtest::math::ticks_to_dollars(snap->best_ask) : -1.0;
    std::cout << "Best Bid: $" << best_bid << "\n";
    std::cout << "Best Ask: $" << best_ask << "\n\n";
    
    // Inspect the orders (positions/order info via runtime when client has UserView*)
    auto order_ids = runtime.get_positions(manager->get_user_id(), ticker);
    std::cout << "Manager has " << order_ids.size() << " orders:\n";
    
    for (const auto& order_id : order_ids) {
        const OrderInfo* info = runtime.get_order(ticker, order_id);
        if (info) {
            double price = backtest::math::ticks_to_dollars(info->price_);
            std::cout << "  Order #" << order_id << ": "
                      << (info->side_ == OrderSide::BID ? "BID" : "ASK")
                      << " $" << std::fixed << std::setprecision(2) << price
                      << " qty=" << info->qty_
                      << " status=";
            
            switch (info->status_) {
                case OrderStatus::OPEN: std::cout << "OPEN"; break;
                case OrderStatus::FILLED: std::cout << "FILLED"; break;
                case OrderStatus::CANCELLED: std::cout << "CANCELLED"; break;
                default: std::cout << "UNKNOWN";
            }
            std::cout << "\n";
        }
    }
    std::cout << "\n";
    
    // Trigger manager strategy (should cancel far orders and place new ones)
    // Test-only: cast to User to trigger one strategy run; normal clients only have UserView*
    std::cout << "--- Running Manager Strategy ---\n";
    static_cast<User*>(manager)->on_book_update();
    runtime.process_pending_orders();
    
    // Inspect orders again
    order_ids = runtime.get_positions(manager->get_user_id(), ticker);
    std::cout << "\nAfter strategy execution, Manager has " << order_ids.size() << " orders:\n";
    
    for (const auto& order_id : order_ids) {
        const OrderInfo* info = runtime.get_order(ticker, order_id);
        if (info) {
            double price = backtest::math::ticks_to_dollars(info->price_);
            std::cout << "  Order #" << order_id << ": "
                      << (info->side_ == OrderSide::BID ? "BID" : "ASK")
                      << " $" << std::fixed << std::setprecision(2) << price
                      << " qty=" << info->qty_;
            
            switch (info->status_) {
                case OrderStatus::OPEN: std::cout << " [OPEN]"; break;
                case OrderStatus::FILLED: std::cout << " [FILLED]"; break;
                case OrderStatus::CANCELLED: std::cout << " [CANCELLED]"; break;
                default: std::cout << " [UNKNOWN]";
            }
            std::cout << "\n";
        }
    }
    
    // Final statistics
    std::cout << "\n=== Final Statistics ===\n";
    std::cout << "Manager:\n";
    std::cout << "  Capital: $" << manager->get_capital() << "\n";
    std::cout << "  Position: " << manager->get_position() << " shares\n";
    std::cout << "  Realized P&L: $" << manager->get_realized_pnl() << "\n";
    std::cout << "  Total Volume: $" << manager->get_total_volume() << "\n\n";
    
    std::cout << "✓ Order inspection test completed!\n";
    std::cout << "✓ Successfully demonstrated get_order_info() for order state inspection\n";
}

int main() {
    try {
        test_order_inspection();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

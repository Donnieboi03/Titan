#include "Exchange.cpp"

// Print engine stats for a specific ticker
void print_stats(const std::string& ticker, const std::shared_ptr<OrderEngine>& engine) 
{
    std::cout << "=== STATS FOR " << ticker << " ===" << std::endl;
    std::cout << "CURRENT PRICE: " << engine->get_price() << std::endl;
    std::cout << "OPEN ORDERS COUNT: " << engine->get_orders_by_status(OrderStatus::OPEN).size() << std::endl;
    std::cout << "FILLED ORDERS COUNT: " << engine->get_orders_by_status(OrderStatus::FILLED).size() << std::endl;
    std::cout << "CANCELED ORDERS COUNT: " << engine->get_orders_by_status(OrderStatus::CANCELLED).size() << std::endl;
    std::cout << "REJECTED ORDERS COUNT: " << engine->get_orders_by_status(OrderStatus::REJECTED).size() << std::endl;
    std::cout << "=== MARKET DEPTH BIDS ===" << std::endl;
    auto bids_depth = engine->get_market_depth(OrderSide::BID, 20);
    for (auto& order: bids_depth)
        std::cout << " Price: " << order.first << " Quantity: " << order.second << std::endl;
    std::cout << "=== MARKET DEPTH ASKS ===" << std::endl;
    auto asks_depth = engine->get_market_depth(OrderSide::ASK, 20);
    for (auto& order: asks_depth)
        std::cout << "Price: " << order.first << " Quantity: " << order.second << std::endl;
    std::cout << "==============================" << std::endl;
}


// Monte Carlo Simulation for a single ticker with skew support
void monte_carlo_simulation
(const std::shared_ptr<Exchange>& StockExchange, const std::string& ticker, int num_orders, double ipo_price, 
 double ipo_qty, double volatility, double skew) // skew: -1.0 (bearish) to 1.0 (bullish)
{
    StockExchange->initialize_stock(ticker, ipo_price, ipo_qty);
    std::mt19937_64 rng(std::random_device{}());

    std::normal_distribution<double> normal_dist(0.0, volatility);
    std::uniform_real_distribution<double> qty_dist(1, 100);
    std::uniform_real_distribution<double> offset_dist(-5, 5);
    std::bernoulli_distribution side_bias(0.5 + skew * 0.5); // skew biases toward BUY if >0

    double cancel_probability = 0.05;
    std::uniform_real_distribution<double> cancel_chance(0.0, 1.0);

    for (int i = 0; i < num_orders; ++i) {
        // Bias order side using skew
        OrderSide side = side_bias(rng) ? OrderSide::BID : OrderSide::ASK;
        OrderType type = (rng() % 2 == 0) ? OrderType::MARKET : OrderType::LIMIT;
        double qty = qty_dist(rng);

        // Apply skewed price change
        double current_price = StockExchange->get_price(ticker);
        double change = normal_dist(rng);

        // Apply skew to upward vs downward moves
        if (change > 0) change *= (1.0 + skew); // upward amplified if bullish
        else change *= (1.0 - skew);            // downward dampened if bullish

        double price = current_price != -1 ? std::max(0.01, current_price * (1.0 + change) + offset_dist(rng)) : ipo_price;

        unsigned int order_id;
        if (type == OrderType::MARKET)
            order_id = StockExchange->market_order(ticker, side, qty);
        else
            order_id = StockExchange->limit_order(ticker, side, price, qty);

        if (cancel_chance(rng) < cancel_probability)
            StockExchange->cancel_order(ticker, order_id);
    }
}
// Main function to spawn threads for multiple tickers
int main() 
{
    std::shared_ptr<Exchange> DBSE = std::make_shared<Exchange>(false);
    std::vector<std::string> tickers = {"AAPL", "TSLA", "AMZN", "NVDA"};
    std::vector<std::thread> threads;

    for (const auto& ticker : tickers) {
        threads.emplace_back([=]() {
            monte_carlo_simulation(DBSE, ticker, 10000, 100.0, 10000, 0.05, 0.15);
        });
    }

    for (auto& t : threads) {
        t.join(); // Wait for all simulations to finish
    }

    // Print stats after all threads finish
    for (const auto& ticker : tickers) {
        print_stats(ticker, DBSE->get_engine(ticker)); // Assumes Exchange::get_engine(ticker) exists
        std::cout << std::endl;
    }
    return 0;
}

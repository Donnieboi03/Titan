#pragma once
#include "OrderEngine.cpp"

using OrderEngines = std::unordered_map<std::string, std::shared_ptr<OrderEngine>>;

class Exchange
{
    public:
        Exchange(std::size_t default_capacity = 100000, bool _verbose = true)
        : default_capacity_(default_capacity), verbose(_verbose)
        { 
        }
        
        bool initialize_stock(const std::string& _ticker, Price _ipo_price, Quantity _ipo_qty, std::size_t capacity = 0)
        {
            try
            {
                // IF ipo price or qty is less than or equal to 0
                if (_ipo_price <= 0.0 || _ipo_qty <= 0.0)
                    throw std::runtime_error("IPO Price/Quantity must be > 0");
                // If ticker is already in Exchange then error
                if (StockExchange.find(_ticker) != StockExchange.end())
                    throw std::runtime_error("Stock Already Exist");

                // Use provided capacity or default
                std::size_t engine_capacity = capacity > 0 ? capacity : default_capacity_;
                auto engine = std::make_shared<OrderEngine>(_ticker, engine_capacity, verbose);
                if (!engine)
                    throw std::runtime_error("Null Matching Engine");

                // Place initial sell at IPO Price and IPO Quantitiy
                OrderId ipo_order = engine->place_order(OrderSide::ASK, OrderType::LIMIT, _ipo_price, _ipo_qty);
                // If no Order then error (check for -1 instead of !ipo_order)
                if (ipo_order == static_cast<OrderId>(-1))
                    throw std::runtime_error("IPO Order Failed to Place");
                StockExchange.emplace(_ticker, std::move(engine));
                return true;
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Stock Initlization Error: " << e.what() << '\n';
                return false;
            }
        }

        OrderId limit_order(const std::string& _ticker, OrderSide _side, Price _price, Quantity _qty) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                // If price or qty less than or equal to 0
                if (_price <= 0 || _qty <= 0)
                    throw std::runtime_error("Price/Quantity must be > 0");
                
                OrderId order = StockExchange.at(_ticker)->place_order(_side, OrderType::LIMIT, _price, _qty);
                // If no Order then error (check for -1 for failure)
                if (order == static_cast<OrderId>(-1))
                    throw std::runtime_error("Order Failed to Place");
                return order;
            }
            catch(const std::exception& e)
            {
               if (verbose)
                std::cerr << "Place Order Error: " << e.what() << '\n';
                return static_cast<OrderId>(-1);
            }
        }

        OrderId market_order(const std::string& _ticker, OrderSide _side, Quantity _qty) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                // If qty less than or equal to 0
                if (_qty <= 0)
                    throw std::runtime_error("Price/Quantity must be > 0");
                
                OrderId order = StockExchange.at(_ticker)->place_order(_side, OrderType::MARKET, -1, _qty);
                // If no Order then error (check for -1 for failure)
                if (order == static_cast<OrderId>(-1))
                    throw std::runtime_error("Order Failed to Place");
                return order;
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Place Order Error: " << e.what() << '\n';
                return static_cast<OrderId>(-1);
            }
        }

        bool cancel_order(const std::string& _ticker, OrderId order_id) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                
                auto is_canceled = StockExchange.at(_ticker)->cancel_order(order_id);
                // If canceled failed then error
                if (!is_canceled)
                    throw std::runtime_error("Order Failed to Cancel");
                return is_canceled;
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Cancel Order Error: " << e.what() << '\n';
                return false;
            }
        }

        OrderId edit_order(const std::string& _ticker, OrderId order_id, OrderSide _side, Price _price, Quantity _qty) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                
                OrderId order = StockExchange.at(_ticker)->edit_order(order_id, _side, _price, _qty);
                // If no Order then error (check for -1 for failure)
                if (order == static_cast<OrderId>(-1))
                    throw std::runtime_error("Order Failed to Edit");
                return order;
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Edit Order Error: " << e.what() << '\n';
                return static_cast<OrderId>(-1);
            }
        }

        const OrderInfo* get_order(const std::string& _ticker, OrderId order_id) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");

                auto order = StockExchange.at(_ticker)->get_order(order_id);
                // If no Order then error
                if (!order)
                    throw std::runtime_error("Failed to Get Order");
                return order;
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Get Order Error: " << e.what() << '\n';
                return nullptr;
            }
        }

        // GET: Average Price
        Price get_price(const std::string& _ticker) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                
                return StockExchange.at(_ticker)->get_price();
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Get Price Error: " << e.what() << '\n';
                return -1;
            }
        }

        Price get_best_bid(const std::string& _ticker) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");

                auto best_bid = StockExchange.at(_ticker)->get_best_bid();
                // If no best bid then error
                if (best_bid == -1)
                    throw std::runtime_error("Bid Side is Empty");
                return best_bid;
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Get Best Bid Error: " << e.what() << '\n';
                return -1;
            }
        }

        Price get_best_ask(const std::string& _ticker) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                
                auto best_ask = StockExchange.at(_ticker)->get_best_ask();
                // If no best ask then error
                if (best_ask == -1)
                    throw std::runtime_error("Ask Side is Empty");
                return best_ask;
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Get Best Ask Error: " << e.what() << '\n';
                return -1;
            }
        }

        std::vector<OrderInfo> get_orders_by_status(const std::string& _ticker, OrderStatus status) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                return StockExchange.at(_ticker)->get_orders_by_status(status);
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Get Orders By Status Error: " << e.what() << '\n';
                return {};
            }
        }

        std::vector<std::pair<Price, Quantity>> get_market_depth(const std::string& _ticker, OrderSide _side, std::size_t depth = 10) const
        {
            try
            {
                if (StockExchange.find(_ticker) == StockExchange.end()) 
                    throw std::runtime_error("Stock Does Not Exist");
                return StockExchange.at(_ticker)->get_market_depth(_side, depth);
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Get Market Depth Error: " << e.what() << '\n';
                return {};
            }
        }

        std::vector<std::string> get_tradable_tickers() const
        {
            std::vector<std::string> tickers;
            // Itterate Through All Stocks in Exchange
            for (auto& stock: StockExchange)
                tickers.push_back(stock.first);
            return std::move(tickers);
        }
        
        std::shared_ptr<OrderEngine> get_engine(const std::string& _ticker) const
        {
            try
            {
                 // If ticker is not in Exchange then error
                if (StockExchange.find(_ticker) == StockExchange.end())
                    throw std::runtime_error("Stock Does Not Exist");
                return StockExchange.at(_ticker);
            }
            catch(const std::exception& e)
            {
                if (verbose)
                    std::cerr << "Get Engine Error: " << e.what() << '\n';
                return nullptr;
            }
            
        }

    private:
        OrderEngines StockExchange;
        std::size_t default_capacity_; // Default capacity for new OrderEngines
        bool verbose; // Verbose Mode
};
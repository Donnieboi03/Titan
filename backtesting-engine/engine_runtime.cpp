#pragma once
#include "order_engine.cpp"
#include "job_scheduler.cpp"
#include "../tools/arena.cpp"
#include <unordered_set>

// Participant ID for tracking ownership
using UserId = std::uint32_t;
constexpr UserId IPO_HOLDER = 0;  // IPO holder owns all initial shares

using EngineId = std::uint32_t;
struct OrderEngineInfo
{
    OrderEngine engine_;  // Direct storage, no heap allocation
    Quantity ipo_shares_;
    EngineId engine_id_;
    
    // Constructor for in-place construction
    OrderEngineInfo(const std::string& ticker, std::size_t capacity, bool verbose, 
                    Quantity ipo_shares, EngineId engine_id)
    : engine_(ticker, capacity, verbose, true),  // auto_match = true
      ipo_shares_(ipo_shares),
      engine_id_(engine_id)
    {}
};

using EngineMap = std::unordered_map<std::string, OrderEngineInfo>;

// Job parameters for order operations
struct OrderJobArgs
{
    OrderEngine* engine;  // Raw pointer - safe because engine lifetime > job lifetime
    OrderSide side;
    OrderType type;
    Price price;
    Quantity qty;
    OrderId order_id;
    UserId user_id;
    OrderId* result_id;
    bool* result_bool;
};

using ArgsArena = Arena<OrderJobArgs>;

// Type alias for user order tracking
using UserOrderMap = std::unordered_map<UserId, std::unordered_map<std::string, std::unordered_set<OrderId>>>;

class EngineRuntime
{
public:
    // Delete copy constructor and assignment operator
    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;
    
    // Singleton instance accessor
    static EngineRuntime& get_instance(std::size_t num_threads = 4, std::size_t default_capacity = 100000, std::size_t batch_size = 0, bool _verbose = true, bool blocking = true)
    {
        static EngineRuntime instance(num_threads, default_capacity, batch_size, _verbose, blocking);
        return instance;
    }
    
    // Register a new stock in the exchange
    bool register_stock(const std::string& _ticker, Price _ipo_price, Quantity _ipo_qty, std::size_t capacity = 0)
    {
        try
        {
            // IF ipo price or qty is less than or equal to 0
            if (_ipo_price <= 0.0 || _ipo_qty <= 0.0)
                throw std::runtime_error("IPO Price/Quantity must be > 0");
            // If ticker is already in Exchange then error
            if (stock_exchange_.find(_ticker) != stock_exchange_.end())
                throw std::runtime_error("Stock Already Exist");

            // Use provided capacity or default
            std::size_t engine_capacity = capacity > 0 ? capacity : default_capacity_;
            auto engine = std::make_shared<OrderEngine>(_ticker, engine_capacity, verbose_);
            if (!engine)
                throw std::runtime_error("Null Matching Engine");

            // Assign engine ID for job routing
            std::size_t engine_id = next_engine_id_++;

            // Construct OrderEngineInfo directly in the map
            auto [it, inserted] = stock_exchange_.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(_ticker),
                std::forward_as_tuple(_ticker, engine_capacity, verbose_, _ipo_qty, 
                                      static_cast<EngineId>(engine_id))
            );
            
            if (!inserted)
                throw std::runtime_error("Failed to insert stock");

            // Place initial sell at IPO Price and IPO Quantity (from IPO holder)
            OrderId ipo_order = it->second.engine_.place_order(OrderSide::ASK, OrderType::LIMIT, _ipo_price, _ipo_qty);
            // If no Order then error (check for -1 instead of !ipo_order)
            if (ipo_order == static_cast<OrderId>(-1))
                throw std::runtime_error("IPO Order Failed to Place");
            
            // Track IPO order ownership
            user_orders_[IPO_HOLDER][_ticker].insert(ipo_order);
            
            if (verbose_)
                std::cout << "[RUNTIME] Registered " << _ticker << " with IPO: " 
                          << _ipo_qty << " shares @ $" << _ipo_price 
                          << " (owned by user " << IPO_HOLDER << ")" << std::endl;
            
            return true;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Stock Registration Error: " << e.what() << '\n';
            return false;
        }
    }
    
    // Unregister a stock from the exchange
    bool unregister_stock(const std::string& _ticker)
    {
        try
        {
            // Check if stock exists
            if (stock_exchange_.find(_ticker) == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            
            // Wait for any pending jobs to complete
            wait_for_jobs();
            
            // Remove stock from exchange
            stock_exchange_.erase(_ticker);
            
            // Remove all user orders for this ticker
            for (auto& [user_id, user_tickers] : user_orders_)
            {
                user_tickers.erase(_ticker);
            }
            
            if (verbose_)
                std::cout << "[RUNTIME] Unregistered " << _ticker << std::endl;
            
            return true;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Stock Unregistration Error: " << e.what() << '\n';
            return false;
        }
    }
    
    // Reset the entire runtime state (allows reusing stock names and changing parameters)
    void reset()
    {
        try
        {
            // Wait for all pending jobs to complete
            wait_for_jobs();
            
            // Clear all stocks
            stock_exchange_.clear();
            
            // Clear user orders
            user_orders_.clear();
            
            // Reset counters
            next_engine_id_ = 0;
            batch_counter_ = 0;
            
            // Clear arenas (free all allocated slots)
            for (auto& arena : worker_arenas_)
            {
                arena.reset();  // Reset arena to initial state
            }
            
            if (verbose_)
                std::cout << "[RUNTIME] Reset complete - all stocks and orders cleared" << std::endl;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Runtime Reset Error: " << e.what() << '\n';
        }
    }

    void limit_order(const std::string& _ticker, OrderSide _side, Price _price, Quantity _qty, OrderId* result_id_ptr, UserId user_id = 1)
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            if (_price <= 0 || _qty <= 0)
                throw std::runtime_error("Price/Quantity must be > 0");
            
            // VALIDATE OWNERSHIP BEFORE SUBMITTING
            if (_side == OrderSide::ASK)
            {
                if (!has_sufficient_shares(user_id, _ticker, _qty))
                {
                    std::string err_msg = "User " + std::to_string(user_id) + 
                                         " does not have sufficient shares to sell " + std::to_string(_qty);
                    throw std::runtime_error(err_msg);
                }
            }
            
            std::size_t engine_id = stock_exchange_.at(_ticker).engine_id_;
            std::size_t worker_id = engine_id % num_workers_;
            auto& arena = worker_arenas_[worker_id];
            
            auto args_idx = arena.emplace(OrderJobArgs{
                &stock_exchange_.at(_ticker).engine_, _side, OrderType::LIMIT,
                _price, _qty, 0, user_id, result_id_ptr, nullptr
            });
            
            // Check if arena is full BEFORE accessing arena[args_idx]
            if (args_idx == static_cast<Arena<OrderJobArgs>::Index>(-1))
                throw std::runtime_error("Arena overflow - increase batch_size or arena capacity");
            
            // Capture arena pointer and args_idx - no heap allocation needed!
            auto arena_ptr = &arena;
            
            // Capture this for user_orders_ access
            auto* runtime = this;
            std::string ticker_copy = _ticker;  // Copy for lambda
            
            Job job(
                // Execute lambda
                [arena_ptr, args_idx, runtime, ticker_copy]() {
                    auto* params = &(*arena_ptr)[args_idx];
                    OrderId order_id = params->engine->place_order(params->side, params->type, params->price, params->qty);
                    *(params->result_id) = order_id;
                    
                    // Track order ownership
                    if (order_id != static_cast<OrderId>(-1))
                        runtime->user_orders_[params->user_id][ticker_copy].insert(order_id);
                },
                // Cleanup lambda
                [arena_ptr, args_idx]() {
                    arena_ptr->free(args_idx);
                },
                engine_id
            );
            
            scheduler_.submit_job(std::move(job));
            
            // Auto-execute batch if batch_size is set and reached
            if (batch_size_ > 0)
            {
                batch_counter_++;
                if (batch_counter_ >= batch_size_)
                {
                    execute_batch();
                }
            }
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Place Order Error: " << e.what() << '\n';
            if (result_id_ptr) *result_id_ptr = static_cast<OrderId>(-1);
        }
    }

    void market_order(const std::string& _ticker, OrderSide _side, Quantity _qty, OrderId* result_id_ptr, UserId user_id = 1)
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            if (_qty <= 0)
                throw std::runtime_error("Quantity must be > 0");
            
            // VALIDATE OWNERSHIP BEFORE SUBMITTING (like Robinhood)
            if (_side == OrderSide::ASK)
            {
                if (!has_sufficient_shares(user_id, _ticker, _qty))
                {
                    std::string err_msg = "User " + std::to_string(user_id) + 
                                         " does not have sufficient shares to sell " + std::to_string(_qty);
                    throw std::runtime_error(err_msg);
                }
            }
            
            std::size_t engine_id = stock_exchange_.at(_ticker).engine_id_;
            std::size_t worker_id = engine_id % num_workers_;
            auto& arena = worker_arenas_[worker_id];
            
            auto args_idx = arena.emplace(OrderJobArgs{
                &stock_exchange_.at(_ticker).engine_, _side, OrderType::MARKET,
                -1, _qty, 0, user_id, result_id_ptr, nullptr
            });
            
            // Check if arena is full
            if (args_idx == static_cast<Arena<OrderJobArgs>::Index>(-1))
                throw std::runtime_error("Arena overflow - increase batch_size or arena capacity");
            
            auto arena_ptr = &arena;
            
            // Capture this for user_orders_ access
            auto* runtime = this;
            std::string ticker_copy = _ticker;  // Copy for lambda
            
            Job job(
                [arena_ptr, args_idx, runtime, ticker_copy]() {
                    auto* params = &(*arena_ptr)[args_idx];
                    OrderId order_id = params->engine->place_order(params->side, params->type, params->price, params->qty);
                    *(params->result_id) = order_id;
                    
                    // Track order ownership
                    if (order_id != static_cast<OrderId>(-1))
                        runtime->user_orders_[params->user_id][ticker_copy].insert(order_id);
                },
                [arena_ptr, args_idx]() {
                    arena_ptr->free(args_idx);
                },
                engine_id
            );
            
            scheduler_.submit_job(std::move(job));
            
            // Auto-execute batch if batch_size is set and reached
            if (batch_size_ > 0)
            {
                batch_counter_++;
                if (batch_counter_ >= batch_size_)
                {
                    execute_batch();
                }
            }
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Place Order Error: " << e.what() << '\n';
            if (result_id_ptr) *result_id_ptr = static_cast<OrderId>(-1);
        }
    }

    void cancel_order(const std::string& _ticker, OrderId order_id, bool* result_ptr, UserId user_id = 1)
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            
            std::size_t engine_id = stock_exchange_.at(_ticker).engine_id_;
            std::size_t worker_id = engine_id % num_workers_;
            auto& arena = worker_arenas_[worker_id];
            
            auto args_idx = arena.emplace(OrderJobArgs{
                &stock_exchange_.at(_ticker).engine_, OrderSide::BID, OrderType::LIMIT,
                0, 0, order_id, user_id, nullptr, result_ptr
            });
            
            // Check if arena is full
            if (args_idx == static_cast<Arena<OrderJobArgs>::Index>(-1))
                throw std::runtime_error("Arena overflow - increase batch_size or arena capacity");
            
            auto arena_ptr = &arena;
            
            // Capture this for user_orders_ access and ticker for cleanup
            auto* runtime = this;
            std::string ticker_copy = _ticker;
            
            Job job(
                [arena_ptr, args_idx, runtime, ticker_copy]() {
                    auto* params = &(*arena_ptr)[args_idx];
                    *(params->result_bool) = params->engine->cancel_order(params->order_id);
                    
                    // Remove order from tracking if cancel was successful
                    if (*(params->result_bool))
                    {
                        runtime->user_orders_[params->user_id][ticker_copy].erase(params->order_id);
                    }
                },
                [arena_ptr, args_idx]() {
                    arena_ptr->free(args_idx);
                },
                engine_id
            );
            
            scheduler_.submit_job(std::move(job));
            
            // Auto-execute batch if batch_size is set and reached
            if (batch_size_ > 0)
            {
                batch_counter_++;
                if (batch_counter_ >= batch_size_)
                {
                    execute_batch();
                }
            }
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Cancel Order Error: " << e.what() << '\n';
            if (result_ptr) *result_ptr = false;
        }
    }

    void edit_order(const std::string& _ticker, OrderId order_id, OrderSide _side, Price _price, Quantity _qty, OrderId* result_id_ptr)
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            
            std::size_t engine_id = stock_exchange_.at(_ticker).engine_id_;
            std::size_t worker_id = engine_id % num_workers_;
            auto& arena = worker_arenas_[worker_id];
            
            auto args_idx = arena.emplace(OrderJobArgs{
                &stock_exchange_.at(_ticker).engine_, _side, OrderType::LIMIT,
                _price, _qty, order_id, 0, result_id_ptr, nullptr
            });
            
            // Check if arena is full
            if (args_idx == static_cast<Arena<OrderJobArgs>::Index>(-1))
                throw std::runtime_error("Arena overflow - increase batch_size or arena capacity");
            
            auto arena_ptr = &arena;
            
            Job job(
                [arena_ptr, args_idx]() {
                    auto* params = &(*arena_ptr)[args_idx];
                    *(params->result_id) = params->engine->edit_order(params->order_id, params->side, params->price, params->qty);
                },
                [arena_ptr, args_idx]() {
                    arena_ptr->free(args_idx);
                },
                engine_id
            );
            
            scheduler_.submit_job(std::move(job));
            
            // Auto-execute batch if batch_size is set and reached
            if (batch_size_ > 0)
            {
                batch_counter_++;
                if (batch_counter_ >= batch_size_)
                {
                    execute_batch();
                }
            }
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Edit Order Error: " << e.what() << '\n';
            if (result_id_ptr) *result_id_ptr = static_cast<OrderId>(-1);
        }
    }

    const OrderInfo* get_order(const std::string& _ticker, OrderId order_id) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            auto order = stock_exchange_.at(_ticker).engine_.get_order(order_id);
            // If no Order then error
            if (!order)
                throw std::runtime_error("Failed to Get Order");
            return order;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Order Error: " << e.what() << '\n';
            return nullptr;
        }
    }


    Price get_market_price(const std::string& _ticker) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            
            return stock_exchange_.at(_ticker).engine_.get_market_price();
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Market Price Error: " << e.what() << '\n';
            return -1;
        }
    }

    Price get_best_bid(const std::string& _ticker) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            auto best_bid = stock_exchange_.at(_ticker).engine_.get_best_bid();
            // If no best bid then error
            if (best_bid == -1)
                throw std::runtime_error("Bid Side is Empty");
            return best_bid;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Best Bid Error: " << e.what() << '\n';
            return -1;
        }
    }

    Price get_best_ask(const std::string& _ticker) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            
            auto best_ask = stock_exchange_.at(_ticker).engine_.get_best_ask();
            // If no best ask then error
            if (best_ask == -1)
                throw std::runtime_error("Ask Side is Empty");
            return best_ask;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Best Ask Error: " << e.what() << '\n';
            return -1;
        }
    }

    std::vector<OrderInfo> get_orders_by_status(const std::string& _ticker, OrderStatus status) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            return stock_exchange_.at(_ticker).engine_.get_orders_by_status(status);
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Orders By Status Error: " << e.what() << '\n';
            return {};
        }
    }

    std::vector<std::pair<Price, Quantity>> get_market_depth(const std::string& _ticker, OrderSide _side, std::size_t depth = 10) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            return stock_exchange_.at(_ticker).engine_.get_market_depth(_side, depth);
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Market Depth Error: " << e.what() << '\n';
            return {};
        }
    }

    std::vector<std::string> get_tradable_tickers() const
    {
        std::vector<std::string> tickers;
        // Itterate Through All Stocks in Exchange
        for (auto& stock: stock_exchange_)
            tickers.push_back(stock.first);
        return std::move(tickers);
    }
    
    OrderEngine* get_engine(const std::string& _ticker)
    {
        try
        {
             // If ticker is not in Exchange then error
            if (stock_exchange_.find(_ticker) == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            return &stock_exchange_.at(_ticker).engine_;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Engine Error: " << e.what() << '\n';
            return nullptr;
        }
    }

    // Set auto-matching for a specific stock
    bool set_auto_match(const std::string& _ticker, bool auto_match)
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            stock_exchange_.at(_ticker).engine_.set_auto_match(auto_match);
            return true;
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Set Auto Match Error: " << e.what() << '\n';
            return false;
        }
    }

    // Get auto-matching status for a specific stock
    bool get_auto_match(const std::string& _ticker) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            return stock_exchange_.at(_ticker).engine_.get_auto_match();
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Auto Match Error: " << e.what() << '\n';
            return false;
        }
    }
        
    // Execute all submitted jobs in the batch
    void execute_batch()
    {
        if (blocking_mode_)
        {
            scheduler_.process_jobs();  // Blocking: waits for completion
        }
        else
        {
            scheduler_.process_jobs_async();  // Non-blocking: just signals workers
        }
        batch_counter_ = 0;  // Reset counter after execution
        // Note: Arena slots are automatically freed by cleanup callbacks
    }
    
    // Wait for all pending jobs to complete (use in non-blocking mode)
    void wait_for_jobs()
    {
        scheduler_.wait_for_completion();
    }
    
    // Check if all jobs are completed (non-blocking check)
    bool jobs_completed() const
    {
        return scheduler_.is_complete();
    }
    
    // Check if a specific stock's jobs are completed (by ticker)
    bool stock_completed(const std::string& _ticker) const
    {
        try
        {
            if (stock_exchange_.find(_ticker) == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            
            std::size_t engine_id = stock_exchange_.at(_ticker).engine_id_;
            std::size_t worker_id = engine_id % num_workers_;
            return scheduler_.is_worker_complete(worker_id);
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Stock Completed Check Error: " << e.what() << '\n';
            return false;
        }
    }
    
    // Set blocking mode (true = wait for completion, false = async)
    void set_blocking_mode(bool blocking)
    {
        blocking_mode_ = blocking;
    }
    
    // Get user's order IDs for a specific ticker
    std::vector<OrderId> get_positions(UserId user_id, const std::string& ticker) const noexcept
    {
        auto user_it = user_orders_.find(user_id);
        if (user_it == user_orders_.end())
            return {};
        
        auto ticker_it = user_it->second.find(ticker);
        if (ticker_it == user_it->second.end())
            return {};
        
        // Return all order IDs for this user and ticker
        return std::vector<OrderId>(ticker_it->second.begin(), ticker_it->second.end());
    }
    
    // Check if user has sufficient shares to sell
    bool has_sufficient_shares(UserId user_id, const std::string& ticker, Quantity qty) const noexcept
    {
        auto user_it = user_orders_.find(user_id);
        if (user_it == user_orders_.end())
            return false;
        
        auto ticker_it = user_it->second.find(ticker);
        if (ticker_it == user_it->second.end())
            return false;
        
        // Find the engine for this ticker
        auto engine_it = stock_exchange_.find(ticker);
        if (engine_it == stock_exchange_.end())
            return false;
        
        // Sum quantities from all OPEN ASK orders (shares available to sell)
        Quantity total = 0;
        const OrderEngine& engine = engine_it->second.engine_;
        for (OrderId order_id : ticker_it->second)
        {
            const OrderInfo* order = engine.get_order(order_id);
            if (order && order->status_ == OrderStatus::OPEN && order->side_ == OrderSide::ASK)
                total += order->qty_;
        }
        
        return total >= qty;
    }

    // Get blocking mode
    bool get_blocking_mode() const { return blocking_mode_; }
    
    // Set batch size for auto-execution (0 = manual batching only)
    void set_batch_size(std::size_t batch_size)
    {
        batch_size_ = batch_size;
        batch_counter_ = 0;
    }
    
    // Get current batch size
    std::size_t get_batch_size() const { return batch_size_; }

private:
    EngineMap stock_exchange_;  // Maps ticker -> OrderEngineInfo (contains engine, ipo_shares, engine_id)
    JobScheduler scheduler_;
    std::vector<ArgsArena> worker_arenas_;  // One arena per worker thread
    std::size_t num_workers_;  // Number of worker threads
    std::size_t default_capacity_; // Default capacity for new OrderEngines
    std::size_t batch_size_;  // Auto-execute batch after this many jobs (0 = manual only)
    std::size_t batch_counter_;  // Counter for auto-batching
    std::size_t next_engine_id_;  // Counter for assigning engine IDs
    bool verbose_; // Verbose Mode
    bool blocking_mode_;  // True = wait for completion, False = async
    
    // Order ownership tracking: user_orders_[user_id][ticker] = {order_ids}
    UserOrderMap user_orders_;
    
    // Private constructor for singleton
    EngineRuntime(std::size_t num_threads, std::size_t default_capacity, std::size_t batch_size, bool _verbose, bool blocking)
    : scheduler_(num_threads),
      num_workers_(num_threads),
      default_capacity_(default_capacity),
      verbose_(_verbose),
      batch_size_(batch_size > 0 ? batch_size : default_capacity),
      batch_counter_(0),
      next_engine_id_(0),
      blocking_mode_(blocking)
    {
        // Arena of Args per worker
        std::size_t arena_capacity_per_worker = batch_size_;
        
        for (std::size_t i = 0; i < num_threads; ++i)
            worker_arenas_.emplace_back(arena_capacity_per_worker);
    }
};

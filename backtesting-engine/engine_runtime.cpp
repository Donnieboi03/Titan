#pragma once
#include "order_engine.cpp"
#include "job_scheduler.cpp"
#include "../tools/arena.cpp"
#include <unordered_set>

using UserId = std::uint32_t;
constexpr UserId IPO_HOLDER = 0;  // IPO holder owns all initial shares

using EngineId = std::uint32_t;
struct OrderEngineInfo
{
    OrderEngine engine_;  // Engine Object
    Quantity ipo_shares_; // Intial IPO
    EngineId engine_id_; // Id for Engine
    WorkerId worker_id_; // Id for Worker
    std::size_t batch_counter_; // Per-engine auto-batching counter
    
    // Constructor for in-place construction
    OrderEngineInfo(const std::string& ticker, std::size_t capacity, bool verbose, 
        Quantity ipo_shares, EngineId engine_id, WorkerId worker_id)
    :engine_(ticker, capacity, verbose, true),  // auto_match = true
    ipo_shares_(ipo_shares),
    engine_id_(engine_id),
    worker_id_(worker_id),
    batch_counter_(0)
    {}
};

enum class RequestStatus : std::uint8_t
{
    Pending,    // Request has been created but not yet submitted
    InProgress,     // Request is currently being processed by a worker
    Completed,      // Request finished successfully and result is available
    Failed,         // Request finished but encountered an error
    Cancelled       // Request was cancelled before completion
};

// Different Request Types
enum class ResultKind : std::uint8_t 
{
    None,
    OrderId,
    Price,
    Bool,
};

struct RequestRecord 
{
    std::atomic<bool> ready;
    ResultKind kind;       
    RequestStatus status; 

    union { // Union Types
        OrderId   order_id;    
        Price     price;       
        bool      ok;          
    } result;
};

using RequestId = std::uint32_t;
using RequestArena = Arena<RequestRecord>;
using RequestMap = std::unordered_map<RequestId, RequestArena::Index>;

template <typename T>
class RequestHandle 
{
public:
    bool ready() const noexcept
    {
        return record_->ready.load(std::memory_order_acquire);
    }

    const T& value() const noexcept
    {
        // caller responsibility: check ready()
        return record_->result;
    }

private:
    friend class EngineRuntime;
    explicit RequestHandle(RequestRecord<T>* rec)
        : record_(rec) {}

    RequestRecord<T>* record_;
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
    static EngineRuntime& get_instance(std::size_t num_threads = 1, std::size_t default_capacity = 32768, std::size_t batch_size = 0, bool _verbose = true, bool blocking = true)
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
            
            // Assign engine ID for job routing
            EngineId engine_id = next_engine_id_++;

            // Construct OrderEngineInfo directly in the map
            auto [it, inserted] = stock_exchange_.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(_ticker),
                std::forward_as_tuple(_ticker, engine_capacity, verbose_, _ipo_qty, 
                                      engine_id, engine_id % num_workers_)
            );
            
            // Place initial sell at IPO Price and IPO Quantity (from IPO holder)
            OrderId ipo_order = it->second.engine_.place_order(OrderSide::ASK, OrderType::LIMIT, _ipo_price, _ipo_qty);
            
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
            // Find the stock once and reuse the iterator
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");

            auto& engine_info = it->second;

            // Wait for worker to finish batch
            scheduler_.process_jobs_on(engine_info.worker_id_);
            // Erase stock from exchange
            stock_exchange_.erase(_ticker);
            
            // Erase all user orders for this ticker
            for (auto& [user_id, user_tickers] : user_orders_)
                user_tickers.erase(_ticker);
            
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
            scheduler_.process_jobs(); // Wait for pending jobs
            stock_exchange_.clear(); // Clear Stocks
            user_orders_.clear(); // Clear User Orders
            
            // Reset counters
            next_engine_id_ = 0;
            
            // Clear arenas (free all allocated slots)
            for (auto& arena : worker_arenas_)
                arena.reset();
            
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
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
            
            auto& engine_info = it->second;
            auto& arena = worker_arenas_[engine_info.worker_id_];
            OrderEngine* engine_ptr = &it->second.engine_;

            auto args_idx = arena.emplace(OrderJobArgs{
                engine_ptr, _side, OrderType::LIMIT,
                _price, _qty, 0, user_id, result_id_ptr, nullptr
            });
            
            // Check if arena is full BEFORE accessing arena[args_idx]
            if (args_idx == static_cast<Arena<OrderJobArgs>::Index>(-1))
                throw std::runtime_error("Arena overflow - increase batch_size or arena capacity");
            
            // Capture arena pointer and args_idx - no heap allocation needed!
            auto arena_ptr = &arena;
            
            // Capture this for user_orders_ access
            auto* runtime = this;
            
            Job job(
                // Execute lambda
                [arena_ptr, args_idx, runtime, _ticker]() {
                    auto* params = &(*arena_ptr)[args_idx];
                    OrderId order_id = params->engine->place_order(params->side, params->type, params->price, params->qty);
                    *(params->result_id) = order_id;
                    
                    // Track order ownership
                    if (order_id != static_cast<OrderId>(-1))
                        runtime->user_orders_[params->user_id][_ticker].insert(order_id);
                },
                // Cleanup lambda
                [arena_ptr, args_idx]() {
                    arena_ptr->free(args_idx);
                },
                engine_info.engine_id_
            );
            
            scheduler_.submit_job(std::move(job));
            
            // Increment per-engine batch counter and auto-execute batch if needed
            engine_info.batch_counter_ += 1;
            if (engine_info.batch_counter_ >= batch_size_)
            {
                execute_batch(engine_info.worker_id_);
                engine_info.batch_counter_ = 0;
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
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
            
            auto& engine_info = it->second;
            auto& arena = worker_arenas_[engine_info.worker_id_];
            OrderEngine* engine_ptr = &it->second.engine_;

            auto args_idx = arena.emplace(OrderJobArgs{
                engine_ptr, _side, OrderType::MARKET,
                -1, _qty, 0, user_id, result_id_ptr, nullptr
            });
            
            // Check if arena is full
            if (args_idx == static_cast<Arena<OrderJobArgs>::Index>(-1))
                throw std::runtime_error("Arena overflow - increase batch_size or arena capacity");
            
            auto arena_ptr = &arena;
            
            // Capture this for user_orders_ access
            auto* runtime = this;
            
            Job job(
                [arena_ptr, args_idx, runtime, _ticker]() {
                    auto* params = &(*arena_ptr)[args_idx];
                    OrderId order_id = params->engine->place_order(params->side, params->type, params->price, params->qty);
                    *(params->result_id) = order_id;
                    
                    // Track order ownership
                    if (order_id != static_cast<OrderId>(-1))
                        runtime->user_orders_[params->user_id][_ticker].insert(order_id);
                },
                [arena_ptr, args_idx]() {
                    arena_ptr->free(args_idx);
                },
                engine_info.engine_id_
            );
            
            scheduler_.submit_job(std::move(job));
            
            // Increment per-engine batch counter and auto-execute batch if needed
            engine_info.batch_counter_ += 1;
            if (engine_info.batch_counter_ >= batch_size_)
            {
                execute_batch(engine_info.worker_id_);
                engine_info.batch_counter_ = 0;
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            auto& engine_info = it->second;
            auto& arena = worker_arenas_[engine_info.worker_id_];
            OrderEngine* engine_ptr = &it->second.engine_;

            auto args_idx = arena.emplace(OrderJobArgs{
                engine_ptr, OrderSide::BID, OrderType::LIMIT,
                0, 0, order_id, user_id, nullptr, result_ptr
            });
            
            // Check if arena is full
            if (args_idx == static_cast<Arena<OrderJobArgs>::Index>(-1))
                throw std::runtime_error("Arena overflow - increase batch_size or arena capacity");
            
            auto arena_ptr = &arena;
            
            // Capture this for user_orders_ access and ticker for cleanup
            auto* runtime = this;
            
            Job job(
                [arena_ptr, args_idx, runtime, _ticker]() {
                    auto* params = &(*arena_ptr)[args_idx];
                    *(params->result_bool) = params->engine->cancel_order(params->order_id);
                    
                    // Remove order from tracking if cancel was successful
                    if (*(params->result_bool))
                    {
                        runtime->user_orders_[params->user_id][_ticker].erase(params->order_id);
                    }
                },
                [arena_ptr, args_idx]() {
                    arena_ptr->free(args_idx);
                },
                engine_info.engine_id_
            );
            
            scheduler_.submit_job(std::move(job));
            
            // Increment per-engine batch counter and auto-execute batch if needed
            engine_info.batch_counter_ += 1;
            if (engine_info.batch_counter_ >= batch_size_)
            {
                execute_batch(engine_info.worker_id_);
                engine_info.batch_counter_ = 0;
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            auto& engine_info = it->second;
            auto& arena = worker_arenas_[engine_info.worker_id_];
            OrderEngine* engine_ptr = &it->second.engine_;

            auto args_idx = arena.emplace(OrderJobArgs{
                engine_ptr, _side, OrderType::LIMIT,
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
                engine_info.engine_id_
            );
            
            scheduler_.submit_job(std::move(job));
            engine_info.batch_counter_ += 1;

            // Auto-execute batch if batch_size is set and reached            
            if (engine_info.batch_counter_ >= batch_size_)
            {
                execute_batch(engine_info.worker_id_);
                engine_info.batch_counter_ = 0;
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            const auto& order = it->second.engine_.get_order(order_id);
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            return it->second.engine_.get_market_price();
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            auto best_bid = it->second.engine_.get_best_bid();
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");

            auto best_ask = it->second.engine_.get_best_ask();
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

    std::unordered_set<OrderId> get_orders_by_status(const std::string& _ticker, OrderStatus status) const
    {
        try
        {
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            return it->second.engine_.get_orders_by_status(status);
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Orders By Status Error: " << e.what() << '\n';
            static const std::unordered_set<OrderId> empty_orders;
            return empty_orders;
        }
    }

    std::vector<std::pair<Price, Quantity>> get_market_depth(const std::string& _ticker, OrderSide _side, std::size_t depth = 10) const
    {
        try
        {
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end()) 
                throw std::runtime_error("Stock Does Not Exist");
            return it->second.engine_.get_market_depth(_side, depth);
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Market Depth Error: " << e.what() << '\n';
            static const std::vector<std::pair<Price, Quantity>> empty_depth;
            return empty_depth;
        }
    }

    std::vector<std::string> list_tickers() const noexcept
    {
        std::vector<std::string> tickers;
        // Iterate Through All Stocks in Exchange
        for (const auto& stock: stock_exchange_)
            tickers.push_back(stock.first);
        return tickers;
    }
    
    const OrderEngine* get_engine(const std::string& _ticker)
    {
        try
        {
             // If ticker is not in Exchange then error
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            return &it->second.engine_;
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            it->second.engine_.set_auto_match(auto_match);
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
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
            return it->second.engine_.get_auto_match();
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Get Auto Match Error: " << e.what() << '\n';
            return false;
        }
    }
        
    // Execute all submitted jobs in the batch
    void execute_batch() noexcept
    {
        if (blocking_mode_)
        {
            scheduler_.process_jobs();  // Blocking: waits for completion
        }
        else
        {
            scheduler_.process_jobs_async();  // Non-blocking: just signals workers
        }
        // Centralized reset: clear per-engine batch counters for all engines
        for (auto& kv : stock_exchange_)
            kv.second.batch_counter_ = 0;
    }

    // Execute all submitted jobs in the batch
    void execute_batch(WorkerId worker_id) noexcept
    {
        if (blocking_mode_)
        {
            scheduler_.process_jobs_on(worker_id);  // Blocking: waits for completion
        }
        else
        {
            scheduler_.process_jobs_on_async(worker_id);  // Non-blocking: just signals workers
        }
        // Centralized reset: clear per-engine batch counters for engines on this worker
        for (auto& kv : stock_exchange_)
            if (kv.second.worker_id_ == worker_id)
                kv.second.batch_counter_ = 0;
    }
    
    // Check if all jobs are completed (non-blocking check)
    bool all_jobs_completed() const noexcept { return scheduler_.is_complete(); }
    
    // Check if a specific stock's jobs are completed (by ticker)
    bool is_engine_completed(const std::string& _ticker) const
    {
        try
        {
            auto it = stock_exchange_.find(_ticker);
            if (it == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");

            std::size_t engine_id = it->second.engine_id_;
            std::size_t worker_id = engine_id % num_workers_;
            return scheduler_.is_worker_complete(worker_id);
        }
        catch(const std::exception& e)
        {
            if (verbose_)
                std::cerr << "Is Engine Completed Error: " << e.what() << '\n';
            return false;
        }
    }
    
    // Set blocking mode (true = wait for completion, false = async)
    void set_blocking_mode(bool blocking) noexcept
    {
        blocking_mode_ = blocking;
    }
    
    // Get user's order IDs for a specific ticker
    std::vector<OrderId> get_positions(UserId user_id, const std::string& ticker) const
    {
        try
        {
            auto user_it = user_orders_.find(user_id);
            if (user_it == user_orders_.end())
                throw std::runtime_error("User Does Not Exist");
            
            auto ticker_it = user_it->second.find(ticker);
            if (ticker_it == user_it->second.end())
                throw std::runtime_error("Stock Does Not Exist");
            
            // Return all order IDs for this user and ticker
            return std::vector<OrderId>(ticker_it->second.begin(), ticker_it->second.end());
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
            return {};
        }
    }
    
    // Check if user has sufficient shares to sell
    bool has_sufficient_shares(UserId user_id, const std::string& ticker, Quantity qty) const
    {
        try
        {
            auto user_it = user_orders_.find(user_id);
            if (user_it == user_orders_.end())
                throw std::runtime_error("User Does Not Exist");
              
            auto engine_it = stock_exchange_.find(ticker);
            if (engine_it == stock_exchange_.end())
                throw std::runtime_error("Stock Does Not Exist");
                
            auto ticker_it = user_it->second.find(ticker);
            if (ticker_it == user_it->second.end())
                throw std::runtime_error("User Does Not Own Stock");
            
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
        catch(const std::exception& e)
        {
            std::cerr << "Has Sufficient Shares Error: " << e.what() << '\n';
            return false;
        }
        
    }

    // Get blocking mode
    bool get_blocking_mode() const noexcept { return blocking_mode_; }
    
    // Set batch size for auto-execution (0 = manual batching only)
    void set_batch_size(std::size_t batch_size) noexcept
    {
        batch_size_ = batch_size;
        // Reset per-engine batch counters
        for (auto& kv : stock_exchange_)
            kv.second.batch_counter_ = 0;
    }
    
    // Get current batch size
    std::size_t get_batch_size() const noexcept { return batch_size_; }

private:
    EngineMap stock_exchange_;  // Maps ticker -> OrderEngineInfo (contains engine, ipo_shares, engine_id)
    JobScheduler scheduler_;
    std::vector<ArgsArena> worker_arenas_;  // One arena per worker thread
    std::size_t num_workers_;  // Number of worker threads
    std::size_t default_capacity_; // Default capacity for new OrderEngines
    std::size_t batch_size_;  // Auto-execute batch after this many jobs (0 = manual only)
    EngineId next_engine_id_;  // Counter for assigning engine IDs
    bool verbose_; // Verbose Mode
    bool blocking_mode_;  // True = wait for completion, False = async
    
    // Order ownership tracking: user_orders_[user_id][ticker] = {order_ids}
    UserOrderMap user_orders_;
    
    // Private constructor for singleton
    EngineRuntime(std::size_t num_threads, std::size_t default_capacity, std::size_t batch_size, bool _verbose, bool blocking)
    : num_workers_(num_threads),
      default_capacity_(default_capacity),
      verbose_(_verbose),
      batch_size_(batch_size > 0 ? batch_size : default_capacity),
      scheduler_(num_threads, batch_size),
      next_engine_id_(0),
      blocking_mode_(blocking)
    {
        // Arena of Args per worker
        std::size_t arena_capacity_per_worker = batch_size_;
        
        for (std::size_t i = 0; i < num_threads; ++i)
            worker_arenas_.emplace_back(arena_capacity_per_worker);
    }
};

#ifndef ENGINE_RUNTIME_H
#define ENGINE_RUNTIME_H

#include "engine_runtime_types.h"
#include "tools/job_scheduler.h"
#include <iostream>
#include <iomanip>
#include <tuple>
#include <atomic>
#include <condition_variable>


namespace backtest
{
    namespace user { class User; }
    namespace runtime
    {
        class EngineRuntime;
        struct UserAPI
        {
            explicit UserAPI(EngineRuntime* r = nullptr) : runtime_(r) {}
            engine::OrderId submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty, user::UserId user_id);
            engine::OrderId submit_market_order(const std::string& ticker, engine::OrderSide side, double qty, user::UserId user_id);
            bool submit_cancel_order(const std::string& ticker, engine::OrderId order_id, user::UserId user_id);
            bool submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id);
            void apply_fill_to_user(user::User* u, const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price);
            void reserve_on_accept_to_user(user::User* u, const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price);
            void release_reservation_for_user(user::User* u, engine::OrderId order_id);
            void setup_user_reservations(user::User* u, std::size_t reserve_size);
        private:
            EngineRuntime* runtime_;
            friend class EngineRuntime;
        };
    }

    namespace user
    {
        class User
        {
            friend class backtest::runtime::UserAPI;
        public:
            User()
            : user_id_(INVALID_USER_ID), runtime_(nullptr), sync_order_api_(nullptr), capital_(0.0), realized_pnl_(0.0), total_volume_(0.0)
            {}

            User(
                backtest::user::Strategy&& strategy,
                backtest::runtime::EngineRuntime* runtime,
                UserId user_id,
                double initial_capital = 100000.0,
                backtest::runtime::UserAPI* sync_api = nullptr
            )
            : strategy_(std::move(strategy)), runtime_(runtime), user_id_(user_id), sync_order_api_(sync_api), capital_(initial_capital), realized_pnl_(0.0), total_volume_(0.0)
            {}

            void on_book_update()
            {
                if (strategy_) strategy_(this);
            }

            // Order Submissions (sync when registered; returns order ID or engine::INVALID_ORDER_ID)
            engine::OrderId submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double quantity);
            engine::OrderId submit_market_order(const std::string& ticker, engine::OrderSide side, double quantity);
            bool submit_cancel_order(const std::string& ticker, engine::OrderId order_id);
            bool submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_quantity);

            // Market Metrics (sync)
            inline double get_best_bid(const std::string& ticker) const;
            inline double get_best_ask(const std::string& ticker) const;
            inline double get_market_price(const std::string& ticker) const;
            inline std::vector<std::pair<double,double>> get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth) const;
            inline std::vector<std::string> list_tickers() const;

            // User Managment
            inline std::vector<engine::OrderId> get_positions(const std::string& ticker) const;  // Returns all order IDs (including freed slots)
            inline std::vector<engine::OrderId> get_active_orders(const std::string& ticker) const;  // Returns only currently active orders (recommended)
            inline bool has_sufficient_shares(const std::string& ticker, engine::Quantity qty) const;
            inline double get_position(const std::string& ticker) const;
            inline double get_committed_sell_qty(const std::string& ticker) const;
            inline const std::unordered_map<std::string,double>& get_all_positions() const;
            inline double get_unrealized_pnl(const std::string& ticker, double current_price) const;
            inline const engine::OrderInfo* get_order_info(const std::string& ticker, engine::OrderId order_id) const;
            UserId get_user_id() const { return user_id_; }
            double get_capital() const { return capital_; }
            double get_realized_pnl() const { return realized_pnl_; }
            double get_total_volume() const { return total_volume_; }

        private:
            // Internals
            inline void update_position(const std::string& ticker, double qty, double price);
            inline void update_realized_pnl(double pnl);
            void reserve_on_accept(const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price);
            void release_reservation(engine::OrderId order_id);
            void apply_fill(const std::string& ticker, engine::OrderId order_id, engine::OrderSide side, double qty, double price);

            Strategy strategy_;
            backtest::runtime::EngineRuntime* runtime_;
            UserId user_id_;
            backtest::runtime::UserAPI* sync_order_api_;

            double capital_;
            double realized_pnl_;
            double total_volume_;

            std::unordered_map<std::string,double> positions_;
            std::unordered_map<std::string,double> avg_prices_;
            std::unordered_map<std::string,double> committed_sell_qty_; // O(1) ASK ownership check

            std::vector<double> reserved_cash_;
            std::vector<double> reserved_qty_;
            std::vector<engine::OrderSide> reserve_side_;
            std::vector<std::string> reserved_ticker_;
        };
    }

    namespace runtime
    {
        // EngineRuntime class declaration
        class EngineRuntime
        {
            friend struct UserAPI;
        public:
            // Delete copy constructor and assignment operator
            EngineRuntime(const EngineRuntime&) = delete;
            EngineRuntime& operator=(const EngineRuntime&) = delete;
            
            // Singleton instance accessor
            static EngineRuntime& get_instance(
                std::size_t num_threads = 1, // Num of Threads to Execute Engines
                std::size_t default_capacity = 1048576, // Default Size Created Engine 
                bool verbose = false, // Notification System ON / OFF
                std::size_t quantum_orders = CACHE_LINE // Interval Between User Strategy Executions & SnapShot Updates
            );
            
            // Reset State of instance to allow reinitialization
            static void reset_instance();
            
            // Stock registration
            bool register_stock(const std::string& ticker, double ipo_price, double ipo_qty, std::size_t capacity = 0);
            bool unregister_stock(const std::string& ticker);

            // Order submission (async by default; returns order ID or engine::INVALID_ORDER_ID; sync path returns real ID when used via User)
            engine::OrderId submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty, user::UserId user_id = user::INVALID_USER_ID);
            engine::OrderId submit_market_order(const std::string& ticker, engine::OrderSide side, double qty, user::UserId user_id = user::INVALID_USER_ID);
            bool submit_cancel_order(const std::string& ticker, engine::OrderId order_id, user::UserId user_id = user::INVALID_USER_ID);
            bool submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id = user::INVALID_USER_ID);

            // Market data queries (sync)
            double get_market_price(const std::string& ticker) const;
            double get_best_bid(const std::string& ticker) const;
            double get_best_ask(const std::string& ticker) const;
            const engine::OrderInfo* get_order(const std::string& ticker, engine::OrderId order_id) const;
            std::vector<std::pair<double, double>> get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth = 10) const;

            // Utilities
            std::vector<std::string> list_tickers() const noexcept;
            const engine::OrderEngine* get_engine(const std::string& ticker) const;
            EngineId get_engine_id(const std::string& ticker) const noexcept;

            // Control order fill notifications
            void set_notify_order(bool enable) noexcept { notify_order_.store(enable, std::memory_order_release); }
            bool get_notify_order() const noexcept { return notify_order_.load(std::memory_order_acquire); }
           
            // Control engine matching (toggle ON/OF allowing for control of books)
            bool set_auto_match(const std::string& ticker, bool auto_match);
            bool get_auto_match(const std::string& ticker) const;
            
            // Control Batching (flush threshold before submitting to scheduler)
            void set_batch_size(std::size_t batch_size) noexcept;
            std::size_t get_batch_size() const noexcept;
            
            // Quantum control (configured at startup)
            std::size_t get_quantum() const noexcept;
            
            // Batch Processing
            void process_pending_orders();
            void process_pending_orders(const std::string& ticker);
            void process_pending_orders_async();
            void process_pending_orders_async(const std::string& ticker);
            inline bool all_jobs_completed() const noexcept;

            // Simulation helper (returns: success/failure)
            // Registers stock and starts simulation asynchronously
            // Use is_simulation_running(), get_simulation_metrics() to monitor progress
            bool simulate(
                const std::string& filepath,
                const std::string& ticker,
                std::size_t target_orders = 0,
                std::size_t price_sample_size = 10,
                double shares_outstanding = 1000000.0
            );
            
            // Async simulation status methods
            bool is_simulation_running(const std::string& ticker) const;
            SimulationMetrics get_simulation_metrics(const std::string& ticker) const;
            
            // TODO: Implement Multi-market simulation
            // // Multi-market simulation (returns: total updates, total orders placed, seconds)
            // // Processes multiple markets concurrently using separate worker threads
            // std::tuple<std::size_t, std::size_t, double> simulate_multi(
            //     const std::vector<std::string>& filepaths,
            //     const std::vector<std::string>& tickers,
            //     std::size_t target_orders_per_market = 0,
            //     std::size_t price_sample_size = 10,
            //     double shares_outstanding = 1000000.0
            // );
            
            // Engine Statistics
            std::size_t get_placed_count(const std::string& ticker) const;
            std::size_t get_cancelled_count(const std::string& ticker) const;
            std::size_t get_filled_count(const std::string& ticker) const;
            std::size_t get_open_count(const std::string& ticker) const;
            
            // Engine Diagnostics
            std::size_t get_capacity(const std::string& ticker) const;
            std::size_t get_utilization(const std::string& ticker) const;
            std::size_t get_pending_count(const std::string& ticker) const;
            bool order_exists(const std::string& ticker, engine::OrderId order_id) const;
            
            // Strategy Registration
            user::User* register_strategy(user::Strategy strategy, double starting_capital = 100000.0);
            bool unregister_strategy(user::UserId user_id);

            // Helper methods for User class (User delegates to these via friend access)
            std::vector<engine::OrderId> user_get_positions(user::UserId user_id, const std::string& ticker) const;
            std::vector<engine::OrderId> user_get_active_orders(user::UserId user_id, const std::string& ticker) const;
            bool user_has_sufficient_shares(user::UserId user_id, const std::string& ticker, engine::Quantity qty) const;

        private:
            // Private constructor for singleton
            EngineRuntime(std::size_t num_threads, std::size_t default_capacity, bool verbose, std::size_t quantum_orders);
            ~EngineRuntime();
            
            static inline bool instance_initialized_ = false; // Track if instance is created
            
             // Notification management
            void start_notification_thread() noexcept;
            void stop_notification_thread() noexcept;
            void notification_loop() noexcept;
            void notify(const std::string& message) noexcept;
                        
            // Snapshot management
            void update_snapshot_internal(EngineId engine_id) const noexcept;
            const engine::MarketSnapshot* get_snapshot_fast(EngineId engine_id) const noexcept;
            
            // Per-Engine Counter (for snapshot updates and strategy executions)
            void increment_order_counter(EngineId engine_id) noexcept;
                        
            // Internal wrapper to submit jobs to a worker while enforcing runtime batch-size
            void submit_job_on_worker(scheduler::WorkerId worker_id, scheduler::Job&& job) noexcept;
            
            // Internal order submission implementations (async returns INVALID_ORDER_ID; sync returns order_id or INVALID_ORDER_ID)
            engine::OrderId submit_limit_order_async_impl(const std::string& ticker, engine::OrderSide side, double price, double qty, user::UserId user_id);
            engine::OrderId submit_limit_order_sync_impl(const std::string& ticker, engine::OrderSide side, double price, double qty, user::UserId user_id);
            engine::OrderId submit_market_order_async_impl(const std::string& ticker, engine::OrderSide side, double qty, user::UserId user_id);
            engine::OrderId submit_market_order_sync_impl(const std::string& ticker, engine::OrderSide side, double qty, user::UserId user_id);
            bool submit_cancel_order_async_impl(const std::string& ticker, engine::OrderId order_id, user::UserId user_id);
            bool submit_cancel_order_sync_impl(const std::string& ticker, engine::OrderId order_id, user::UserId user_id);
            bool submit_edit_order_async_impl(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id);
            bool submit_edit_order_sync_impl(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id);
            
            // Helper functions for common operations
            void notify_order_event(const std::string& prefix, engine::OrderId order_id, engine::EventKind event_kind) noexcept;
            void track_user_order(engine::OrderId order_id, user::UserId user_id, EngineId engine_id) noexcept;
            void handle_accept_event(engine::OrderId order_id, user::UserId user_id, EngineId engine_id, 
                                     engine::OrderSide side, engine::Quantity qty_ticks, engine::Price price_ticks) noexcept;
            void handle_fill_event(const engine::EngineMsg& msg, EngineId engine_id) noexcept;

            mutable EngineMap engines_info_;  // Maps engine_id -> OrderEngineInfo (mutable for lazy snapshot updates)
            TickerMap ticker_to_engine_id_; // Maps ticker -> engine_id for user-facing API
            scheduler::JobScheduler scheduler_; // Scheduler for concurrent async job execution
            std::size_t num_workers_;  // Number of worker threads
            std::size_t default_capacity_; // Default capacity for new OrderEngines
            std::atomic<std::size_t> runtime_batch_size_; // Runtime-enforced batch size for flushing jobs to workers
            bool verbose_; // Verbose Mode
            std::atomic<bool> notify_order_{false}; // Control order fill notifications
            
            // Order ownership tracking: user_orders_[user_id][engine_id] = {order_ids}
            // Index 0 reserved for IPO_HOLDER, registered users start from index 1
            UserOrderMap user_orders_;
            std::unordered_map<engine::OrderId, user::UserId> order_to_user_; // OrderId -> UserId
            // Strategy management (raw pointers, caller manages lifetime)
            std::vector<user::User> users_;
            
            // Fast snapshot access cache: store pointer to the engine so we can fetch
            std::vector<engine::OrderEngine*> snapshot_cache_;
            
            const std::size_t quantum_orders_; // Quantum in order count (immutable after construction)
            // Global quantum counter for strategy execution (shared across engines)
            alignas(CACHE_LINE) std::atomic<std::size_t> global_orders_since_quantum_{0};

            alignas(CACHE_LINE) std::atomic<bool> notification_thread_running_{false}; // Notification thread control
            std::thread notification_thread_; // Notification thread for verbose output
            // TODO: Make Lock-Free Design (NO MUTEX)
            LazyQueue<std::string> notification_buffer_; // Buffer for notification messages
            mutable std::mutex notification_mutex_; // Mutex for notification buffer
            std::condition_variable notification_cv_; // Sleep-lock for notification thread

            UserAPI sync_order_api_;
        };

        // Extract the compact slot index from an external order ID.
        // External ID format: [16-bit engine_id | 32-bit slot | 16-bit generation]
        inline std::size_t order_slot_idx(engine::OrderId external_id) noexcept
        {
            constexpr std::uint64_t INTERNAL_MASK = (1ULL << 48) - 1ULL;
            return static_cast<std::size_t>((external_id & INTERNAL_MASK) >> 16);
        }

        inline bool EngineRuntime::all_jobs_completed() const noexcept
        {
            return scheduler_.is_complete();
        }
    }

    // Inline User method implementations (must be after EngineRuntime is fully defined)
    namespace user
    {
        inline std::vector<engine::OrderId> User::get_positions(const std::string& ticker) const
        {
            return runtime_->user_get_positions(user_id_, ticker);
        }

        inline std::vector<engine::OrderId> User::get_active_orders(const std::string& ticker) const
        {
            return runtime_->user_get_active_orders(user_id_, ticker);
        }

        inline bool User::has_sufficient_shares(const std::string& ticker, engine::Quantity qty) const
        {
            return runtime_->user_has_sufficient_shares(user_id_, ticker, qty);
        }

        inline double User::get_best_bid(const std::string& ticker) const
        {
            return runtime_->get_best_bid(ticker);
        }

        inline double User::get_best_ask(const std::string& ticker) const
        {
            return runtime_->get_best_ask(ticker);
        }

        inline double User::get_market_price(const std::string& ticker) const
        {
            return runtime_->get_market_price(ticker);
        }

        inline std::vector<std::pair<double, double>> User::get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth) const
        {
            return runtime_->get_market_depth(ticker, side, depth);
        }

        inline std::vector<std::string> User::list_tickers() const
        {
            return runtime_->list_tickers();
        }

        inline const engine::OrderInfo* User::get_order_info(const std::string& ticker, engine::OrderId order_id) const
        {
            return runtime_->get_order(ticker, order_id);
        }

        inline double User::get_position(const std::string& ticker) const
        {
            auto it = positions_.find(ticker);
            return it != positions_.end() ? it->second : 0.0;
        }

        inline double User::get_committed_sell_qty(const std::string& ticker) const
        {
            auto it = committed_sell_qty_.find(ticker);
            return it != committed_sell_qty_.end() ? it->second : 0.0;
        }

        inline const std::unordered_map<std::string, double>& User::get_all_positions() const
        {
            return positions_;
        }

        inline double User::get_unrealized_pnl(const std::string& ticker, double current_price) const
        {
            auto it = positions_.find(ticker);
            if (it == positions_.end()) return 0.0;

            auto avg_it = avg_prices_.find(ticker);
            if (avg_it == avg_prices_.end()) return 0.0;

            return it->second * (current_price - avg_it->second);
        }

        inline void User::update_realized_pnl(double pnl)
        {
            realized_pnl_ += pnl;
        }

        inline void User::update_position(const std::string& ticker, double qty, double price)
        {
            double prev_pos = 0.0;
            auto it = positions_.find(ticker);
            if (it != positions_.end()) prev_pos = it->second;

            // Compute realized pnl when this fill reduces or flips exposure
            // If signs differ, some quantity is closing existing position
            double realized = 0.0;
            if (prev_pos != 0.0 && ((prev_pos > 0 && qty < 0) || (prev_pos < 0 && qty > 0))) {
                double closing_qty = std::min(std::abs(prev_pos), std::abs(qty));
                // Get previous average price (if present)
                double prev_avg = 0.0;
                auto a_it = avg_prices_.find(ticker);
                if (a_it != avg_prices_.end()) prev_avg = a_it->second;

                if (prev_pos > 0 && qty < 0) {
                    // Long being sold
                    realized = closing_qty * (price - prev_avg);
                } else if (prev_pos < 0 && qty > 0) {
                    // Short being covered
                    realized = closing_qty * (prev_avg - price);
                }
                // Update realized pnl and capital for closed portion
                realized_pnl_ += realized;
                capital_ += realized; // realize gains/losses into capital
            }

            // Update position and average price
            double new_pos = prev_pos + qty;
            positions_[ticker] = new_pos;

            if (new_pos != 0.0) {
                // Recalculate average price for remaining/net position
                double prev_avg = 0.0;
                auto a_it = avg_prices_.find(ticker);
                if (a_it != avg_prices_.end()) prev_avg = a_it->second;

                // If previous position had same sign as new trade, update weighted avg
                if ((prev_pos >= 0 && qty >= 0) || (prev_pos <= 0 && qty <= 0)) {
                    double total_qty = std::abs(prev_pos) + std::abs(qty);
                    if (total_qty > 0) {
                        avg_prices_[ticker] = (prev_avg * std::abs(prev_pos) + price * std::abs(qty)) / total_qty;
                    } else {
                        avg_prices_[ticker] = price;
                    }
                } else {
                    // If position decreased or flipped, preserve previous avg for remaining portion
                    if (std::abs(new_pos) > 0) {
                        // remaining side keeps previous avg
                        avg_prices_[ticker] = prev_avg;
                    } else {
                        // Position closed
                        avg_prices_.erase(ticker);
                    }
                }
            } else {
                // Position fully closed
                avg_prices_.erase(ticker);
            }

            // Update capital for the trade amount (buys reduce capital, sells increase)
            // NOTE: capital impact for accepted orders is handled via reservation logic.
            // Here we only update realized pnl and positions; do not deduct/add trade cash
            // because cash was reserved on ACCEPT and reconciled on fills via apply_fill().
            total_volume_ += std::abs(qty);
        }
    }
}

#endif // ENGINE_RUNTIME_H

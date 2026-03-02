#ifndef ENGINE_RUNTIME_H
#define ENGINE_RUNTIME_H

#include "engine_runtime_types.h"
#include "tools/job_scheduler.h"
#include "tools/double_buffer.h"
#include "market_data_stream.h"
#include <iostream>
#include <iomanip>
#include <tuple>
#include <atomic>


namespace backtest
{
    namespace user { class User; class UserView; }
    namespace runtime
    {
        class EngineRuntime;
        struct UserAPI
        {
            explicit UserAPI(EngineRuntime* r = nullptr) : runtime_(r) {}
            engine::OrderId submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty, user::UserId user_id);
            engine::OrderId submit_market_order(const std::string& ticker, engine::OrderSide side, double qty, user::UserId user_id);
            bool submit_cancel_order(const std::string& ticker, engine::OrderId order_id, user::UserId user_id);
            bool submit_replace_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id);
            bool submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_qty, user::UserId user_id);
            void apply_fill_to_user(user::User* u, engine::OrderId order_id, engine::OrderSide side, double qty, double price);
            void reserve_on_accept_to_user(user::User* u, engine::OrderId order_id, engine::OrderSide side, double qty, double price);
            void release_reservation_for_user(user::User* u, engine::OrderId order_id, engine::OrderSide side, double remaining_qty, double price);
            void setup_user_reservations(user::User* u, std::size_t reserve_size);

            std::vector<engine::OrderId> get_positions(user::UserId user_id, const std::string& ticker) const;
            std::vector<engine::OrderId> get_active_orders(user::UserId user_id, const std::string& ticker) const;
            bool has_sufficient_shares(user::UserId user_id, const std::string& ticker, engine::Quantity qty) const;

        private:
            EngineRuntime* runtime_;
            friend class EngineRuntime;
        };
    }

    namespace user
    {
        // Observational-only interface returned to client after registration. No submit* methods.
        class UserView
        {
        public:
            virtual ~UserView() = default;
            virtual const UserSnapshot& get_snapshot() const = 0;
            // Convenience observers delegating to snapshot (for client code using view->get_capital() etc.)
            double get_capital() const { return get_snapshot().capital; }
            double get_realized_pnl() const { return get_snapshot().realized_pnl; }
            double get_total_volume() const { return get_snapshot().total_volume; }
            UserId get_user_id() const { return get_snapshot().user_id; }
            const std::string& get_ticker() const { return get_snapshot().ticker; }
            double get_position() const { return get_snapshot().position; }
            virtual double get_committed_sell_qty() const { return 0.0; }
            virtual double get_total_reserved_cash() const { return 0.0; }
            virtual std::vector<std::pair<double, double>> get_open_bids() const { return {}; }
            virtual std::vector<std::pair<double, double>> get_open_asks() const { return {}; }
            double get_unrealized_pnl() const { return get_snapshot().unrealized_pnl; }
            std::unordered_map<std::string, double> get_all_positions() const {
                const auto& s = get_snapshot();
                if (s.ticker.empty()) return {};
                return {{ s.ticker, s.position }};
            }
        };

        class User : public UserView
        {
            friend struct backtest::runtime::UserAPI;
        public:
            User()
            : strategy_(), runtime_(nullptr), user_id_(INVALID_USER_ID), sync_order_api_(nullptr), capital_(0.0), realized_pnl_(0.0), total_volume_(0.0), strategy_engine_id_(backtest::runtime::INVALID_ENGINE_ID), position_(0.0), avg_price_(0.0)
            { published_snapshot_ptr_.store(&snapshots_[0], std::memory_order_relaxed); }

            User(
                backtest::user::Strategy&& strategy,
                backtest::runtime::EngineRuntime* runtime,
                UserId user_id,
                double initial_capital = 100000.0,
                backtest::runtime::UserAPI* sync_api = nullptr,
                backtest::runtime::EngineId strategy_engine_id = backtest::runtime::INVALID_ENGINE_ID
            )
            : strategy_(std::move(strategy)), runtime_(runtime), user_id_(user_id), sync_order_api_(sync_api), capital_(initial_capital), realized_pnl_(0.0), total_volume_(0.0), strategy_engine_id_(strategy_engine_id), position_(0.0), avg_price_(0.0)
            {
                published_snapshot_ptr_.store(&snapshots_[0], std::memory_order_relaxed);
                update_snapshot();
            }

            User(User&& other) noexcept;
            User& operator=(User&& other) noexcept;

            void on_book_update()
            {
                if (strategy_) strategy_(this);
            }

            // UserView: observational snapshot (override)
            const UserSnapshot& get_snapshot() const override;
            double get_committed_sell_qty() const override;
            double get_total_reserved_cash() const override;
            std::vector<std::pair<double, double>> get_open_bids() const override;
            std::vector<std::pair<double, double>> get_open_asks() const override;

            // Called by runtime after each strategy tick to publish state to snapshot (main-thread reads)
            void update_snapshot() noexcept;

            // Order Submissions (sync when registered; strategy bound to one ticker, no ticker param)
            engine::OrderId submit_limit_order(engine::OrderSide side, double price, double quantity);
            engine::OrderId submit_market_order(engine::OrderSide side, double quantity);
            bool submit_cancel_order(engine::OrderId order_id);
            bool submit_replace_order(engine::OrderId order_id, double new_price, double new_quantity);
            bool submit_edit_order(engine::OrderId order_id, double new_quantity);

            // Market Metrics (sync; use strategy's engine)
            inline double get_best_bid() const;
            inline double get_best_ask() const;
            inline double get_market_price() const;
            inline std::vector<std::pair<double,double>> get_market_depth(engine::OrderSide side, std::size_t depth = 10) const;
            inline std::vector<std::string> list_tickers() const;

            // User Management (strategy's engine)
            inline std::vector<engine::OrderId> get_positions() const;
            inline std::vector<engine::OrderId> get_active_orders() const;
            inline bool has_sufficient_shares(engine::Quantity qty) const;
            inline const engine::OrderInfo* get_order_info(engine::OrderId order_id) const;

        private:
            inline double calculate_unrealized_pnl(double current_price) const;
            inline void update_position(double qty, double price);
            inline void update_realized_pnl(double pnl);
            void reserve_on_accept(engine::OrderId order_id, engine::OrderSide side, double qty, double price);
            void release_reservation(engine::OrderId order_id, engine::OrderSide side, double remaining_qty, double price);
            void apply_fill(engine::OrderId order_id, engine::OrderSide side, double qty, double price);

            Strategy strategy_;
            backtest::runtime::EngineRuntime* runtime_;
            UserId user_id_;
            backtest::runtime::UserAPI* sync_order_api_;

            double capital_;
            double realized_pnl_;
            double total_volume_;

            backtest::runtime::EngineId strategy_engine_id_;
            double position_;
            double avg_price_;

            // Double-buffered snapshot for lock-free main-thread reads (mirror OrderEngine)
            UserSnapshot snapshots_[2];
            std::atomic<std::size_t> active_snapshot_index_{0};
            alignas(engine::CACHE_LINE) std::atomic<const UserSnapshot*> published_snapshot_ptr_;
        };
    }

    namespace runtime
    {
        // EngineRuntime class declaration
        class EngineRuntime
        {
            friend struct UserAPI;
            friend class user::User;
        public:
            // Delete copy constructor and assignment operator
            EngineRuntime(const EngineRuntime&) = delete;
            EngineRuntime& operator=(const EngineRuntime&) = delete;
            
            // Singleton instance accessor
            static EngineRuntime& get_instance(
                std::size_t num_threads = 1, // Num of Threads to Execute Engines
                bool verbose = false, // Notification System ON / OFF
                std::size_t quantum_orders = 4096, // Interval Between User Strategy Executions & SnapShot Updates
                std::size_t max_capacity = 1048576, // Max order pool size per engine
                std::size_t max_engine_count = 100, // Reserve space for this many stocks/engines (avoids realloc of engines_info_ / user_orders_)
                std::size_t max_strategies = 1000 // Reserve space for this many strategies/users (keeps UserView* from register_strategy valid)
            );
            
            // Reset State of instance to allow reinitialization
            static void reset_instance();
            
            // Stock registration (rvalue; use std::forward when passing strings on)
            bool register_stock(const std::string& ticker, double ipo_price, double ipo_qty, std::size_t capacity = 0);
            bool unregister_stock(const std::string& ticker);

            // Order submission (async by default; untracked only). For tracked orders use User::submit_* (sync).
            engine::OrderId submit_limit_order(const std::string& ticker, engine::OrderSide side, double price, double qty);
            engine::OrderId submit_market_order(const std::string& ticker, engine::OrderSide side, double qty);
            bool submit_cancel_order(const std::string& ticker, engine::OrderId order_id);
            bool submit_replace_order(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty);
            bool submit_edit_order(const std::string& ticker, engine::OrderId order_id, double new_qty);

            // Market data / snapshot (public; ticker only)
            const engine::MarketSnapshot* get_snapshot(const std::string& ticker) const;
            bool request_snapshot(const std::string& ticker);
            const engine::OrderInfo* get_order(const std::string& ticker, engine::OrderId order_id) const;
            std::vector<std::string> list_tickers() const noexcept;

            // Backwards-compat snapshot convenience methods
            double get_market_price(const std::string& ticker) const;
            double get_best_bid(const std::string& ticker) const;
            double get_best_ask(const std::string& ticker) const;
            std::vector<std::pair<double,double>> get_market_depth(const std::string& ticker, engine::OrderSide side, std::size_t depth = 10) const;
            bool get_auto_match(const std::string& ticker) const;

            // Utilities
            const engine::OrderEngine* get_engine(const std::string& ticker) const;
            EngineId get_engine_id(const std::string& ticker) const noexcept;

            // Control order fill notifications
            void set_notify_order(bool enable) noexcept { notify_order_.store(enable, std::memory_order_release); }
            bool get_notify_order() const noexcept { return notify_order_.load(std::memory_order_acquire); }

            // Per-ticker L2 recording (written by event management thread; hot path is lock-free)
            void set_record(const std::string& ticker, bool enable) noexcept;
            void set_record(const std::string& ticker, bool enable, const std::string& path_override) noexcept;
            bool get_record(const std::string& ticker) const noexcept;
           
            // Control engine matching (toggle ON/OF allowing for control of books)
            bool set_auto_match(const std::string& ticker, bool auto_match);
            
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
                double shares_outstanding = 1000000.0,
                const std::string& record_path = {}
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
            
            // Engine Statistics / Diagnostics
            std::size_t get_capacity(const std::string& ticker) const;
            std::size_t get_pending_count(const std::string& ticker) const;
            std::size_t get_placed_count(const std::string& ticker) const;
            std::size_t get_filled_count(const std::string& ticker) const;
            std::size_t get_cancelled_count(const std::string& ticker) const;
            std::size_t get_open_count(const std::string& ticker) const;

            // User positions by user_id (ticker only)
            std::vector<engine::OrderId> get_positions(user::UserId user_id, const std::string& ticker) const;
            std::vector<engine::OrderId> get_active_orders(user::UserId user_id, const std::string& ticker) const;

            // Strategy Registration (ticker required for deterministic per-engine quantum). Returns UserView* so client cannot call submit*.
            user::UserView* register_strategy(const std::string& ticker, user::Strategy strategy, double starting_capital = 100000.0);
            bool unregister_strategy(user::UserId user_id);

        private:
            // Private constructor for singleton
            EngineRuntime(std::size_t num_threads, bool verbose, std::size_t quantum_orders, std::size_t max_capacity, std::size_t max_engine_count, std::size_t max_strategies);
            ~EngineRuntime();
            
            static inline bool instance_initialized_ = false; // Track if instance is created
            
            // Event management thread (log + record)
            void start_event_management_thread() noexcept;
            void stop_event_management_thread() noexcept;
            void event_management_loop() noexcept;
            void notify(const std::string& message) noexcept;
            void record(EngineId engine_id, const stream::L2Update& update) noexcept;
            void record_book_snapshot(EngineId engine_id) noexcept;

            // Snapshot management
            void update_snapshot_internal(EngineId engine_id) const noexcept;
            void refresh_user_snapshots_for_engine(EngineId engine_id);
            const engine::MarketSnapshot* get_snapshot_fast(EngineId engine_id) const noexcept;
            const engine::MarketSnapshot* get_snapshot(EngineId engine_id) const;
            const std::string& get_ticker(EngineId engine_id) const;
            const engine::OrderInfo* get_order(EngineId engine_id, engine::OrderId order_id) const;
            std::vector<engine::OrderId> get_positions(user::UserId user_id, EngineId engine_id) const;
            std::vector<engine::OrderId> get_active_orders(user::UserId user_id, EngineId engine_id) const;

            // User-scoped helper (private; exposed via UserAPI::has_sufficient_shares)
            bool user_has_sufficient_shares(user::UserId user_id, const std::string& ticker, engine::Quantity qty) const;
            
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
            bool submit_replace_order_async_impl(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id);
            bool submit_replace_order_sync_impl(const std::string& ticker, engine::OrderId order_id, double new_price, double new_qty, user::UserId user_id);
            bool submit_edit_order_async_impl(const std::string& ticker, engine::OrderId order_id, double new_qty, user::UserId user_id);
            bool submit_edit_order_sync_impl(const std::string& ticker, engine::OrderId order_id, double new_qty, user::UserId user_id);
            
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
            std::size_t max_capacity_; // Max order pool size per engine
            std::atomic<std::size_t> runtime_batch_size_; // Runtime-enforced batch size for flushing jobs to workers
            bool verbose_; // Verbose Mode
            std::atomic<bool> notify_order_{false}; // Control order fill notifications
            
            // Order ownership: engine-first user_orders_[engine_id].by_user[user_id] = set; index 0 reserved for IPO_HOLDER
            UserOrderMap user_orders_;
            // Strategy management (raw pointers, caller manages lifetime)
            std::vector<user::User> users_;
            // strategy_engine_id_[idx] = engine_id for users_[idx] (parallel to users_)
            std::vector<EngineId> user_strategy_engine_id_;
            
            const std::size_t quantum_orders_; // Quantum in order count (immutable after construction)
            const std::size_t max_engine_count_; // Max engines (register_stock fails when at limit)
            const std::size_t max_strategies_; // Max strategies (for reserving user_strategy_engine_id_ / by_user, avoids resize realloc)

            alignas(engine::CACHE_LINE) std::atomic<bool> event_management_thread_running_{false}; // Event management thread control
            std::thread event_management_thread_; // Event management thread (drains log + record buffers)
            static constexpr std::size_t LOG_BUFFER_CAPACITY = 65536;
            static constexpr std::size_t RECORD_BUFFER_CAPACITY = 65536;
            static constexpr std::size_t USER_ORDERS_PER_USER_BASE_CAPACITY = 4096;
            using RecordItem = std::pair<EngineId, stream::L2Update>;
            DoubleBuffer<std::string> log_buffer_;
            DoubleBuffer<RecordItem> record_buffer_;

            // Per-engine recording state (grown in register_stock; unique_ptr for atomic non-copyability)
            std::vector<std::unique_ptr<std::atomic<bool>>> record_enabled_;
            std::vector<std::string> record_path_override_;
            std::unordered_map<EngineId, std::unique_ptr<stream::L2Stream>> record_streams_; // Lazy-open in event management thread

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
        inline std::vector<engine::OrderId> User::get_positions() const
        {
            return sync_order_api_ ? sync_order_api_->get_positions(user_id_, get_ticker()) : std::vector<engine::OrderId>{};
        }

        inline std::vector<engine::OrderId> User::get_active_orders() const
        {
            return sync_order_api_ ? sync_order_api_->get_active_orders(user_id_, get_ticker()) : std::vector<engine::OrderId>{};
        }

        inline bool User::has_sufficient_shares(engine::Quantity qty) const
        {
            return sync_order_api_ ? sync_order_api_->has_sufficient_shares(user_id_, get_ticker(), qty) : false;
        }

        inline double User::get_best_bid() const
        {
            const auto* s = runtime_ && strategy_engine_id_ != backtest::runtime::INVALID_ENGINE_ID ? runtime_->get_snapshot(strategy_engine_id_) : nullptr;
            return s && s->best_bid != static_cast<engine::Price>(-1) ? math::ticks_to_dollars(s->best_bid) : -1.0;
        }

        inline double User::get_best_ask() const
        {
            const auto* s = runtime_ && strategy_engine_id_ != backtest::runtime::INVALID_ENGINE_ID ? runtime_->get_snapshot(strategy_engine_id_) : nullptr;
            return s && s->best_ask != static_cast<engine::Price>(-1) ? math::ticks_to_dollars(s->best_ask) : -1.0;
        }

        inline double User::get_market_price() const
        {
            const auto* s = runtime_ && strategy_engine_id_ != backtest::runtime::INVALID_ENGINE_ID ? runtime_->get_snapshot(strategy_engine_id_) : nullptr;
            if (!s) return -1.0;
            if (s->best_bid != static_cast<engine::Price>(-1) && s->best_ask != static_cast<engine::Price>(-1))
                return (math::ticks_to_dollars(s->best_bid) + math::ticks_to_dollars(s->best_ask)) / 2.0;
            if (s->best_ask != static_cast<engine::Price>(-1)) return math::ticks_to_dollars(s->best_ask);
            if (s->best_bid != static_cast<engine::Price>(-1)) return math::ticks_to_dollars(s->best_bid);
            if (s->market_price != static_cast<engine::Price>(-1)) return math::ticks_to_dollars(s->market_price);
            return -1.0;
        }

        inline std::vector<std::pair<double, double>> User::get_market_depth(engine::OrderSide side, std::size_t depth) const
        {
            const auto* snap = runtime_ && strategy_engine_id_ != backtest::runtime::INVALID_ENGINE_ID ? runtime_->get_snapshot(strategy_engine_id_) : nullptr;
            std::vector<std::pair<double, double>> out;
            if (!snap) return out;
            if (side == engine::OrderSide::BID) {
                size_t n = std::min(static_cast<size_t>(snap->bid_levels), depth);
                out.reserve(n);
                for (size_t i = 0; i < n; ++i)
                    out.emplace_back(math::ticks_to_dollars(snap->bid_prices[i]), math::internal_to_qty(snap->bid_depth[i]));
            } else {
                size_t n = std::min(static_cast<size_t>(snap->ask_levels), depth);
                out.reserve(n);
                for (size_t i = 0; i < n; ++i)
                    out.emplace_back(math::ticks_to_dollars(snap->ask_prices[i]), math::internal_to_qty(snap->ask_depth[i]));
            }
            return out;
        }

        inline std::vector<std::string> User::list_tickers() const
        {
            return runtime_ ? runtime_->list_tickers() : std::vector<std::string>{};
        }

        inline const engine::OrderInfo* User::get_order_info(engine::OrderId order_id) const
        {
            return runtime_ && strategy_engine_id_ != backtest::runtime::INVALID_ENGINE_ID ? runtime_->get_order(strategy_engine_id_, order_id) : nullptr;
        }

        inline double User::calculate_unrealized_pnl(double current_price) const
        {
            return position_ * (current_price - avg_price_);
        }

        inline void User::update_realized_pnl(double pnl)
        {
            realized_pnl_ += pnl;
        }

        inline void User::update_position(double qty, double price)
        {
            double prev_pos = position_;

            double realized = 0.0;
            if (prev_pos != 0.0 && ((prev_pos > 0 && qty < 0) || (prev_pos < 0 && qty > 0))) {
                double closing_qty = std::min(std::abs(prev_pos), std::abs(qty));
                double prev_avg = avg_price_;

                if (prev_pos > 0 && qty < 0)
                    realized = closing_qty * (price - prev_avg);
                else if (prev_pos < 0 && qty > 0)
                    realized = closing_qty * (prev_avg - price);

                realized_pnl_ += realized;
                capital_ += realized;
            }

            double new_pos = prev_pos + qty;
            position_ = new_pos;

            if (new_pos != 0.0) {
                double prev_avg = avg_price_;
                if ((prev_pos >= 0 && qty >= 0) || (prev_pos <= 0 && qty <= 0)) {
                    double total_qty = std::abs(prev_pos) + std::abs(qty);
                    avg_price_ = total_qty > 0 ? (prev_avg * std::abs(prev_pos) + price * std::abs(qty)) / total_qty : price;
                } else {
                    if (std::abs(new_pos) > 0)
                        avg_price_ = prev_avg;
                    else
                        avg_price_ = 0.0;
                }
            } else {
                avg_price_ = 0.0;
            }

            total_volume_ += std::abs(qty);
        }
    }
}

#endif // ENGINE_RUNTIME_H

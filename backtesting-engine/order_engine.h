#ifndef ORDER_ENGINE_H
#define ORDER_ENGINE_H

#ifndef CACHE_LINE
#define CACHE_LINE 128
#endif

#include "engine_types.h"
#include <vector>
#include <atomic>
#include <unordered_set>
#include <random>
#include <chrono>
#include <algorithm>

namespace engine
{
    class OrderEngine
    {
    public:
        OrderEngine(std::size_t capacity = 1048576, bool verbose = true, bool auto_match = true, std::uint16_t engine_id = 0) noexcept;
        ~OrderEngine() = default;

        using OrderId = std::uint64_t; // external id (engine-prefixed)

        // Place / edit / cancel
        OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept;
        OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty) noexcept;
        bool cancel_order(OrderId id, EngineMsg& msg) noexcept;
        bool cancel_order(OrderId id) noexcept;
        OrderId edit_order(OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>& msgs) noexcept;
        OrderId edit_order(OrderId id, OrderSide side, Price price, Quantity qty) noexcept;

        // Auto-match control
        void set_auto_match(bool auto_match) noexcept;
        bool get_auto_match() const noexcept;

        // Snapshot access
        void update_snapshot() noexcept;
        const MarketSnapshot& get_snapshot() const noexcept;

        // Order lookup (returns nullptr if invalid/freed)
        const OrderInfo* get_order(OrderId id) const noexcept;

    private:
        // OrderId encoding helpers
        static constexpr OrderId ENGINE_ID_SHIFT = 48;
        static constexpr OrderId ENGINE_ID_MASK = (static_cast<OrderId>(0xFFFF) << ENGINE_ID_SHIFT);
        static constexpr OrderId INTERNAL_HANDLE_MASK = ((static_cast<OrderId>(1) << ENGINE_ID_SHIFT) - 1ULL);
        
        static inline engine::OrderId encode_external(std::uint16_t engine_id, engine::OrderId internal_handle) noexcept 
        {
            if (internal_handle == OrderMemoryPool::INVALID_HANDLE) return internal_handle;
            return (static_cast<engine::OrderId>(engine_id) << ENGINE_ID_SHIFT) | (internal_handle & INTERNAL_HANDLE_MASK);
        }

        static inline engine::OrderId decode_external(engine::OrderId external_id) noexcept 
        {
            if (external_id == OrderMemoryPool::INVALID_HANDLE) return external_id;
            return external_id & INTERNAL_HANDLE_MASK;
        }

        // Extract the engine id prefix from an external engine::OrderId
        static inline std::uint16_t extract_engine_id(engine::OrderId external_id) noexcept 
        {
            if (external_id == OrderMemoryPool::INVALID_HANDLE) return static_cast<std::uint16_t>(-1);
            return static_cast<std::uint16_t>((external_id & ENGINE_ID_MASK) >> ENGINE_ID_SHIFT);
        }

        // Private implementation methods
        OrderId place_order_impl(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>* msgs) noexcept;
        bool cancel_order_impl(OrderId id, EngineMsg* msg) noexcept;
        OrderId edit_order_impl(OrderId id, OrderSide side, Price price, Quantity qty, std::vector<EngineMsg>* msgs) noexcept;
        void update_snapshot_impl() noexcept;

        // Member variables
        OrderMemoryPool order_pool_;
        LazyQueue<std::pair<OrderId, Timestamp>> order_queue_;
        
        BidBook bid_book_;
        AskBook ask_book_;
        LevelMap bid_levels_;
        LevelMap ask_levels_;
        TopBidHeap top_bid_heap_;
        TopAskHeap top_ask_heap_;
        std::unordered_map<Price, Quantity> bid_depth_cache_;
        std::unordered_map<Price, Quantity> ask_depth_cache_;

        MarketSnapshot snapshots_[2];
        alignas(CACHE_LINE) std::atomic<const MarketSnapshot*> published_snapshot_ptr_;
        alignas(CACHE_LINE) std::atomic<std::size_t> active_snapshot_;

        alignas(CACHE_LINE) std::size_t placed_count_;
        std::size_t cancelled_count_;
        std::size_t filled_count_;
        Price last_trade_price_;
        
        std::uint16_t engine_id_;
        bool verbose_;
        bool auto_match_;
    
        // Inline hot path functions for performance
        inline void push_into_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept;
        inline void pop_from_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept;
        inline void matching_engine(OrderId recent_internal_id, OrderInfo* recent_order_ptr, std::vector<EngineMsg>* fill_notifications) noexcept;
        inline void matching(OrderId best_ask_id, OrderId best_bid_id, OrderLevel& best_ask_level, OrderLevel& best_bid_level, std::vector<EngineMsg>* fill_notifications) noexcept;
    };

    // Inline implementations (must be in header)
    inline void OrderEngine::push_into_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept
    {
        const Quantity qty = order_pool_[id].qty_;
        
        if (side == OrderSide::BID)
        {
            auto level_it = bid_levels_.find(price);
            const bool new_level = (level_it == bid_levels_.end());
            if (new_level)
            {
                bid_book_.emplace(price);
                level_it = bid_levels_.emplace(price, OrderLevel()).first;
                bid_depth_cache_[price] = 0;
                
                if (top_bid_heap_.size() < DEPTH_K)
                {
                    top_bid_heap_.emplace(price);
                }
                else
                {
                    Price worst_price = top_bid_heap_.peek();
                    int worst_idx = 0;
                    for (int i = 1; i < top_bid_heap_.size(); ++i)
                    {
                        if (top_bid_heap_.peek(i) < worst_price)
                        {
                            worst_price = top_bid_heap_.peek(i);
                            worst_idx = i;
                        }
                    }
                    if (price > worst_price)
                    {
                        top_bid_heap_.pop(worst_idx);
                        top_bid_heap_.emplace(price);
                    }
                }
            }
            level_it->second.emplace(time, id);
            bid_depth_cache_[price] += qty;
        }
        else
        {
            auto level_it = ask_levels_.find(price);
            const bool new_level = (level_it == ask_levels_.end());
            if (new_level)
            {
                ask_book_.emplace(price);
                level_it = ask_levels_.emplace(price, OrderLevel()).first;
                ask_depth_cache_[price] = 0;
                
                if (top_ask_heap_.size() < DEPTH_K)
                {
                    top_ask_heap_.emplace(price);
                }
                else
                {
                    Price worst_price = top_ask_heap_.peek();
                    int worst_idx = 0;
                    for (int i = 1; i < top_ask_heap_.size(); ++i)
                    {
                        if (top_ask_heap_.peek(i) > worst_price)
                        {
                            worst_price = top_ask_heap_.peek(i);
                            worst_idx = i;
                        }
                    }
                    if (price < worst_price)
                    {
                        top_ask_heap_.pop(worst_idx);
                        top_ask_heap_.emplace(price);
                    }
                }
            }
            level_it->second.emplace(time, id);
            ask_depth_cache_[price] += qty;
        }
    }

    inline void OrderEngine::pop_from_book(OrderSide side, Price price, Timestamp time, OrderId id) noexcept
    {
        const Quantity qty = order_pool_[id].qty_;
        
        if (side == OrderSide::BID)
        {
            bid_depth_cache_[price] -= qty;
        }
        else
        {
            ask_depth_cache_[price] -= qty;
        }
        
        OrderLevel& price_level = (side == OrderSide::BID) ? bid_levels_[price] : ask_levels_[price];
        price_level.pop(price_level.find(std::pair<Timestamp, OrderId>(time, id)));
        
        if (price_level.empty())
        {
            if (side == OrderSide::BID)
            {
                const auto idx = bid_book_.find(price);
                if (idx != -1) bid_book_.pop(idx);
                bid_levels_.erase(price);
                bid_depth_cache_.erase(price);
                
                int top_idx = top_bid_heap_.find(price);
                if (top_idx != -1)
                {
                    top_bid_heap_.pop(top_idx);
                }
            }
            else
            {
                const auto idx = ask_book_.find(price);
                if (idx != -1) ask_book_.pop(idx);
                ask_levels_.erase(price);
                ask_depth_cache_.erase(price);
                
                int top_idx = top_ask_heap_.find(price);
                if (top_idx != -1)
                {
                    top_ask_heap_.pop(top_idx);
                }
            }
        }
    }

    inline void OrderEngine::matching_engine(OrderId recent_internal_id, OrderInfo* recent_order_ptr, std::vector<EngineMsg>* fill_notifications) noexcept
    {
        if (recent_internal_id == OrderMemoryPool::INVALID_HANDLE) return;
        if (!order_pool_.is_valid(recent_internal_id)) return;
        if (ask_book_.empty() || bid_book_.empty()) return;

        OrderInfo* recent = recent_order_ptr;

        while (order_pool_.is_valid(recent_internal_id))
        {
            if (!recent) recent = order_pool_.get(recent_internal_id);
            if (!recent) break;
            if (recent->qty_ == 0) break;

            const Price best_ask_price = ask_book_.peek();
            const Price best_bid_price = bid_book_.peek();

            if (recent->type_ == OrderType::MARKET)
            {
                const Price new_price = (recent->side_ == OrderSide::ASK) ? best_bid_price : best_ask_price;
                if (new_price != recent->price_)
                {
                    if (recent->in_book_)
                    {
                        pop_from_book(recent->side_, recent->price_, recent->time_, recent_internal_id);
                        recent->price_ = new_price;
                        push_into_book(recent->side_, recent->price_, recent->time_, recent_internal_id);
                    }
                    else
                    {
                        recent->price_ = new_price;
                    }
                }
            }

            const bool can_trade = (recent->side_ == OrderSide::ASK && best_bid_price >= recent->price_) ||
                                    (recent->side_ == OrderSide::BID && best_ask_price <= recent->price_);
            if (!can_trade) break;

            OrderLevel& best_ask_level = ask_levels_[best_ask_price];
            OrderLevel& best_bid_level = bid_levels_[best_bid_price];

            if (best_ask_level.empty() || best_bid_level.empty()) break;

            const OrderId best_ask_id = best_ask_level.peek().second;
            const OrderId best_bid_id = best_bid_level.peek().second;

            if (recent->side_ == OrderSide::ASK)
                matching(recent_internal_id, best_bid_id, best_ask_level, best_bid_level, fill_notifications);
            else
                matching(best_ask_id, recent_internal_id, best_ask_level, best_bid_level, fill_notifications);

            recent = nullptr;
        }
    }

    inline void OrderEngine::matching(OrderId best_ask_id, OrderId best_bid_id, OrderLevel& best_ask_level,
                OrderLevel& best_bid_level, std::vector<EngineMsg>* fill_notifications) noexcept
    {   
        OrderInfo& best_ask = order_pool_[best_ask_id];
        OrderInfo& best_bid = order_pool_[best_bid_id];
        
        const Quantity qty_filled = std::min(best_ask.qty_, best_bid.qty_);
        
        ask_depth_cache_[best_ask.price_] -= qty_filled;
        bid_depth_cache_[best_bid.price_] -= qty_filled;
        
        best_ask.qty_ -= qty_filled;
        best_bid.qty_ -= qty_filled;
        
        last_trade_price_ = best_ask.price_;
        
        const bool ask_filled = (best_ask.qty_ == 0);
        const bool bid_filled = (best_bid.qty_ == 0);
        
        if (ask_filled)
        {
            best_ask_level.pop();
            if (best_ask_level.empty())
            {
                ask_book_.pop();
                ask_levels_.erase(best_ask.price_);
                top_ask_heap_.pop();
                ask_depth_cache_.erase(best_ask.price_);
            }

            best_ask.status_ = OrderStatus::FILLED;
            if (verbose_ && fill_notifications)
            {
                fill_notifications->emplace_back(EventKind::FILL, encode_external(engine_id_, best_ask_id), best_ask.price_, qty_filled, OrderSide::ASK);
            }

            order_pool_.free(best_ask_id);
            filled_count_ += 1;
        }
        else if (verbose_ && fill_notifications)
        {
            fill_notifications->emplace_back(EventKind::PARTIAL_FILL, encode_external(engine_id_, best_ask_id));
        }
        
        if (bid_filled)
        {
            best_bid_level.pop();
            if (best_bid_level.empty())
            {
                bid_book_.pop();
                bid_levels_.erase(best_bid.price_);
                top_bid_heap_.pop();
                bid_depth_cache_.erase(best_bid.price_);
            }

            best_bid.status_ = OrderStatus::FILLED;
            if (verbose_ && fill_notifications)
            {
                fill_notifications->emplace_back(EventKind::FILL, encode_external(engine_id_, best_bid_id), best_bid.price_, qty_filled, OrderSide::BID);
            }

            order_pool_.free(best_bid_id);
            filled_count_ += 1;
        }
        else if (verbose_ && fill_notifications)
        {
            fill_notifications->emplace_back(EventKind::PARTIAL_FILL, encode_external(engine_id_, best_bid_id));
        }
    }
}

#endif // ORDER_ENGINE_H

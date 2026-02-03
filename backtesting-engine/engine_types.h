#ifndef ENGINE_TYPES_H
#define ENGINE_TYPES_H

#include "../tools/heap.h"
#include "../tools/memory_pool.h"
#include "../tools/lazy_queue.h"
#include <unordered_map>


namespace engine
{
    // High-precision timestamp (nanoseconds)
    using Timestamp = std::uint64_t;
    inline Timestamp now_ns() noexcept {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    // Order Status
    enum class OrderStatus : std::uint8_t
    {
        OPEN,
        FILLED,
        CANCELLED,
        NONE
    };

    // Order Types
    enum class OrderType : std::uint8_t
    {
        LIMIT,
        MARKET
    };

    // Order Sides
    enum class OrderSide : std::uint8_t
    {
        BID, 
        ASK
    };

    // price and qty in ticks
    using Price = std::uint64_t;   
    using Quantity = std::uint32_t;

    struct OrderInfo
    {
        Timestamp time_; // 8 bytes
        Price price_; // 8 bytes
        Quantity qty_; // 4 bytes
        OrderStatus status_; // 1 byte
        OrderType type_; // 1 byte
        OrderSide side_; // 1 byte
        bool in_book_ = true; // whether this order is currently pushed into the book

        OrderInfo() = default;
        OrderInfo(OrderSide side, OrderType type, Quantity qty, Price price) noexcept
        : side_(side), type_(type), status_(OrderStatus::OPEN), qty_(qty), price_(price), time_(now_ns())
        {
        }
    };

    // Memory Pool with generational handles
    using OrderMemoryPool = MemoryPool<OrderInfo>;
    using OrderId = OrderMemoryPool::Handle;  // Generational handle: [48-bit slot | 16-bit generation]
    constexpr OrderId INVALID_ID = OrderMemoryPool::INVALID_HANDLE;

    using OrderLevel = Heap<std::pair<Timestamp, OrderId>, HeapType::MIN>;
    using LevelMap = std::unordered_map<Price, OrderLevel>;
    using BidBook = Heap<Price, HeapType::MAX>;
    using AskBook = Heap<Price, HeapType::MIN>;
    
    // Top-K depth tracking (K = number of levels to track)
    constexpr std::size_t DEPTH_K = 10;
    using TopBidHeap = Heap<Price, HeapType::MAX>; // Top K bid prices
    using TopAskHeap = Heap<Price, HeapType::MIN>; // Top K ask prices

    enum class EventKind : uint8_t 
    {
        NONE,
        ACCEPT,
        REJECT,
        MODIFY,
        PARTIAL_FILL,
        FILL,
        CANCEL
    };

    enum class RejectReason : uint8_t 
    {
        NO_MARKET_LIQUIDITY,
        ENGINE_FULL,
        ORDER_NOT_FOUND
    };

    struct EngineMsg 
    {
        EventKind kind = EventKind::NONE;
        // Common payload for most events
        OrderId order_id = INVALID_ID;
        Price price = static_cast<Price>(-1);
        Quantity qty = 0;
        OrderSide side = OrderSide::BID;

        // Optional reject reason (used when kind == REJECT)
        RejectReason reject = RejectReason::NO_MARKET_LIQUIDITY;

        EngineMsg() = default;

        // Generic order-related event (ACCEPT, MODIFY, CANCEL, PARTIAL_FILL with just id)
        EngineMsg(EventKind k, OrderId oid) : kind(k), order_id(oid) {}

        // Reject event with reason
        EngineMsg(EventKind k, RejectReason rr) : kind(k), reject(rr) {}

        // Fill/PARTIAL_FILL event with detailed execution info
        EngineMsg(EventKind k, OrderId oid, Price p, Quantity q, OrderSide s)
            : kind(k), order_id(oid), price(p), qty(q), side(s) {}
    };

    // Lock-free market snapshot for instant reads
    struct MarketSnapshot
    {
        Price best_bid;
        Price best_ask;
        Price market_price;      // Last trade execution price
        bool auto_match;         // Current auto-match flag
        Quantity bid_depth[10];  // Top 10 bid levels
        Quantity ask_depth[10];  // Top 10 ask levels
        Price bid_prices[10];
        Price ask_prices[10];
        std::uint8_t bid_levels;
        std::uint8_t ask_levels;
        std::size_t placed_count;   // Total orders placed
        std::size_t cancelled_count; // Total orders cancelled
        std::size_t filled_count;    // Total orders filled
        std::size_t open_count;      // Current open orders

        MarketSnapshot() noexcept
            : best_bid(-1), best_ask(-1), market_price(-1),
              bid_levels(0), ask_levels(0),
              auto_match(false),
              placed_count(0), cancelled_count(0), filled_count(0), open_count(0)
        {
            for (int i = 0; i < 10; ++i) 
            {
                bid_depth[i] = 0;
                ask_depth[i] = 0;
                bid_prices[i] = 0;
                ask_prices[i] = 0;
            }
        }
    };

} // namespace engine

#endif // ENGINE_TYPES_H

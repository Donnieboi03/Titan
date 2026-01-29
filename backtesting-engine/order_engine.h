#ifndef ORDER_ENGINE_H
#define ORDER_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

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

    // Order information (public view)
    struct OrderInfo
    {
        Timestamp time_;
        Price price_;
        Quantity qty_;
        OrderStatus status_;
        OrderType type_;
        OrderSide side_;
        bool in_book_ = true; // whether currently pushed into the orderbook

        OrderInfo() = default;
    };

    // Engine event kinds
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
        // payload
        // Note: external id encoding uses engine prefix; callers work with engine::OrderId
        using OrderId = std::uint64_t;
        OrderId order_id = static_cast<OrderId>(-1);
        Price price = static_cast<Price>(-1);
        Quantity qty = 0;
        OrderSide side = OrderSide::BID;
        RejectReason reject = RejectReason::NO_MARKET_LIQUIDITY;

        EngineMsg() = default;
        EngineMsg(EventKind k, OrderId oid) : kind(k), order_id(oid) {}
        EngineMsg(EventKind k, RejectReason rr) : kind(k), reject(rr) {}
        EngineMsg(EventKind k, OrderId oid, Price p, Quantity q, OrderSide s) : kind(k), order_id(oid), price(p), qty(q), side(s) {}
    };

    // Snapshot structure (lock-free double-buffered in implementation)
    struct MarketSnapshot
    {
        Price best_bid;
        Price best_ask;
        Price market_price;
        bool auto_match;
        Quantity bid_depth[10];
        Quantity ask_depth[10];
        Price bid_prices[10];
        Price ask_prices[10];
        std::uint8_t bid_levels;
        std::uint8_t ask_levels;
        std::size_t placed_count;
        std::size_t cancelled_count;
        std::size_t filled_count;
        std::size_t open_count;

        MarketSnapshot() noexcept;
    };

    // OrderEngine interface (implementation lives in order_engine.cpp)
    class OrderEngine
    {
    public:
        OrderEngine(std::size_t capacity = 1048576, bool verbose = true, bool auto_match = true, std::uint16_t engine_id = 0) noexcept;
        ~OrderEngine();

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
        // Implementation-defined members are private in .cpp
        struct Impl;
        Impl* impl_ = nullptr;
    };
}

#endif // ORDER_ENGINE_H

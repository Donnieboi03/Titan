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

    // Price and quantity in ticks
    using Price = std::uint64_t;   
    using Quantity = std::uint32_t;
    using OrderId = std::uint32_t;

    constexpr OrderId INVALID_ID = static_cast<OrderId>(-1);

    // Order information structure
    struct OrderInfo
    {
        Timestamp time_;
        Price price_;
        Quantity qty_;
        OrderId id_;
        OrderSide side_;
        OrderType type_;
        OrderStatus status_;

        OrderInfo() = default;
        OrderInfo(Timestamp t, Price p, Quantity q, OrderId id, OrderSide s, OrderType type)
            : time_(t), price_(p), qty_(q), id_(id), side_(s), type_(type), status_(OrderStatus::OPEN) {}
    };

    // Event kinds for engine messages
    enum class EventKind : std::uint8_t
    {
        NONE,
        ACCEPT,
        REJECT,
        FILL,
        CANCEL,
        EDIT
    };

    // Engine message structure
    struct EngineMsg
    {
        EventKind kind;
        OrderId order_id;
        Price price;
        Quantity qty;
        Timestamp time;

        EngineMsg() : kind(EventKind::NONE), order_id(INVALID_ID), price(0), qty(0), time(0) {}
        EngineMsg(EventKind k, OrderId id, Price p, Quantity q, Timestamp t)
            : kind(k), order_id(id), price(p), qty(q), time(t) {}
    };

    // Market snapshot structure
    struct MarketSnapshot
    {
        Price market_price;
        Price best_bid;
        Price best_ask;
        std::size_t placed_count;
        std::size_t cancelled_count;
        std::size_t filled_count;
        std::size_t open_count;

        MarketSnapshot()
            : market_price(static_cast<Price>(-1)),
              best_bid(static_cast<Price>(-1)),
              best_ask(static_cast<Price>(-1)),
              placed_count(0),
              cancelled_count(0),
              filled_count(0),
              open_count(0) {}
    };

    // OrderEngine class declaration
    class OrderEngine
    {
    public:
        OrderEngine(std::size_t capacity, bool verbose = false, bool auto_match = false);
        ~OrderEngine();

        // Order operations
        OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty, std::vector<EngineMsg>& msgs);
        OrderId place_order(OrderSide side, OrderType type, Price price, Quantity qty);
        bool cancel_order(OrderId order_id, std::vector<EngineMsg>& msgs);
        bool cancel_order(OrderId order_id);
        OrderId edit_order(OrderId order_id, Price new_price, Quantity new_qty, std::vector<EngineMsg>& msgs);
        OrderId edit_order(OrderId order_id, Price new_price, Quantity new_qty);

        // Matching
        void match_orders(std::vector<EngineMsg>& msgs);
        void match_orders();

        // Queries
        const OrderInfo* get_order(OrderId order_id) const;
        std::vector<std::pair<Price, Quantity>> get_market_depth(OrderSide side, std::size_t depth) const;
        
        // Snapshot
        void update_snapshot();
        const MarketSnapshot& get_snapshot() const { return snapshot_; }

        // Configuration
        void set_auto_match(bool auto_match) { auto_match_ = auto_match; }
        bool get_auto_match() const { return auto_match_; }

        // Statistics
        std::size_t get_placed_count() const { return placed_count_; }
        std::size_t get_cancelled_count() const { return cancelled_count_; }
        std::size_t get_filled_count() const { return filled_count_; }
        std::size_t get_open_count() const;

    private:
        class Impl;
        Impl* pimpl_;
        
        std::string ticker_;
        bool verbose_;
        bool auto_match_;
        
        MarketSnapshot snapshot_;
        std::size_t placed_count_;
        std::size_t cancelled_count_;
        std::size_t filled_count_;
    };
}

#endif // ORDER_ENGINE_H

#ifndef ENGINE_TYPES_H
#define ENGINE_TYPES_H

#include "tools/heap.h"
#include "tools/memory_pool.h"
#include "tools/lazy_queue.h"
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
    using ExternalOrderId = OrderId;  // User-facing order ID (same type; alias for API clarity)
    constexpr OrderId INVALID_ORDER_ID = OrderMemoryPool::INVALID_HANDLE;

    // Intrusive linked list node for FIFO order queues (O(1) operations)
    struct OrderNode {
        OrderId id;
        OrderNode* next = nullptr;
        OrderNode* prev = nullptr;
    };

    // Simple freelist allocator for OrderNodes to avoid heap allocation overhead
    struct OrderNodePool {
        OrderNode* freelist = nullptr;
        std::vector<OrderNode*> blocks;
        static constexpr std::size_t BLOCK_SIZE = 4096; // Allocate nodes in blocks

        OrderNode* allocate() {
            if (!freelist) {
                // Allocate a new block
                OrderNode* block = new OrderNode[BLOCK_SIZE];
                blocks.push_back(block);
                // Link all nodes in the block to the freelist
                for (std::size_t i = 0; i < BLOCK_SIZE - 1; ++i) {
                    block[i].next = &block[i + 1];
                }
                block[BLOCK_SIZE - 1].next = nullptr;
                freelist = block;
            }
            OrderNode* node = freelist;
            freelist = freelist->next;
            return node;
        }

        void deallocate(OrderNode* node) {
            node->next = freelist;
            freelist = node;
        }

        ~OrderNodePool() {
            for (auto* block : blocks) {
                delete[] block;
            }
        }
    };

    // FIFO queue for orders at a price level using intrusive linked list
    struct OrderLevel {
        OrderNode* front = nullptr;
        OrderNode* back = nullptr;
        std::size_t count = 0;
        OrderNodePool* pool = nullptr;  // Shared pool pointer

        inline void emplace(Timestamp time, OrderId id, OrderNodePool* node_pool) noexcept {
            pool = node_pool;
            OrderNode* node = pool->allocate();
            node->id = id;
            node->next = nullptr;
            node->prev = nullptr;
            
            if (!front) {
                front = back = node;
            } else {
                back->next = node;
                node->prev = back;
                back = node;
            }
            ++count;
        }

        inline std::pair<Timestamp, OrderId> peek() const noexcept {
            return {0, front->id};  // time no longer stored, return 0
        }

        inline void pop() noexcept {
            if (!front || !pool) return;
            OrderNode* old_front = front;
            front = front->next;
            if (front) {
                front->prev = nullptr;
            } else {
                back = nullptr;
            }
            pool->deallocate(old_front);
            --count;
        }

        // Find and remove specific order (used for cancellation/editing)
        inline bool remove(Timestamp time, OrderId id) noexcept {
            if (!pool) return false;
            OrderNode* current = front;
            while (current) {
                if (current->id == id) {  // Only check id, it's unique
                    if (current->prev) {
                        current->prev->next = current->next;
                    } else {
                        front = current->next;
                    }
                    if (current->next) {
                        current->next->prev = current->prev;
                    } else {
                        back = current->prev;
                    }
                    pool->deallocate(current);
                    --count;
                    return true;
                }
                current = current->next;
            }
            return false;
        }

        inline bool empty() const noexcept {
            return count == 0;
        }

        inline std::size_t size() const noexcept {
            return count;
        }

        ~OrderLevel() {
            if (!pool) return;
            while (front) {
                OrderNode* next = front->next;
                pool->deallocate(front);
                front = next;
            }
        }
    };

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
        OrderId order_id = INVALID_ORDER_ID;
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
        Price bid_prices[10];
        Price ask_prices[10];
        std::size_t placed_count;   // Total orders placed
        std::size_t cancelled_count; // Total orders cancelled
        std::size_t filled_count;    // Total orders filled
        std::size_t open_count;      // Current open orders
        Quantity bid_depth[10];  // Top 10 bid levels
        Quantity ask_depth[10];  // Top 10 ask levels
        std::uint8_t bid_levels;
        std::uint8_t ask_levels;
        bool auto_match;         // Current auto-match flag

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

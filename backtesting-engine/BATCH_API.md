# EngineRuntime Batch API

## Overview
The EngineRuntime now uses a **batch-by-default** pattern for all order operations. This provides maximum performance by allowing multiple orders to be submitted before executing them in parallel across worker threads.

## API Design

### Order Methods (Batch-by-Default)
All order methods now take a pointer to store results and do **not** execute immediately:

```cpp
void limit_order(const std::string& ticker, OrderSide side, Price price, Quantity qty, OrderId* result_ptr)
void market_order(const std::string& ticker, OrderSide side, Quantity qty, OrderId* result_ptr)
void cancel_order(const std::string& ticker, OrderId order_id, bool* result_ptr)
void edit_order(const std::string& ticker, OrderId order_id, OrderSide side, Price price, Quantity qty, OrderId* result_ptr)
```

### Batch Execution
After submitting orders, call `execute_batch()` to process all pending operations:

```cpp
void execute_batch()  // Processes all submitted jobs and waits for completion
```

## Usage Patterns

### Single Order (Immediate Execution)
```cpp
EngineRuntime runtime(4);
runtime.initialize_stock("AAPL", 100.0, 1000);

OrderId order_id = -1;
runtime.limit_order("AAPL", OrderSide::BID, 99.0, 10, &order_id);
runtime.execute_batch();  // Execute immediately

std::cout << "Order ID: " << order_id << std::endl;
```

### Batch Orders (Optimal Performance)
```cpp
EngineRuntime runtime(4);
runtime.initialize_stock("AAPL", 100.0, 1000);

std::vector<OrderId> order_ids(100, -1);

// Submit 100 orders
for (int i = 0; i < 100; ++i)
{
    runtime.limit_order("AAPL", OrderSide::BID, 99.0 + i, 10, &order_ids[i]);
}

// Execute all at once (parallel execution across worker threads)
runtime.execute_batch();

// All results are now available
for (const auto& id : order_ids)
{
    std::cout << "Order ID: " << id << std::endl;
}
```

### Multi-Stock Batch
```cpp
EngineRuntime runtime(4);
runtime.initialize_stock("AAPL", 150.0, 1000);
runtime.initialize_stock("MSFT", 300.0, 500);
runtime.initialize_stock("GOOGL", 2500.0, 200);

OrderId aapl_order = -1, msft_order = -1, googl_order = -1;

// Submit orders across different stocks
runtime.limit_order("AAPL", OrderSide::BID, 149.0, 10, &aapl_order);
runtime.limit_order("MSFT", OrderSide::BID, 299.0, 5, &msft_order);
runtime.limit_order("GOOGL", OrderSide::BID, 2499.0, 2, &googl_order);

// Execute all at once (routed to correct engines via owner_id)
runtime.execute_batch();
```

### Mixed Operations Batch
```cpp
EngineRuntime runtime(4);
runtime.initialize_stock("TSLA", 200.0, 500);

// Place initial orders
OrderId id1 = -1, id2 = -1;
runtime.limit_order("TSLA", OrderSide::BID, 195.0, 10, &id1);
runtime.limit_order("TSLA", OrderSide::BID, 190.0, 20, &id2);
runtime.execute_batch();

// Batch submit: new orders, cancel, and edit
OrderId new_order = -1;
bool cancel_result = false;
OrderId edit_result = -1;

runtime.limit_order("TSLA", OrderSide::BID, 185.0, 15, &new_order);
runtime.cancel_order("TSLA", id2, &cancel_result);
runtime.edit_order("TSLA", id1, OrderSide::BID, 196.0, 12, &edit_result);

// Execute all operations together
runtime.execute_batch();
```

## Performance Benefits

### Staged Batch Processing
- **Submit Phase**: All jobs are staged in thread-safe queues
- **Process Phase**: Workers execute jobs in parallel across engines
- **Wait Phase**: Main thread waits for all jobs to complete
- **No Race Conditions**: Submit and process phases are separate

### Memory Management
- Uses **Arena allocator** for job arguments (stable addresses, no reallocation)
- Pre-allocated arena capacity avoids allocation overhead
- Automatic cleanup when EngineRuntime is destroyed

### Thread Safety
- **Engine ID mapping**: Each ticker maps to unique engine_id for job routing
- **Lock-free scheduler**: Uses atomic operations with memory ordering
- **Round-robin distribution**: Jobs distributed evenly across worker threads

### Expected Speedup
- **2.9x - 3.8x** faster than sequential execution (based on scheduler_test.cpp)
- Scales with number of worker threads (4 threads recommended)
- Best performance with large batches (1000+ orders)

## Implementation Details

### Job Structure
```cpp
struct Job
{
    void(*f)(void*);      // Function pointer to execute
    void* args;           // Arguments (allocated in Arena)
    std::size_t owner_id; // Engine ID for routing
};
```

### Internal Components
- **JobScheduler**: Staged batch scheduler with worker thread pool
- **ArgsArena**: Arena<OrderJobArgs> for stable memory addresses
- **EngineIdMap**: Maps ticker → engine_id for job routing
- **RingBuffer**: Lock-free queue for job storage (per worker)

### Code Formatting
Consistent formatting with no indenting of struct/function parameters:
```cpp
// Correct formatting
auto args_idx = args_arena_.emplace(OrderJobArgs{
stock_exchange_.at(_ticker),
_side,
OrderType::LIMIT,
_price,
_qty,
0,
result_id_ptr,
nullptr
});
```

## Testing
See `test/runtime_test.cpp` for comprehensive examples:
- Basic batch orders
- Mixed batch operations (place/cancel/edit)
- Multi-stock batching
- Large batch performance (10K orders)
- Sequential vs batch comparison

# ⚙️ High-Performance C++ Exchange Simulator

A **multithreaded, low-latency** exchange simulation framework built in modern C++.  
Originally designed as a standalone order-matching engine, this project has evolved into a **complete simulated exchange** capable of:

- Managing multiple tickers
- Handling concurrent order flows across markets
- Simulating real-world exchange dynamics with Monte Carlo-driven activity

---

## 🚀 Key Features

### 🏛 Exchange-Level Architecture
- **Multi-Ticker Support** – Trade multiple instruments (e.g., AAPL, TSLA, AMZN) concurrently.  
- **Centralized Exchange Layer** – Routes orders to their respective order books, manages state, and provides global statistics.  
- **Thread-Per-Ticker Design** – Each instrument runs on its own dedicated thread for parallelized market simulation.  

### 📈 Advanced Order Matching Engine
- **Full Order Lifecycle**
  - `market_order()` / `limit_order()` – Support for standard trading actions  
  - `cancel_order()` – Cancel any open order by ID  
  - `edit_order()` – Amend live orders in the book  
- **Price-Time Priority Matching** – Ensures FIFO matching within each price level.  
- **Custom Heap-Based Order Books** – Dual min/max heaps for optimal bid/ask management.  

### 🧪 Simulation & Market Dynamics
- **Monte Carlo Market Generator** – Injects realistic, randomized BID/ASK flows to stress-test the system.  
- **Volatility & Skew Control** – Adjust market behavior with parameters like volatility, skew, and order flow intensity.  
- **Exchange-Wide Metrics** – Query global stats: price levels, order counts, fills, cancellations.  

### 🧵 Concurrency & Performance
- **Thread-Safe Execution** – Uses `std::thread`, `std::mutex`, `std::shared_ptr`, and `std::atomic` to ensure low-latency operation.  
- **Scalable Design** – Easily extendable to simulate hundreds of symbols simultaneously.  

### 📡 Real-Time Monitoring
- **Console-Based Event Log** – Tracks `[OPEN]`, `[FILLED]`, `[PARTIALLY FILLED]`, `[CANCELLED]` events in real time.  
- **Live Price Discovery** – Functions like `get_price()`, `get_best_bid()`, and `get_best_ask()` per ticker.  

---

## 🛠 Tech Stack

| Library | Purpose |
|--------|---------|
| `<thread>`, `<mutex>`, `<atomic>` | Safe multithreading & concurrency |
| `<map>`, `<unordered_map>`, `<set>` | Order indexing & lookup |
| `<vector>`, `<deque>` | Price level management |
| `<random>` | Market simulation |
| `<memory>` | Smart pointers (`shared_ptr`, `unique_ptr`) for ownership control |

---

## ⚡ Performance & Optimization

### 🎯 Optimal Configuration

For **maximum throughput** while minimizing **memory footprint** and **cache contention**:

```cpp
// Initialize EngineRuntime with optimal settings
auto& runtime = runtime::EngineRuntime::get_instance(
    4,          // num_workers: 4 threads (optimal for cache locality)
    1048576,    // default_capacity: 1M orders per engine
    false       // verbose: disable for production (no notification overhead)
);

// Register 8-12 stocks (2-3 stocks per worker for hot cache paths)
runtime.register_stock("AAPL", 180.0, 10000.0);
runtime.register_stock("GOOGL", 2800.0, 10000.0);
// ... 8-12 total tickers recommended
```

**Why 4 Workers?**
- ✅ **Less cache thrashing**: Each worker handles 2-3 stocks with hot L1/L2 cache
- ✅ **Reduced atomic contention**: Only 4 workers competing on lock-free queue operations
- ✅ **Better memory bandwidth**: Doesn't saturate memory controller
- ✅ **Hot instruction paths**: OrderEngine matching logic stays in instruction cache

**Throughput vs Latency Tradeoff:**

| Configuration | Per-Stock Latency | Aggregate Throughput | Cache Efficiency |
|--------------|-------------------|---------------------|------------------|
| 1 worker, 1 stock | ~133 μs | 7.5M orders/sec | Excellent (L1/L2 hot) |
| 4 workers, 4 stocks | ~133 μs | 30M orders/sec | Excellent (1:1 ratio) |
| **4 workers, 8-12 stocks** | **~200 μs** | **35-48M orders/sec** | **Good (optimal)** |
| 8 workers, 8 stocks | ~133 μs | 28-32M orders/sec | Poor (cache thrashing) |

> **Key Insight**: Fewer workers handling multiple stocks = higher aggregate throughput due to better cache utilization, even though individual stock latency increases slightly (~50% slowdown per stock, but 2-3x total system throughput).

---

### 💾 Memory Footprint Analysis

**Per-Engine Memory Usage:**

Each `OrderEngine` uses arena allocation with the following memory breakdown:

```
OrderInfo structure: 24 bytes
  ├─ Timestamp:  8 bytes
  ├─ Price:      8 bytes
  ├─ Quantity:   4 bytes
  ├─ Side:       1 byte
  ├─ UserId:     1 byte
  ├─ Status:     1 byte
  └─ Padding:    1 byte (alignment)

Arena slot overhead: 8 bytes (heap metadata per allocation)
Total per order:    32 bytes
```

**Capacity Planning:**

| Capacity | Memory per Engine | 8 Engines | 12 Engines |
|----------|------------------|-----------|------------|
| 1M orders | 32 MB | 256 MB | 384 MB |
| 2M orders | 64 MB | 512 MB | 768 MB |
| 4M orders | 128 MB | 1.0 GB | 1.5 GB |
| 16M orders | 512 MB | 4.1 GB | 6.1 GB |

**Optimal Production Setup (4 workers, 8-12 stocks):**
- **1M capacity**: 256-384 MB total (ideal for most use cases)
- **2M capacity**: 512-768 MB total (high-frequency trading simulation)
- With **85% fill rate**, 1M capacity handles **6M+ order flow** (matching frees slots)

**Memory Formula:**
```
Total RAM = num_stocks × capacity × 32 bytes
          = 8 stocks × 1M × 32 bytes = 256 MB
```

> **Note**: Order matching continuously frees capacity. With typical 80-90% fill rates, effective throughput is 5-10x the nominal capacity before exhaustion.

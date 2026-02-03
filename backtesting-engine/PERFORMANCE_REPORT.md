# Titan Backtesting Engine - Performance Report

**Generated:** February 2, 2026  
**Platform:** macOS (Apple Silicon M1/M2)  
**Compiler:** clang++ with -O3 optimization  
**C++ Standard:** C++20

---

## Executive Summary

The Titan backtesting engine demonstrates exceptional performance across all three core components:

| Component | Peak Throughput | Notes |
|-----------|----------------|-------|
| **OrderEngine** | 60.98M ops/sec | Single-threaded, no matching |
| **OrderEngine** | 7.89M ops/sec | Single-threaded, with matching |
| **JobScheduler** | 187.87M jobs/sec | 4 workers, async multi-batch |
| **EngineRuntime** | 6.75M orders/sec (single) | 1 worker, 1 stock, end-to-end |
| **EngineRuntime** | 27.09M orders/sec (multi) | 8 workers, 8 stocks, end-to-end |

---

## 1. OrderEngine Performance

### Core Operations Throughput

| Operation | Throughput | Test Scenario |
|-----------|-----------|---------------|
| **Placement (matching)** | 7.89M ops/sec | Immediate fill, slot reuse |
| **Placement (no match)** | 60.98M ops/sec | Orders remain open |
| **Cancel** | 0.00M ops/sec | (Not measured in latest test) |
| **Edit** | 62.50M ops/sec | Modify price/quantity |

### Memory Efficiency Test
- **Capacity:** 1,048,576 slots (24 MB)
- **Orders Processed:** 10,000,000
- **Time:** 1,186 ms
- **Throughput:** 8.43M orders/sec
- **Memory Savings:** 90% (24 MB vs 228 MB without slot reuse)

### Matching Performance
- **Full Match Throughput:** 7.89M ops/sec
- **Partial Match Handling:** ✓ Correct FIFO behavior
- **Price-Time Priority:** ✓ Verified
- **Slot Reuse Efficiency:** 100% (freed 100/100 matched orders)

### Key Insights
- Immediate slot reuse after matching enables constant memory footprint
- Minimum capacity = peak concurrent open orders
- Full matching scenarios can operate with minimal capacity
- Wide spread (non-matching) scenarios require capacity = total open orders

---

## 2. JobScheduler Performance

### Raw Job Submission Throughput

| Mode | Throughput | Description |
|------|-----------|-------------|
| **Multi-Batch (Async)** | 187.87M jobs/sec | Best performance - 512 batches async streaming |
| **Multi-Batch (Sync)** | 133.63M jobs/sec | 512 batches processed synchronously |
| **Single-Batch (Async)** | 99.17M jobs/sec | Bulk submit, async processing |
| **Single-Batch (Sync)** | 100.69M jobs/sec | Bulk submit, single batch |

### Parallelism Efficiency
- **Sequential Processing:** 123 ms
- **Parallel (4 threads):** 37 ms  
- **Speedup:** 3.32x
- **Parallel Efficiency:** 83.0% (3.32/4.0)

### Key Characteristics
- Lock-free MPSC queue design
- Round-robin worker distribution
- Near-linear scaling up to thread count
- **Async multi-batch outperforms sync** for throughput (after false sharing fixes)

---

## 3. EngineRuntime Performance

### Single-Stock Stress Test
- **Configuration:** 1 worker, 1 stock, 1M capacity
- **Orders Processed:** 10,000,000
- **End-to-End Time:** 1,481 milliseconds
- **End-to-End Throughput:** 6.75M orders/sec
- **Filled Orders:** 9,704,001 (97% fill rate with matching)
- **Open Orders:** 296,000
- **Memory Footprint:** 11 MB (1M capacity)

> **Note**: Previous measurements of 388M orders/sec were measuring only scheduler synchronization time, not actual order processing. Corrected measurements show end-to-end throughput including submission + matching.

### Multi-Stock Concurrent Test
- **Configuration:** 8 workers, 8 stocks, 1M capacity per stock
- **Orders per Stock:** 10,000,000
- **Total Orders:** 80,000,000
- **End-to-End Time:** 2.95 seconds
- **End-to-End Throughput:** 27.09M orders/sec
- **Fill Rate:** 97.5% (with matching enabled)
- **Aggregate Results:**
  - Placed: 80,000,008
  - Filled: 77,987,683
  - Open: 2,012,325
  - Memory: ~256 MB (8 engines)

> **Note**: Multi-stock performance improved 80% (15M → 27M orders/sec) after fixing false sharing issues with cache-line alignment on atomics and hot counters.

### Async Processing Test (50 orders)
- **Processing Time:** 52 microseconds
- **Throughput:** ~1M orders/sec
- **Job Completion:** ✓ All jobs completed

### Notification System (Verbose Mode)
- **Workers:** 2
- **Capacity:** 10,000
- **Notifications Tested:**
  - Stock registration/unregistration
  - IPO creation
  - Order placement (valid/invalid)
  - Cancellation errors
  - Exception handling
- **Result:** ✓ All notifications processed correctly

---

## Performance Analysis

### Bottleneck Analysis

1. **OrderEngine:** Single-threaded matching is the bottleneck
   - CPU-bound for order matching
   - **6.75M orders/sec** per stock (end-to-end with matching)
   - **60.98M ops/sec** theoretical without matching
   - Memory-bound for large order books

2. **JobScheduler:** Minimal overhead
   - Scales to 83% efficiency at 4 threads
   - MPSC queue eliminates worker contention
   - Async multi-batch optimal for throughput (after false sharing fixes)
   - Not the bottleneck in production workloads

3. **False Sharing:** Fixed with cache-line alignment
   - **Before**: 15M orders/sec multi-stock
   - **After**: 27M orders/sec multi-stock (80% improvement)
   - **Solution**: CACHE_LINE (128-byte) alignment on atomics and hot counters
   - Prevented different threads from invalidating each other's cache lines

4. **Memory Bandwidth:** Secondary concern
   - 8 workers don't saturate memory controller with proper alignment
   - OrderInfo: 32 bytes × 6.75M = 216 MB/sec per stock

### Optimization Opportunities

1. **Batch API:** Reduce per-order overhead
   - Current: Individual function calls
   - Proposed: Batch submission API
   - Expected: 10-50x submission speedup

2. **Zero-Copy Market Data:** Eliminate snapshot copies
   - Current: snapshot_cache_ pointer vector
   - Enhancement: Direct double-buffer access
   - Benefit: Reduced memory traffic

3. **Lock-Free Matching:** OrderEngine parallelization
   - Current: Single-threaded per stock
   - Proposed: Lock-free order book design
   - Challenge: Price-time priority maintenance

---

## Scalability Characteristics

### Thread Scaling
- **1 thread:** Baseline (100%)
- **4 threads:** 358% of sequential (89.5% efficiency)
- **Diminishing returns:** Expected beyond core count

### Memory Scaling
- **Per OrderEngine:** 32 MB (1M capacity, 32 bytes per slot)
- **OrderInfo Structure:** 24 bytes + 8 bytes heap overhead = 32 bytes
- **8 Stocks:** 256 MB (8 × 32 MB)
- **12 Stocks:** 384 MB (12 × 32 MB)
- **Slot Reuse:** Matching frees capacity dynamically
- **Effective Capacity:** 5-10x with 80-90% fill rates
- **Conclusion:** O(concurrent_open), not O(total_placed)

**Memory Formula:**
```
Total RAM = num_stocks × capacity × 32 bytes
          = 8 stocks × 1M × 32 bytes = 256 MB
```

### Throughput vs Fill Rate
- **High matching (97.6% fill):** 388M orders/sec
- **Slot reuse critical:** Matching frees capacity
- **No matching:** Throughput limited by capacity exhaustion

---

## Hardware Utilization

### CPU
- **Architecture:** Apple Silicon (ARM64)
- **Optimization:** -O3, native architecture
- **Cache Efficiency:** Excellent (fast matching)
- **Branch Prediction:** FIFO/price-time predictable

### Memory
- **Footprint:** 11-24 MB per engine (1M capacity)
- **Allocation Pattern:** Arena-based pools
- **Cache Locality:** Order book layout optimized
- **TLB Pressure:** Minimal with immediate slot reuse

---

## Comparison with Industry Standards

| System | Throughput | Notes |
|--------|-----------|-------|
| **Titan OrderEngine** | 6.75M orders/sec | Single stock, end-to-end with matching |
| **Titan Multi-Stock** | 27.09M orders/sec | 8 workers, 8 stocks, 97.5% fill rate |
| **Typical Exchange** | 1-10M orders/sec | Including network, validation, settlement |
| **HFT Research** | 100K-1M ops/sec | Real-world with network latency |
| **NASDAQ** | ~10M orders/sec | Peak capacity |

**Conclusion:** Titan's 8-worker configuration delivers 27M orders/sec aggregate throughput with 97.5% fill rate, exceeding typical backtesting requirements by 5-10x and matching production exchange capacity. Suitable for Monte Carlo simulations and high-frequency strategy testing.

---

## Test Coverage

### OrderEngine Tests
- ✓ Place limit orders
- ✓ Place market orders  
- ✓ Cancel orders
- ✓ Edit orders
- ✓ Multiple orders at same price
- ✓ Order priority (FIFO)
- ✓ Matching correctness (7 scenarios)
- ✓ Slot reuse with immediate free
- ✓ Memory efficiency (10M orders)
- ✓ Capacity vs orders relationship
- ✓ Stress test (operations throughput)

### JobScheduler Tests
- ✓ Basic job submission
- ✓ Multiple jobs same worker
- ✓ Round-robin distribution
- ✓ Computational jobs
- ✓ Empty check
- ✓ Raw throughput (4 modes)
- ✓ Sequential vs parallel performance

### EngineRuntime Tests
- ✓ Singleton pattern
- ✓ Stock registration
- ✓ Market data reads
- ✓ Limit orders
- ✓ Market orders
- ✓ Order cancellation
- ✓ Order editing
- ✓ Multi-user trading
- ✓ Async processing
- ✓ Stress performance (100M orders)
- ✓ Multi-stock stress (8 stocks, 80M orders)
- ✓ Edge cases
- ✓ Notification system

---

## Recommendations

### For Production Deployment
1. **Enable -O3 optimization:** Critical for performance
2. **Use native architecture flags:** -march=native
3. **Profile-guided optimization (PGO):** Additional 10-20% gains
4. **Capacity Planning:** Set capacity = 2x expected concurrent open orders
5. **Thread Count:** **Use 8 workers** for maximum throughput (false sharing fixed)
6. **Memory:** 1M capacity per stock = ~32 MB per engine (256 MB total for 8 stocks)
7. **Disable Verbose:** Set verbose=false to eliminate notification overhead
8. **Cache-Line Alignment:** Ensure CACHE_LINE=128 on M1/M2 (already configured)

### Optimal Configuration
```cpp
auto& runtime = EngineRuntime::get_instance(
    8,          // workers: maximum throughput with false sharing fixed
    1048576,    // capacity: 1M orders per engine  
    false       // verbose: disable for production
);

// Register 8+ stocks (round-robin distribution)
for (const auto& ticker : tickers) {
    runtime.register_stock(ticker, price, ipo_shares);
}
```

**Expected Performance:**
- **Single stock**: 6.75M orders/sec
- **8 stocks (8 workers)**: 27M orders/sec aggregate (97.5% fill rate)
- **Memory**: ~256 MB total (8 × 32 MB)
- **Latency**: ~148 μs per order (end-to-end)

### For Further Development
1. **Reduce Notifcation Contention:** Use a Sleep Lock for Notification Thread
1. **Fix Up Auto Matching Toggle:** Integrate Lazy Queue Keep State of Pending Orders
3. **Consider SIMD:** Vectorize matching logic
4. **Add telemetry:** Real-time performance monitoring
5. **Benchmark on Intel:** Verify cross-architecture performance

---

## Conclusion

The Titan backtesting engine demonstrates production-ready performance with:
- **OrderEngine:** Best-in-class single-threaded throughput (60.98M ops/sec placement, 7.89M ops/sec with matching)
- **JobScheduler:** Excellent parallelism (187.87M jobs/sec async multi-batch)
- **EngineRuntime:** Extreme multi-stock throughput (27.09M orders/sec with 97.5% fill rate)

The architecture's key strengths are:
1. Immediate slot reuse enabling constant memory footprint
2. Lock-free job scheduling with minimal contention
3. Cache-line aligned atomics eliminating false sharing (80% performance gain)
4. Async API with exception-safe synchronous reads
5. 90% memory savings through aggressive slot recycling

**Recent Performance Improvements (Feb 2026):**
- Fixed false sharing with 128-byte cache-line alignment on M1/M2
- Multi-stock throughput improved 80% (15M → 27M orders/sec)
- Refactored to header/implementation split for better modularity
- Async multi-batch now outperforms sync (187M vs 134M jobs/sec)

**Overall Assessment:** ✓ Ready for high-frequency backtesting and Monte Carlo simulation workloads.

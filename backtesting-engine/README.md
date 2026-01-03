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

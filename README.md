## Titan: Multi-Agent Market Microstructure Backtesting Engine

Titan is a high‑performance backtesting engine for **algorithmic trading strategies** built on a modern C++20 core with a **Python-first** interface.  
It is designed for realistic **Level‑3 (order‑by‑order)** market simulations, multi‑agent experiments, and research‑grade performance studies.

**Why Titan:** For practitioners and researchers in high-frequency trading (crypto or equities), Titan lets you stress-test and evaluate multiple strategies at once across multiple simulated markets—all concurrently—with a single, deterministic run.

---

### What Titan Gives You

- **Multi‑agent simulation**: Run many strategies competing in the same order book.
- **Realistic microstructure**: FIFO price–time priority, ownership validation, and exchange‑style matching.
- **High throughput**: C++ core capable of tens to hundreds of millions of orders/sec on a single machine.
- **Python strategies**: Write strategies in Python; execution and matching happen in C++ via `pybind11`.
- **Level‑3 data replay**: Parse and replay historical L3 feeds (ADD / MODIFY / CANCEL / EXECUTE).
- **Deterministic experiments**: Same data + same code → identical results run‑to‑run.

---

### Repository Layout

- `backtesting-engine/` – C++20 core:
  - `order_engine.cpp` – price–time priority matching engine
  - `engine_runtime.cpp` – multi‑stock runtime, job scheduling, snapshots
  - `test/` – C++ benchmarks and correctness tests
- `python/titan/` – Python package:
  - `strategy.py` – base `TradingStrategy` abstraction
  - `strategies/` – example strategies (`market_maker.py`, `momentum.py`)
- `examples/` – end‑to‑end backtests and data utilities:
  - `backtest_single.py`, `backtest_multi_agent.py`
  - `backtest_from_tardis.py`, `tardis/` L2/L3 converters and replayers
- `docs/` – user and developer documentation (API, architecture, performance, data format).

---

### Installation (from source)

Prerequisites:
- Python 3.8+
- CMake 3.15+
- A C++20 compiler (clang++ or g++)

```bash
# Clone repository
git clone https://github.com/Donnieboi03/Titan.git
cd Titan

# Python deps
pip install -r requirements.txt

# Build C++ core (via CMake)
mkdir build && cd build
cmake ..
cmake --build . -j8
cd ..

# Install Python package in editable mode
pip install -e .
```

**Quick validation:** From the repo root, run C++ tests with `cd build && ctest` (after building). Once the Python extension is built, run `python examples/backtest_single.py` for a minimal end‑to‑end backtest.

---

### Python Quickstart

```python
import titan

# Initialize runtime (8 workers, 4M order capacity per engine)
runtime = titan.EngineRuntime(num_threads=8, capacity=4 * 1024 * 1024)

# Register a stock
runtime.register_stock("AAPL", ipo_price=150.0, ipo_qty=1_000_000.0)

# Submit a limit order
runtime.submit_limit_order("AAPL", side="BID", price=149.95, qty=100.0)
runtime.process_pending_orders()

# Query market state
price = runtime.get_market_price("AAPL")
depth = runtime.get_market_depth("AAPL", side="BID", depth=10)
print(price, depth)
```

#### Simple strategy example

```python
from titan.strategy import TradingStrategy

class SimpleMarketMaker(TradingStrategy):
    def __init__(self, runtime, user_id, ticker, spread_bps: float = 10.0):
        super().__init__(runtime, user_id)
        self.ticker = ticker
        self.spread_bps = spread_bps

    def on_market_data(self, ticker, best_bid, best_ask):
        if best_bid is None or best_ask is None:
            return

        mid = 0.5 * (best_bid + best_ask)
        spread = mid * (self.spread_bps / 10_000.0)

        self.runtime.submit_limit_order(self.ticker, "BID", mid - spread / 2, 100.0, self.user_id)
        self.runtime.submit_limit_order(self.ticker, "ASK", mid + spread / 2, 100.0, self.user_id)
```

More examples live in `examples/` and the docs.

---

### C++ Core & Performance

- **OrderEngine**:
  - Pure C++20 price–time priority order book per instrument.
  - Top‑K depth caching and lock‑free snapshots for fast reads.
- **EngineRuntime**:
  - Manages many `OrderEngine` instances across worker threads.
  - Batching and job scheduling for high throughput.

Representative performance (depends on hardware and config):

| Scenario                    | Orders/sec (approx.) |
|----------------------------|----------------------|
| Single‑stock stress test   | 100M+               |
| Multi‑stock, 8 workers     | 10M–100M           |

For deeper numbers and methodology, see `docs/performance.md` and `backtesting-engine/PERFORMANCE_REPORT.md`.

---

### Documentation & Learning More

- `docs/installation.md` – build and environment setup
- `docs/quickstart.md` – step‑by‑step first backtest
- `docs/api.md` – Python API reference
- `docs/strategies.md` – writing custom strategies
- `docs/multi_agent.md` – multi‑agent experiments
- `docs/cpp_internals.md` – deep dive into the C++ core

---

### Testing

```bash
# C++ tests (from repo root)
mkdir -p build && cd build
cmake ..
ctest

# Python tests (if present)
cd ..
pytest
```

---

### Contributing & License

- **License**: MIT – see `LICENSE`.
- **Contributions**: Issues and PRs are welcome.  
  Please read `docs/contributing.md` before opening large changes.

If you use Titan in research or production and are allowed to share, consider mentioning it in your paper, blog post, or release notes.

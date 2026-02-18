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

- `core/` – C++20 core:
  - `order_engine.cpp` – price–time priority matching engine
  - `engine_runtime.cpp` – multi‑stock runtime, job scheduling, snapshots
  - `test/` – C++ benchmarks and correctness tests; sample data in `test/examples/`
- `python/titan/` – Python package (C++ bindings via pybind11).
- `python/tests/` – Python tests and utilities:
  - `test_bindings.py`, `test_binance_strategy_throughput.py`, `test_stress_multiworker.py`
  - `download_market_data.py`, `convert_l2_to_csv.py` (Tardis L2 download/convert)
- `docs/` – documentation (installation, quickstart, API).

---

### Installing the Python library

Titan is a Python package with a C++ extension. Install from source as follows.

**Prerequisites**

- **Python** 3.8 or newer  
- **C++20 compiler** (e.g. clang++ on macOS, g++ 10+ or clang++ 11+ on Linux)  
- **CMake** 3.15+ (optional; only needed if you want to run the C++ test suite)

**Steps**

```bash
# 1. Clone the repository
git clone https://github.com/Donnieboi03/Titan.git
cd Titan

# 2. Install Python dependencies (pybind11, numpy, pandas, etc.)
pip install -r requirements.txt

# 3. Install the Titan package (builds the C++ extension via setuptools)
pip install -e .
```

Step 3 compiles the C++ core and creates the `titan` Python package. No separate CMake build is required to use the library.

**Verify installation**

```bash
python -c "import titan; print(titan.__version__)"
# Expected: 0.1.0
```

**Run Python tests (optional)**

From the repo root:

```bash
python python/tests/test_bindings.py
```

**Run C++ tests (optional)**

If you want to run the C++ unit tests as well:

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j8
ctest
cd ..
```

---

### Python Quickstart

```python
import titan

# Initialize runtime (singleton: 8 workers, 4M order capacity)
titan.EngineRuntime.reset_instance()
runtime = titan.EngineRuntime.get_instance(num_threads=8, capacity=4 * 1024 * 1024)

# Register a stock
runtime.register_stock("AAPL", 150.0, 1_000_000.0)

# Submit a limit order
runtime.submit_limit_order("AAPL", "BID", 149.95, 100.0)
runtime.process_pending_orders()

# Query market state
price = runtime.get_market_price("AAPL")
print(price)
```

See `python/tests/test_bindings.py` for more usage and `docs/` for full API and strategy patterns.

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

For deeper numbers and methodology, see `core/PERFORMANCE_REPORT.md` if present.

---

### Documentation

- [docs/README.md](docs/README.md) – documentation index
- [docs/installation.md](docs/installation.md) – install the Python library
- [docs/quickstart.md](docs/quickstart.md) – first backtest
- [docs/api.md](docs/api.md) – Python API reference

---

### Testing

```bash
# Python tests (from repo root)
python python/tests/test_bindings.py

# Optional: C++ tests
mkdir -p build && cd build && cmake .. && cmake --build . -j8 && ctest && cd ..
```

---

### Contributing & License

- **License**: MIT – see `LICENSE`.
- **Contributions**: Issues and PRs are welcome.  
  Open an issue or PR for large changes.

If you use Titan in research or production and are allowed to share, consider mentioning it in your paper, blog post, or release notes.

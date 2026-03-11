import os
import time
from pathlib import Path

# Ensure repo root is on path when run from python/tests/
_repo_root = Path(__file__).resolve().parents[2]
if str(_repo_root) not in __import__("sys").path:
    __import__("sys").path.insert(0, str(_repo_root))

print("============================================================")
print("Titan - Binance Data Strategy Throughput Test (single Python thread)")
print("============================================================")

try:
    from titan import titan_core as tc
except Exception as e:
    print(f"✗ Failed to import titan_core: {e}")
    raise SystemExit(1)

# Default data path relative to repo root
_default_data = _repo_root / "core/test/examples/binance-futures_incremental_book_L2_2024-12-01_BTCUSDT.bin"
DATA_FILE = os.getenv("TITAN_DATA_FILE", str(_default_data))
NUM_WORKERS = int(os.getenv("TITAN_WORKERS", "1"))
CAPACITY = int(os.getenv("TITAN_CAPACITY", "100000"))
BATCH_SIZE = int(os.getenv("TITAN_BATCH", "100000"))
QUANTUM = int(os.getenv("TITAN_QUANTUM", "1000"))
TARGET_ORDERS = int(os.getenv("TITAN_TARGET_ORDERS", "100000"))
BURST_PER_CALLBACK = int(os.getenv("TITAN_BURST", "4"))
TICKER = os.getenv("TITAN_TICKER", "BTCUSDT")

if not os.path.exists(DATA_FILE):
    # Try common fallbacks
    candidates = [
        DATA_FILE.replace(".bin", ".csv.gz"),
        DATA_FILE.replace(".bin", ".csv"),
    ]
    for c in candidates:
        if os.path.exists(c):
            DATA_FILE = c
            break
    if not os.path.exists(DATA_FILE):
        print(f"✗ Data file not found: {DATA_FILE}")
        print("  Set TITAN_DATA_FILE to an existing .bin/.csv(.gz) or run the downloader/converter in core/test/examples.")
        raise SystemExit(1)

# Initialize runtime (internal threads only; Python side stays single-threaded)
print("1) Initializing runtime and registering ticker...")
tc.EngineRuntime.reset_instance()
runtime = tc.EngineRuntime.get_instance(
    num_threads=NUM_WORKERS, max_capacity=CAPACITY, verbose=False, quantum=QUANTUM
)
runtime.set_batch_size(BATCH_SIZE)
ok = runtime.register_stock(TICKER, 30000.0, 1_000_000.0, capacity=CAPACITY)
print(f"   ✓ register_stock({TICKER}) -> {ok}")

# Shared state between data loop and Python strategy callback
state = {"tick": 0, "burst": BURST_PER_CALLBACK}

def make_strategy(state_dict):
    def strategy(user: tc.User):
        t = state_dict.get("tick", 0)
        n = state_dict.get("burst", 4)
        base = 30000.0
        for i in range(n):
            side = "BID" if ((t + i) % 2 == 0) else "ASK"
            price = base * (0.9995 if side == "BID" else 1.0005)
            user.submit_limit_order(side, float(price), 0.001)
        state_dict["tick"] = t + 1
    return strategy

print("2) Registering Python strategy (called by C++ threads)...")
runtime.register_user(TICKER, make_strategy(state), starting_capital=1_000_000.0)

print("3) Running C++-driven simulation (no Python parser.next calls)...")
print(f"   • Using data file: {DATA_FILE}")
if DATA_FILE.endswith(".bin"):
    try:
        size = os.path.getsize(DATA_FILE)
        total_records_est = size // 32  # 32-byte fixed record
        print(f"   • Estimated records (bin): {total_records_est}")
    except Exception:
        pass
started = runtime.simulate(DATA_FILE, TICKER, TARGET_ORDERS)
assert started, "simulate() failed to start"
while runtime.is_simulation_running(TICKER):
    time.sleep(0.05)
m = runtime.get_simulation_metrics(TICKER)
updates = m.market_updates_processed
placed_total = m.orders_placed
elapsed = m.simulation_time_seconds

print("4) Results:")
print(f"   - Workers: {NUM_WORKERS}")
print(f"   - Batch size: {BATCH_SIZE}")
print(f"   - Quantum: {QUANTUM}")
print(f"   - Updates processed: {updates}")
print(f"   - Orders placed: {placed_total}")
if elapsed > 0:
    print(f"   - Elapsed: {elapsed:.3f}s; Throughput: {placed_total/elapsed:,.0f} orders/sec")
else:
    print("   - Elapsed time too small to compute throughput")

print("============================================================")
print("✓ Binance strategy throughput test completed")
print("============================================================")

# Note: uses runtime.simulate() which parses and processes entirely in C++.

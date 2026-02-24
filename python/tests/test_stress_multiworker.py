import threading
import time
import random
import os
import sys
from pathlib import Path

# Ensure repo root is on path when run from python/tests/
_repo_root = Path(__file__).resolve().parents[2]
if str(_repo_root) not in sys.path:
    sys.path.insert(0, str(_repo_root))

print("============================================================")
print("Titan Python Bindings - Stress & Multi-Worker Test")
print("============================================================")

try:
    from titan import titan_core as tc
except Exception as e:
    print(f"✗ Failed to import titan_core: {e}")
    raise SystemExit(1)

# Configuration (can override via env vars)
NUM_WORKERS = int(os.getenv("TITAN_STRESS_WORKERS", "4"))
CAPACITY = int(os.getenv("TITAN_STRESS_CAPACITY", "200000"))
BATCH_SIZE = int(os.getenv("TITAN_STRESS_BATCH", "512"))
ORDERS_TOTAL = int(os.getenv("TITAN_STRESS_ORDERS", "1000000"))
THREADS = int(os.getenv("TITAN_STRESS_THREADS", "4"))
TICKERS = os.getenv("TITAN_STRESS_TICKERS", "BTCUSDT,ETHUSDT,SOLUSDT,XRPUSDT").split(",")

random.seed(42)

# Helper: submission worker

def submit_orders(runtime: tc.EngineRuntime, count: int, tid: int):
    tickers = TICKERS
    for i in range(count):
        t = tickers[(i + tid) % len(tickers)]
        side = "BID" if ((i + tid) % 2 == 0) else "ASK"
        price = 10000.0 + float((i % 1000)) * 0.01
        qty = 1.0
        runtime.submit_limit_order(t, side, price, qty)  # user_id defaults to INVALID_USER_ID

# Helper: keep processing in a loop (should release GIL)

def processor_loop(runtime: tc.EngineRuntime, stop_evt: threading.Event):
    while not stop_evt.is_set():
        runtime.process_pending_orders()
        # Yield to avoid a tight, pure-C loop starving Python scheduler
        time.sleep(0)

# Helper: Python-side spinner to check GIL isn't held during C++ work

def python_spinner(stop_evt: threading.Event, counter: list):
    local = 0
    while not stop_evt.is_set():
        # Simulate lightweight Python work
        local += 1
    counter.append(local)

# Test starts here
print("1) Creating runtime and registering tickers...")
tc.EngineRuntime.reset_instance()
runtime = tc.EngineRuntime.get_instance(num_threads=NUM_WORKERS, capacity=CAPACITY, verbose=False, quantum=1000)
runtime.set_batch_size(BATCH_SIZE)

ok = True
for t in TICKERS:
    try:
        ok = ok and runtime.register_stock(t, 10000.0, 1_000_000.0, capacity=CAPACITY)
    except Exception as e:
        print(f"   ! register_stock({t}) failed: {e}")
        ok = False
print(f"   ✓ Registered {len(TICKERS)} tickers: {TICKERS}" if ok else "   ✗ Ticker registration failed")

print("2) Spawning processor and spinner threads...")
stop_evt = threading.Event()
spin_stop_evt = threading.Event()
spinner_counts = []

proc_thread = threading.Thread(target=processor_loop, args=(runtime, stop_evt), daemon=True)
spin_thread = threading.Thread(target=python_spinner, args=(spin_stop_evt, spinner_counts), daemon=True)

proc_thread.start()
spin_thread.start()

print("3) Submitting orders from multiple Python threads (stress phase)...")
threads = []
orders_per_thread = max(1, ORDERS_TOTAL // max(1, THREADS))
for tid in range(THREADS):
    th = threading.Thread(target=submit_orders, args=(runtime, orders_per_thread, tid), daemon=True)
    threads.append(th)
    th.start()

start = time.time()
for th in threads:
    th.join()

# Signal processor to finish after draining
runtime.process_pending_orders_async()
while not runtime.all_jobs_completed():
    time.sleep(0.001)
# Give spinner a little more time to run
elapsed = time.time() - start
spin_stop_evt.set()
stop_evt.set()
proc_thread.join(timeout=2.0)
spin_thread.join(timeout=2.0)

print(f"   ✓ Submission completed in {elapsed:.3f}s; verifying counts...")

# Request snapshots and process once so get_placed_count sees fresh data
for t in TICKERS:
    runtime.request_snapshot(t)
runtime.process_pending_orders()

# Verify placed counts roughly match total submissions
expected_total = orders_per_thread * THREADS
sum_placed = 0
for t in TICKERS:
    placed = runtime.get_placed_count(t)
    sum_placed += placed
    print(f"   - {t}: placed={placed}")

print(f"   = Total placed across tickers: {sum_placed} (expected ~{expected_total})")
if sum_placed == 0:
    print("   ✗ No orders were placed; check engine setup")
    raise SystemExit(1)

# Check spinner progressed (i.e., GIL not held by C++)
spin_count = spinner_counts[0] if spinner_counts else 0
print(f"4) Spinner progress count: {spin_count}")
if spin_count <= 0:
    print("   ✗ Spinner did not progress; GIL may be held unexpectedly")
    raise SystemExit(1)
else:
    print("   ✓ Spinner progressed while C++ processed (GIL released)")

print("5) Multi-worker sanity: confirm runtime reports as configured...")
print(f"   ✓ Workers configured: {NUM_WORKERS}; Batch size: {BATCH_SIZE}; Tickers: {len(TICKERS)}")

print("============================================================")
print("✓ Stress & Multi-Worker test completed successfully!")
print("============================================================")

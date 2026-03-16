# API Reference

Complete reference for Titan's Python API.

## Core Classes

### EngineRuntime

Central coordinator for all market activity.

#### get_instance()

Get the singleton runtime (create or reuse). Call `reset_instance()` first to reinitialize.

**Process exit:** The package registers `reset_instance` with `atexit`, so when the Python process exits the runtime is torn down and log/record buffers are flushed before shutdown.

```python
EngineRuntime.get_instance(
    num_threads=4,
    verbose=False,
    quantum=1000,
    max_capacity=1048576,
    max_engine_count=100,
    max_strategies=1000
) -> EngineRuntime
```

**Parameters:**
- `num_threads` (int): Number of worker threads. Default: 4
- `verbose` (bool): Enable notification/logging (e.g. order accept/fill/cancel). Default: False
- `quantum` (int): Scheduling quantum in orders. Default: 1000
- `max_capacity` (int): Max order pool size per engine. Default: 1M (1048576)
- `max_engine_count` (int): Reserve space for this many stocks/engines (avoids realloc). Default: 100
- `max_strategies` (int): Reserve space for this many strategies; keeps `UserView*` from `register_user()` valid. Default: 1000

**Example:**
```python
titan.EngineRuntime.reset_instance()
runtime = titan.EngineRuntime.get_instance(num_threads=8, max_capacity=2*1024*1024, verbose=True)
```

---

#### register_stock()

Register a new stock with IPO.

```python
register_stock(ticker: str, ipo_price: float, ipo_qty: float) -> None
```

**Parameters:**
- `ticker` (str): Stock symbol (e.g., "AAPL")
- `ipo_price` (float): Initial offering price in dollars
- `ipo_qty` (float): Initial shares available

**Notes:**
- Creates order book for ticker
- IPO holder assigned `user_id=0` (constant: `IPO_HOLDER`)
- IPO holder owns initial shares placed at IPO price

**Example:**
```python
runtime.register_stock("AAPL", ipo_price=150.0, ipo_qty=1_000_000.0)
```

---

#### submit_limit_order()

Submit a limit order.

```python
submit_limit_order(
    ticker: str,
    side: str,
    price: float,
    qty: float,
    user_id: int = INVALID_USER_ID
) -> int
```

**Parameters:**
- `ticker` (str): Stock symbol
- `side` (str | OrderSide): "BID"/"bid"/"buy" or "ASK"/"ask"/"sell", or `OrderSide.BID` / `OrderSide.ASK`
- `price` (float): Limit price in dollars
- `qty` (float): Order quantity in shares
- `user_id` (int, optional): User identifier. Default: `INVALID_USER_ID` (untracked)

**Returns:**
- `int`: Order ID, or `INVALID_ORDER_ID` if rejected

**Notes:**
- Order ID is globally unique
- Orders are queued until `process_pending_orders()` is called
- ASK orders validated for ownership if user_id tracked

**Example:**
```python
order_id = runtime.submit_limit_order("AAPL", "BID", 149.50, 100.0, user_id=100)
```

---

#### submit_market_order()

Submit a market order (immediate execution).

```python
submit_market_order(
    ticker: str,
    side: str | OrderSide,
    qty: float,
    user_id: int = INVALID_USER_ID
) -> int
```

**Parameters:**
- `ticker` (str): Stock symbol
- `side` (str | OrderSide): "BID"/"ASK" or `OrderSide.BID` / `OrderSide.ASK`
- `qty` (float): Order quantity
- `user_id` (int, optional): User identifier

**Returns:**
- `int`: Order ID, or `INVALID_ORDER_ID` if rejected

**Notes:**
- Executes at best available price
- May partially fill if insufficient liquidity

**Example:**
```python
order_id = runtime.submit_market_order("AAPL", "BID", 1000.0, user_id=100)
```

---

#### submit_cancel_order()

Cancel an existing order.

```python
submit_cancel_order(
    ticker: str,
    order_id: int,
    user_id: int = INVALID_USER_ID
) -> bool
```

**Parameters:**
- `ticker` (str): Stock symbol
- `order_id` (int): Order ID to cancel
- `user_id` (int, optional): User identifier (for validation)

**Returns:** `True` if the cancel was accepted, `False` on failure (e.g. order not found).

**Notes:**
- Only owner can cancel order
- No-op if order already filled

**Example:**
```python
runtime.submit_cancel_order("AAPL", order_id=12345, user_id=100)
```

---

#### submit_replace_order()

Replace an existing order (new price and/or quantity). Full replace: order is removed from the book and re-inserted at the new price/quantity; loses time priority.

```python
submit_replace_order(
    ticker: str,
    order_id: int,
    new_price: float,
    new_qty: float,
    user_id: int = INVALID_USER_ID
) -> bool
```

**Parameters:** `ticker`, `order_id`, `new_price`, `new_qty`, `user_id` (optional).

**Returns:** `True` if accepted, `False` on failure.

**Example:**
```python
runtime.submit_replace_order("AAPL", order_id=12345, new_price=149.75, new_qty=50.0, user_id=100)
```

---

#### submit_edit_order()

Edit an existing order's quantity only (same price). O(1) in-engine update; keeps time priority.

```python
submit_edit_order(
    ticker: str,
    order_id: int,
    new_qty: float,
    user_id: int = INVALID_USER_ID
) -> bool
```

**Parameters:** `ticker`, `order_id`, `new_qty`, `user_id` (optional). No `new_price` — use `submit_replace_order` to change price.

**Returns:** `True` if accepted, `False` on failure.

**Example:**
```python
runtime.submit_edit_order("AAPL", order_id=12345, new_qty=75.0, user_id=100)
```

---

#### request_snapshot()

Request a snapshot refresh for a ticker. The update is applied when the next `process_pending_orders()` runs. Use the **request → process → get** pattern when you need up-to-date market data or engine stats after processing.

```python
request_snapshot(ticker: str) -> bool
```

**Parameters:**
- `ticker` (str): Stock symbol (must be registered).

**Returns:** `True` if the ticker exists and the snapshot job was queued; `False` otherwise.

**Pattern:** For fresh snapshot-derived data (best bid/ask, placed/filled counts, utilization, etc.), call `request_snapshot(ticker)` before `process_pending_orders()`, then call your getters (e.g. `get_best_bid`, `get_placed_count`) after processing.

**Example:**
```python
runtime.request_snapshot("AAPL")
runtime.process_pending_orders()
bid = runtime.get_best_bid("AAPL")
placed = runtime.get_placed_count("AAPL")
```

---

#### process_pending_orders()

Execute all pending orders.

```python
process_pending_orders() -> None
```

**Notes:**
- Blocks until all workers complete
- Must be called after order submission
- Processes orders across all stocks in parallel
- For up-to-date snapshot data after processing, call `request_snapshot(ticker)` before `process_pending_orders()`, then read market/stats (see **request_snapshot()**).

**Example:**
```python
runtime.submit_limit_order("AAPL", "BID", 149.50, 100.0)
runtime.submit_limit_order("MSFT", "BID", 299.50, 50.0)
runtime.request_snapshot("AAPL")
runtime.request_snapshot("MSFT")
runtime.process_pending_orders()  # Execute both; snapshot caches updated
```

---

#### process_pending_orders_async()

Process pending orders asynchronously (non-blocking). Optional overload to process a single ticker.

```python
process_pending_orders_async() -> None
process_pending_orders_async(ticker: str) -> None
```

**Parameters:**
- `ticker` (str, optional): If provided, process only orders for this ticker.

---

#### simulate()

Run a C++-driven simulation: parse an L2 data file and apply updates to the order book for a ticker. The simulation job runs on a worker thread; poll for completion with `is_simulation_running(ticker)` and read results with `get_simulation_metrics(ticker)`.

Orders are matched as the L2 stream is applied.

```python
simulate(
    filepath: str,
    ticker: str,
    target_orders: int = 0,
    price_sample_size: int = 10,
    shares_outstanding: float = 1000000.0,
    record_path: str = ""
) -> bool
```

**Parameters:**
- `filepath`: Path to `.bin`, `.csv`, or `.csv.gz` L2 data file.
- `ticker`: Symbol to register and simulate (stock is registered internally if needed; initial price sampled from data).
- `target_orders`: Stop after this many market updates (0 = process entire file).
- `price_sample_size`: Number of data points to sample for IPO price. Default: 10.
- `shares_outstanding`: Total shares for IPO registration. Default: 1000000.0.
- `record_path`: If non-empty, enable L2 recording to this path during simulation.

**Returns:** `True` if the simulation job was started successfully, `False` on error (e.g. file not found, ticker invalid).

**Example:**
```python
runtime.simulate("data/btcusdt_l2.csv", "BTCUSDT", target_orders=0)
while runtime.is_simulation_running("BTCUSDT"):
    time.sleep(0.05)
metrics = runtime.get_simulation_metrics("BTCUSDT")
print(metrics.orders_placed, metrics.simulation_time_seconds)
```

---

#### set_auto_match() / get_auto_match()

**Advanced.** Enable or disable automatic order matching for a ticker. When disabled, orders are queued and are matched only when you call `set_auto_match(ticker, True)` (in arrival order). `simulate()` always runs with matching enabled.

```python
set_auto_match(ticker: str, auto_match: bool) -> bool
get_auto_match(ticker: str) -> bool
```

**Parameters:**
- `ticker`: Stock symbol (must be registered).
- `auto_match`: `True` to match orders immediately on place/replace; `False` to queue and match only when toggled back to `True` (drain).

**Returns:** `set_auto_match` returns `True` if the ticker exists; `get_auto_match` returns the current setting for the ticker.

---

#### get_quantum()

Return the current quantum (order count between strategy runs, snapshot updates, and L2 record snapshots). Set at runtime creation via `get_instance(..., quantum=1000)`.

```python
get_quantum() -> int
```

---

#### set_notify_order() / get_notify_order()

Enable or query order-fill notifications. When enabled and the runtime was created with `verbose=True`, the engine emits events (e.g. to stdout) with **EventKind** (ACCEPT, REJECT, MODIFY, PARTIAL_FILL, FILL, CANCEL). Reject events use **RejectReason** (e.g. NO_MARKET_LIQUIDITY, ENGINE_FULL, ORDER_NOT_FOUND). No strategy registration is required to see these notifications. No Python callback is invoked unless the bindings add one; current behavior is C++-side only (e.g. printing). Disable in production for maximum throughput.

```python
set_notify_order(enable: bool) -> None
get_notify_order() -> bool
```

---

#### set_record() / get_record() / get_record_type()

Enable or disable per-ticker L2 recording and choose the **recording mode**. Recording is lock-free and applies to both simulation (e.g. `simulate()`) and live order flow. Output is written by the event management thread.

**Recording modes (`RecordType`):**
- **TOPK** (default): At each **quantum**, write a full **order book snapshot** (top 20 bid and 20 ask levels, L2 format) to CSV. Same cadence as strategy callbacks. Default path `{ticker}.csv`, or use `path_override`.
- **FEATURES**: At each quantum, write one row of **feature scalars** (timestamp, best_bid, best_ask, mid_price, spread, order_imbalance, spread_bps) to a CSV (e.g. `{ticker}_features.csv`). Suited for training pipelines.

Calls that do not pass `record_type` default to **TOPK**.

```python
set_record(ticker: str, enable: bool) -> None
set_record(ticker: str, enable: bool, path_override: str) -> None
set_record(ticker: str, enable: bool, path_override: str, record_type: RecordType) -> None
get_record(ticker: str) -> bool
get_record_type(ticker: str) -> RecordType
```

**Parameters:**
- `ticker`: Stock symbol (must be registered).
- `enable`: `True` to record, `False` to stop.
- `path_override` (optional): Custom output path (e.g. `"output/AAPL.csv"`). When provided, recordings go to this file (or, for FEATURES, a derived path such as `output/AAPL_features.csv`).
- `record_type` (optional): One of `RecordType.TOPK` or `RecordType.FEATURES`. Omitted calls default to `RecordType.TOPK`.

**Example:**
```python
import titan.titan_core as tc

runtime.register_stock("AAPL", 150.0, 1_000_000.0)
runtime.set_record("AAPL", True)  # TOPK (default): book snapshot every quantum to AAPL.csv
runtime.set_record("AAPL", True, "recordings/aapl.csv")  # Custom path, still TOPK
runtime.set_record("AAPL", True, "", tc.RecordType.FEATURES)  # Feature rows to AAPL_features.csv
runtime.simulate("data/aapl_l2.csv", "AAPL")
print(runtime.get_record("AAPL"))   # True
print(runtime.get_record_type("AAPL"))  # RecordType.FEATURES
runtime.set_record("AAPL", False)  # Stop recording
```

**See also:** [L2 Data and Recording](l2_data_and_recording.md) – Recording modes and when to use each.

---

#### get_order()

Look up an order by ticker and order ID.

```python
get_order(ticker: str, order_id: int) -> Optional[OrderInfo]
```

**Returns:** A **copy** of `OrderInfo` if the order exists and is valid, else `None`. The returned value is safe to hold after `process_pending_orders()` or other engine steps; it is not a reference to internal state.

**Example:**
```python
info = runtime.get_order("AAPL", order_id)
if info is not None:
    print(info)  # e.g. <OrderInfo BID LIMIT $149.50 x 100.0 [OPEN]>
```

---

#### register_user()

Register a strategy (callable) for a specific ticker. The strategy is bound to that ticker and invoked every **quantum** (every N orders on that ticker)—the same cadence as snapshot updates and L2 recording. The quantum interval is set at runtime creation via `get_instance(..., quantum=1000)` and can be read with `get_quantum()`.

```python
register_user(
    ticker: str,
    strategy: Callable[[User], None],
    starting_capital: float = 100000.0
) -> Optional[UserView]
```

**Parameters:**
- `ticker` (str): Stock symbol this strategy is bound to (must already be registered via `register_stock()`).
- `strategy` (callable): Function receiving a **`User`** handle. The strategy is bound to this ticker; inside the callback, **`User`** methods (submit_limit_order, get_best_bid, get_positions, etc.) do **not** take a ticker argument—they operate on the registered ticker only. Called on each quantum for this ticker.
- `starting_capital` (float): Initial cash balance. Default: 100000.0

**Returns:** **`UserView`** handle for observing positions/PnL (e.g. `get_snapshot()`, `get_capital()`, `get_user_id()`), or `None` if ticker not found. The returned handle does **not** expose order submission; to place orders from the main thread use `runtime.submit_limit_order(ticker, side, price, qty, user_id=view.get_user_id())`. For order IDs use `runtime.get_positions(view.get_user_id(), ticker)` and `runtime.get_active_orders(view.get_user_id(), ticker)`.

---

#### unregister_user()

Remove a registered strategy (user) by user ID. When a stock is unregistered, all strategies for that ticker are automatically unregistered.

```python
unregister_user(user_id: int) -> bool
```

**Returns:** `True` on success, `False` if user_id not found.

---

#### reset_user()

Resets the given user to the state they were in when first registered: cancels all of the user's orders, restores capital to the initial value from registration, zeros position and PnL, and clears run-stats (mean_return, variance_of_returns, max_drawdown_pct, n_returns). Use before a new run so the user starts clean and stats reflect only that run.

```python
reset_user(user_id: int) -> bool
```

**Parameters:** `user_id` — from `UserView.get_user_id()`.

**Returns:** `True` if the user exists and was reset, `False` otherwise.

---

#### get_market_price()

Get current market price (last trade). Value comes from the snapshot cache. For data that reflects the just-processed orders, use **request_snapshot(ticker)** before **process_pending_orders()**, then call this getter.

```python
get_market_price(ticker: str) -> float
```

**Parameters:**
- `ticker` (str): Stock symbol

**Returns:**
- `float`: Last traded price, or IPO price if no trades yet

**Example:**
```python
runtime.request_snapshot("AAPL")
runtime.process_pending_orders()
price = runtime.get_market_price("AAPL")
print(f"AAPL: ${price:.2f}")
```

---

#### get_best_bid() / get_best_ask()

Get best bid or ask price. Values come from the snapshot cache. For data that reflects the just-processed orders, use **request_snapshot(ticker)** before **process_pending_orders()**, then call these getters.

```python
get_best_bid(ticker: str) -> float
get_best_ask(ticker: str) -> float
```

**Parameters:**
- `ticker` (str): Stock symbol

**Returns:**
- `float`: Best price, or `0.0` if no orders

**Example:**
```python
runtime.request_snapshot("AAPL")
runtime.process_pending_orders()
bid = runtime.get_best_bid("AAPL")
ask = runtime.get_best_ask("AAPL")
spread = ask - bid
```

---

#### get_market_depth()

Get order book depth (multiple levels). The engine exposes up to **20** levels per side.

```python
get_market_depth(ticker: str, side: str | OrderSide, depth: int = 20) -> List[Tuple[float, float]]
```

**Parameters:**
- `ticker` (str): Stock symbol
- `side` (str | OrderSide): "BID"/"ASK" or `OrderSide.BID` / `OrderSide.ASK`
- `depth` (int, optional): Number of levels to return (up to 20). Default: 20

**Returns:**
- `List[Tuple[float, float]]`: List of (price, total_qty) tuples

**Example:**
```python
bid_levels = runtime.get_market_depth("AAPL", "BID", depth=10)
for price, qty in bid_levels:
    print(f"${price:.2f}: {qty:.0f} shares")
```

---

#### get_positions()

Get user's open orders for a stock.

```python
get_positions(user_id: int, ticker: str) -> List[int]
```

**Parameters:**
- `user_id` (int): User identifier
- `ticker` (str): Stock symbol

**Returns:**
- `List[int]`: List of order IDs

**Example:**
```python
orders = runtime.get_positions(user_id=100, ticker="AAPL")
print(f"User 100 has {len(orders)} open AAPL orders")
```

---

#### get_active_orders()

Get a user's active (open, unfilled, uncancelled) order IDs for a stock.

```python
get_active_orders(user_id: int, ticker: str) -> List[int]
```

**Parameters:**
- `user_id` (int): User identifier
- `ticker` (str): Stock symbol

**Returns:**
- `List[int]`: List of active order IDs

**Example:**
```python
active = runtime.get_active_orders(user_id=100, ticker="AAPL")
print(f"User 100 has {len(active)} active AAPL orders")
```

---

#### Statistics Methods

```python
get_placed_count(ticker: str) -> int
get_filled_count(ticker: str) -> int
get_cancelled_count(ticker: str) -> int
get_open_count(ticker: str) -> int
```

**Returns:** Count of orders by state. Values come from the snapshot cache. For counts that reflect the just-processed orders, call **request_snapshot(ticker)** before **process_pending_orders()**, then these getters.

**Example:**
```python
runtime.request_snapshot("AAPL")
runtime.process_pending_orders()
print(f"Placed: {runtime.get_placed_count('AAPL')}")
print(f"Filled: {runtime.get_filled_count('AAPL')}")
print(f"Open: {runtime.get_open_count('AAPL')}")
```

---

### L3CSVParser

Parse Level-3 orderbook CSV files.

#### Constructor

```python
L3CSVParser(filepath: str)
```

**Parameters:**
- `filepath` (str): Path to CSV file

**Example:**
```python
parser = titan.L3CSVParser("data/AAPL_20250115.csv")
```

---

#### parse_header()

Parse CSV header line.

```python
parse_header() -> bool
```

**Returns:**
- `bool`: True if successful

**Notes:**
- Must be called before `parse_next_event()`
- Validates required columns present

**Example:**
```python
if not parser.parse_header():
    print("Invalid CSV format")
```

---

#### parse_next_event()

Parse next event from CSV.

```python
parse_next_event() -> Optional[L3Event]
```

**Returns:**
- `L3Event`: Parsed event, or `None` if EOF

**Example:**
```python
while True:
    event = parser.parse_next_event()
    if event is None:
        break
    
    print(f"Event: {event.event_type}, Order: {event.order_id}")
```

---

### L3Event

Level-3 order event.

#### Attributes

```python
class L3Event:
    timestamp: int        # Nanoseconds since epoch
    sequence: int         # Sequence number (optional)
    event_type: EventType # ADD, MODIFY, CANCEL, EXECUTE
    order_id: int         # Order identifier
    side: Side            # BUY or SELL
    price: int            # Price in cents (integer)
    quantity: int         # Quantity in shares
    symbol: str           # Stock symbol (optional)
```

---

### TradingStrategy

Optional base class that provides callback hooks (e.g. on_order_fill, on_market_data). Use it if you want event-driven callbacks; you can also use a plain callable with `register_user()`. How you implement trading logic is up to you.

#### Constructor

```python
TradingStrategy(runtime: EngineRuntime, user_id: int)
```

**Parameters:**
- `runtime`: EngineRuntime instance
- `user_id`: Unique identifier for this agent

---

#### Callback Methods

Optional hooks; override only what you use:

```python
def on_order_accept(ticker: str, order_id: int, side: str, price: float, qty: float):
    """Called when order accepted."""
    pass

def on_order_fill(ticker: str, order_id: int, fill_price: float, fill_qty: float, remaining_qty: float):
    """Called when order filled."""
    pass

def on_order_reject(ticker: str, order_id: int, reason: str):
    """Called when order rejected."""
    pass

def on_order_cancel(ticker: str, order_id: int):
    """Called when order cancelled."""
    pass

def on_market_data(ticker: str, best_bid: float, best_ask: float):
    """Called on top-of-book update."""
    pass

def on_timer(timestamp_ns: int):
    """Called at regular intervals."""
    pass

def on_start():
    """Strategy initialization."""
    pass

def on_stop():
    """Strategy teardown."""
    pass
```

**Example (minimal; illustrates the hook only):**
```python
class MyStrategy(TradingStrategy):
    def on_market_data(self, ticker, best_bid, best_ask):
        # Your logic here
        pass
```

---

## Constants

```python
INVALID_USER_ID: int  # -1 (uint32_t max)
INVALID_ORDER_ID: int       # Invalid/rejected order ID (uint64_t max)
IPO_HOLDER: int       # 0
```

---

## Enumerations

### EventKind

Order/engine event type (used in notifications and reject reasons).

```python
class EventKind(Enum):
    NONE = 0
    ACCEPT = 1
    REJECT = 2
    MODIFY = 3
    PARTIAL_FILL = 4
    FILL = 5
    CANCEL = 6
```

### RejectReason

Reason for order rejection.

```python
class RejectReason(Enum):
    NO_MARKET_LIQUIDITY = 0
    ENGINE_FULL = 1
    ORDER_NOT_FOUND = 2
```

### EventType

```python
class EventType(Enum):
    ADD = 0      # New order
    MODIFY = 1   # Order modified
    CANCEL = 2   # Order cancelled
    EXECUTE = 3  # Order executed
```

### Side

```python
class Side(Enum):
    BUY = 0   # Buy side (bid)
    SELL = 1  # Sell side (ask)
```

### OrderSide

```python
class OrderSide(Enum):
    BID = 0  # Buy order
    ASK = 1  # Sell order
```

---

### UserView and UserSnapshot

**`UserView`** is the type returned by `register_user()`. It exposes only observational methods: `get_snapshot()`, `get_capital()`, `get_realized_pnl()`, `get_total_volume()`, `get_user_id()`, `get_ticker()`, `get_position()`, `get_all_positions()`, `get_committed_sell_qty()`, `get_unrealized_pnl()`. (Each strategy is tied to one ticker, so position/committed/unrealized PnL are scalars.) `get_unrealized_pnl()` returns the snapshot value (mark-to-market at the strategy ticker's market price as of the last snapshot update). It does **not** expose `submit_limit_order` or other order submission; use `runtime.submit_*(..., user_id=view.get_user_id())` for that. For order IDs (positions/active orders) use `runtime.get_positions(view.get_user_id(), ticker)` and `runtime.get_active_orders(view.get_user_id(), ticker)`.

**`UserSnapshot`** is the struct returned by `UserView.get_snapshot()`. It contains a copy of user state updated each quantum: `user_id`, `capital`, `realized_pnl`, `total_volume`, `ticker`, `position`, `avg_price`, `committed_sell_qty`, `unrealized_pnl` (single-ticker per strategy), plus run-stats: `sum_pnl_deltas`, `sum_sq_pnl_deltas`, `n_returns`, `max_drawdown_pct` (stored), and `mean_return` and `variance_of_returns` (computed on demand, like SimulationMetrics rates). Call `runtime.reset_user(user_id)` to reset the user to just-registered state (cancel orders, restore initial capital, clear run-stats) for a fresh run. Safe to hold and inspect from the main thread.

**`User`** is the type passed **into** the strategy callback. It extends `UserView` and adds full order submission and market data. All methods operate on the strategy's registered ticker; **no ticker parameter** is passed (e.g. `user.submit_limit_order(side, price, qty)`, `user.get_best_bid()`, `user.get_positions()`, `user.get_order_info(order_id)`). Use it only inside the strategy callable.

---

### OrderInfo

Returned by `get_order(ticker, order_id)` and `User.get_order_info(order_id)` (User is bound to one ticker). **Lifetime:** The API returns a **copy** of the order snapshot, not a pointer or reference. You may store and use the returned `OrderInfo` after calling `process_pending_orders()` or other engine steps; it will not be invalidated.

**Attributes:** `side`, `type`, `status`, `time`, plus `get_price_dollars()`, `get_qty()`. Has a readable `__repr__` (e.g. `<OrderInfo BID LIMIT $149.50 x 100.0 [OPEN]>`).

---

## Type Aliases

```python
OrderId = int      # uint64_t
UserId = int       # uint32_t
Price = float      # double
Quantity = float   # double
Timestamp = int    # uint64_t (nanoseconds)
```

---

## Next Steps

- [Quick Start](quickstart.md) – Tutorial and examples
- [Python tests](../python/tests/) – Bindings and utility scripts

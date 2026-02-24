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
    capacity=1048576,
    verbose=False,
    quantum=1000
) -> EngineRuntime
```

**Parameters:**
- `num_threads` (int): Number of worker threads. Default: 4
- `capacity` (int): Maximum orders per engine. Default: 1M (1048576)
- `verbose` (bool): Enable notification/logging (e.g. order accept/fill/cancel). Default: False
- `quantum` (int): Scheduling quantum in orders. Default: 1000

**Example:**
```python
titan.EngineRuntime.reset_instance()
runtime = titan.EngineRuntime.get_instance(num_threads=8, capacity=2*1024*1024, verbose=True)
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

#### submit_edit_order()

Modify an existing order.

```python
submit_edit_order(
    ticker: str,
    order_id: int,
    new_price: float,
    new_qty: float,
    user_id: int = INVALID_USER_ID
) -> bool
```

**Parameters:**
- `ticker` (str): Stock symbol
- `order_id` (int): Order ID to modify
- `new_price` (float): New limit price
- `new_qty` (float): New quantity
- `user_id` (int, optional): User identifier (for validation)

**Returns:** `True` if the edit was accepted, `False` on failure (e.g. order not found).

**Notes:**
- Only owner can edit order
- Loses time priority at new price level

**Example:**
```python
runtime.submit_edit_order("AAPL", order_id=12345, new_price=149.75, user_id=100)
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

#### set_record() / get_record()

Enable or disable per-ticker L2 recording. When enabled, the **order book snapshot** (top 20 bid and 20 ask levels, in L2 format) is written at each **quantum**—the same cadence as strategy callbacks and snapshot updates. Recording is lock-free and applies to both simulation (e.g. `simulate()`) and live order flow (e.g. `submit_limit_order`, `process_pending_orders`). Each quantum emits up to 40 L2 rows (20 bid + 20 ask) for that ticker. Output is written by the event management thread to the default path `{ticker}.csv`, or to a custom path when provided.

```python
set_record(ticker: str, enable: bool) -> None
set_record(ticker: str, enable: bool, path_override: str) -> None
get_record(ticker: str) -> bool
```

**Parameters:**
- `ticker`: Stock symbol (must be registered).
- `enable`: `True` to record, `False` to stop.
- `path_override` (optional): Custom output path (e.g. `"output/AAPL.csv"`). When provided, recordings go to this file instead of the default `{ticker}.csv`.

**Example:**
```python
runtime.register_stock("AAPL", 150.0, 1_000_000.0)
runtime.set_record("AAPL", True)  # Enable quantum-based book snapshot recording to AAPL.csv
# Or with custom path:
runtime.set_record("AAPL", True, "recordings/aapl_replay.csv")
runtime.simulate("data/aapl_l2.csv", "AAPL")  # (or use live orders + process_pending_orders)
print(runtime.get_record("AAPL"))  # True
runtime.set_record("AAPL", False)  # Stop recording
```

**See also:** [L2 Data and Recording](l2_data_and_recording.md) – When to use runtime snapshots vs an incremental stream for replay and analysis.

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

#### register_strategy()

Register a strategy (callable) for a specific ticker. The strategy is bound to that ticker and invoked every **quantum** (every N orders on that ticker)—the same cadence as snapshot updates and L2 recording. The quantum interval is set at runtime creation via `get_instance(..., quantum=1000)` and can be read with `get_quantum()`.

```python
register_strategy(
    ticker: str,
    strategy: Callable[[User], None],
    starting_capital: float = 100000.0
) -> Optional[User]
```

**Parameters:**
- `ticker` (str): Stock symbol this strategy is bound to (must already be registered via `register_stock()`).
- `strategy` (callable): Function receiving a `User` handle; called on each quantum for this ticker.
- `starting_capital` (float): Initial cash balance. Default: 100000.0

**Returns:** `User` handle for positions/PnL, or `None` if ticker not found.

---

#### unregister_strategy()

Remove a registered strategy (user) by user ID. When a stock is unregistered, all strategies for that ticker are automatically unregistered.

```python
unregister_strategy(user_id: int) -> bool
```

**Returns:** `True` on success, `False` if user_id not found.

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

Optional base class that provides callback hooks (e.g. on_order_fill, on_market_data). Use it if you want event-driven callbacks; you can also use a plain callable with `register_strategy()`. How you implement trading logic is up to you.

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

### OrderInfo

Returned by `get_order(ticker, order_id)` and `User.get_order_info(ticker, order_id)`. **Lifetime:** The API returns a **copy** of the order snapshot, not a pointer or reference. You may store and use the returned `OrderInfo` after calling `process_pending_orders()` or other engine steps; it will not be invalidated.

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

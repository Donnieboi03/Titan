# API Reference

Complete reference for Titan's Python API.

## Core Classes

### EngineRuntime

Central coordinator for all market activity.

#### Constructor

```python
EngineRuntime(num_threads=4, capacity=1048576, verbose=False)
```

**Parameters:**
- `num_threads` (int): Number of worker threads. Default: 4
- `capacity` (int): Maximum orders per engine. Default: 1M (1048576)
- `verbose` (bool): Enable debug logging. Default: False

**Example:**
```python
runtime = titan.EngineRuntime(num_threads=8, capacity=2*1024*1024)
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
- `side` (str): "BID" (buy) or "ASK" (sell)
- `price` (float): Limit price in dollars
- `qty` (float): Order quantity in shares
- `user_id` (int, optional): User identifier. Default: `INVALID_USER_ID` (untracked)

**Returns:**
- `int`: Order ID, or `INVALID_ID` if rejected

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
    side: str,
    qty: float,
    user_id: int = INVALID_USER_ID
) -> int
```

**Parameters:**
- `ticker` (str): Stock symbol
- `side` (str): "BID" (buy) or "ASK" (sell)
- `qty` (float): Order quantity
- `user_id` (int, optional): User identifier

**Returns:**
- `int`: Order ID, or `INVALID_ID` if rejected

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
) -> None
```

**Parameters:**
- `ticker` (str): Stock symbol
- `order_id` (int): Order ID to cancel
- `user_id` (int, optional): User identifier (for validation)

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
    user_id: int = INVALID_USER_ID
) -> None
```

**Parameters:**
- `ticker` (str): Stock symbol
- `order_id` (int): Order ID to modify
- `new_price` (float): New limit price
- `user_id` (int, optional): User identifier (for validation)

**Notes:**
- Only owner can edit order
- Loses time priority at new price level

**Example:**
```python
runtime.submit_edit_order("AAPL", order_id=12345, new_price=149.75, user_id=100)
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

**Example:**
```python
runtime.submit_limit_order("AAPL", "BID", 149.50, 100.0)
runtime.submit_limit_order("MSFT", "BID", 299.50, 50.0)
runtime.process_pending_orders()  # Execute both
```

---

#### get_market_price()

Get current market price (last trade).

```python
get_market_price(ticker: str) -> float
```

**Parameters:**
- `ticker` (str): Stock symbol

**Returns:**
- `float`: Last traded price, or IPO price if no trades yet

**Example:**
```python
price = runtime.get_market_price("AAPL")
print(f"AAPL: ${price:.2f}")
```

---

#### get_best_bid() / get_best_ask()

Get best bid or ask price.

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
bid = runtime.get_best_bid("AAPL")
ask = runtime.get_best_ask("AAPL")
spread = ask - bid
```

---

#### get_market_depth()

Get order book depth (multiple levels).

```python
get_market_depth(ticker: str, side: str, depth: int = 5) -> List[Tuple[float, float]]
```

**Parameters:**
- `ticker` (str): Stock symbol
- `side` (str): "BID" or "ASK"
- `depth` (int): Number of levels to return. Default: 5

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

#### Statistics Methods

```python
get_placed_count(ticker: str) -> int
get_filled_count(ticker: str) -> int
get_cancelled_count(ticker: str) -> int
get_open_count(ticker: str) -> int
```

**Returns:** Count of orders by state

**Example:**
```python
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

Base class for trading strategies.

#### Constructor

```python
TradingStrategy(runtime: EngineRuntime, user_id: int)
```

**Parameters:**
- `runtime`: EngineRuntime instance
- `user_id`: Unique identifier for this agent

---

#### Callback Methods

Override these in your strategy:

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

**Example:**
```python
class MyStrategy(TradingStrategy):
    def on_market_data(self, ticker, best_bid, best_ask):
        mid = (best_bid + best_ask) / 2
        self.runtime.submit_limit_order(ticker, "BID", mid - 0.05, 100.0, self.user_id)
```

---

## Constants

```python
INVALID_USER_ID: int  # -1 (uint32_t max)
INVALID_ID: int       # -1 (uint64_t max)
IPO_HOLDER: int       # 0
```

---

## Enumerations

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

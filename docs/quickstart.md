# Quick Start Tutorial

Get up and running with Titan in a few minutes. Make sure you have [installed the Python library](installation.md) first.

## Your first backtest

### Step 1: Import Titan

```python
import titan
```

### Step 2: Initialize the runtime

Titan uses a **singleton** runtime. Get the instance (and optionally reset it first):

```python
titan.EngineRuntime.reset_instance()
runtime = titan.EngineRuntime.get_instance(
    num_threads=4,
    capacity=1024 * 1024,
    verbose=False
)
```

**Parameters:**

- `num_threads`: Number of worker threads (default: 1)
- `capacity`: Maximum orders per order book (default: 1M)
- `verbose`: Enable debug logging (default: False)
- `quantum`: Scheduling quantum (default: 1000)

### Step 3: Register a stock

```python
# Register AAPL: initial price $150, 1M shares (IPO quantity)
runtime.register_stock("AAPL", 150.0, 1_000_000.0)
```

This creates an order book for AAPL and an IPO holder with 1M shares at $150.

### Step 4: Submit orders

```python
# Submit a limit order: BUY 100 shares @ $149.50
runtime.submit_limit_order("AAPL", "BID", 149.50, 100.0)

# Process pending orders (matching)
runtime.process_pending_orders()
```

Order APIs:

- `submit_limit_order(ticker, side, price, qty, user_id=...)` – limit order
- `submit_market_order(ticker, side, qty, user_id=...)` – market order
- `submit_cancel_order(ticker, order_id, user_id=...)` – cancel order
- `submit_edit_order(ticker, order_id, new_price, new_qty, user_id=...)` – modify order

Use `"BID"` or `"ASK"` for `side`.

### Step 5: Query market state

```python
price = runtime.get_market_price("AAPL")
best_bid = runtime.get_best_bid("AAPL")
best_ask = runtime.get_best_ask("AAPL")
print(f"Market: ${price:.2f}  Bid: ${best_bid:.2f}  Ask: ${best_ask:.2f}")

# Order book depth (list of (price, qty) tuples)
bid_levels = runtime.get_market_depth("AAPL", "BID", depth=5)
for p, q in bid_levels:
    print(f"  ${p:.2f}: {q:.0f} shares")
```

### Step 6: View statistics

```python
print(f"Placed: {runtime.get_placed_count('AAPL')}")
print(f"Filled: {runtime.get_filled_count('AAPL')}")
print(f"Cancelled: {runtime.get_cancelled_count('AAPL')}")
print(f"Open: {runtime.get_open_count('AAPL')}")
```

## Complete minimal example

```python
import titan

def main():
    titan.EngineRuntime.reset_instance()
    runtime = titan.EngineRuntime.get_instance(num_threads=4, capacity=1024 * 1024)

    runtime.register_stock("AAPL", 150.0, 1_000_000.0)

    runtime.submit_limit_order("AAPL", "BID", 149.50, 100.0)
    runtime.submit_limit_order("AAPL", "ASK", 150.50, 100.0)
    runtime.process_pending_orders()

    print(f"Market price: ${runtime.get_market_price('AAPL'):.2f}")
    print(f"Best bid: ${runtime.get_best_bid('AAPL'):.2f}")
    print(f"Best ask: ${runtime.get_best_ask('AAPL'):.2f}")
    print(f"Placed: {runtime.get_placed_count('AAPL')}, Filled: {runtime.get_filled_count('AAPL')}")

if __name__ == "__main__":
    main()
```

## Using a Python strategy (callback)

You register a **callable** that receives a `User` handle and submits orders on behalf of that user.

### Step 1: Define a strategy function

```python
def my_strategy(user):
    """Called by the engine; user is a titan.User handle."""
    for ticker in user.list_tickers():
        bid = user.get_best_bid(ticker)
        ask = user.get_best_ask(ticker)
        if bid > 0 and ask > 0:
            mid = (bid + ask) / 2.0
            user.submit_limit_order(ticker, "BID", mid - 1.0, 10.0)
            user.submit_limit_order(ticker, "ASK", mid + 1.0, 10.0)
```

### Step 2: Register the strategy

```python
trader = runtime.register_strategy(my_strategy, starting_capital=50_000.0)
print(f"User ID: {trader.get_user_id()}, Capital: ${trader.get_capital():.2f}")
```

### Step 3: Add liquidity and process

```python
runtime.submit_limit_order("AAPL", "BID", 149.0, 50.0)
runtime.submit_limit_order("AAPL", "ASK", 151.0, 50.0)
runtime.process_pending_orders()
```

### Step 4: Inspect the strategy’s state

```python
capital = trader.get_capital()
position = trader.get_position("AAPL")
active = trader.get_active_orders("AAPL")
print(f"Capital: ${capital:.2f}, Position: {position:.2f}, Active orders: {len(active)}")
```

See `python/tests/test_bindings.py` for more.

## Multi-stock example

```python
titan.EngineRuntime.reset_instance()
runtime = titan.EngineRuntime.get_instance(num_threads=4, capacity=1024 * 1024)

for ticker, price in [("AAPL", 150.0), ("MSFT", 300.0), ("GOOGL", 2800.0)]:
    runtime.register_stock(ticker, price, 1_000_000.0)

runtime.submit_limit_order("AAPL", "BID", 149.50, 100.0)
runtime.submit_limit_order("MSFT", "BID", 299.50, 50.0)
runtime.submit_limit_order("GOOGL", "BID", 2799.00, 10.0)
runtime.process_pending_orders()

for ticker in ["AAPL", "MSFT", "GOOGL"]:
    print(f"{ticker}: ${runtime.get_market_price(ticker):.2f}")
```

## Historical L2 data replay

Titan can parse L2 updates from `.bin`, `.csv`, or `.csv.gz` files. Example:

```python
from titan import MarketDataParser

parser = MarketDataParser("core/test/examples/binance-futures_incremental_book_L2_2024-01-01_BTCUSDT_titan.csv")
while True:
    update = parser.parse_next()
    if update is None:
        break
    # update has: price, amount, side, timestamp, is_snapshot, etc.
    # Submit orders or feed your strategy based on update
parser.close()
```

For full C++-driven simulation (parse and match inside the engine), use `runtime.simulate(filepath, ticker, target_orders)`. See `python/tests/test_binance_strategy_throughput.py`.

## Common patterns

### Batch order submission

```python
for i in range(10):
    runtime.submit_limit_order("AAPL", "BID", 150.0 - i * 0.10, 100.0)
runtime.process_pending_orders()
```

### Market order

```python
runtime.submit_market_order("AAPL", "BID", 1000.0)
runtime.process_pending_orders()
```

### Order modification

```python
order_id = runtime.submit_limit_order("AAPL", "BID", 149.50, 100.0)
runtime.process_pending_orders()
# Later:
runtime.submit_edit_order("AAPL", order_id, 149.75, 100.0)
runtime.process_pending_orders()
```

## Next steps

- [API Reference](api.md) – full API  
- [Python tests](../python/tests/) – runnable examples (`test_bindings.py`, etc.)

#!/usr/bin/env python3
"""
Test script for Titan Python bindings.
Run from repo root: python python/tests/test_bindings.py
Or after building: pip install -e . && python -m python.tests.test_bindings
"""

import sys
import os
from pathlib import Path

# Ensure repo root is on path when run from python/tests/
_repo_root = Path(__file__).resolve().parents[2]
if str(_repo_root) not in sys.path:
    sys.path.insert(0, str(_repo_root))

try:
    from titan import (
        EngineRuntime,
        User,
        L2Stream,
        OrderSide,
        OrderStatus,
        INVALID_USER_ID
    )
    print("✓ Successfully imported Titan C++ extension\n")
except ImportError as e:
    print(f"✗ Failed to import Titan extension: {e}")
    print("\nPlease build the extension first:")
    print("  pip install -e .")
    sys.exit(1)

def test_basic_functionality():
    """Test basic runtime operations."""
    print("=" * 60)
    print("Test 1: Basic Runtime Operations")
    print("=" * 60)

    # Reset and create runtime
    EngineRuntime.reset_instance()
    runtime = EngineRuntime.get_instance(num_threads=2, capacity=1024*1024, verbose=False)
    print("✓ Created runtime instance")

    # Register stock
    success = runtime.register_stock("AAPL", 150.0, 1000.0)
    print(f"✓ Registered AAPL: {success}")

    # Check tickers
    tickers = runtime.list_tickers()
    print(f"✓ Listed tickers: {tickers}")

    # Submit orders
    runtime.submit_limit_order("AAPL", "BID", 149.0, 100.0)
    runtime.submit_limit_order("AAPL", "ASK", 151.0, 100.0)
    print("✓ Submitted orders")

    # Process
    runtime.process_pending_orders()
    print("✓ Processed orders")

    # Query market
    bid = runtime.get_best_bid("AAPL")
    ask = runtime.get_best_ask("AAPL")
    mid = runtime.get_market_price("AAPL")
    print(f"✓ Market: Bid=${bid:.2f}, Ask=${ask:.2f}, Mid=${mid:.2f}")

    # Statistics
    placed = runtime.get_placed_count("AAPL")
    filled = runtime.get_filled_count("AAPL")
    print(f"✓ Stats: {placed} placed, {filled} filled")

    print("\n")

def test_strategy_registration():
    """Test Python strategy registration."""
    print("=" * 60)
    print("Test 2: Strategy Registration")
    print("=" * 60)

    EngineRuntime.reset_instance()
    runtime = EngineRuntime.get_instance(num_threads=1, quantum=100)
    runtime.register_stock("TSLA", 700.0, 500.0)

    # Define Python strategy
    def my_strategy(user: User):
        tickers = user.list_tickers()
        for ticker in tickers:
            bid = user.get_best_bid(ticker)
            ask = user.get_best_ask(ticker)

            if bid > 0 and ask > 0:
                mid = (bid + ask) / 2.0
                # Place orders around mid price
                user.submit_limit_order(ticker, "BID", mid - 1.0, 10.0)
                user.submit_limit_order(ticker, "ASK", mid + 1.0, 10.0)

    # Register strategy
    trader = runtime.register_strategy(my_strategy, starting_capital=50000.0)
    print(f"✓ Registered strategy, User ID: {trader.get_user_id()}")
    print(f"✓ Starting capital: ${trader.get_capital():.2f}")

    # Submit some market orders to create liquidity
    for i in range(5):
        runtime.submit_limit_order("TSLA", "BID", 695.0 + i, 50.0)
        runtime.submit_limit_order("TSLA", "ASK", 705.0 + i, 50.0)

    runtime.process_pending_orders()

    # Check trader's positions
    capital = trader.get_capital()
    position = trader.get_position("TSLA")
    active_orders = trader.get_active_orders("TSLA")

    print(f"✓ Trader capital: ${capital:.2f}")
    print(f"✓ Trader position: {position:.2f} shares")
    print(f"✓ Active orders: {len(active_orders)}")

    print("\n")

def test_market_data_parser():
    """Test L2 stream (replay)."""
    print("=" * 60)
    print("Test 3: L2 Stream")
    print("=" * 60)

    data_file = _repo_root / "core/test/examples/binance-futures_incremental_book_L2_2024-01-01_BTCUSDT_titan.csv"
    data_file = str(data_file)

    if not os.path.exists(data_file):
        print(f"⚠ Test data not found: {data_file}")
        print("  Skipping parser test\n")
        return

    try:
        parser = L2Stream(data_file)
        print(f"✓ Opened data file: {data_file}")

        # Parse first 10 updates
        count = 0
        for i in range(10):
            update = parser.parse_next()
            if update is None:
                break
            count += 1
            if i < 3:  # Show first 3
                print(f"  Update: price=${update['price']:.2f}, "
                      f"amount={update['amount']:.4f}, "
                      f"side={update['side']}")

        print(f"✓ Parsed {count} updates")
        parser.close()

    except Exception as e:
        print(f"✗ Parser error: {e}")

    print("\n")

def test_order_management():
    """Test order inspection and management."""
    print("=" * 60)
    print("Test 4: Order Management")
    print("=" * 60)

    EngineRuntime.reset_instance()
    runtime = EngineRuntime.get_instance()
    runtime.register_stock("SPY", 450.0, 1000.0)

    # Strategy that manages orders
    def order_manager(user: User):
        tickers = user.list_tickers()
        for ticker in tickers:
            bid = user.get_best_bid(ticker)
            ask = user.get_best_ask(ticker)

            if bid <= 0 or ask <= 0:
                continue

            mid = (bid + ask) / 2.0

            # Cancel orders far from market
            active_orders = user.get_active_orders(ticker)
            for order_id in active_orders:
                info = user.get_order_info(ticker, order_id)
                if info:
                    order_price = info.get_price_dollars()
                    distance = abs(order_price - mid)
                    if distance > 5.0:  # More than $5 away
                        user.submit_cancel_order(ticker, order_id)

            # Place new orders
            user.submit_limit_order(ticker, "BID", mid - 2.0, 5.0)
            user.submit_limit_order(ticker, "ASK", mid + 2.0, 5.0)

    manager = runtime.register_strategy(order_manager, 100000.0)
    print(f"✓ Registered order manager, User ID: {manager.get_user_id()}")

    # Create market
    for i in range(10):
        runtime.submit_limit_order("SPY", "BID", 448.0 + i * 0.5, 100.0)
        runtime.submit_limit_order("SPY", "ASK", 452.0 + i * 0.5, 100.0)

    runtime.process_pending_orders()

    # Check orders
    active_orders = manager.get_active_orders("SPY")
    print(f"✓ Manager has {len(active_orders)} active orders")

    if active_orders:
        order_id = active_orders[0]
        info = manager.get_order_info("SPY", order_id)
        if info:
            print(f"✓ Order #{order_id}: "
                  f"${info.get_price_dollars():.2f} x {info.get_qty():.2f} shares, "
                  f"Status={info.status}")

        exists = runtime.order_exists("SPY", order_id)
        print(f"✓ Order exists in engine: {exists}")

    print("\n")

def test_diagnostics():
    """Test diagnostic methods."""
    print("=" * 60)
    print("Test 5: Diagnostics")
    print("=" * 60)

    EngineRuntime.reset_instance()
    runtime = EngineRuntime.get_instance()
    runtime.register_stock("GOOGL", 2800.0, 500.0)

    # Submit orders
    for i in range(100):
        runtime.submit_limit_order("GOOGL", "BID", 2790.0 + i * 0.5, 10.0)
        runtime.submit_limit_order("GOOGL", "ASK", 2810.0 + i * 0.5, 10.0)

    runtime.process_pending_orders()

    # Get diagnostics
    capacity = runtime.get_capacity("GOOGL")
    utilization = runtime.get_utilization("GOOGL")
    pending = runtime.get_pending_count("GOOGL")
    placed = runtime.get_placed_count("GOOGL")
    filled = runtime.get_filled_count("GOOGL")

    print(f"✓ Capacity: {capacity:,} orders")
    print(f"✓ Utilization: {utilization} active orders")
    print(f"✓ Utilization %: {utilization/capacity*100:.4f}%")
    print(f"✓ Pending: {pending} queued")
    print(f"✓ Placed: {placed}")
    print(f"✓ Filled: {filled}")
    print(f"✓ Fill rate: {filled/placed*100:.2f}%" if placed > 0 else "✓ Fill rate: N/A")

    print("\n")

def main():
    """Run all tests."""
    print("\n")
    print("╔══════════════════════════════════════════════════════════╗")
    print("║          TITAN PYTHON BINDINGS TEST SUITE               ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print("\n")

    try:
        test_basic_functionality()
        test_strategy_registration()
        test_market_data_parser()
        test_order_management()
        test_diagnostics()

        print("╔══════════════════════════════════════════════════════════╗")
        print("║              ALL TESTS PASSED ✓                          ║")
        print("╚══════════════════════════════════════════════════════════╝")
        print("\n")

    except Exception as e:
        print(f"\n✗ Test failed with error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

if __name__ == "__main__":
    main()

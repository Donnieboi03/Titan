"""
Type stubs for titan.titan_core (pybind11 extension).
Enables IntelliSense, go-to-definition, and type checking in IDEs.
"""
from __future__ import annotations

from typing import Callable, Dict, List, Optional, Tuple, Union, overload
import enum

# ---------------------------------------------------------------------------
# Module constants
# ---------------------------------------------------------------------------
INVALID_USER_ID: int
"""Sentinel UserId meaning "no user / anonymous"."""

IPO_HOLDER: int
"""Reserved UserId for the IPO liquidity holder (UserId 0)."""

INVALID_ORDER_ID: int
"""Sentinel OrderId (uint64 max) meaning "invalid / not found"."""


# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------

class OrderSide(enum.Enum):
    """Order side: BID (buy) or ASK (sell)."""
    BID: OrderSide
    ASK: OrderSide


class OrderStatus(enum.Enum):
    """Order lifecycle state."""
    OPEN: OrderStatus
    FILLED: OrderStatus
    CANCELLED: OrderStatus


class OrderType(enum.Enum):
    """Order type."""
    LIMIT: OrderType
    MARKET: OrderType


class EventKind(enum.Enum):
    """Engine event type emitted during order processing."""
    NONE: EventKind
    ACCEPT: EventKind
    """Order was accepted into the book."""
    REJECT: EventKind
    """Order was rejected (see RejectReason)."""
    MODIFY: EventKind
    """Order was edited."""
    PARTIAL_FILL: EventKind
    """Order partially matched."""
    FILL: EventKind
    """Order fully matched and removed from book."""
    CANCEL: EventKind
    """Order was cancelled."""


class RejectReason(enum.Enum):
    """Reason an order was rejected."""
    NO_MARKET_LIQUIDITY: RejectReason
    """No opposing orders to fill against (market order)."""
    ENGINE_FULL: RejectReason
    """Order pool capacity exhausted."""
    ORDER_NOT_FOUND: RejectReason
    """Cancel/edit targeted an order that does not exist."""


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

class OrderInfo:
    """
    Immutable snapshot of an order.
    
    Raw ``price`` and ``qty`` are in internal engine ticks.
    Use ``get_price_dollars()`` / ``get_qty()`` for human-readable units.
    """
    price: int
    """Price in internal ticks (use get_price_dollars() for USD)."""
    qty: int
    """Quantity in internal ticks (use get_qty() for BTC/shares)."""
    side: OrderSide
    type: OrderType
    status: OrderStatus
    time: int
    """Placement timestamp in nanoseconds (steady_clock)."""

    def get_price_dollars(self) -> float:
        """Return price converted to dollars."""
        ...

    def get_qty(self) -> float:
        """Return quantity in human-readable units (shares, BTC, etc.)."""
        ...

    def __repr__(self) -> str:
        """e.g. <OrderInfo BID LIMIT $29990.50 x 0.001 [OPEN]>"""
        ...


class SimulationMetrics:
    """Read-only simulation run metrics returned by ``get_simulation_metrics()``."""
    market_updates_processed: int
    """Total L2 updates streamed from the data file."""
    orders_placed: int
    orders_filled: int
    orders_cancelled: int
    simulation_time_seconds: float
    """Wall time of the C++ simulation loop (seconds)."""
    orders_per_second: float
    updates_per_second: float
    peak_open_orders: int
    final_open_orders: int
    average_utilization_percent: float
    initial_price: float
    """First sampled price from the data file (used for IPO)."""
    final_price: float
    unique_price_levels: int
    cache_entries: int
    simulation_running: bool
    """True while the async simulation job is still executing."""


# ---------------------------------------------------------------------------
# User
# ---------------------------------------------------------------------------

class User:
    """
    Trading user / agent handle.
    
    Obtained from ``EngineRuntime.register_strategy()``.
    All order methods are synchronous when called from within a strategy
    callback (the C++ engine processes them immediately before returning).
    """

    # --- Order submission ---
    def submit_limit_order(self, ticker: str, side: str, price: float, quantity: float) -> int:
        """Place a limit order. Returns order_id or INVALID_ORDER_ID on failure."""
        ...
    def submit_market_order(self, ticker: str, side: str, quantity: float) -> int:
        """Place a market order. Returns order_id or INVALID_ORDER_ID on failure."""
        ...
    def submit_cancel_order(self, ticker: str, order_id: int) -> bool:
        """Cancel an existing order. Returns True on success."""
        ...
    def submit_edit_order(self, ticker: str, order_id: int, new_price: float, new_quantity: float) -> bool:
        """Edit price and/or quantity of an open order. Returns True on success."""
        ...

    # --- Market data ---
    def get_best_bid(self, ticker: str) -> float:
        """Return current best bid price in dollars, or -1 if no bids."""
        ...
    def get_best_ask(self, ticker: str) -> float:
        """Return current best ask price in dollars, or -1 if no asks."""
        ...
    def get_market_price(self, ticker: str) -> float:
        """Return last trade execution price in dollars."""
        ...
    def get_market_depth(self, ticker: str, side: str, depth: int = 10) -> List[Tuple[float, float]]:
        """Return top-N price levels as list of (price_dollars, qty) tuples."""
        ...
    def list_tickers(self) -> List[str]:
        """Return all registered ticker symbols."""
        ...

    # --- Positions & orders ---
    def get_positions(self, ticker: str) -> List[int]:
        """Return all order IDs ever placed (including freed slots). Prefer get_active_orders()."""
        ...
    def get_active_orders(self, ticker: str) -> List[int]:
        """Return IDs of currently open (unfilled, uncancelled) orders."""
        ...
    def get_position(self, ticker: str) -> float:
        """Return net position in shares/BTC (positive = long, negative = short)."""
        ...
    def get_all_positions(self) -> Dict[str, float]:
        """Return net positions for all tickers as {ticker: qty}."""
        ...
    def get_order_info(self, ticker: str, order_id: int) -> Optional[OrderInfo]:
        """Return order snapshot or None if not found / already freed."""
        ...
    def has_sufficient_shares(self, ticker: str, qty: float) -> bool:
        """Return True if user holds at least qty shares of ticker (for sell validation)."""
        ...

    # --- Account ---
    def get_user_id(self) -> int:
        """Return this user's unique integer ID."""
        ...
    def get_capital(self) -> float:
        """Return available cash balance in dollars."""
        ...
    def get_realized_pnl(self) -> float:
        """Return cumulative realized profit/loss in dollars."""
        ...
    def get_unrealized_pnl(self, ticker: str, current_price: float) -> float:
        """Return unrealized PnL in dollars at the given current price."""
        ...
    def get_total_volume(self) -> float:
        """Return total traded volume (sum of all fill quantities)."""
        ...


# ---------------------------------------------------------------------------
# EngineRuntime
# ---------------------------------------------------------------------------

class EngineRuntime:
    """
    Engine runtime singleton.
    
    Manages order books, worker threads, user strategies, and the job
    scheduler. Obtain via ``get_instance()``; destroy and reinitialize
    via ``reset_instance()``.
    """

    # --- Lifecycle ---
    @staticmethod
    def get_instance(
        num_threads: int = 1,
        capacity: int = 1048576,
        verbose: bool = False,
        quantum: int = 1000,
    ) -> EngineRuntime:
        """
        Return (or create) the singleton EngineRuntime.

        Args:
            num_threads: Number of C++ worker threads.
            capacity: Default order pool capacity per engine (max concurrent open orders).
            verbose: Enable notification system (order accept/fill/cancel messages).
            quantum: Number of orders processed between strategy callbacks / snapshot updates.
        """
        ...

    @staticmethod
    def reset_instance() -> None:
        """Destroy the singleton and free all engines. Call before get_instance() in tests."""
        ...

    # --- Stock registration ---
    def register_stock(
        self,
        ticker: str,
        ipo_price: float,
        ipo_qty: float,
        capacity: int = 0,
    ) -> bool:
        """
        Register a new order book for ticker.

        Args:
            ticker: Symbol string (e.g. "AAPL", "BTCUSDT").
            ipo_price: Initial price in dollars.
            ipo_qty: Total IPO shares / units outstanding.
            capacity: Per-engine order pool size (0 = use runtime default).
        Returns:
            True on success, False if ticker already exists or args invalid.
        """
        ...

    def unregister_stock(self, ticker: str) -> bool:
        """Remove a registered ticker and free its engine. Returns True on success."""
        ...

    # --- Order submission ---
    def submit_limit_order(
        self,
        ticker: str,
        side: str,
        price: float,
        qty: float,
        user_id: int = ...,
    ) -> int:
        """Submit a limit order asynchronously. Returns INVALID_ORDER_ID (order ID not yet assigned)."""
        ...

    def submit_market_order(
        self,
        ticker: str,
        side: str,
        qty: float,
        user_id: int = ...,
    ) -> int:
        """Submit a market order asynchronously. Returns INVALID_ORDER_ID."""
        ...

    def submit_cancel_order(
        self,
        ticker: str,
        order_id: int,
        user_id: int = ...,
    ) -> bool:
        """Cancel an order asynchronously. Returns True if accepted into the queue."""
        ...

    def submit_edit_order(
        self,
        ticker: str,
        order_id: int,
        new_price: float,
        new_qty: float,
        user_id: int = ...,
    ) -> bool:
        """Edit an order asynchronously. Returns True if accepted into the queue."""
        ...

    # --- Market data queries ---
    def get_market_price(self, ticker: str) -> float:
        """Return last trade price in dollars."""
        ...
    def get_best_bid(self, ticker: str) -> float:
        """Return best bid price in dollars."""
        ...
    def get_best_ask(self, ticker: str) -> float:
        """Return best ask price in dollars."""
        ...
    def get_market_depth(
        self,
        ticker: str,
        side: str,
        depth: int = 10,
    ) -> List[Tuple[float, float]]:
        """Return top-N depth levels as list of (price_dollars, qty) tuples."""
        ...
    def list_tickers(self) -> List[str]:
        """Return all registered ticker symbols."""
        ...
    def get_order(self, ticker: str, order_id: int) -> Optional[OrderInfo]:
        """
        Look up an order directly by ID. Returns OrderInfo snapshot or None if
        the order does not exist or has already been freed (filled/cancelled).
        """
        ...

    # --- Batch processing ---
    @overload
    def process_pending_orders(self) -> None:
        """Flush all pending orders across all engines (synchronous)."""
        ...
    @overload
    def process_pending_orders(self, ticker: str) -> None:
        """Flush pending orders for a single ticker (synchronous)."""
        ...
    def process_pending_orders(self, ticker: Optional[str] = None) -> None: ...

    @overload
    def process_pending_orders_async(self) -> None:
        """Flush all pending orders across all engines (async, non-blocking)."""
        ...
    @overload
    def process_pending_orders_async(self, ticker: str) -> None:
        """Flush pending orders for a single ticker (async, non-blocking)."""
        ...
    def process_pending_orders_async(self, ticker: Optional[str] = None) -> None: ...

    def all_jobs_completed(self) -> bool:
        """Return True when the job scheduler has no pending work."""
        ...

    # --- Simulation ---
    def simulate(
        self,
        filepath: str,
        ticker: str,
        target_orders: int = 0,
        price_sample_size: int = 10,
        shares_outstanding: float = 1000000.0,
    ) -> bool:
        """
        Start an async simulation by streaming a data file through the C++ engine.

        Internally registers the stock (samples initial price from data), then
        dispatches a simulation job to a worker thread. Poll
        ``is_simulation_running()`` and read ``get_simulation_metrics()`` when done.

        Args:
            filepath: Path to ``.bin``, ``.csv``, or ``.csv.gz`` L2 data file.
            ticker: Symbol to register and simulate.
            target_orders: Stop after this many orders placed (0 = full file).
            price_sample_size: Number of data points to sample for IPO price.
            shares_outstanding: Total shares for IPO registration.
        Returns:
            True if simulation started successfully, False on error.
        """
        ...

    def is_simulation_running(self, ticker: str) -> bool:
        """Return True while the async simulation job is still running."""
        ...
    def get_simulation_metrics(self, ticker: str) -> SimulationMetrics:
        """Return a snapshot of simulation progress/results for ticker."""
        ...

    # --- Control ---
    def set_auto_match(self, ticker: str, auto_match: bool) -> bool:
        """Enable or disable automatic order matching for ticker."""
        ...
    def get_auto_match(self, ticker: str) -> bool:
        """Return current auto-match setting for ticker."""
        ...
    def set_batch_size(self, batch_size: int) -> None:
        """Set the flush threshold: number of queued orders before auto-submit to scheduler."""
        ...
    def get_batch_size(self) -> int:
        """Return the current batch size threshold."""
        ...
    def get_quantum(self) -> int:
        """Return the quantum (orders between strategy callbacks). Immutable after construction."""
        ...
    def set_notify_order(self, enable: bool) -> None:
        """
        Enable or disable verbose order-fill notifications.
        
        When enabled, the runtime emits ACCEPT/FILL/CANCEL messages to the
        notification thread. Useful for debugging; disable in production for
        maximum throughput.
        """
        ...
    def get_notify_order(self) -> bool:
        """Return True if order-fill notifications are currently enabled."""
        ...

    # --- Statistics ---
    def get_placed_count(self, ticker: str) -> int:
        """Return total orders placed on ticker since registration."""
        ...
    def get_cancelled_count(self, ticker: str) -> int:
        """Return total orders cancelled on ticker."""
        ...
    def get_filled_count(self, ticker: str) -> int:
        """Return total orders fully filled on ticker."""
        ...
    def get_open_count(self, ticker: str) -> int:
        """Return current number of open (unfilled) orders on ticker."""
        ...

    # --- Diagnostics ---
    def get_capacity(self, ticker: str) -> int:
        """Return order pool capacity (max concurrent open orders) for ticker."""
        ...
    def get_utilization(self, ticker: str) -> int:
        """Return current number of allocated order slots for ticker."""
        ...
    def get_pending_count(self, ticker: str) -> int:
        """Return number of orders queued but not yet processed for ticker."""
        ...
    def order_exists(self, ticker: str, order_id: int) -> bool:
        """Return True if the order currently exists and is open in the engine."""
        ...

    # --- Strategy management ---
    def register_strategy(
        self,
        strategy: Callable[[User], None],
        starting_capital: float = 100000.0,
    ) -> User:
        """
        Register a Python strategy function as a trading agent.

        The strategy callable is invoked by C++ worker threads on every quantum.
        The GIL is re-acquired before calling into Python.

        Args:
            strategy: Callable receiving a ``User`` handle. Example::

                def my_strategy(user: titan.User) -> None:
                    bid = user.get_best_bid("BTCUSDT")
                    user.submit_limit_order("BTCUSDT", "BID", bid - 1, 0.001)

            starting_capital: Initial cash balance in dollars.
        Returns:
            User handle for inspecting positions and PnL.
        """
        ...

    def unregister_strategy(self, user_id: int) -> bool:
        """
        Remove a registered strategy by its user ID.
        
        The slot is freed and may be reused by a future ``register_strategy()``
        call. Returns True on success, False if user_id not found.
        """
        ...


# ---------------------------------------------------------------------------
# MarketDataParser
# ---------------------------------------------------------------------------

class MarketDataParser:
    """
    Streaming parser for L2 market data files.
    
    Supports ``.bin`` (Titan binary format), ``.csv``, and ``.csv.gz``
    (Tardis incremental_book_L2 format).
    """

    def __init__(self, filepath: str) -> None:
        """Open a data file for streaming. Raises on file not found or unsupported format."""
        ...

    def parse_next(self) -> Optional[Dict[str, Union[int, float, str, bool]]]:
        """
        Return the next L2 update as a dict, or None when the file is exhausted.

        Dict keys:
            ``timestamp`` (int)    — nanosecond timestamp  
            ``price``     (float)  — price in dollars  
            ``amount``    (float)  — quantity  
            ``side``      (str)    — ``"b"`` (bid) or ``"a"`` (ask)  
            ``is_snapshot`` (bool) — True for full snapshot rows
        """
        ...

    def is_open(self) -> bool:
        """Return True if the file is still open and parse_next() may return data."""
        ...

    def close(self) -> None:
        """No-op (C++ closes in destructor). Provided for API compatibility."""
        ...

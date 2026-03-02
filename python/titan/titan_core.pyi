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
    Immutable snapshot of an order (copy returned by get_order / get_order_info).
    
    Safe to hold after process_pending_orders() or other engine steps; the value
    is a copy, not a reference to internal engine state.
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
    """Computed: (placed + cancelled + edited + replaced) / simulation_time_seconds."""
    updates_per_second: float
    """Computed: market_updates_processed / simulation_time_seconds."""
    peak_open_orders: int
    final_open_orders: int
    initial_price: float
    """First sampled price from the data file (used for IPO)."""
    final_price: float
    cache_entries: int
    """Distinct (side, price) levels in the simulate cache; use for book breadth."""
    simulation_running: bool
    """True while the async simulation job is still executing."""


# ---------------------------------------------------------------------------
# UserSnapshot (read-only state copy; see UserView.get_snapshot)
# ---------------------------------------------------------------------------

class UserSnapshot:
    """
    Read-only snapshot of user state (capital, position for strategy ticker, etc.).
    Returned by ``UserView.get_snapshot()``. Single-ticker: each strategy is tied to one ticker.
    """
    user_id: int
    capital: float
    realized_pnl: float
    total_volume: float
    ticker: str
    """Strategy's ticker."""
    position: float
    """Net position in shares (positive = long, negative = short)."""
    avg_price: float
    unrealized_pnl: float


# ---------------------------------------------------------------------------
# UserView (observational handle returned by register_strategy)
# ---------------------------------------------------------------------------

class UserView:
    """
    Observational handle returned by ``EngineRuntime.register_strategy()``.
    
    Does **not** expose order submission (submit_limit_order, etc.). Use
    ``runtime.submit_limit_order(ticker, side, price, qty, user_id=view.get_user_id())``
    and similar for orders from the main thread. For positions/order IDs use
    ``runtime.get_positions(view.get_user_id(), ticker)`` and
    ``runtime.get_active_orders(view.get_user_id(), ticker)``.
    """
    def get_snapshot(self) -> UserSnapshot:
        """Return a copy of the latest user state (updated each quantum)."""
        ...
    def get_capital(self) -> float: ...
    def get_realized_pnl(self) -> float: ...
    def get_total_volume(self) -> float: ...
    def get_user_id(self) -> int: ...
    def get_ticker(self) -> str: ...
    def get_position(self) -> float: ...
    def get_all_positions(self) -> Dict[str, float]: ...
    def get_committed_sell_qty(self) -> float: ...
    def get_unrealized_pnl(self) -> float: ...


# ---------------------------------------------------------------------------
# User (full handle passed to strategy callback; extends UserView)
# ---------------------------------------------------------------------------

class User(UserView):
    """
    Full trading user handle passed **inside** the strategy callback.

    The strategy is bound to a single ticker at registration. All methods
    below operate on that ticker only; no ticker argument is required.
    The value returned by ``register_strategy()`` is a ``UserView`` (observational only).
    """

    # --- Order submission (strategy's ticker) ---
    def submit_limit_order(
        self, side: Union[str, OrderSide], price: float, quantity: float
    ) -> int:
        """Place a limit order. Returns order_id or INVALID_ORDER_ID on failure."""
        ...
    def submit_market_order(self, side: Union[str, OrderSide], quantity: float) -> int:
        """Place a market order. Returns order_id or INVALID_ORDER_ID on failure."""
        ...
    def submit_cancel_order(self, order_id: int) -> bool:
        """Cancel an existing order. Returns True if accepted, False on failure."""
        ...
    def submit_replace_order(
        self, order_id: int, new_price: float, new_quantity: float
    ) -> bool:
        """Replace an open order (new price and/or quantity). Returns True if accepted, False on failure."""
        ...
    def submit_edit_order(self, order_id: int, new_quantity: float) -> bool:
        """Edit quantity only of an open order (same price). Returns True if accepted, False on failure."""
        ...

    # --- Market data (strategy's ticker) ---
    def get_best_bid(self) -> float:
        """Return current best bid price in dollars, or -1 if no bids."""
        ...
    def get_best_ask(self) -> float:
        """Return current best ask price in dollars, or -1 if no asks."""
        ...
    def get_market_price(self) -> float:
        """Return last trade execution price in dollars."""
        ...
    def get_market_depth(
        self, side: Union[str, OrderSide], depth: int = 10
    ) -> List[Tuple[float, float]]:
        """Return top-N price levels as list of (price_dollars, qty) tuples."""
        ...
    def list_tickers(self) -> List[str]:
        """Return all registered ticker symbols."""
        ...

    # --- Positions & orders (strategy's ticker) ---
    def get_positions(self) -> List[int]:
        """Return all order IDs ever placed (including freed slots). Prefer get_active_orders()."""
        ...
    def get_active_orders(self) -> List[int]:
        """Return IDs of currently open (unfilled, uncancelled) orders."""
        ...
    def get_position(self) -> float:
        """Return net position in shares (positive = long, negative = short) for strategy ticker."""
        ...
    def get_all_positions(self) -> Dict[str, float]:
        """Return {ticker: position} (single entry for strategy ticker)."""
        ...
    def get_order_info(self, order_id: int) -> Optional[OrderInfo]:
        """Return a copy of the order snapshot, or None if not found / already freed. Safe to hold."""
        ...
    def has_sufficient_shares(self, qty: float) -> bool:
        """Return True if user holds at least qty shares (for sell validation)."""
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
    def get_unrealized_pnl(self) -> float:
        """Return unrealized PnL in dollars (from snapshot, strategy ticker)."""
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
        verbose: bool = False,
        quantum: int = 1000,
        max_capacity: int = 1048576,
        max_engine_count: int = 100,
        max_strategies: int = 1000,
    ) -> EngineRuntime:
        """
        Return (or create) the singleton EngineRuntime.

        Args:
            num_threads: Number of C++ worker threads.
            verbose: Enable notification system (order accept/fill/cancel messages).
            quantum: Number of orders processed between strategy callbacks / snapshot updates.
            max_capacity: Max order pool size per engine (max concurrent open orders).
            max_engine_count: Reserve space for this many stocks/engines (avoids realloc).
            max_strategies: Reserve space for this many strategies (keeps UserView* from register_strategy valid).
        """
        ...

    @staticmethod
    def reset_instance() -> None:
        """Destroy the singleton and free all engines. Call before get_instance() in tests.
        Registered with atexit so the runtime is torn down (and log/record buffers flushed)
        when the process exits."""
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
        side: Union[str, OrderSide],
        price: float,
        qty: float,
    ) -> int:
        """Submit a limit order asynchronously. Returns INVALID_ORDER_ID (order ID not yet assigned)."""
        ...

    def submit_market_order(
        self,
        ticker: str,
        side: Union[str, OrderSide],
        qty: float,
    ) -> int:
        """Submit a market order asynchronously. Returns INVALID_ORDER_ID."""
        ...

    def submit_cancel_order(
        self,
        ticker: str,
        order_id: int,
    ) -> bool:
        """Cancel an order asynchronously. Returns True if accepted into the queue, False on failure."""
        ...

    def submit_replace_order(
        self,
        ticker: str,
        order_id: int,
        new_price: float,
        new_qty: float,
    ) -> bool:
        """Replace an order (new price and/or qty) asynchronously. Returns True if accepted, False on failure."""
        ...
    def submit_edit_order(
        self,
        ticker: str,
        order_id: int,
        new_qty: float,
    ) -> bool:
        """Edit order quantity only asynchronously. Returns True if accepted, False on failure."""
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
        side: Union[str, OrderSide],
        depth: int = 10,
    ) -> List[Tuple[float, float]]:
        """Return top-N depth levels as list of (price_dollars, qty) tuples."""
        ...
    def list_tickers(self) -> List[str]:
        """Return all registered ticker symbols."""
        ...
    def get_order(self, ticker: str, order_id: int) -> Optional[OrderInfo]:
        """
        Look up an order by ID. Returns a copy of OrderInfo or None if the order
        does not exist or has been freed. Safe to hold after process_pending_orders().
        """
        ...

    def get_positions(self, user_id: int, ticker: str) -> List[int]:
        """Return order IDs for the given user on the given ticker."""
        ...

    def get_active_orders(self, user_id: int, ticker: str) -> List[int]:
        """Return active (open) order IDs for the given user on the given ticker."""
        ...

    # --- Batch processing ---
    def request_snapshot(self, ticker: str) -> bool:
        """
        Request a snapshot refresh for the given ticker. The update is applied
        on the next process_pending_orders(). For up-to-date market/stats,
        use: request_snapshot(ticker) → process_pending_orders() → get_best_bid / get_placed_count / etc.
        Returns True if the ticker exists and the job was queued.
        """
        ...

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
        record_path: str = "",
    ) -> bool:
        """
        Start an async simulation by streaming a data file through the C++ engine.
        Orders are matched as the L2 stream is applied.

        Internally registers the stock (samples initial price from data), then
        dispatches a simulation job to a worker thread. Poll
        ``is_simulation_running()`` and read ``get_simulation_metrics()`` when done.

        Args:
            filepath: Path to ``.bin``, ``.csv``, or ``.csv.gz`` L2 data file.
            ticker: Symbol to register and simulate.
            target_orders: Stop after this many orders placed (0 = full file).
            price_sample_size: Number of data points to sample for IPO price.
            shares_outstanding: Total shares for IPO registration.
            record_path: If non-empty, record L2 updates to this file during simulation.
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
        """
        Enable or disable automatic order matching for ticker.
        Advanced. When disabled, orders are queued until matching is turned back on.
        """
        ...
    def get_auto_match(self, ticker: str) -> bool:
        """Return current auto-match setting for ticker. Advanced use."""
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
        Enable or disable order-fill notifications (EventKind: ACCEPT, FILL, CANCEL, REJECT, etc.).
        When enabled and runtime was created with verbose=True, the engine prints events to stdout;
        RejectReason is used for REJECT events. No Python callback is invoked unless added by bindings.
        Disable in production for maximum throughput.
        """
        ...
    def get_notify_order(self) -> bool:
        """Return True if order-fill notifications are currently enabled."""
        ...

    @overload
    def set_record(self, ticker: str, enable: bool) -> None:
        """
        Enable or disable per-ticker L2 recording for simulate().
        When enabled, L2 updates for this ticker are written to {ticker}.csv by the event management thread.
        Use set_record(ticker, enable, path_override) to specify a custom output path.
        """
        ...

    @overload
    def set_record(self, ticker: str, enable: bool, path_override: str) -> None:
        """
        Enable or disable per-ticker L2 recording with a custom output file path.
        When enable=True, recordings go to path_override instead of the default {ticker}.csv.
        """
        ...

    def set_record(self, ticker: str, enable: bool, path_override: str = "") -> None: ...

    def get_record(self, ticker: str) -> bool:
        """Return True if L2 recording is enabled for the given ticker."""
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
    def get_pending_count(self, ticker: str) -> int:
        """Return number of orders queued but not yet processed for ticker."""
        ...

    # --- Strategy management ---
    def register_strategy(
        self,
        ticker: str,
        strategy: Callable[[User], None],
        starting_capital: float = 100000.0,
    ) -> Optional[UserView]:
        """
        Register a Python strategy function as a trading agent for the given ticker.

        The strategy is bound to the given ticker. Its callable is invoked by C++
        worker threads on that ticker's quantum only (every N orders on that ticker),
        ensuring deterministic execution order. The GIL is re-acquired before calling
        into Python.

        Args:
            ticker: Stock ticker this strategy is registered for (must already be
                registered via register_stock). The strategy callback runs when
                this ticker's per-engine quantum is reached.
            strategy: Callable receiving a ``User`` handle (full API; methods use the registered ticker). Example::

                def my_strategy(user: titan.User) -> None:
                    bid = user.get_best_bid()
                    user.submit_limit_order("BID", bid - 1, 0.001)

            starting_capital: Initial cash balance in dollars.
        Returns:
            UserView handle for inspecting positions and PnL via get_snapshot() / get_capital() etc.,
            or None if ticker not found. To submit orders from the main thread use
            runtime.submit_limit_order(..., user_id=view.get_user_id()).
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
# L2Stream
# ---------------------------------------------------------------------------

class StreamMode:
    """Mode for L2Stream: Read (replay) or Write (record)."""
    Read: int
    Write: int


class L2Stream:
    """
    L2 market data stream: read (replay) or write (record) in Titan canonical format.
    Supports ``.bin`` (binary), ``.csv``, and ``.csv.gz``.
    """

    def __init__(self, filepath: str, streaming: bool = True) -> None:
        """Open for read (replay). Raises on file not found or unsupported format."""
        ...

    def __init__(self, filepath: str, mode: StreamMode) -> None:
        """Open for write when mode is StreamMode.Write (record to file)."""
        ...

    def parse_next(self) -> Optional[Dict[str, Union[int, float, str, bool]]]:
        """
        Return the next L2 update as a dict, or None when exhausted (read mode only).

        Dict keys: ``timestamp``, ``price``, ``amount``, ``side``, ``is_snapshot``.
        """
        ...

    def get_total_records(self) -> int:
        """Total records (read mode; binary format only)."""
        ...

    def is_open(self) -> bool:
        """True if the stream is open."""
        ...

    def write(self, update: Dict[str, Union[int, float, str, bool]]) -> bool:
        """Append one L2 update (write mode only). Returns True on success."""
        ...

    def flush(self) -> None:
        """Flush buffered data (write mode only)."""
        ...

    def close(self) -> None:
        """No-op (C++ closes in destructor). Provided for API compatibility."""
        ...

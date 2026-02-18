"""
Type stubs for titan.titan_core (pybind11 extension).
Enables IntelliSense, go-to-definition, and type checking in IDEs.
"""
from __future__ import annotations

from typing import Dict, List, Optional, Tuple, Union, overload
import enum

# Module constants
INVALID_USER_ID: int
IPO_HOLDER: int


class OrderSide(enum.Enum):
    """Order side: BID (buy) or ASK (sell)."""
    BID: OrderSide
    ASK: OrderSide


class OrderStatus(enum.Enum):
    """Order lifecycle: OPEN, FILLED, or CANCELLED."""
    OPEN: OrderStatus
    FILLED: OrderStatus
    CANCELLED: OrderStatus


class OrderType(enum.Enum):
    """Order type: LIMIT or MARKET."""
    LIMIT: OrderType
    MARKET: OrderType


class OrderInfo:
    """Immutable snapshot of an order (price/qty in engine ticks; use get_price_dollars/get_qty for human units)."""
    price: int  # internal ticks
    qty: int    # internal ticks
    side: OrderSide
    type: OrderType
    status: OrderStatus

    def get_price_dollars(self) -> float:
        """Return price in dollars."""
        ...

    def get_qty(self) -> float:
        """Return quantity in human-readable units (e.g. shares)."""
        ...


class User:
    """Trading user/agent handle. Submit orders and query account/positions."""

    def submit_limit_order(self, ticker: str, side: str, price: float, quantity: float) -> int: ...
    def submit_market_order(self, ticker: str, side: str, quantity: float) -> int: ...
    def submit_cancel_order(self, ticker: str, order_id: int) -> bool: ...
    def submit_edit_order(self, ticker: str, order_id: int, new_price: float, new_quantity: float) -> bool: ...

    def get_best_bid(self, ticker: str) -> float: ...
    def get_best_ask(self, ticker: str) -> float: ...
    def get_market_price(self, ticker: str) -> float: ...
    def get_market_depth(self, ticker: str, side: str, depth: int = 10) -> List[Tuple[float, float]]: ...
    def list_tickers(self) -> List[str]: ...

    def get_positions(self, ticker: str) -> List[int]: ...
    def get_active_orders(self, ticker: str) -> List[int]: ...
    def get_position(self, ticker: str) -> float: ...
    def get_all_positions(self) -> Dict[str, float]: ...
    def get_order_info(self, ticker: str, order_id: int) -> Optional[OrderInfo]: ...
    def has_sufficient_shares(self, ticker: str, qty: float) -> bool: ...

    def get_user_id(self) -> int: ...
    def get_capital(self) -> float: ...
    def get_realized_pnl(self) -> float: ...
    def get_unrealized_pnl(self, ticker: str, current_price: float) -> float: ...
    def get_total_volume(self) -> float: ...


class SimulationMetrics:
    """Read-only simulation run metrics from get_simulation_metrics()."""
    market_updates_processed: int
    orders_placed: int
    orders_filled: int
    orders_cancelled: int
    simulation_time_seconds: float
    orders_per_second: float
    updates_per_second: float
    peak_open_orders: int
    final_open_orders: int
    average_utilization_percent: float
    initial_price: float
    final_price: float
    unique_price_levels: int
    cache_entries: int
    simulation_running: bool


class EngineRuntime:
    """Engine runtime singleton. Manages order books, users, and scheduling."""

    @staticmethod
    def get_instance(
        num_threads: int = 1,
        capacity: int = 1048576,
        verbose: bool = False,
        quantum: int = 1000,
    ) -> EngineRuntime: ...

    @staticmethod
    def reset_instance() -> None: ...

    def register_stock(
        self,
        ticker: str,
        ipo_price: float,
        ipo_qty: float,
        capacity: int = 0,
    ) -> bool: ...
    def unregister_stock(self, ticker: str) -> bool: ...

    def submit_limit_order(
        self,
        ticker: str,
        side: str,
        price: float,
        qty: float,
        user_id: int = ...,
    ) -> int: ...
    def submit_market_order(
        self,
        ticker: str,
        side: str,
        qty: float,
        user_id: int = ...,
    ) -> int: ...
    def submit_cancel_order(
        self,
        ticker: str,
        order_id: int,
        user_id: int = ...,
    ) -> bool: ...
    def submit_edit_order(
        self,
        ticker: str,
        order_id: int,
        new_price: float,
        new_qty: float,
        user_id: int = ...,
    ) -> bool: ...

    def get_market_price(self, ticker: str) -> float: ...
    def get_best_bid(self, ticker: str) -> float: ...
    def get_best_ask(self, ticker: str) -> float: ...
    def get_market_depth(
        self,
        ticker: str,
        side: str,
        depth: int = 10,
    ) -> List[Tuple[float, float]]: ...
    def list_tickers(self) -> List[str]: ...

    @overload
    def process_pending_orders(self) -> None: ...
    @overload
    def process_pending_orders(self, ticker: str) -> None: ...
    def process_pending_orders(self, ticker: Optional[str] = None) -> None: ...

    @overload
    def process_pending_orders_async(self) -> None: ...
    @overload
    def process_pending_orders_async(self, ticker: str) -> None: ...
    def process_pending_orders_async(self, ticker: Optional[str] = None) -> None: ...

    def simulate(
        self,
        filepath: str,
        ticker: str,
        target_orders: int = 0,
        price_sample_size: int = 10,
        shares_outstanding: float = 1000000.0,
    ) -> bool:
        """Run simulation (parses file in C++, registers stock, processes L2 updates). Returns True if started successfully."""
        ...

    def is_simulation_running(self, ticker: str) -> bool: ...
    def get_simulation_metrics(self, ticker: str) -> SimulationMetrics: ...
    def all_jobs_completed(self) -> bool: ...

    def set_auto_match(self, ticker: str, auto_match: bool) -> bool: ...
    def get_auto_match(self, ticker: str) -> bool: ...
    def set_batch_size(self, batch_size: int) -> None: ...
    def get_batch_size(self) -> int: ...
    def get_quantum(self) -> int: ...

    def get_placed_count(self, ticker: str) -> int: ...
    def get_cancelled_count(self, ticker: str) -> int: ...
    def get_filled_count(self, ticker: str) -> int: ...
    def get_open_count(self, ticker: str) -> int: ...

    def get_capacity(self, ticker: str) -> int: ...
    def get_utilization(self, ticker: str) -> int: ...
    def get_pending_count(self, ticker: str) -> int: ...
    def order_exists(self, ticker: str, order_id: int) -> bool: ...

    def register_strategy(
        self,
        strategy: object,  # Callable[[User], None]
        starting_capital: float = 100000.0,
    ) -> Optional[User]: ...


class MarketDataParser:
    """Streaming parser for L2 updates from .bin, .csv, or .csv.gz files."""

    def __init__(self, filepath: str) -> None: ...

    def parse_next(self) -> Optional[Dict[str, Union[int, float, str, bool]]]:
        """Return next L2 update as dict (timestamp, price, amount, side, is_snapshot) or None when finished."""
        ...

    def is_open(self) -> bool:
        """Return True if the file is still open and more data may be available."""
        ...

    def close(self) -> None:
        """Release resources. No-op when using C++ destructor; provided for API compatibility."""
        ...

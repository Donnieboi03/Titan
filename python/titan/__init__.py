"""
Titan: Multi-Agent Market Microstructure Backtesting Engine

A high-performance Python library (C++ core) for backtesting algorithmic 
trading strategies against historical Level-2 order book data.
"""

from __future__ import annotations

__version__ = "1.2.0"

import atexit
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .titan_core import (
        EngineRuntime,
        User,
        UserView,
        UserSnapshot,
        L2Stream,
        StreamMode,
        OrderInfo,
        OrderSide,
        OrderStatus,
        OrderType,
        EventKind,
        RejectReason,
        SimulationMetrics,
        INVALID_USER_ID,
        IPO_HOLDER,
        INVALID_ORDER_ID,
    )
else:
    # Import C++ extension module (will be built by pybind11)
    try:
        from .titan_core import (
            EngineRuntime,
            User,
            UserView,
            UserSnapshot,
            L2Stream,
            StreamMode,
            OrderInfo,
            OrderSide,
            OrderStatus,
            OrderType,
            EventKind,
            RejectReason,
            SimulationMetrics,
            INVALID_USER_ID,
            IPO_HOLDER,
            INVALID_ORDER_ID,
        )
        # Ensure runtime is torn down (and buffers flushed) when process exits
        atexit.register(EngineRuntime.reset_instance)
    except ImportError as e:
        import sys
        print(f"Warning: C++ extension module not found. Please build the extension.", file=sys.stderr)
        print(f"Run: pip install -e . (from the Titan root directory)", file=sys.stderr)
        print(f"Error: {e}", file=sys.stderr)
        # Placeholder classes/constants so "from titan import ..." succeeds; runtime will fail on use
        class EngineRuntime:
            pass
        class User:
            pass
        class UserView:
            pass
        class UserSnapshot:
            pass
        class L2Stream:
            pass
        class StreamMode:
            Read = 0
            Write = 1
        class OrderInfo:
            pass
        class SimulationMetrics:
            pass
        class OrderSide:
            BID = 0
            ASK = 1
        class OrderStatus:
            OPEN = 0
            FILLED = 1
            CANCELLED = 2
        class OrderType:
            LIMIT = 0
            MARKET = 1
        class EventKind:
            NONE = 0
            ACCEPT = 1
            REJECT = 2
            MODIFY = 3
            PARTIAL_FILL = 4
            FILL = 5
            CANCEL = 6
        class RejectReason:
            NO_MARKET_LIQUIDITY = 0
            ENGINE_FULL = 1
            ORDER_NOT_FOUND = 2
        INVALID_USER_ID = -1
        IPO_HOLDER = 0
        INVALID_ORDER_ID = 18446744073709551615  # uint64 max

__all__ = [
    "EngineRuntime",
    "User",
    "UserView",
    "UserSnapshot",
    "L2Stream",
    "StreamMode",
    "OrderInfo",
    "SimulationMetrics",
    "OrderSide",
    "OrderStatus",
    "OrderType",
    "EventKind",
    "RejectReason",
    "INVALID_USER_ID",
    "IPO_HOLDER",
    "INVALID_ORDER_ID",
]

"""
Titan: Multi-Agent Market Microstructure Backtesting Engine

A high-performance Python library (C++ core) for backtesting algorithmic 
trading strategies against historical Level-2 order book data.
"""

__version__ = "2.1.0"

# Import C++ extension module (will be built by pybind11)
try:
    from .titan_core import (
        EngineRuntime,
        User,
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
except ImportError as e:
    import sys
    print(f"Warning: C++ extension module not found. Please build the extension.", file=sys.stderr)
    print(f"Run: pip install -e . (from the Titan root directory)", file=sys.stderr)
    print(f"Error: {e}", file=sys.stderr)
    # Define placeholder classes for development
    class EngineRuntime:
        pass
    class User:
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

__all__ = [
    "EngineRuntime",
    "User",
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

# Titan Documentation

**Start here:** [Installation](installation.md) → [Quick Start](quickstart.md) → [API Reference](api.md).

## Getting started

- [Installation](installation.md) – Install the Titan Python library (from source or [Docker](installation.md#docker))
- [Quick Start](quickstart.md) – Your first backtest in a few minutes

## Guides

- [L2 Data and Recording](l2_data_and_recording.md) – Incremental stream vs TopK snapshots, when to use each, replay vs analysis, rationale for each

## API

- [API Reference](api.md) – Python API (EngineRuntime, User, UserView, UserSnapshot, L2Stream, etc.)

## Examples and scripts

- [test_bindings.py](../python/tests/test_bindings.py) – Runtime and strategy registration
- [test_binance_strategy_throughput.py](../python/tests/test_binance_strategy_throughput.py) – L2 data replay
- [test_stress_multiworker.py](../python/tests/test_stress_multiworker.py) – Concurrency test
- [download_market_data.py](../python/tests/download_market_data.py) – Tardis L2 download
- [convert_l2_to_csv.py](../python/tests/convert_l2_to_csv.py) – Tardis → Titan CSV

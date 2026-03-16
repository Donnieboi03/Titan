# L2 Data and Recording

This guide explains the two ways Titan can record order book data, when to use each, and the rationale behind choosing one over the other.

## Recording modes (runtime)

The runtime supports two recording modes via `set_record(ticker, enable, path_override?, record_type?)`:

- **TOPK** (default) – Every **quantum**, write a full top-of-book snapshot (top K bid/ask levels) to a CSV. Per-engine state lives in `OrderEngineInfo`; buffers and streams used by the event thread live in a dedicated cache-line-aligned struct to avoid false sharing.
- **FEATURES** – Every quantum, write one row of feature scalars (timestamp, best_bid, best_ask, mid_price, spread, order_imbalance, spread_bps) to a CSV (e.g. `{ticker}_features.csv`). Suited for training (e.g. Prometheus).

If you do not pass `record_type`, the default is **TOPK**. See [API Reference – set_record](api.md#set_record--get_record--get_record_type).

## Two ways to record L2

### 1. Incremental stream (level updates)

**What it is:** One row per level update. Each row = one (timestamp, price, amount, side): "at this price, this side, size is now X." The file is a time-ordered sequence of level changes—the same shape as an exchange feed.

**Where it lives:**

- When ingesting a **live feed** (e.g. WebSocket), record in the same process that receives updates: write each (price, amount, side) to a file (e.g. via `L2Stream(path, StreamMode.Write)`) as you push updates into the engine. This is typically done in **Python** (e.g. in your bot or adapter), not via the runtime's built-in record.
- For **file-based replay**, Titan reads incremental L2 from `.bin`, `.csv`, or `.csv.gz` in this format.

**Use for:**

- **Replay and backtest** – Replaying this stream preserves the exact order of events so you can run strategies again on the same market.
- **Recording the market in real time while trading on it** – Same stream is pushed to the engine and written to file; recording and trading stay in lockstep.
- **Pure market record** – No strategy orders in the file; only the exchange (or feed) updates. Replay = same market for any strategy.

---

### 2. TopK snapshots (periodic book state)

**What it is:** At fixed intervals (every **quantum**—e.g. every N orders), write the **current top K levels** of the book (e.g. top 10 bid and top 10 ask). Each write = full state of the top of book at that moment.

**Where it lives:**

- **Titan runtime:** `set_record(ticker, True)` (and optional `path_override`). When enabled, the engine writes a book snapshot every quantum via its event management thread. See [API Reference – set_record](api.md#set_record--get_record--get_record_type).

**Use for:**

- **Point-in-time analysis** – "What did the book look like when my order filled?" Load the snapshot nearest to that time; no need to replay the full stream.
- **Time-series of top-of-book** – Best bid, best ask, spread, depth over time (e.g. plot or compute imbalance) without replaying ticks.
- **Debugging** – Inspect engine state at quantum boundaries.

**Do not use as the only source for replay** – Snapshots lose the event sequence between intervals. For faithful replay, use the incremental stream. See [API Reference – set_record](api.md#set_record--get_record--get_record_type) for how to enable TopK recording in the runtime.

---

## Replay vs analysis

| Goal | Use |
|------|-----|
| Replay a strategy on the same market | **Incremental stream** – replay the L2 file so event order is preserved. |
| See what the book looked like at specific times | **TopK snapshots** – load the snapshot at or near that time. |
| Record how the simulated market was affected by a strategy | Record **TopK snapshots of engine state** (market + strategy orders), or record the **incremental stream** (pure market) plus a separate **strategy event log** (place/cancel/fill). |

Replay stays accurate when it uses the **incremental** stream. Snapshots **add** information (state at discrete times) but do not replace the stream for replay.

---

## Summary

- **Incremental stream** = event-by-event level updates. Use for replay, real-time recording while trading, and pure market capture. Preserves event order.
- **TopK snapshots** = periodic state of the top of book. Use for point-in-time analysis and time-series of the book; do not use as the only source for replay.
- **Recording in Python** – When the feed is ingested in Python (e.g. WebSocket), recording the incremental stream in Python (e.g. `L2Stream` write) keeps recording and trading in lockstep; the runtime's `set_record` is optional and adds TopK snapshots for analysis.

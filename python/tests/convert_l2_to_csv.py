"""
Convert Tardis L2 data to Titan-compatible CSV format.

Tardis provides L2 data in JSON format. This script converts it to CSV
format compatible with Titan's backtesting engine.

Run from repo root:
  python python/tests/convert_l2_to_csv.py --input path/to/file.json.gz [--output path/to_out.csv]

Usage:
  python python/tests/convert_l2_to_csv.py --input core/test/examples/binance-futures_incremental_book_L2_2024-01-01_BTCUSDT.json.gz
"""

import argparse
import gzip
import json
import csv
from pathlib import Path
from typing import Iterator


def read_tardis_l2_data(filepath: str) -> Iterator[dict]:
    """
    Read Tardis L2 data from gzipped JSON file.

    Args:
        filepath: Path to .json.gz file

    Yields:
        L2 update events
    """
    with gzip.open(filepath, 'rt') as f:
        for line in f:
            yield json.loads(line)


def convert_to_titan_format(tardis_event: dict) -> list[dict]:
    """
    Convert Tardis L2 event to Titan CSV format.

    Tardis L2 format:
    {
        "timestamp": "2024-01-01T00:00:00.123456Z",
        "symbol": "BTCUSDT",
        "bids": [[price, amount], ...],
        "asks": [[price, amount], ...]
    }

    Titan format:
    timestamp,event_type,order_id,side,price,quantity,symbol

    Args:
        tardis_event: Tardis L2 event dict

    Returns:
        List of Titan-compatible event dicts
    """
    events = []
    timestamp_ns = parse_timestamp(tardis_event['timestamp'])
    symbol = tardis_event.get('symbol', '')

    # Convert bids
    for i, (price_str, amount_str) in enumerate(tardis_event.get('bids', [])):
        price_cents = int(float(price_str) * 100)
        quantity = int(float(amount_str))

        # Generate synthetic order ID (timestamp + side + index)
        order_id = int(f"{timestamp_ns}{0}{i:04d}")

        # Determine event type based on amount
        if quantity == 0:
            event_type = "CANCEL"
        else:
            event_type = "ADD"  # Or MODIFY if order exists

        events.append({
            'timestamp': timestamp_ns,
            'sequence': 0,  # Not provided by Tardis
            'event_type': event_type,
            'order_id': order_id,
            'side': 'BUY',
            'price': price_cents,
            'quantity': quantity,
            'symbol': symbol
        })

    # Convert asks
    for i, (price_str, amount_str) in enumerate(tardis_event.get('asks', [])):
        price_cents = int(float(price_str) * 100)
        quantity = int(float(amount_str))

        order_id = int(f"{timestamp_ns}{1}{i:04d}")

        if quantity == 0:
            event_type = "CANCEL"
        else:
            event_type = "ADD"

        events.append({
            'timestamp': timestamp_ns,
            'sequence': 0,
            'event_type': event_type,
            'order_id': order_id,
            'side': 'SELL',
            'price': price_cents,
            'quantity': quantity,
            'symbol': symbol
        })

    return events


def parse_timestamp(ts_str: str) -> int:
    """Convert ISO timestamp to nanoseconds."""
    from datetime import datetime

    # Parse ISO format: 2024-01-01T00:00:00.123456Z
    dt = datetime.fromisoformat(ts_str.replace('Z', '+00:00'))
    return int(dt.timestamp() * 1e9)


def convert_file(input_path: str, output_path: str, limit: int = None):
    """
    Convert Tardis L2 file to Titan CSV.

    Args:
        input_path: Input .json.gz file
        output_path: Output .csv file
        limit: Maximum number of events to convert (None = all)
    """
    print(f"Converting: {input_path}")
    print(f"Output: {output_path}")

    # Create output directory
    Path(output_path).parent.mkdir(parents=True, exist_ok=True)

    with open(output_path, 'w', newline='') as csvfile:
        fieldnames = ['timestamp', 'sequence', 'event_type', 'order_id', 'side', 'price', 'quantity', 'symbol']
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        count = 0
        for tardis_event in read_tardis_l2_data(input_path):
            # Convert to Titan format
            titan_events = convert_to_titan_format(tardis_event)

            # Write events
            for event in titan_events:
                writer.writerow(event)
                count += 1

                if limit and count >= limit:
                    print(f"Reached limit of {limit} events")
                    return

            if count % 10000 == 0:
                print(f"Processed {count} events...")

    print(f"✓ Conversion complete! Wrote {count} events")


def main():
    parser = argparse.ArgumentParser(description="Convert Tardis L2 data to Titan CSV format")

    parser.add_argument(
        "--input",
        type=str,
        required=True,
        help="Input Tardis .json.gz file"
    )

    parser.add_argument(
        "--output",
        type=str,
        default=None,
        help="Output CSV file (default: <input>_titan.csv)"
    )

    parser.add_argument(
        "--limit",
        type=int,
        default=None,
        help="Limit number of events to convert (for testing)"
    )

    args = parser.parse_args()

    # Generate output filename if not provided
    if args.output is None:
        input_path = Path(args.input)
        args.output = str(input_path.with_suffix('').with_suffix('')) + '_titan.csv'

    # Convert
    convert_file(args.input, args.output, args.limit)


if __name__ == "__main__":
    main()

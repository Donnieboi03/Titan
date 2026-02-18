"""
Download Level-2 market data using tardis-dev.

This script downloads incremental order book data (L2) from various crypto exchanges.
L2 data includes best bid/ask updates and is more accessible than full L3 data.

Requirements:
    pip install tardis-dev

Run from repo root:
  python python/tests/download_market_data.py [--download-dir core/test/examples]

Usage:
  python python/tests/download_market_data.py
  python python/tests/download_market_data.py --from-date 2024-01-01 --to-date 2024-01-31 --symbols BTCUSDT ETHUSDT
  python python/tests/download_market_data.py --exchange binance --symbols BTCUSDT --download-dir core/test/examples
"""

import argparse
import sys
from datetime import datetime, timedelta
from pathlib import Path

try:
    from tardis_dev import datasets
except ImportError:
    print("ERROR: tardis-dev not installed")
    print("Install with: pip install tardis-dev")
    sys.exit(1)


def download_market_data(
    exchange: str,
    symbols: list[str],
    from_date: str,
    to_date: str,
    data_types: list[str],
    download_dir: str = "./data",
    api_key: str = None
):
    """
    Download market data from exchange using tardis-dev.

    Args:
        exchange: Exchange name (e.g., "binance-futures", "binance", "coinbase")
        symbols: List of trading symbols (e.g., ["BTCUSDT", "ETHUSDT"])
        from_date: Start date in YYYY-MM-DD format
        to_date: End date in YYYY-MM-DD format
        data_types: List of data types to download (e.g., ["incremental_book_L2"])
        download_dir: Directory to save downloaded data
        api_key: Tardis API key (optional, for authenticated access)
    """
    # Create download directory
    Path(download_dir).mkdir(parents=True, exist_ok=True)

    print(f"=== Tardis Market Data Download ===")
    print(f"Exchange: {exchange}")
    print(f"Symbols: {', '.join(symbols)}")
    print(f"Date Range: {from_date} to {to_date}")
    print(f"Data Types: {', '.join(data_types)}")
    print(f"Download Dir: {download_dir}")
    print()

    try:
        # Download data
        datasets.download(
            exchange=exchange,
            data_types=data_types,
            from_date=from_date,
            to_date=to_date,
            symbols=symbols,
            download_dir=download_dir,
            api_key=api_key
        )

        print(f"\n✓ Download complete!")
        print(f"Data saved to: {download_dir}")

    except Exception as e:
        print(f"\n✗ Download failed: {e}")
        sys.exit(1)


def parse_args():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Download Level-2 market data from crypto exchanges",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples (run from repo root):
  python python/tests/download_market_data.py
  python python/tests/download_market_data.py --symbols BTCUSDT ETHUSDT SOLUSDT --from-date 2024-01-01 --to-date 2024-01-07
  python python/tests/download_market_data.py --download-dir core/test/examples

Supported Exchanges:
  - binance-futures (default)
  - binance (spot)
  - coinbase
  - kraken
  - bybit
  - deribit
  - huobi
  - okex
  ... and many more (see tardis.dev docs)
        """
    )

    parser.add_argument(
        "--exchange",
        type=str,
        default="binance-futures",
        help="Exchange name (default: binance-futures)"
    )

    parser.add_argument(
        "--symbols",
        nargs="+",
        default=["BTCUSDT"],
        help="Trading symbols (default: BTCUSDT)"
    )

    parser.add_argument(
        "--from-date",
        type=str,
        default=(datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d"),
        help="Start date YYYY-MM-DD (default: yesterday)"
    )

    parser.add_argument(
        "--to-date",
        type=str,
        default=(datetime.now() - timedelta(days=1)).strftime("%Y-%m-%d"),
        help="End date YYYY-MM-DD (default: yesterday)"
    )

    parser.add_argument(
        "--data-types",
        nargs="+",
        default=["incremental_book_L2"],
        choices=["incremental_book_L2", "trades", "quotes", "book_snapshot_25"],
        help="Data types to download (default: incremental_book_L2)"
    )

    parser.add_argument(
        "--download-dir",
        type=str,
        default="./data",
        help="Directory to save data (default: ./data)"
    )

    parser.add_argument(
        "--api-key",
        type=str,
        default=None,
        help="Tardis API key (optional, for higher rate limits)"
    )

    return parser.parse_args()


def validate_date(date_str: str) -> bool:
    """Validate date format."""
    try:
        datetime.strptime(date_str, "%Y-%m-%d")
        return True
    except ValueError:
        return False


def main():
    """Main entry point."""
    args = parse_args()

    # Validate dates
    if not validate_date(args.from_date):
        print(f"ERROR: Invalid from-date format: {args.from_date}")
        print("Use YYYY-MM-DD format")
        sys.exit(1)

    if not validate_date(args.to_date):
        print(f"ERROR: Invalid to-date format: {args.to_date}")
        print("Use YYYY-MM-DD format")
        sys.exit(1)

    # Check date order
    from_dt = datetime.strptime(args.from_date, "%Y-%m-%d")
    to_dt = datetime.strptime(args.to_date, "%Y-%m-%d")

    if from_dt > to_dt:
        print(f"ERROR: from-date ({args.from_date}) is after to-date ({args.to_date})")
        sys.exit(1)

    # Calculate days
    days = (to_dt - from_dt).days + 1
    if days > 31:
        print(f"WARNING: Downloading {days} days of data. This may take a while...")
        response = input("Continue? (y/n): ")
        if response.lower() != 'y':
            print("Download cancelled")
            sys.exit(0)

    # Download data
    download_market_data(
        exchange=args.exchange,
        symbols=args.symbols,
        from_date=args.from_date,
        to_date=args.to_date,
        data_types=args.data_types,
        download_dir=args.download_dir,
        api_key=args.api_key
    )


if __name__ == "__main__":
    main()

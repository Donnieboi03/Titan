# Titan: Multi-Agent Market Microstructure Backtesting Engine
# Builds the C++ extension with Highway (SIMD) and zlib; installs the Python package.
#
# Build:  docker build -t titan .
# Run:    docker run -it titan python -c "import titan; print(titan.__version__)"
# Shell:  docker run -it titan bash

FROM python:3.11-slim-bookworm

# System deps: C++20 compiler, Highway (SIMD), zlib (compressed data)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libhighway-dev \
    zlib1g-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy project (build context = repo root)
COPY setup.py README.md ./
COPY core/ core/
COPY python/ python/

# Install Titan and runtime dependencies from setup.py
RUN pip install --no-cache-dir .

# Default: drop into Python so "docker run titan" is useful
CMD ["python"]

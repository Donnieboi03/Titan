# Installation Guide

This guide explains how to install the **Titan Python library** (C++ extension + Python bindings) from source.

## Prerequisites

- **Python**: 3.8 or higher  
- **C++ compiler** with C++20 support:  
  - **macOS**: clang++ (install Xcode Command Line Tools: `xcode-select --install`)  
  - **Linux**: g++ 10+ or clang++ 11+  
- **CMake**: 3.15+ (only required if you want to run the C++ test suite; not required for `pip install`)

The Python dependencies (pybind11, numpy, pandas, etc.) are installed automatically when you run `pip install -r requirements.txt` and `pip install -e .`.

## How to install the Python library

### 1. Clone the repository

```bash
git clone https://github.com/Donnieboi03/Titan.git
cd Titan
```

### 2. Install Python dependencies

```bash
pip install -r requirements.txt
```

This installs pybind11, numpy, pandas, and optional tools (e.g. tardis-dev for market data).

### 3. Install system libraries (Highway, zlib)

The C++ extension links against **Highway** (SIMD) and **zlib**. Install them before building:

- **macOS (Homebrew):** `brew install highway` (zlib is usually present; if not: `brew install zlib`)
- **Linux (Debian/Ubuntu):** `sudo apt install libhighway-dev zlib1g-dev`
- **Other Linux:** Install the Highway and zlib development packages for your distro.

### 4. Install the Titan package

```bash
pip install -e .
```

This builds the C++ extension (order engine, runtime, market data parser) and installs the `titan` package in editable mode. No separate CMake step is needed to use the library.

To install without editable mode (fixed install): `pip install .`

### 5. Verify installation

```bash
python -c "import titan; print(titan.__version__)"
```

You should see `2.1.0` (or the current version).

### One-liner (from repo root)

After installing system libraries (step 3), run:

```bash
pip install -r requirements.txt && pip install -e .
```

## Platform-specific notes

### macOS

Install Xcode Command Line Tools:

```bash
xcode-select --install
```

Install Homebrew dependencies (optional):

```bash
brew install cmake python@3.11
```

### Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    python3-dev \
    python3-pip
```

### Linux (CentOS/RHEL)

```bash
sudo yum install -y \
    gcc-c++ \
    cmake3 \
    python3-devel
```

## Testing the installation

### Python tests

From the repository root:

```bash
python python/tests/test_bindings.py
```

You should see a short run of runtime, strategy registration, parser, and diagnostics, ending with **ALL TESTS PASSED**.

### C++ tests (optional)

To run the C++ unit tests, build with CMake first:

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j8
ctest --verbose
cd ..
```

## Troubleshooting

### pybind11 Not Found

If CMake can't find pybind11:

```bash
# Install with pip
pip install pybind11

# Verify installation
python -c "import pybind11; print(pybind11.get_cmake_dir())"
```

### C++20 Compiler Issues

If you get C++20 compilation errors:

```bash
# Check compiler version
clang++ --version  # Should be 11+ for C++20
g++ --version      # Should be 10+ for C++20

# Update compiler if needed (Ubuntu)
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install g++-11
```

### CMake Version Too Old

```bash
# macOS
brew upgrade cmake

# Ubuntu (install from Kitware repository)
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo apt-key add -
sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ focal main'
sudo apt-get update
sudo apt-get install cmake
```

### Import Error After Installation

If `import titan` fails:

```bash
# Ensure module is built
ls python/titan/titan_core*.so  # Should exist

# Check Python can find it
python -c "import sys; print(sys.path)"

# Reinstall in development mode
pip uninstall titan-backtesting
pip install -e .
```

### Segmentation Fault on Import

This usually indicates a compiler mismatch. Rebuild with:

```bash
# Clean build
rm -rf build/
pip uninstall titan-backtesting

# Rebuild
./build.sh
```

## Development installation

To install with optional dev dependencies (pytest, black, mypy):

```bash
pip install -e ".[dev]"
```

Optional extras: `.[dev,ml,viz]` for ML and visualization dependencies (see `setup.py`).

## Next Steps

- [Quick Start Tutorial](quickstart.md) - Your first backtest
- [API Reference](api.md) - Complete API documentation
- [Python tests](../python/tests/) - Sample tests and data utilities

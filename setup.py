"""
Setup script for Titan Python package.

Build dependencies (install before pip install .):
  - macOS:   brew install highway
  - Linux:  e.g. apt install libhighway-dev zlib1g-dev
  - Windows: vcpkg install highway (and zlib if needed); build with vcpkg toolchain
See README.md for full prerequisites.
"""

from setuptools import setup, Extension, find_packages
import sys
import os
import subprocess

try:
    import pybind11
    pybind11_include = pybind11.get_include()
except ImportError:
    pybind11_include = None

# Determine platform-specific compilation flags
extra_compile_args = ['-std=c++20', '-O3', '-DNDEBUG']
extra_link_args = []

if sys.platform == 'darwin':  # macOS
    extra_compile_args += ['-stdlib=libc++']
    extra_link_args += ['-stdlib=libc++']
    # Add Homebrew paths for dependencies
    if os.path.exists('/opt/homebrew/include'):
        extra_compile_args += ['-I/opt/homebrew/include']
        extra_link_args += ['-L/opt/homebrew/lib']
    elif os.path.exists('/usr/local/include'):
        extra_compile_args += ['-I/usr/local/include']
        extra_link_args += ['-L/usr/local/lib']

# Build include directories
include_dirs = ['core', 'python']
if pybind11_include:
    include_dirs.append(pybind11_include)

# Define the extension module
ext_modules = [
    Extension(
        'titan.titan_core',
        sources=[
            'python/bindings.cpp',
            'core/market_data_stream.cpp',
            'core/order_engine.cpp',
            'core/engine_runtime.cpp',
        ],
        include_dirs=include_dirs,
        libraries=['hwy', 'z'],  # Highway SIMD and zlib
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
        language='c++',
    ),
]

# Read README
try:
    with open("README.md", "r", encoding="utf-8") as fh:
        long_description = fh.read()
except FileNotFoundError:
    long_description = "Titan: Multi-Agent Market Microstructure Backtesting Library"


setup(
    name="titan-backtesting",
    version="2.2.0",
    author="https://github.com/Donnieboi03",
    description="Multi-agent market microstructure backtesting library",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/Donnieboi03/Titan",
    packages=find_packages(where="python"),
    package_dir={"": "python"},
    ext_modules=ext_modules,
    setup_requires=[
        "pybind11>=2.10.0",
    ],
    install_requires=[
        "numpy>=1.20.0",
        "pandas>=1.3.0",
        "pybind11>=2.10.0",
    ],
    extras_require={
        "dev": [
            "pytest>=6.0",
            "pytest-cov",
            "black",
            "mypy",
        ],
        "ml": [
            "scikit-learn>=1.0.0",
            "torch>=1.10.0",
        ],
        "viz": [
            "matplotlib>=3.4.0",
            "seaborn>=0.11.0",
        ],
    },
    python_requires=">=3.8",
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Financial and Insurance Industry",
        "Intended Audience :: Science/Research",
        "Topic :: Office/Business :: Financial :: Investment",
        "License :: OSI Approved :: Apache Software License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: C++",
    ],
)

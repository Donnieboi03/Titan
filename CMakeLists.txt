cmake_minimum_required(VERSION 3.15)
project(titan VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")

# Find pybind11
find_package(pybind11 CONFIG)
if(NOT pybind11_FOUND)
    # Try to find via Python
    execute_process(
        COMMAND python3 -c "import pybind11; print(pybind11.get_cmake_dir())"
        OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(PYBIND11_CMAKE_DIR)
        list(APPEND CMAKE_PREFIX_PATH ${PYBIND11_CMAKE_DIR})
        find_package(pybind11 CONFIG REQUIRED)
    else()
        message(FATAL_ERROR "pybind11 not found. Install with: pip install pybind11")
    endif()
endif()

# C++ Core Library (header-only components)
add_library(titan_core INTERFACE)
target_include_directories(titan_core INTERFACE 
    ${CMAKE_CURRENT_SOURCE_DIR}/backtesting-engine
    ${CMAKE_CURRENT_SOURCE_DIR}/tools
)

# Python bindings module
pybind11_add_module(titan_core_ext 
    python/bindings.cpp
)

target_link_libraries(titan_core_ext PRIVATE titan_core)

# Set output directory for Python module
set_target_properties(titan_core_ext PROPERTIES
    OUTPUT_NAME "titan_core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/titan"
)

# C++ Tests (optional)
option(BUILD_TESTS "Build C++ tests" ON)
if(BUILD_TESTS)
    enable_testing()
    
    # Engine runtime tests
    add_executable(engine_runtime_test 
        backtesting-engine/test/engine_runtime_test.cpp
    )
    target_link_libraries(engine_runtime_test PRIVATE titan_core)
    add_test(NAME engine_runtime_test COMMAND engine_runtime_test)
    
    # Order engine tests
    add_executable(order_test 
        backtesting-engine/test/order_test.cpp
    )
    target_link_libraries(order_test PRIVATE titan_core)
    add_test(NAME order_test COMMAND order_test)
    
    # Scheduler tests
    add_executable(scheduler_test 
        backtesting-engine/test/scheduler_test.cpp
    )
    target_link_libraries(scheduler_test PRIVATE titan_core)
    add_test(NAME scheduler_test COMMAND scheduler_test)
endif()

# Installation
install(TARGETS titan_core_ext 
    LIBRARY DESTINATION python/titan
)

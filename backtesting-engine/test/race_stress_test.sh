cd /Users/donnieb/Desktop/AI/Titan/backtesting-engine/test

echo "Running 100 iterations of scheduler_test..."
echo "============================================"

PASSED=0
FAILED=0

for i in {1..100}; do
    clang++ -std=c++20 scheduler_test.cpp -o scheduler_test 2>/dev/null
    if ./scheduler_test >/dev/null 2>&1; then
        ((PASSED++))
        echo -n "."
    else
        ((FAILED++))
        echo -n "X"
    fi
    
    # Add newline every 50 runs for readability
    if [ $((i % 50)) -eq 0 ]; then
        echo " [$i/100]"
    fi
done

echo ""
echo "============================================"
echo "Results: $PASSED passed, $FAILED failed out of 100 runs"
echo "Success rate: $PASSED%"
echo "============================================"

# Cleanup
rm -f scheduler_test

# Run one more time with output if there were failures
if [ $FAILED -gt 0 ]; then
    echo ""
    echo "Running one final test with output to show failure:"
    clang++ -std=c++20 scheduler_test.cpp -O2 -o scheduler_test 2>/dev/null
    ./scheduler_test
    rm -f scheduler_test
fi

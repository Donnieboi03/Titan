#include "../Heap.cpp"
#include "../RingBuffer.cpp"
#include "../Arena.cpp"
#include <deque>
#include <iostream>
#include <set>
#include <map>
#include <cassert>

void test_ringbuffer()
{
    std::cout << "=== Testing RingBuffer ===\n";
    
    RingBuffer<int> rb;
    const int NUM_ELEMENTS = 100000;
    
    // Push 100,000 elements
    std::cout << "Pushing " << NUM_ELEMENTS << " elements...\n";
    for (int i = 0; i < NUM_ELEMENTS; ++i) {
        rb.push(i);
    }
    std::cout << "Push complete!\n";
    
    // Pop all elements and verify order
    std::cout << "Popping all elements...\n";
    for (int i = 0; i < NUM_ELEMENTS; ++i) {
        assert(rb.front() == i && "Front should match expected value");
        rb.pop();
    }
    std::cout << "Pop complete!\n";
    
    assert(rb.empty() && "RingBuffer should be empty after popping all elements");
    
    std::cout << "RingBuffer test PASSED! All " << NUM_ELEMENTS << " elements pushed and popped correctly.\n\n";
}

void test_heap()
{
    std::cout << "=== Testing Heap (Min Heap) ===\n";
    
    Heap<int, HeapType::MIN> minHeap;
    const int NUM_ELEMENTS = 100000;
    
    // Push random elements
    std::cout << "Pushing " << NUM_ELEMENTS << " elements...\n";
    for (int i = 0; i < NUM_ELEMENTS; ++i) {
        minHeap.push(i);
    }
    std::cout << "Push complete! Size: " << minHeap.size() << "\n";
    
    // Verify heap property (peek should always return smallest)
    std::cout << "Verifying heap property by popping all elements...\n";
    int prev = minHeap.peek();
    minHeap.pop();
    
    for (int i = 1; i < NUM_ELEMENTS; ++i) {
        int current = minHeap.peek();
        assert(current >= prev && "Heap property violated: current should be >= previous");
        prev = current;
        minHeap.pop();
    }
    
    assert(minHeap.size() == 0 && "Heap should be empty after all pops");
    std::cout << "Min Heap test PASSED!\n";
    
    // Test Max Heap
    std::cout << "\n=== Testing Heap (Max Heap) ===\n";
    Heap<int, HeapType::MAX> maxHeap;
    
    std::cout << "Pushing " << NUM_ELEMENTS << " elements...\n";
    for (int i = 0; i < NUM_ELEMENTS; ++i) {
        maxHeap.push(i);
    }
    std::cout << "Push complete! Size: " << maxHeap.size() << "\n";
    
    std::cout << "Verifying max heap property...\n";
    prev = maxHeap.peek();
    maxHeap.pop();
    
    for (int i = 1; i < NUM_ELEMENTS; ++i) {
        int current = maxHeap.peek();
        assert(current <= prev && "Max heap property violated: current should be <= previous");
        prev = current;
        maxHeap.pop();
    }
    
    assert(maxHeap.size() == 0 && "Max heap should be empty after all pops");
    std::cout << "Max Heap test PASSED!\n\n";
}

void test_arena()
{
    std::cout << "=== Testing Arena ===\n";
    
    const int CAPACITY = 100000;
    Arena<int> arena(CAPACITY);
    
    std::cout << "Arena capacity: " << arena.capacity() << "\n";
    
    // Allocate elements
    std::cout << "Allocating " << CAPACITY << " elements...\n";
    std::vector<Arena<int>::Index> indices;
    for (int i = 0; i < CAPACITY; ++i) {
        auto idx = arena.allocate(std::move(i));
        assert(idx != static_cast<Arena<int>::Index>(-1) && "Allocation should succeed");
        indices.push_back(idx);
    }
    std::cout << "Allocation complete! Size: " << arena.size() << "\n";
    
    // Verify values
    std::cout << "Verifying allocated values...\n";
    for (int i = 0; i < CAPACITY; ++i) {
        assert(arena[indices[i]] == i && "Arena value should match");
    }
    std::cout << "Verification complete!\n";
    
    // Free some elements
    std::cout << "Freeing first 50,000 elements...\n";
    for (int i = 0; i < 50000; ++i) {
        arena.free(indices[i]);
    }
    std::cout << "Free complete! Size: " << arena.size() << "\n";
    assert(arena.size() == 50000 && "Arena size should be 50000 after freeing half");
    
    // Reallocate freed slots
    std::cout << "Reallocating 25,000 elements...\n";
    for (int i = 0; i < 25000; ++i) {
        auto idx = arena.allocate(std::move(i + 1000000));
        assert(idx != static_cast<Arena<int>::Index>(-1) && "Reallocation should succeed");
        assert(arena[idx] == i + 1000000 && "Reallocated value should match");
    }
    std::cout << "Reallocation complete! Size: " << arena.size() << "\n";
    assert(arena.size() == 75000 && "Arena size should be 75000");
    
    std::cout << "Arena test PASSED!\n\n";
}

int main()
{
    std::cout << "========================================\n";
    std::cout << "  Data Structure Stress Tests\n";
    std::cout << "========================================\n\n";
    
    test_ringbuffer();
    test_heap();
    test_arena();
    
    std::cout << "========================================\n";
    std::cout << "  All Tests PASSED!\n";
    std::cout << "========================================\n";
    
    return 0;
}
#pragma once

#include <vector>
#include <atomic>
#include <algorithm>
#include <thread>

// Cache Line Size for M3 Mac Pro
#define CACHE_LINE 128

template <typename T>
class alignas(CACHE_LINE) DoubleBuffer
{
public:
    explicit DoubleBuffer(std::size_t capacity) noexcept;
    DoubleBuffer(DoubleBuffer&& other) noexcept;
    DoubleBuffer& operator=(DoubleBuffer&& other) noexcept;

    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;

    bool try_push(T&& value) noexcept;

    template <typename... Args>
    bool try_emplace(Args&&... args) noexcept;
    
    bool try_flush() noexcept;
    
    bool try_pop(T& out) noexcept;
    
    inline bool empty() const noexcept;

    inline bool full() const noexcept;
    
    inline std::size_t pending_writes() const noexcept;
    
    inline std::size_t pending_reads() const noexcept;

private:

    const std::size_t capacity_; // Buffer Capacity

    // Consumer Touchable Producer-only state (writer thread) - grouped on one cache line
    struct alignas(CACHE_LINE) ProducerState 
    {
        std::atomic<std::size_t> write_index{0};
        std::atomic<std::vector<T>*> write_buffer{nullptr};
        std::atomic<bool> swap_requested{false};
    } producer_;

    // Producer Touchable Consumer-only state (reader thread) - grouped on another cache line
    struct alignas(CACHE_LINE) ConsumerState 
    {
        std::atomic<std::size_t> read_index{0};
        std::atomic<std::size_t> read_size{0};
        std::atomic<std::vector<T>*> read_buffer{nullptr};
    } consumer_;

    std::vector<T> buffer_a_; // Data Buffer A
    std::vector<T> buffer_b_; // Data Buffer B
};

template <typename T>
DoubleBuffer<T>::DoubleBuffer(std::size_t capacity) noexcept
: capacity_(capacity)
{
    buffer_a_.reserve(capacity); 
    buffer_b_.reserve(capacity);
    
    producer_.write_buffer.store(&buffer_a_, std::memory_order_relaxed);
    consumer_.read_buffer.store(&buffer_b_, std::memory_order_relaxed);
}

template<typename T>
DoubleBuffer<T>::DoubleBuffer(DoubleBuffer&& other) noexcept
    : buffer_a_(std::move(other.buffer_a_))
    , buffer_b_(std::move(other.buffer_b_))
    , capacity_(other.capacity_)
{
    // Copy atomic state values from other into our aligned producer/consumer state
    producer_.swap_requested.store(other.producer_.swap_requested.load(std::memory_order_relaxed), std::memory_order_relaxed);
    producer_.write_index.store(other.producer_.write_index.load(std::memory_order_relaxed), std::memory_order_relaxed);
    consumer_.read_index.store(other.consumer_.read_index.load(std::memory_order_relaxed), std::memory_order_relaxed);
    consumer_.read_size.store(other.consumer_.read_size.load(std::memory_order_relaxed), std::memory_order_relaxed);

    auto* other_write = other.producer_.write_buffer.load(std::memory_order_relaxed);
    auto* other_read = other.consumer_.read_buffer.load(std::memory_order_relaxed);

    if (other_write == &other.buffer_a_)
        producer_.write_buffer.store(&buffer_a_, std::memory_order_relaxed);
    else
        producer_.write_buffer.store(&buffer_b_, std::memory_order_relaxed);

    if (other_read == &other.buffer_a_)
        consumer_.read_buffer.store(&buffer_a_, std::memory_order_relaxed);
    else
        consumer_.read_buffer.store(&buffer_b_, std::memory_order_relaxed);
}

template <typename T>
DoubleBuffer<T>& DoubleBuffer<T>::operator=(DoubleBuffer&& other) noexcept
{
    if (this != &other)
    {
        buffer_a_ = std::move(other.buffer_a_);
        buffer_b_ = std::move(other.buffer_b_);
        producer_.swap_requested.store(other.producer_.swap_requested.load(std::memory_order_relaxed), std::memory_order_relaxed);
        producer_.write_index.store(other.producer_.write_index.load(std::memory_order_relaxed), std::memory_order_relaxed);
        consumer_.read_index.store(other.consumer_.read_index.load(std::memory_order_relaxed), std::memory_order_relaxed);
        consumer_.read_size.store(other.consumer_.read_size.load(std::memory_order_relaxed), std::memory_order_relaxed);
        capacity_ = other.capacity_;
    
        auto* other_write = other.producer_.write_buffer.load(std::memory_order_relaxed);
        auto* other_read = other.consumer_.read_buffer.load(std::memory_order_relaxed);

        if (other_write == &other.buffer_a_)
            producer_.write_buffer.store(&buffer_a_, std::memory_order_relaxed);
        else
            producer_.write_buffer.store(&buffer_b_, std::memory_order_relaxed);

        if (other_read == &other.buffer_a_)
            consumer_.read_buffer.store(&buffer_a_, std::memory_order_relaxed);
        else
            consumer_.read_buffer.store(&buffer_b_, std::memory_order_relaxed);
    }

    return *this;
}

template <typename T>
bool DoubleBuffer<T>::try_push(T&& value) noexcept
{
    if (producer_.swap_requested.load(std::memory_order_acquire)) return false;

    auto widx = producer_.write_index.load(std::memory_order_relaxed);
    if (widx >= capacity_) return false;

    producer_.write_index.fetch_add()

    (*producer_.write_buffer.load(std::memory_order_relaxed))[widx] = std::move(value);
    producer_.write_index.store(widx + 1, std::memory_order_release);
    return true;
}

template <typename T>
template <typename... Args>
bool DoubleBuffer<T>::try_emplace(Args&&... args) noexcept
{
    if (producer_.swap_requested.load(std::memory_order_acquire))
        return false;

    auto widx = producer_.write_index.load(std::memory_order_relaxed);
    if (widx >= capacity_) return false;

    auto* w = producer_.write_buffer.load(std::memory_order_relaxed);
    (*w)[widx].~T();
    new (&(*w)[widx]) T(std::forward<Args>(args)...);
    producer_.write_index.store(widx + 1, std::memory_order_release);
    return true;
}

template <typename T>
bool DoubleBuffer<T>::try_flush() noexcept
{
    auto write_sz = producer_.write_index.load(std::memory_order_relaxed);
    if (write_sz == 0) return true;

    if (consumer_.read_index.load(std::memory_order_relaxed) < consumer_.read_size.load(std::memory_order_relaxed))
        return false;

    producer_.swap_requested.store(true, std::memory_order_release);

    auto* old_write = producer_.write_buffer.load(std::memory_order_relaxed);
    producer_.write_buffer.store(consumer_.read_buffer.load(std::memory_order_relaxed), std::memory_order_relaxed);
    consumer_.read_buffer.store(old_write, std::memory_order_relaxed);

    consumer_.read_size.store(write_sz, std::memory_order_release);
    consumer_.read_index.store(0, std::memory_order_release);
    producer_.write_index.store(0, std::memory_order_release);

    producer_.swap_requested.store(false, std::memory_order_release);
    return true;
}

template <typename T>
bool DoubleBuffer<T>::try_pop(T& out) noexcept
{
    std::size_t idx = consumer_.read_index.load(std::memory_order_acquire);
    if (idx >= consumer_.read_size.load(std::memory_order_acquire)) return false;

    out = std::move((*consumer_.read_buffer.load(std::memory_order_relaxed))[idx]);
    consumer_.read_index.store(idx + 1, std::memory_order_release);
    return true;
}

template <typename T>
inline bool DoubleBuffer<T>::empty() const noexcept
{
    return producer_.write_index.load(std::memory_order_relaxed) == 0;
}

template <typename T>
inline bool DoubleBuffer<T>::full() const noexcept
{
    return producer_.write_index.load(std::memory_order_relaxed) >= capacity_;
}

template <typename T>
inline std::size_t DoubleBuffer<T>::pending_writes() const noexcept
{
    return producer_.write_index.load(std::memory_order_relaxed);
}

template <typename T>
inline std::size_t DoubleBuffer<T>::pending_reads() const noexcept
{
    std::size_t idx = consumer_.read_index.load(std::memory_order_relaxed);
    std::size_t size = consumer_.read_size.load(std::memory_order_relaxed);
    if (idx >= size) return 0;
    return size - idx;
}
#pragma once
#include <vector>
#include <atomic>
#include <algorithm>
#include <utility>
#include <thread>

template <typename T>
class DoubleBuffer
{
private:
    std::vector<T> buffer_a_;
    std::vector<T> buffer_b_;
    
    // Use atomic pointers to vectors
    std::atomic<std::vector<T>*> write_buffer_;
    std::atomic<std::vector<T>*> read_buffer_;
    
    std::atomic<bool> swap_requested_{false};
    
    std::atomic<std::size_t> read_index_{0};
    std::atomic<std::size_t> write_index_{0};
    std::atomic<std::size_t> read_size_{0};
    std::size_t capacity_;
    
public:
    explicit DoubleBuffer(std::size_t capacity) noexcept
    : capacity_(capacity)
    {
        buffer_a_.resize(capacity); 
        buffer_b_.resize(capacity);
        
        write_buffer_.store(&buffer_a_, std::memory_order_relaxed);
        read_buffer_.store(&buffer_b_, std::memory_order_relaxed);
    }

    DoubleBuffer(DoubleBuffer&& other) noexcept
    : buffer_a_(std::move(other.buffer_a_))
    , buffer_b_(std::move(other.buffer_b_))
    , swap_requested_(other.swap_requested_.load())
    , read_index_(other.read_index_.load())
    , capacity_(other.capacity_)
    {
        auto* other_write = other.write_buffer_.load();
        auto* other_read = other.read_buffer_.load();
        
        if (other_write == &other.buffer_a_)
            write_buffer_.store(&buffer_a_, std::memory_order_relaxed);
        else
            write_buffer_.store(&buffer_b_, std::memory_order_relaxed);
        
        if (other_read == &other.buffer_a_)
            read_buffer_.store(&buffer_a_, std::memory_order_relaxed);
        else
            read_buffer_.store(&buffer_b_, std::memory_order_relaxed);
    }

    DoubleBuffer& operator=(DoubleBuffer&& other) noexcept
    {
        if (this != &other)
        {
            buffer_a_ = std::move(other.buffer_a_);
            buffer_b_ = std::move(other.buffer_b_);
            swap_requested_.store(other.swap_requested_.load());
            read_index_.store(other.read_index_.load());
            capacity_ = other.capacity_;
            
            auto* other_write = other.write_buffer_.load();
            auto* other_read = other.read_buffer_.load();
            
            if (other_write == &other.buffer_a_)
                write_buffer_.store(&buffer_a_, std::memory_order_relaxed);
            else
                write_buffer_.store(&buffer_b_, std::memory_order_relaxed);
            
            if (other_read == &other.buffer_a_)
                read_buffer_.store(&buffer_a_, std::memory_order_relaxed);
            else
                read_buffer_.store(&buffer_b_, std::memory_order_relaxed);
        }
        return *this;
    }

    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;

    bool try_push(T&& value) noexcept
    {
        // If requesting swap, then fail push
        if (swap_requested_.load(std::memory_order_acquire))
            return false;

        // Check if write idx is at capacity
        auto widx = write_index_.load(std::memory_order_relaxed);
        if (widx >= capacity_) 
            return false;

        // Insert new value, and itterate idx
        auto* w = write_buffer_.load(std::memory_order_relaxed);
        (*w)[widx] = std::move(value);
        write_index_.store(widx + 1, std::memory_order_release);
        return true;
    }
    
    void flush() noexcept
    {
        // Check if write idx is 0
        auto write_sz = write_index_.load(std::memory_order_acquire);
        if (write_sz == 0) 
            return;

        // Ask consumer to finish the current read buffer
        swap_requested_.store(true, std::memory_order_release);

        // Wait until consumer drained read buffer
        while (read_index_.load(std::memory_order_acquire) < read_size_.load(std::memory_order_acquire))
            std::this_thread::yield();

        // Swap buffers
        auto* w = write_buffer_.load(std::memory_order_relaxed);
        auto* r = read_buffer_.load(std::memory_order_relaxed);
        write_buffer_.store(r, std::memory_order_relaxed);
        read_buffer_.store(w, std::memory_order_relaxed);

        // Publish the new readable size
        read_size_.store(write_sz, std::memory_order_release);
        read_index_.store(0, std::memory_order_release);
        write_index_.store(0, std::memory_order_release);

        // Swap complete
        swap_requested_.store(false, std::memory_order_release);
    }
    
    bool try_pop(T& out) noexcept
    {
        std::size_t idx  = read_index_.load(std::memory_order_acquire);
        std::size_t size = read_size_.load(std::memory_order_acquire);

        if (idx >= size) 
        {
            // If producer is waiting to swap, yield to let it proceed
            if (swap_requested_.load(std::memory_order_acquire))
                std::this_thread::yield();
            return false;
        }

        // Pop into out, and itterate idx
        auto* r = read_buffer_.load(std::memory_order_relaxed);
        out = std::move((*r)[idx]);
        read_index_.store(idx + 1, std::memory_order_release);
        return true;
    }
    
    bool empty() const noexcept
    {
        std::size_t read_idx = read_index_.load(std::memory_order_acquire);
        std::size_t read_sz = read_size_.load(std::memory_order_acquire);
        std::size_t write_idx = write_index_.load(std::memory_order_acquire);
        return read_idx >= read_sz && write_idx == 0;
    }

    bool full() const noexcept
    {
        std::size_t write_idx = write_index_.load(std::memory_order_acquire);
        return write_idx >= capacity_;
    }
    
    std::size_t pending_writes() const noexcept
    {
        return write_index_.load(std::memory_order_acquire);
    }
    
    std::size_t pending_reads() const noexcept
    {
        std::size_t idx = read_index_.load(std::memory_order_acquire);
        std::size_t size = read_size_.load(std::memory_order_acquire);
        if (idx >= size)
            return 0;
        return size - idx;
    }
};

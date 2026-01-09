#pragma once
#include <vector>

template <typename T>
struct RingBuffer 
{
private:
    static constexpr std::size_t DEFAULT_RESERVED_SIZE = 32768; // 32K default

public:
    RingBuffer() noexcept
    : head_(0)
    {
        q_.reserve(DEFAULT_RESERVED_SIZE); 
    }
    
    explicit RingBuffer(std::size_t reserve_size) noexcept
    : head_(0)
    {
        q_.reserve(reserve_size);
    }
    
    void reserve(std::size_t new_capacity) noexcept
    {
        q_.reserve(new_capacity);
    }

    void push(T&& value) noexcept { q_.push_back(std::forward<T>(value)); }
    void push(const T& value) noexcept { q_.push_back(value); }

    void pop() noexcept
    {
        ++head_;
        maybe_compact();
    }
    
    T& front() noexcept { return q_[head_]; }
    const T& front() const noexcept { return q_[head_]; }
    
    bool empty() const noexcept { return head_ >= q_.size(); }
    std::size_t size() const noexcept { return q_.size() - head_; }

private:
    std::vector<T> q_;
    std::size_t head_;

    void maybe_compact() noexcept
    {
        constexpr std::size_t COMPACT_THRESHOLD = 1024;
        if (head_ > COMPACT_THRESHOLD && head_ * 2 > q_.size()) 
        {
            q_.erase(q_.begin(), q_.begin() + head_);
            head_ = 0;
        }
    }
};
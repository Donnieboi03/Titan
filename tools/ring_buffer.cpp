#pragma once
#include <vector>

template <typename T>
struct RingBuffer 
{
private:
    static constexpr std::size_t ELEMENTS_PER_LINE = 64 / sizeof(T); // L1 Cache Line Size
    static constexpr std::size_t RESERVED_SIZE = ELEMENTS_PER_LINE * 512; // Default Buffer Size

public:
    RingBuffer() noexcept
    : head_(0)
    {
        q_.reserve(RESERVED_SIZE); 
    }

    void push(T value) noexcept { q_.push_back(value); }

    void pop() noexcept
    {
        ++head_;
        maybe_compact();
    }
    
    T front() const noexcept { return q_[head_]; }
    bool empty() const noexcept { return head_ >= q_.size(); }

private:
    std::vector<T> q_; // Ring-Buffer
    std::size_t head_; // Current Head

    void maybe_compact() 
    {
        if (head_ > ELEMENTS_PER_LINE && head_ * 2 > q_.size()) 
        {
            q_.erase(q_.begin(), q_.begin() + head_);
            head_ = 0;
        }
    }
};
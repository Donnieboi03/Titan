#pragma once
#include <vector>

template <typename T>
struct LazyQueue 
{
    LazyQueue() noexcept;
    explicit LazyQueue(std::size_t reserve_size) noexcept;
    
    void reserve(std::size_t new_capacity) noexcept;

    void push(T&& value) noexcept;
    void push(const T& value) noexcept;

    template <typename... Args>
    T& emplace(Args&&... args) noexcept;

    void emplace(T&& value) noexcept;

    void pop() noexcept;
    
    T& front() noexcept;
    const T& front() const noexcept;
    
    bool empty() const noexcept;
    std::size_t size() const noexcept;

private:
    static constexpr std::size_t DEFAULT_RESERVED_SIZE = 4096; // 4KB default

    std::size_t head_;
    std::vector<T> q_;

    void maybe_compact() noexcept;
};

template <typename T>
LazyQueue<T>::LazyQueue() noexcept
: head_(0)
{
    q_.reserve(DEFAULT_RESERVED_SIZE); 
}

template <typename T>
LazyQueue<T>::LazyQueue(std::size_t reserve_size) noexcept
: head_(0)
{
    q_.reserve(reserve_size);
}

template <typename T>
void LazyQueue<T>::reserve(std::size_t new_capacity) noexcept
{
    q_.reserve(new_capacity);
}

template <typename T>
void LazyQueue<T>::push(T&& value) noexcept 
{ 
    q_.push_back(std::forward<T>(value)); 
}

template <typename T>
void LazyQueue<T>::push(const T& value) noexcept 
{ 
    q_.push_back(value); 
}

template <typename T>
template <typename... Args>
T& LazyQueue<T>::emplace(Args&&... args) noexcept
{
    q_.emplace_back(std::forward<Args>(args)...);
    return q_.back();
}

template <typename T>
void LazyQueue<T>::emplace(T&& value) noexcept 
{ 
    q_.emplace(std::forward<T>(value)); 
}

template <typename T>
void LazyQueue<T>::pop() noexcept
{
    ++head_;
    maybe_compact();
}

template <typename T>
T& LazyQueue<T>::front() noexcept 
{ 
    return q_[head_]; 
}

template <typename T>
const T& LazyQueue<T>::front() const noexcept 
{ 
    return q_[head_]; 
}

template <typename T>
bool LazyQueue<T>::empty() const noexcept 
{ 
    return head_ >= q_.size(); 
}

template <typename T>
std::size_t LazyQueue<T>::size() const noexcept 
{ 
    return q_.size() - head_; 
}

template <typename T>
void LazyQueue<T>::maybe_compact() noexcept
{
    constexpr std::size_t COMPACT_THRESHOLD = 1024;
    if (head_ > COMPACT_THRESHOLD && head_ * 2 > q_.size()) 
    {
        q_.erase(q_.begin(), q_.begin() + head_);
        head_ = 0;
    }
}

#pragma once
#include "OrderEngine.cpp"

template <typename T>
struct Arena
{
    using Index = std::uint32_t;

    Arena(std::size_t capacity) noexcept
    {
        data_.reserve(capacity);
        free_.reserve(capacity / 2);
    }

    Index allocate(T&& value) noexcept
    {
        if (!free_.empty())
        {
            Index idx = free_.back();
            free_.pop_back();
            data_[idx] = std::move(value);
            return idx;
        }
        
        data_.push_back(std::move(value));
        return static_cast<Index>(data_.size() - 1);
    }

    void free(Index idx) noexcept
    {
        free_.push_back(idx);
    }

    T& operator[](Index idx) noexcept
    {
        return data_[idx];
    }

    const T& operator[](Index idx) const noexcept 
    {
        return data_[idx];
    }

    std::size_t capacity() const noexcept { return data_.capacity(); }
    std::size_t size() const noexcept { return data_.size() - free_.size(); }

private:
    std::vector<T> data_;
    std::vector<std::size_t>free_;
};
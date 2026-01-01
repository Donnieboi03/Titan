#pragma once
#include <vector>
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
        // Check if we can reuse a free slot
        if (!free_.empty())
        {
            Index idx = free_.back();
            free_.pop_back();
            data_[idx] = std::move(value);
            return idx;
        }
        
        // Check if we have space to add a new element
        if (data_.size() >= data_.capacity())
            return -1;
        
        data_.push_back(std::move(value));
        return static_cast<Index>(data_.size() - 1);
    }

    template <typename... Args>
    Index emplace(Args&&... args) noexcept
    {
        // Check if we can reuse a free slot
        if (!free_.empty())
        {
            Index idx = free_.back();
            free_.pop_back();
            data_[idx] = T(std::forward<Args>(args)...);
            return idx;
        }
        
        // Check if we have space to add a new element
        if (data_.size() >= data_.capacity())
            return -1;
        
        data_.emplace_back(std::forward<Args>(args)...);
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
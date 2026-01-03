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
            // Reuse existing slot by move-assigning new value over old one
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

    void reset() noexcept
    {
        data_.clear();
        free_.clear();
    }

private:
    std::vector<T> data_;
    std::vector<Index> free_;
};
#pragma once
#include <vector>
#include <cstdint>
#include <new>
#include <utility>

// Memory Pool with generational handles - O(1) validation without maps
template <typename T>
struct MemoryPool {
    using Index = std::uint32_t;
    using Generation = std::uint16_t;
    using Handle = std::uint64_t;  // [48-bit slot | 16-bit generation]

    static constexpr Handle INVALID_HANDLE = static_cast<Handle>(-1);
    static constexpr Index INVALID_INDEX = static_cast<Index>(-1);

    MemoryPool(std::size_t capacity)
    {
        data_.resize(capacity);
        generation_.resize(capacity, 0);
        active_.resize(capacity, false);
        free_.reserve(capacity);

        // Populate free list (capacity-1 down to 0)
        for (Index i = 0; i < static_cast<Index>(capacity); ++i) {
            free_.push_back(static_cast<Index>(capacity) - 1 - i);
        }
    }

    // Allocate slot, construct T, return handle
    template <typename... Args>
    Handle emplace(Args&&... args) noexcept
    {
        if (free_.empty()) return INVALID_HANDLE;

        Index slot = free_.back();
        free_.pop_back();

        new (&data_[slot]) T(std::forward<Args>(args)...);
        active_[slot] = true;

        return make_handle(slot, generation_[slot]);
    }

    // Free slot, increment generation (invalidates old handles)
    void free(Handle handle) noexcept
    {
        Index slot = get_slot(handle);
        Generation gen = get_generation(handle);

        if (slot >= active_.size() || !active_[slot]) return;
        if (generation_[slot] != gen) return;  // Stale handle

        active_[slot] = false;
        generation_[slot]++;  // Invalidate old handles to this slot
        free_.push_back(slot);
    }

    // Check if handle is valid (O(1) - no map lookup!)
    bool is_valid(Handle handle) const noexcept
    {
        if (handle == INVALID_HANDLE) return false;
        Index slot = get_slot(handle);
        Generation gen = get_generation(handle);
        return slot < active_.size() && active_[slot] && generation_[slot] == gen;
    }

    // Access by handle (caller should check is_valid first or use get())
    T& operator[](Handle handle) noexcept
    {
        return data_[get_slot(handle)];
    }

    const T& operator[](Handle handle) const noexcept
    {
        return data_[get_slot(handle)];
    }

    // Safe access - returns nullptr if invalid
    T* get(Handle handle) noexcept
    {
        if (!is_valid(handle)) return nullptr;
        return &data_[get_slot(handle)];
    }

    const T* get(Handle handle) const noexcept
    {
        if (!is_valid(handle)) return nullptr;
        return &data_[get_slot(handle)];
    }

    // Extract slot from handle
    static constexpr Index get_slot(Handle h) noexcept
    {
        return static_cast<Index>(h >> 16);
    }

    // Extract generation from handle
    static constexpr Generation get_generation(Handle h) noexcept
    {
        return static_cast<Generation>(h & 0xFFFF);
    }

    // Create handle from slot + generation
    static constexpr Handle make_handle(Index slot, Generation gen) noexcept
    {
        return (static_cast<Handle>(slot) << 16) | gen;
    }

    void reset() noexcept
    {
        free_.clear();
        for (Index i = 0; i < static_cast<Index>(data_.size()); ++i)
        {
            active_[i] = false;
            generation_[i]++;
            free_.push_back(static_cast<Index>(data_.size()) - 1 - i);
        }
    }

    std::size_t size() const noexcept { return data_.size() - free_.size(); }
    std::size_t capacity() const noexcept { return data_.size(); }
    std::size_t active_count() const noexcept { return data_.size() - free_.size(); }

private:
    std::vector<T> data_;
    std::vector<Generation> generation_;
    std::vector<uint8_t> active_;
    std::vector<Index> free_;
};

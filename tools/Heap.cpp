#pragma once
#include <vector>

enum class HeapType
{
    MIN,
    MAX
};

template <typename T, HeapType TYPE = HeapType::MIN>
struct Heap
{
    Heap() noexcept
    {
        heap_.reserve(RESERVED_SIZE);
    }

    void push(T data) noexcept
    {
        heap_.push_back(data);
        heapify_up(heap_.size() - 1);
    }

    template <typename... Args>
    void emplace(Args&&... args) noexcept
    {
        heap_.emplace_back(std::forward<Args>(args)...);
        heapify_up(heap_.size() - 1);
    }

    void pop(const int idx = 0) noexcept
    {
            if (!heap_.size()) return;
            std::swap(heap_[idx], heap_[heap_.size() - 1]);
            heap_.pop_back();
            heapify_down(idx);
    }

    T peek(int idx = 0) const noexcept { return heap_[idx]; }

    int find(T data) const noexcept
    {
        for (int i = 0; i < heap_.size(); i++)
        {
            if (heap_[i] == data) return i;
        }  
        return -1;
    }

    int size() const noexcept { return heap_.size(); }
    bool empty() const noexcept { return !heap_.size(); }

private:
    std::vector<T> heap_;
    static constexpr std::size_t RESERVED_SIZE = sizeof(T) * 32;

    // For Pushing from the End
    void heapify_up(int idx)
    {
        while (idx > 0)
        {
            int parent = (idx - 1) / 2;
            if constexpr (TYPE == HeapType::MIN)
            {
                if (heap_[idx] >= heap_[parent]) break;
            }
            else
            {
                if (heap_[idx] <= heap_[parent]) break;
            }
            std::swap(heap_[idx], heap_[parent]);
            idx = parent;
        }
    }

    // For Popping from Front
    void heapify_down(int idx)
    {
        while (idx < heap_.size())
        {
            int left_child = (idx * 2) + 1 < heap_.size() ? (idx * 2) + 1 : idx;
            int right_child = (idx * 2) + 2 < heap_.size() ? (idx * 2) + 2 : idx;
            int best_child = idx;

            if (left_child < heap_.size()) 
            {
                if constexpr (TYPE == HeapType::MIN)
                {
                    if (heap_[left_child] < heap_[best_child])
                        best_child = left_child;
                }
                else
                {
                    if (heap_[left_child] > heap_[best_child])
                        best_child = left_child;
                }
            }

            if (right_child < heap_.size()) 
            {
                if constexpr (TYPE == HeapType::MIN)
                {
                    if (heap_[right_child] < heap_[best_child])
                        best_child = right_child;
                }
                else
                {
                    if (heap_[right_child] > heap_[best_child])
                        best_child = right_child;
                }
            }
            
            if (best_child == idx) break;

            std::swap(heap_[idx], heap_[best_child]);
            idx = best_child;
        }
    }
};
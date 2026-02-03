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
    Heap() noexcept;

    void push(T data) noexcept;

    template <typename... Args>
    void emplace(Args&&... args) noexcept;

    void pop(const int idx = 0) noexcept;

    T peek(int idx = 0) const noexcept;

    int find(T data) const noexcept;

    int size() const noexcept;
    bool empty() const noexcept;

private:
    static constexpr std::size_t RESERVED_SIZE = 64 * sizeof(T);

    std::vector<T> heap_;

    void heapify_up(int idx);
    void heapify_down(int idx);
};

template <typename T, HeapType TYPE>
Heap<T, TYPE>::Heap() noexcept
{
    heap_.reserve(RESERVED_SIZE);
}

template <typename T, HeapType TYPE>
void Heap<T, TYPE>::push(T data) noexcept
{
    heap_.push_back(data);
    heapify_up(heap_.size() - 1);
}

template <typename T, HeapType TYPE>
template <typename... Args>
void Heap<T, TYPE>::emplace(Args&&... args) noexcept
{
    heap_.emplace_back(std::forward<Args>(args)...);
    heapify_up(heap_.size() - 1);
}

template <typename T, HeapType TYPE>
void Heap<T, TYPE>::pop(const int idx) noexcept
{
    if (!heap_.size()) return;
    std::swap(heap_[idx], heap_[heap_.size() - 1]);
    heap_.pop_back();
    heapify_down(idx);
}

template <typename T, HeapType TYPE>
T Heap<T, TYPE>::peek(int idx) const noexcept 
{ 
    return heap_[idx]; 
}

template <typename T, HeapType TYPE>
int Heap<T, TYPE>::find(T data) const noexcept
{
    for (int i = 0; i < heap_.size(); i++)
    {
        if (heap_[i] == data) return i;
    }  
    return -1;
}

template <typename T, HeapType TYPE>
int Heap<T, TYPE>::size() const noexcept 
{ 
    return heap_.size(); 
}

template <typename T, HeapType TYPE>
bool Heap<T, TYPE>::empty() const noexcept 
{ 
    return !heap_.size(); 
}

template <typename T, HeapType TYPE>
void Heap<T, TYPE>::heapify_up(int idx)
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

template <typename T, HeapType TYPE>
void Heap<T, TYPE>::heapify_down(int idx)
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

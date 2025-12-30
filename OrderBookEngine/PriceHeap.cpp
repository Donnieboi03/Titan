#pragma once
#include "OrderEngine.cpp"
struct PriceHeap
{
    PriceHeap() noexcept
    : min_(true)
    {
        heap_.reserve(RESERVED_SIZE);
    }

    PriceHeap(const bool min) noexcept
    : min_(min)
    {
        heap_.reserve(RESERVED_SIZE);
    }

    void push(Price data) noexcept
    {
        heap_.push_back(data);
        heapify_up(heap_.size() - 1);
    }

    void pop(const int index = 0) noexcept
    {
            if (!heap_.size()) return;
            std::swap(heap_[index], heap_[heap_.size() - 1]);
            heap_.pop_back();
            heapify_down(index);
    }

    double peek() const noexcept
    { return heap_[0]; }

    double at(const int index) const noexcept
    { return heap_[index]; }

    int find(Price data) const noexcept
    {
        for (int i = 0; i < heap_.size(); i++)
        {
            if (heap_[i] == data) return i;
        }  
        return -1;
    }

    int size() const noexcept 
    { return heap_.size(); }

private:
    std::vector<Price> heap_; // Heap 
    const bool min_; // Flag for Min or Max Heap
    static constexpr std::size_t RESERVED_SIZE = sizeof(Price) * 32; // Default Heap Size

    // For Pushing from the End
    void heapify_up(int index)
    {
        while (index > 0)
        {
            int parent = (index - 1) / 2;
            if (min_ && heap_[index] >= heap_[parent]) break;
            else if (!min_ && heap_[index] <= heap_[parent]) break;
            std::swap(heap_[index], heap_[parent]);
            index = parent;
        }
    }

    // For Popping from Front
    void heapify_down(int index)
    {
        while (index < heap_.size())
        {
            int left_child = (index * 2) + 1 < heap_.size() ? (index * 2) + 1 : index;
            int right_child = (index * 2) + 2 < heap_.size() ? (index * 2) + 2 : index;
            int best_child = index;

            if (left_child < heap_.size()) 
            {
                if ((min_ && heap_[left_child] < heap_[best_child]) || (!min_ && heap_[left_child] > heap_[best_child])) 
                {
                    best_child = left_child;
                }
            }

            if (right_child < heap_.size()) 
            {
                if ((min_ && heap_[right_child] < heap_[best_child]) || (!min_ && heap_[right_child] > heap_[best_child])) 
                {
                    best_child = right_child;
                }
            }
            
            if (best_child == index) break;

            std::swap(heap_[index], heap_[best_child]);
            index = best_child;
        }
    }
};
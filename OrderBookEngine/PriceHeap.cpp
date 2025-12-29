#pragma once
#include <vector>
#include <iostream>

// Order Book
class PriceHeap
{
public:
    PriceHeap()
    : heap(0), min(true)
    {
    }

    PriceHeap(const bool _min)
    : heap(0), min(_min)
    {
    }

    void push(const double data)
    {
        heap.push_back(data);
        heapify_up(heap.size() - 1);
    }

    void pop(const int index = 0)
    {
            if (!heap.size()) 
                return;
            std::swap(heap[index], heap[heap.size() - 1]);
            heap.pop_back();
            heapify_down(index);
    }

    double peek() const 
    { 
        if (!heap.size()) 
            return -1;
        return heap[0]; 
    }

    double at(const int index) const
    {
        if (!heap.size()) 
            return -1;
        return heap[index]; 
    }

    int find(double data) const
    {
        for (int i = 0; i < heap.size(); i++)
        {
            if (heap[i] == data) return i;
        }  
        return -1;
    }

    int size() const { return heap.size(); }

private:
    std::vector<double> heap;
    const bool min;

    // For Pushing from the End
    void heapify_up(int index)
    {
        while (index > 0)
        {
            int parent = (index - 1) / 2;
            if (min && heap[index] >= heap[parent]) break;
            else if (!min && heap[index] <= heap[parent]) break;
            std::swap(heap[index], heap[parent]);
            index = parent;
        }
    }

    // For Popping from Front
    void heapify_down(int index)
    {
        while (index < heap.size())
        {
            int left_child = (index * 2) + 1 < heap.size() ? (index * 2) + 1 : index;
            int right_child = (index * 2) + 2 < heap.size() ? (index * 2) + 2 : index;
            int best_child = index;

            if (left_child < heap.size()) 
            {
                if ((min && heap[left_child] < heap[best_child]) || (!min && heap[left_child] > heap[best_child])) 
                {
                    best_child = left_child;
                }
            }

            if (right_child < heap.size()) 
            {
                if ((min && heap[right_child] < heap[best_child]) || (!min && heap[right_child] > heap[best_child])) 
                {
                    best_child = right_child;
                }
            }
            
            if (best_child == index) break;

            std::swap(heap[index], heap[best_child]);
            index = best_child;
        }
    }
};
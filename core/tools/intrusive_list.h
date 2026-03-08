#pragma once

#include <cstddef>
#include <utility>
#include <vector>
#include <memory>

// Block-based pool of nodes for intrusive lists (OrderNodePool-style:
// allocate nodes in blocks, pointer-based freelist, stable addresses).

template <typename T>
class IntrusiveListPool {
public:
    static constexpr std::size_t BLOCK_SIZE = 4096;

    struct Node {
        T value;
        Node* next = nullptr;
        Node* prev = nullptr;
    };

    explicit IntrusiveListPool(std::size_t capacity_hint = 0);

    IntrusiveListPool(const IntrusiveListPool&) = delete;
    IntrusiveListPool& operator=(const IntrusiveListPool&) = delete;

    Node* allocate() noexcept;
    void deallocate(Node* node) noexcept;

    std::size_t capacity() const noexcept { return capacity_; }

private:
    void grow_block() noexcept;

    std::vector<std::unique_ptr<Node[]>> blocks_;
    Node* freelist_ = nullptr;
    std::size_t capacity_ = 0;
};

// List head: pointer-based, shared block pool. Same semantics as OrderLevel.

template <typename T>
class IntrusiveList {
public:
    using Pool = IntrusiveListPool<T>;
    using Node = typename Pool::Node;

    explicit IntrusiveList(Pool* pool) noexcept;

    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    void push_back(T&& value) noexcept;

    void pop_front() noexcept;

    T peek_front() const noexcept;

    bool remove(T const& value) noexcept;

    bool empty() const noexcept;

    std::size_t size() const noexcept;

    T back_index() const noexcept;

private:
    Pool* pool_;
    Node* front_ = nullptr;
    Node* back_ = nullptr;
    std::size_t count_ = 0;
};

// --- IntrusiveListPool implementation ---

template <typename T>
IntrusiveListPool<T>::IntrusiveListPool(std::size_t capacity_hint)
{
    if (capacity_hint > 0) {
        const std::size_t n_blocks = (capacity_hint + BLOCK_SIZE - 1) / BLOCK_SIZE;
        for (std::size_t b = 0; b < n_blocks; ++b)
            grow_block();
    }
}

template <typename T>
void IntrusiveListPool<T>::grow_block() noexcept
{
    std::unique_ptr<Node[]> block(new Node[BLOCK_SIZE]);
    for (std::size_t i = 0; i < BLOCK_SIZE - 1; ++i)
        block[i].next = &block[i + 1];
    block[BLOCK_SIZE - 1].next = freelist_;
    freelist_ = &block[0];
    blocks_.push_back(std::move(block));
    capacity_ += BLOCK_SIZE;
}

template <typename T>
typename IntrusiveListPool<T>::Node* IntrusiveListPool<T>::allocate() noexcept
{
    if (!freelist_)
        grow_block();
    Node* node = freelist_;
    freelist_ = freelist_->next;
    return node;
}

template <typename T>
void IntrusiveListPool<T>::deallocate(Node* node) noexcept
{
    node->next = freelist_;
    freelist_ = node;
}

// --- IntrusiveList implementation ---

template <typename T>
IntrusiveList<T>::IntrusiveList(Pool* pool) noexcept
    : pool_(pool)
{
}

template <typename T>
void IntrusiveList<T>::push_back(T&& value) noexcept
{
    Node* node = pool_->allocate();
    node->value = std::move(value);
    node->next = nullptr;
    node->prev = back_;

    if (back_) {
        back_->next = node;
        back_ = node;
    } else {
        front_ = back_ = node;
    }
    ++count_;
}

template <typename T>
void IntrusiveList<T>::pop_front() noexcept
{
    if (!front_) return;
    Node* old = front_;
    front_ = front_->next;
    if (front_)
        front_->prev = nullptr;
    else
        back_ = nullptr;
    pool_->deallocate(old);
    --count_;
}

template <typename T>
T IntrusiveList<T>::peek_front() const noexcept
{
    return front_ ? front_->value : T{};
}

template <typename T>
bool IntrusiveList<T>::remove(T const& value) noexcept
{
    Node* current = front_;
    while (current) {
        if (current->value == value) {
            if (current->prev)
                current->prev->next = current->next;
            else
                front_ = current->next;
            if (current->next)
                current->next->prev = current->prev;
            else
                back_ = current->prev;
            pool_->deallocate(current);
            --count_;
            return true;
        }
        current = current->next;
    }
    return false;
}

template <typename T>
bool IntrusiveList<T>::empty() const noexcept
{
    return count_ == 0;
}

template <typename T>
std::size_t IntrusiveList<T>::size() const noexcept
{
    return count_;
}

template <typename T>
T IntrusiveList<T>::back_index() const noexcept
{
    return back_ ? back_->value : T{};
}

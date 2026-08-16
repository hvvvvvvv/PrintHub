#pragma once

#include <cstddef>
#include <queue>
#include <utility>

// Synchronization is intentionally owned by the caller so the queue can be
// used with the same lock that protects its associated socket lifecycle.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : m_capacity(capacity) {}

    bool TryPush(T value) {
        if (m_items.size() >= m_capacity) return false;
        m_items.push(std::move(value));
        return true;
    }

    bool Empty() const noexcept { return m_items.empty(); }
    size_t Size() const noexcept { return m_items.size(); }

    T Pop() {
        T value = std::move(m_items.front());
        m_items.pop();
        return value;
    }

    void Clear() { std::queue<T>{}.swap(m_items); }

private:
    size_t m_capacity;
    std::queue<T> m_items;
};

#ifndef OBJ2VOXEL_THREADING_HPP
#define OBJ2VOXEL_THREADING_HPP

#include "voxelio/assert.hpp"
#include "voxelio/primitives.hpp"

#include <condition_variable>
#include <mutex>

namespace obj2voxel {

using namespace voxelio;

/**
 * @brief A simple ring buffer implementation for trivially destructible types.
 * A ring buffer is a FIFO container with a constant capacity.
 */
template <typename T, usize N, std::enable_if_t<std::is_trivially_destructible_v<T>, int> = 0>
class RingBuffer {
    T content[N];
    usize r = 0, w = 0, avail = 0;

public:
    constexpr RingBuffer() = default;

    /// Pops one element from the ring buffer.
    /// The result is always the least recently pushed element.
    /// This method fails if the buffer is empty.
    constexpr T pop()
    {
        VXIO_DEBUG_ASSERT_NE(avail, 0);
        --avail;
        r = (r + 1) % N;
        return std::move(content[r]);
    }

    /// Pushes one element to the ring buffer.
    /// This method fails if the buffer is full.
    constexpr void push(T value)
    {
        VXIO_DEBUG_ASSERT_NE(avail, N);
        ++avail;
        w = (w + 1) % N;
        content[w] = std::move(value);
    }

    /// Clears the ring buffer.
    constexpr void clear()
    {
        r = w = avail = 0;
    }

    /// Returns the least recently pushed element without popping it.
    /// This method fails if the buffer is empty.
    constexpr const T &peek() const
    {
        VXIO_DEBUG_ASSERT_NE(avail, 0);
        return content[r];
    }

    /// Returns the current size of the ring buffer which can be at most N.
    constexpr usize size() const
    {
        return avail;
    }

    /// Returns true if the buffer is empty.
    constexpr bool empty() const
    {
        return avail == 0;
    }

    /// Returns true if the buffer is full.
    constexpr bool full() const
    {
        return avail == N;
    }
};

namespace async {

/**
 * @brief An asynchronous implementation of a ring buffer.
 * This allows multiple threads to push and pop elements from/to the ring buffer.
 *
 * In the cases where a pop() or push() would fail for a regular ring buffer, this implementation blocks the executing
 * thread until the operation can be completed.
 */
template <typename T, usize N>
class RingBuffer {
    obj2voxel::RingBuffer<T, N> buffer{};

    mutable std::mutex mutex{};
    std::condition_variable readCon{}, writeCon{};

public:
    /// Pops one element from the ring buffer.
    /// If the buffer is empty, the thread will be blocked until an element is available.
    T pop()
    {
        std::unique_lock<std::mutex> lock{mutex};
        readCon.wait(lock, [this] {
            return not buffer.empty();
        });
        T result = buffer.pop();
        writeCon.notify_one();
        return result;
    }

    /// Pushes one element to the ring buffer.
    /// If the buffer is full, the thread will be blocked until space is available.
    void push(T value)
    {
        std::unique_lock<std::mutex> lock{mutex};
        writeCon.wait(lock, [this] {
            return not buffer.full();
        });
        buffer.push(std::move(value));
        readCon.notify_one();
    }

    /// Tries to pop one element from the ring buffer.
    /// If the buffer is empty, false will be returned without waiting for a pushed element.
    /// Otherwise, the result will written to out and true is returned.
    bool tryPop(T &out)
    {
        std::lock_guard<std::mutex> lock{mutex};
        if (buffer.empty()) {
            return false;
        }
        out = buffer.pop();
        writeCon.notify_one();
        return true;
    }

#if 0
    /// Blocks the thread until the buffer has been emptied by other treads.
    void waitUntilEmpty()
    {
        std::unique_lock<std::mutex> lock{mutex};
        writeCon.wait(lock, [this] { return buffer.empty(); });
    }
#endif

    /// Clears the ring buffer.
    void clear()
    {
        std::lock_guard<std::mutex> lock{mutex};
        buffer.clear();
        writeCon.notify_all();
    }

    /// Returns the least recently pushed element without popping it.
    /// If the buffer is empty, the thread will be blocked until an element is available.
    const T &peek() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        readCon.wait(lock, [this] {
            return not buffer.empty();
        });
        return buffer.peek();
    }

    /// Returns the current size thread-safely.
    usize size() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return buffer.size();
    }

    /// Returns true if the buffer is empty thread-safely.
    bool empty() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return buffer.empty();
    }

    /// Returns true if the buffer is full thread-safely.
    bool full() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return buffer.full();
    }
};

/// A simple extension of conidition variables that allows waiting until a certain event is triggered.
/// A thread that should wait for the event must call wait().
/// Any other thread can then trigger the event with trigger(), which unblocks all waiting threads.
/// Unlike with condition variables, triggering is permanent, so an event that has once been triggered must be reset().
class Event {
    bool flag = false;
    mutable std::mutex mutex;
    mutable std::condition_variable condition;

public:
    explicit Event(bool triggered = false) : flag{triggered} {}

    bool wait()
    {
        std::unique_lock<std::mutex> lock{mutex};
        if (flag) {
            return false;
        }
        condition.wait(lock);
        return true;
    }

    void trigger()
    {
        std::lock_guard<std::mutex> lock{mutex};
        flag = true;
        condition.notify_all();
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock{mutex};
        flag = false;
    }
};

/// An atomic counter which allows waiting until a certain value has been reached.
template <typename T = umax>
class Counter {
public:
    using type = T;

private:
    T count;
    mutable std::mutex mutex;
    mutable std::condition_variable condition;

public:
    Counter(T count = {}) : count{count} {}

    /// Increments the counter atomically.
    Counter &operator++()
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++count;
        condition.notify_all();
        return *this;
    }

    /// Decrements the counter atomically.
    Counter &operator--()
    {
        std::lock_guard<std::mutex> lock{mutex};
        --count;
        condition.notify_all();
        return *this;
    }

    template <typename Predicate, std::enable_if_t<std::is_invocable_r_v<bool, Predicate, type>, int> = 0>
    void wait(Predicate predicate) const
    {
        std::unique_lock<std::mutex> lock{mutex};
        if (predicate(count)) {
            return;
        }
        condition.wait(lock, [this, &predicate]() -> bool {
            return predicate(count);
        });
    }

    void waitUntilZero() const
    {
        wait([this](type t) {
            return t == 0;
        });
    }

    const type &operator*() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return count;
    }
};

}  // namespace async

}  // namespace obj2voxel

#endif  // OBJ2VOXEL_THREADING_HPP
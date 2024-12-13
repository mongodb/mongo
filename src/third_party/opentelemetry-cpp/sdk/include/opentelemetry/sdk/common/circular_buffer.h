// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/atomic_unique_ptr.h"
#include "opentelemetry/sdk/common/circular_buffer_range.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
/*
 * A lock-free circular buffer that supports multiple concurrent producers
 * and a single consumer.
 */
template <class T>
class CircularBuffer
{
public:
  explicit CircularBuffer(size_t max_size)
      : data_{new AtomicUniquePtr<T>[max_size + 1]}, capacity_{max_size + 1}
  {}

  /**
   * @return a range of the elements in the circular buffer
   *
   * Note: This method must only be called from the consumer thread.
   */
  CircularBufferRange<const AtomicUniquePtr<T>> Peek() const noexcept
  {
    return const_cast<CircularBuffer *>(this)->PeekImpl();
  }

  /**
   * Consume elements from the circular buffer's tail.
   * @param n the number of elements to consume
   * @param callback the callback to invoke with an AtomicUniquePtr to each
   * consumed element.
   *
   * Note: The callback must set the passed AtomicUniquePtr to null.
   *
   * Note: This method must only be called from the consumer thread.
   */
  template <class Callback>
  void Consume(size_t n, Callback callback) noexcept
  {
    assert(n <= static_cast<size_t>(head_ - tail_));
    auto range = PeekImpl().Take(n);
    static_assert(noexcept(callback(range)), "callback not allowed to throw");
    tail_ += n;
    callback(range);
  }

  /**
   * Consume elements from the circular buffer's tail.
   * @param n the number of elements to consume
   *
   * Note: This method must only be called from the consumer thread.
   */
  void Consume(size_t n) noexcept
  {
    Consume(n, [](CircularBufferRange<AtomicUniquePtr<T>> &range) noexcept {
      range.ForEach([](AtomicUniquePtr<T> &ptr) noexcept {
        ptr.Reset();
        return true;
      });
    });
  }

  /**
   * Adds an element into the circular buffer.
   * @param ptr a pointer to the element to add
   * @return true if the element was successfully added; false, otherwise.
   */
  bool Add(std::unique_ptr<T> &ptr) noexcept
  {
    while (true)
    {
      uint64_t tail = tail_;
      uint64_t head = head_;

      // The circular buffer is full, so return false.
      if (head - tail >= capacity_ - 1)
      {
        return false;
      }

      uint64_t head_index = head % capacity_;
      if (data_[head_index].SwapIfNull(ptr))
      {
        auto new_head      = head + 1;
        auto expected_head = head;
        if (head_.compare_exchange_weak(expected_head, new_head, std::memory_order_release,
                                        std::memory_order_relaxed))
        {
          // free the swapped out value
          ptr.reset();

          return true;
        }

        // If we reached this point (unlikely), it means that between the last
        // iteration elements were added and then consumed from the circular
        // buffer, so we undo the swap and attempt to add again.
        data_[head_index].Swap(ptr);
      }
    }
  }

  bool Add(std::unique_ptr<T> &&ptr) noexcept
  {
    // rvalue to lvalue reference
    bool result = Add(std::ref(ptr));
    ptr.reset();
    return result;
  }

  /**
   * Clear the circular buffer.
   *
   * Note: This method must only be called from the consumer thread.
   */
  void Clear() noexcept { Consume(size()); }

  /**
   * @return the maximum number of bytes that can be stored in the buffer.
   */
  size_t max_size() const noexcept { return capacity_ - 1; }

  /**
   * @return true if the buffer is empty.
   */
  bool empty() const noexcept { return head_ == tail_; }

  /**
   * @return the number of bytes stored in the circular buffer.
   *
   * Note: this method will only return a correct snapshot of the size if called
   * from the consumer thread.
   */
  size_t size() const noexcept
  {
    uint64_t tail = tail_;
    uint64_t head = head_;
    assert(tail <= head);
    return head - tail;
  }

  /**
   * @return the number of elements consumed from the circular buffer.
   */
  uint64_t consumption_count() const noexcept { return tail_; }

  /**
   * @return the number of elements added to the circular buffer.
   */
  uint64_t production_count() const noexcept { return head_; }

private:
  std::unique_ptr<AtomicUniquePtr<T>[]> data_;
  size_t capacity_;
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> tail_{0};

  CircularBufferRange<AtomicUniquePtr<T>> PeekImpl() noexcept
  {
    uint64_t tail_index = tail_ % capacity_;
    uint64_t head_index = head_ % capacity_;
    if (head_index == tail_index)
    {
      return {};
    }
    auto data = data_.get();
    if (tail_index < head_index)
    {
      return CircularBufferRange<AtomicUniquePtr<T>>{nostd::span<AtomicUniquePtr<T>>{
          data + tail_index, static_cast<std::size_t>(head_index - tail_index)}};
    }
    return {nostd::span<AtomicUniquePtr<T>>{data + tail_index,
                                            static_cast<std::size_t>(capacity_ - tail_index)},
            nostd::span<AtomicUniquePtr<T>>{data, static_cast<std::size_t>(head_index)}};
  }
};
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

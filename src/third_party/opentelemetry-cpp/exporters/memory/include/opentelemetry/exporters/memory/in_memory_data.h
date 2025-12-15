// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/sdk/common/circular_buffer.h"

#include <vector>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace memory
{
/**
 * A wrapper class holding in memory exporter data
 */
template <typename T>
class InMemoryData
{
public:
  /**
   * @param buffer_size a required value that sets the size of the CircularBuffer
   */
  InMemoryData(size_t buffer_size) : data_(buffer_size) {}

  /**
   * @param data a required unique pointer to the data to add to the CircularBuffer
   */
  void Add(std::unique_ptr<T> data) noexcept { data_.Add(data); }

  /**
   * @return Returns a vector of unique pointers containing all the data in the
   * CircularBuffer. This operation will empty the Buffer, which is why the data
   * is returned as unique pointers
   */
  std::vector<std::unique_ptr<T>> Get() noexcept
  {
    std::vector<std::unique_ptr<T>> res;

    // Pointer swap is required because the Consume function requires that the
    // AtomicUniquePointer be set to null
    data_.Consume(
        data_.size(), [&](opentelemetry::sdk::common::CircularBufferRange<
                          opentelemetry::sdk::common::AtomicUniquePtr<T>> range) noexcept {
          range.ForEach([&](opentelemetry::sdk::common::AtomicUniquePtr<T> &ptr) noexcept {
            std::unique_ptr<T> swap_ptr = nullptr;
            ptr.Swap(swap_ptr);
            res.push_back(std::move(swap_ptr));
            return true;
          });
        });

    return res;
  }

private:
  opentelemetry::sdk::common::CircularBuffer<T> data_;
};
}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE

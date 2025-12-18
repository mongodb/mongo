// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <limits>
#include <vector>

#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

/**
 * An integer array that automatically expands its memory consumption (via copy/allocation) when
 * reaching limits. This assumes counts remain low, to lower memory overhead.
 *
 * This class is NOT thread-safe. It is expected to be behind a synchronized incrementer.
 *
 * Instances start by attempting to store one-byte per-cell in the integer array. As values grow,
 * this will automatically instantiate the next-size integer array (uint8_t -> uint16_t -> uint32_t
 * -> uint64_t) and copy over values into the larger array. This class expects most usage to remain
 * within the uint8_t boundary (e.g. cell values < 256).
 */
class AdaptingIntegerArray
{
public:
  // Construct an adapting integer array of a given size.
  explicit AdaptingIntegerArray(size_t size) : backing_(std::vector<uint8_t>(size, 0)) {}
  AdaptingIntegerArray(const AdaptingIntegerArray &other)            = default;
  AdaptingIntegerArray(AdaptingIntegerArray &&other)                 = default;
  AdaptingIntegerArray &operator=(const AdaptingIntegerArray &other) = default;
  AdaptingIntegerArray &operator=(AdaptingIntegerArray &&other)      = default;

  /**
   * Increments the value at the specified index by the given count in the array.
   *
   * @param index The index of the value to increment.
   * @param count The count by which to increment the value.
   */
  void Increment(size_t index, uint64_t count);

  /**
   * Returns the value at the specified index from the array.
   *
   * @param index The index of the value to retrieve.
   * @return The value at the specified index.
   */
  uint64_t Get(size_t index) const;

  /**
   * Returns the size of the array.
   *
   * @return The size of the array.
   */
  size_t Size() const;

  /**
   * Clears the array, resetting all values to zero.
   */
  void Clear();

private:
  void EnlargeToFit(uint64_t value);

  nostd::variant<std::vector<uint8_t>,
                 std::vector<uint16_t>,
                 std::vector<uint32_t>,
                 std::vector<uint64_t>>
      backing_;
};

/**
 * A circle-buffer-backed exponential counter.
 *
 * The first recorded value becomes the 'base_index'. Going backwards leads to start/stop index.
 *
 * This expand start/end index as it sees values.
 *
 * This class is NOT thread-safe. It is expected to be behind a synchronized incrementer.
 */
class AdaptingCircularBufferCounter
{
public:
  explicit AdaptingCircularBufferCounter(size_t max_size) : backing_(max_size) {}
  AdaptingCircularBufferCounter(const AdaptingCircularBufferCounter &other)            = default;
  AdaptingCircularBufferCounter(AdaptingCircularBufferCounter &&other)                 = default;
  AdaptingCircularBufferCounter &operator=(const AdaptingCircularBufferCounter &other) = default;
  AdaptingCircularBufferCounter &operator=(AdaptingCircularBufferCounter &&other)      = default;

  /**
   * The first index with a recording. May be negative.
   *
   * Note: the returned value is not meaningful when Empty returns true.
   *
   * @return the first index with a recording.
   */
  int32_t StartIndex() const { return start_index_; }

  /**
   * The last index with a recording. May be negative.
   *
   * Note: the returned value is not meaningful when Empty returns true.
   *
   * @return The last index with a recording.
   */
  int32_t EndIndex() const { return end_index_; }

  /**
   * Returns true if no recordings, false if at least one recording.
   */
  bool Empty() const { return base_index_ == kNullIndex; }

  /**
   * Returns the maximum number of buckets allowed in this counter.
   */
  size_t MaxSize() const { return backing_.Size(); }

  /** Resets all bucket counts to zero and resets index start/end tracking. **/
  void Clear();

  /**
   * Persist new data at index, incrementing by delta amount.
   *
   * @param index The index of where to perform the incrementation.
   * @param delta How much to increment the index by.
   * @return success status.
   */
  bool Increment(int32_t index, uint64_t delta);

  /**
   * Get the number of recordings for the given index.
   *
   * @return the number of recordings for the index, or 0 if the index is out of bounds.
   */
  uint64_t Get(int32_t index) const;

private:
  size_t ToBufferIndex(int32_t index) const;

  static constexpr int32_t kNullIndex = (std::numeric_limits<int32_t>::min)();

  // Index of the first populated element, may be kNullIndex if container is empty.
  int32_t start_index_ = kNullIndex;
  // Index of the last populated element, may be kNullIndex if container is empty.
  int32_t end_index_ = kNullIndex;
  // Index corresponding to the element located at the start of the backing array, may be kNullIndex
  // if container is empty.
  int32_t base_index_ = kNullIndex;
  AdaptingIntegerArray backing_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

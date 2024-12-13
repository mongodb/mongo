// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <cassert>
#include <type_traits>

#include "opentelemetry/nostd/span.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
/**
 * A non-owning view into a range of elements in a circular buffer.
 */
template <class T>
class CircularBufferRange
{
public:
  CircularBufferRange() noexcept = default;

  explicit CircularBufferRange(nostd::span<T> first) noexcept : first_{first} {}

  CircularBufferRange(nostd::span<T> first, nostd::span<T> second) noexcept
      : first_{first}, second_{second}
  {}

  operator CircularBufferRange<const T>() const noexcept { return {first_, second_}; }

  /**
   * Iterate over the elements in the range.
   * @param callback the callback to call for each element
   * @return true if we iterated over all elements
   */
  template <class Callback>
  bool ForEach(Callback callback) const
      noexcept(noexcept(std::declval<Callback>()(std::declval<T &>())))
  {
    for (auto &value : first_)
    {
      if (!callback(value))
      {
        return false;
      }
    }
    for (auto &value : second_)
    {
      if (!callback(value))
      {
        return false;
      }
    }
    return true;
  }

  /**
   * @return the number of elements in the range
   */
  size_t size() const noexcept { return first_.size() + second_.size(); }

  /**
   * @return true if the range is empty
   */
  bool empty() const noexcept { return first_.empty(); }

  /**
   * Return a subrange taken from the start of this range.
   * @param n the number of element to take in the subrange
   * @return a subrange of the first n elements in this range
   */
  CircularBufferRange Take(size_t n) const noexcept
  {
    assert(n <= size());
    if (first_.size() >= n)
    {
      return CircularBufferRange{nostd::span<T>{first_.data(), n}};
    }
    return {first_, nostd::span<T>{second_.data(), n - first_.size()}};
  }

private:
  nostd::span<T> first_;
  nostd::span<T> second_;
};
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

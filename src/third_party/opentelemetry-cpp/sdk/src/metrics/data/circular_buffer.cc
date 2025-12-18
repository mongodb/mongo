// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/data/circular_buffer.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

namespace
{

struct AdaptingIntegerArrayIncrement
{
  size_t index;
  uint64_t count;

  template <typename T>
  uint64_t operator()(std::vector<T> &backing)
  {
    const uint64_t result = backing[index] + count;
    if OPENTELEMETRY_LIKELY_CONDITION (result <= uint64_t(std::numeric_limits<T>::max()))
    {
      backing[index] = static_cast<T>(result);
      return 0;
    }
    return result;
  }
};

struct AdaptingIntegerArrayGet
{
  size_t index;

  template <typename T>
  uint64_t operator()(const std::vector<T> &backing)
  {
    return backing[index];
  }
};

struct AdaptingIntegerArraySize
{
  template <typename T>
  size_t operator()(const std::vector<T> &backing)
  {
    return backing.size();
  }
};

struct AdaptingIntegerArrayClear
{
  template <typename T>
  void operator()(std::vector<T> &backing)
  {
    backing.assign(backing.size(), static_cast<T>(0));
  }
};

struct AdaptingIntegerArrayCopy
{
  template <class T1, class T2>
  void operator()(const std::vector<T1> &from, std::vector<T2> &to)
  {
    for (size_t i = 0; i < from.size(); i++)
    {
      to[i] = static_cast<T2>(from[i]);
    }
  }
};

}  // namespace

void AdaptingIntegerArray::Increment(size_t index, uint64_t count)
{
  const uint64_t result = nostd::visit(AdaptingIntegerArrayIncrement{index, count}, backing_);
  if OPENTELEMETRY_LIKELY_CONDITION (result == 0)
  {
    return;
  }
  EnlargeToFit(result);
  Increment(index, count);
}

uint64_t AdaptingIntegerArray::Get(size_t index) const
{
  return nostd::visit(AdaptingIntegerArrayGet{index}, backing_);
}

size_t AdaptingIntegerArray::Size() const
{
  return nostd::visit(AdaptingIntegerArraySize{}, backing_);
}

void AdaptingIntegerArray::Clear()
{
  nostd::visit(AdaptingIntegerArrayClear{}, backing_);
}

void AdaptingIntegerArray::EnlargeToFit(uint64_t value)
{
  const size_t backing_size = Size();
  decltype(backing_) backing;
  if (value <= std::numeric_limits<uint16_t>::max())
  {
    backing = std::vector<uint16_t>(backing_size, 0);
  }
  else if (value <= std::numeric_limits<uint32_t>::max())
  {
    backing = std::vector<uint32_t>(backing_size, 0);
  }
  else
  {
    backing = std::vector<uint64_t>(backing_size, 0);
  }
  std::swap(backing_, backing);
  nostd::visit(AdaptingIntegerArrayCopy{}, backing, backing_);
}

void AdaptingCircularBufferCounter::Clear()
{
  start_index_ = kNullIndex;
  end_index_   = kNullIndex;
  base_index_  = kNullIndex;
  backing_.Clear();
}

bool AdaptingCircularBufferCounter::Increment(int32_t index, uint64_t delta)
{
  if (Empty())
  {
    start_index_ = index;
    end_index_   = index;
    base_index_  = index;
    backing_.Increment(0, delta);
    return true;
  }

  if (index > end_index_)
  {
    // Move end, check max size.
    if (index + 1 > static_cast<int32_t>(backing_.Size()) + start_index_)
    {
      return false;
    }
    end_index_ = index;
  }
  else if (index < start_index_)
  {
    // Move end, check max size.
    if (end_index_ + 1 > static_cast<int32_t>(backing_.Size()) + index)
    {
      return false;
    }
    start_index_ = index;
  }
  backing_.Increment(ToBufferIndex(index), delta);
  return true;
}

uint64_t AdaptingCircularBufferCounter::Get(int32_t index) const
{
  if (index < start_index_ || index > end_index_)
  {
    return 0;
  }
  return backing_.Get(ToBufferIndex(index));
}

size_t AdaptingCircularBufferCounter::ToBufferIndex(int32_t index) const
{
  // Figure out the index relative to the start of the circular buffer.
  if (index < base_index_)
  {
    // If index is before the base one, wrap around.
    return static_cast<size_t>(index + backing_.Size() - base_index_);
  }
  return static_cast<size_t>(index - base_index_);
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

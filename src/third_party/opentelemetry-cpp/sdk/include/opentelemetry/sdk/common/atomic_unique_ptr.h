// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <memory>

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{
/**
 * An owning pointer similar to std::unique_ptr but with methods for atomic
 * operations.
 */
template <class T>
class AtomicUniquePtr
{
public:
  AtomicUniquePtr() noexcept {}

  explicit AtomicUniquePtr(std::unique_ptr<T> &&other) noexcept : ptr_(other.release()) {}

  ~AtomicUniquePtr() noexcept { Reset(); }

  T &operator*() const noexcept { return *Get(); }

  T *operator->() const noexcept { return Get(); }

  /**
   * @return the underly pointer managed.
   */
  T *Get() const noexcept { return ptr_; }

  /**
   * @return true if the pointer is null
   */
  bool IsNull() const noexcept { return ptr_.load() == nullptr; }

  /**
   * Atomically swap the pointer only if it's null.
   * @param owner the pointer to swap with
   * @return true if the swap was successful
   */
  bool SwapIfNull(std::unique_ptr<T> &owner) noexcept
  {
    auto ptr            = owner.get();
    T *expected         = nullptr;
    auto was_successful = ptr_.compare_exchange_weak(expected, ptr, std::memory_order_release,
                                                     std::memory_order_relaxed);
    if (was_successful)
    {
      owner.release();
      return true;
    }
    return false;
  }

  /**
   * Atomically swap the pointer with another.
   * @param ptr the pointer to swap with
   */
  void Swap(std::unique_ptr<T> &other) noexcept { other.reset(ptr_.exchange(other.release())); }

  /**
   * Set the pointer to a new value and delete the current value if non-null.
   * @param ptr the new pointer value to set
   */
  void Reset(T *ptr = nullptr) noexcept
  {
    ptr = ptr_.exchange(ptr);
    if (ptr != nullptr)
    {
      delete ptr;
    }
  }

private:
  std::atomic<T *> ptr_{nullptr};
};
}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

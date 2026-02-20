/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_WasiAtomic_h
#define mozilla_WasiAtomic_h

#include <cstddef>  // For _LIBCPP_VERSION and ptrdiff_t

// Clang >= 14 supports <atomic> for wasm targets.
#if _LIBCPP_VERSION >= 14000
#  include <atomic>
#else

#  include <cstdint>

// WASI doesn't support <atomic> and we use it as single-threaded for now.
// This is a stub implementation of std atomics to build WASI port of SM.

namespace std {
enum memory_order {
  relaxed,
  consume,  // load-consume
  acquire,  // load-acquire
  release,  // store-release
  acq_rel,  // store-release load-acquire
  seq_cst   // store-release load-acquire
};

inline constexpr auto memory_order_relaxed = memory_order::relaxed;
inline constexpr auto memory_order_consume = memory_order::consume;
inline constexpr auto memory_order_acquire = memory_order::acquire;
inline constexpr auto memory_order_release = memory_order::release;
inline constexpr auto memory_order_acq_rel = memory_order::acq_rel;
inline constexpr auto memory_order_seq_cst = memory_order::seq_cst;

template <class T>
struct atomic {
  using value_type = T;
  value_type value_;

  atomic() noexcept = default;
  constexpr atomic(T desired) noexcept : value_{desired} {}

  atomic(const atomic&) = delete;
  atomic& operator=(const atomic&) = delete;
  atomic& operator=(const atomic&) volatile = delete;
  ~atomic() noexcept = default;

  T load(memory_order m = memory_order_seq_cst) const volatile noexcept {
    return value_;
  }

  void store(T desired,
             memory_order m = memory_order_seq_cst) volatile noexcept {
    value_ = desired;
  }

  T operator=(T desired) volatile noexcept { return value_ = desired; }

  T exchange(T desired,
             memory_order m = memory_order_seq_cst) volatile noexcept {
    T tmp = value_;
    value_ = desired;
    return tmp;
  }

  bool compare_exchange_weak(T& expected, T desired, memory_order,
                             memory_order) volatile noexcept {
    expected = desired;
    return true;
  }

  bool compare_exchange_weak(
      T& expected, T desired,
      memory_order m = memory_order_seq_cst) volatile noexcept {
    expected = desired;
    return true;
  }

  bool compare_exchange_strong(T& expected, T desired, memory_order,
                               memory_order) volatile noexcept {
    expected = desired;
    return true;
  }

  bool compare_exchange_strong(
      T& expected, T desired,
      memory_order m = memory_order_seq_cst) volatile noexcept {
    expected = desired;
    return true;
  }

  T fetch_add(T arg, memory_order m = memory_order_seq_cst) volatile noexcept {
    T previous = value_;
    value_ = value_ + arg;
    return previous;
  }

  T fetch_sub(T arg, memory_order m = memory_order_seq_cst) volatile noexcept {
    T previous = value_;
    value_ = value_ - arg;
    return previous;
  }

  T fetch_or(T arg, memory_order m = memory_order_seq_cst) volatile noexcept {
    T previous = value_;
    value_ = value_ | arg;
    return previous;
  }

  T fetch_xor(T arg, memory_order m = memory_order_seq_cst) volatile noexcept {
    T previous = value_;
    value_ = value_ ^ arg;
    return previous;
  }

  T fetch_and(T arg, memory_order m = memory_order_seq_cst) volatile noexcept {
    T previous = value_;
    value_ = value_ & arg;
    return previous;
  }
};

template <class T>
struct atomic<T*> {
  using value_type = T*;
  using difference_type = ptrdiff_t;

  value_type value_;

  atomic() noexcept = default;
  constexpr atomic(T* desired) noexcept : value_{desired} {}
  atomic(const atomic&) = delete;
  atomic& operator=(const atomic&) = delete;
  atomic& operator=(const atomic&) volatile = delete;

  T* load(memory_order m = memory_order_seq_cst) const volatile noexcept {
    return value_;
  }

  void store(T* desired,
             memory_order m = memory_order_seq_cst) volatile noexcept {
    value_ = desired;
  }

  T* operator=(T* other) volatile noexcept { return value_ = other; }

  T* exchange(T* desired,
              memory_order m = memory_order_seq_cst) volatile noexcept {
    T* previous = value_;
    value_ = desired;
    return previous;
  }

  bool compare_exchange_weak(T*& expected, T* desired, memory_order s,
                             memory_order f) volatile noexcept {
    expected = desired;
    return true;
  }

  bool compare_exchange_weak(
      T*& expected, T* desired,
      memory_order m = memory_order_seq_cst) volatile noexcept {
    expected = desired;
    return true;
  }

  bool compare_exchange_strong(T*& expected, T* desired, memory_order s,
                               memory_order f) volatile noexcept {
    expected = desired;
    return true;
  }

  T* fetch_add(ptrdiff_t arg,
               memory_order m = memory_order_seq_cst) volatile noexcept {
    T* previous = value_;
    value_ = value_ + arg;
    return previous;
  }

  T* fetch_sub(ptrdiff_t arg,
               memory_order m = memory_order_seq_cst) volatile noexcept {
    T* previous = value_;
    value_ = value_ - arg;
    return previous;
  }
};

using atomic_uint8_t = atomic<uint8_t>;
using atomic_uint16_t = atomic<uint16_t>;
using atomic_uint32_t = atomic<uint32_t>;
using atomic_uint64_t = atomic<uint64_t>;

}  // namespace std

#endif

#endif  // mozilla_WasiAtomic_h

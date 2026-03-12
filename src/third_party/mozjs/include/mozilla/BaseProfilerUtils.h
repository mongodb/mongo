/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BaseProfilerUtils_h
#define BaseProfilerUtils_h

// This header contains most process- and thread-related functions.
// It is safe to include unconditionally.

// --------------------------------------------- WASI process & thread ids
#if defined(__wasi__)

namespace mozilla::baseprofiler::detail {
using ProcessIdType = unsigned;
using ThreadIdType = unsigned;
}  // namespace mozilla::baseprofiler::detail

// --------------------------------------------- Windows process & thread ids
#elif defined(XP_WIN)

namespace mozilla::baseprofiler::detail {
using ProcessIdType = int;
using ThreadIdType = unsigned long;
}  // namespace mozilla::baseprofiler::detail

// --------------------------------------------- Non-Windows process id
#else
// All non-Windows platforms are assumed to be POSIX, which has getpid().

#  include <unistd.h>
namespace mozilla::baseprofiler::detail {
using ProcessIdType = decltype(getpid());
}  // namespace mozilla::baseprofiler::detail

// --------------------------------------------- Non-Windows thread id
// ------------------------------------------------------- macOS
#  if defined(XP_MACOSX)

namespace mozilla::baseprofiler::detail {
using ThreadIdType = uint64_t;
}  // namespace mozilla::baseprofiler::detail

// ------------------------------------------------------- Android
// Test Android before Linux, because Linux includes Android.
#  elif defined(__ANDROID__) || defined(ANDROID)

#    include <sys/types.h>
namespace mozilla::baseprofiler::detail {
using ThreadIdType = decltype(gettid());
}  // namespace mozilla::baseprofiler::detail

// ------------------------------------------------------- Linux
#  elif defined(XP_LINUX)

namespace mozilla::baseprofiler::detail {
using ThreadIdType = long;
}  // namespace mozilla::baseprofiler::detail

// ------------------------------------------------------- FreeBSD
#  elif defined(XP_FREEBSD)

namespace mozilla::baseprofiler::detail {
using ThreadIdType = long;
}  // namespace mozilla::baseprofiler::detail

// ------------------------------------------------------- Others
#  else

#    include <thread>

namespace mozilla::baseprofiler::detail {
using ThreadIdType = std::thread::id;
}  // namespace mozilla::baseprofiler::detail

#  endif
#endif  // End of non-XP_WIN.

#include <stdint.h>
#include <string.h>
#include <type_traits>

namespace mozilla::baseprofiler {

// Trivially-copyable class containing a process id. It may be left unspecified.
class BaseProfilerProcessId {
 public:
  using NativeType = detail::ProcessIdType;

  using NumberType =
      std::conditional_t<(sizeof(NativeType) <= 4), uint32_t, uint64_t>;
  static_assert(sizeof(NativeType) <= sizeof(NumberType));

  // Unspecified process id.
  constexpr BaseProfilerProcessId() = default;

  [[nodiscard]] constexpr bool IsSpecified() const {
    return mProcessId != scUnspecified;
  }

  // Construct from a native type.
  [[nodiscard]] static BaseProfilerProcessId FromNativeId(
      const NativeType& aNativeProcessId) {
    BaseProfilerProcessId id;
    // Convert trivially-copyable native id by copying its bits.
    static_assert(std::is_trivially_copyable_v<NativeType>);
    memcpy(&id.mProcessId, &aNativeProcessId, sizeof(NativeType));
    return id;
  }

  // Get the process id as a number, which may be unspecified.
  // This should only be used for serialization or logging.
  [[nodiscard]] constexpr NumberType ToNumber() const { return mProcessId; }

  // BaseProfilerProcessId from given number (which may be unspecified).
  constexpr static BaseProfilerProcessId FromNumber(
      const NumberType& aProcessId) {
    BaseProfilerProcessId id;
    id.mProcessId = aProcessId;
    return id;
  }

  [[nodiscard]] constexpr bool operator==(
      const BaseProfilerProcessId& aOther) const {
    return mProcessId == aOther.mProcessId;
  }
  [[nodiscard]] constexpr bool operator!=(
      const BaseProfilerProcessId& aOther) const {
    return mProcessId != aOther.mProcessId;
  }

 private:
  static constexpr NumberType scUnspecified = 0;
  NumberType mProcessId = scUnspecified;
};

// Check traits. These should satisfy usage in std::atomic.
static_assert(std::is_trivially_copyable_v<BaseProfilerProcessId>);
static_assert(std::is_copy_constructible_v<BaseProfilerProcessId>);
static_assert(std::is_move_constructible_v<BaseProfilerProcessId>);
static_assert(std::is_copy_assignable_v<BaseProfilerProcessId>);
static_assert(std::is_move_assignable_v<BaseProfilerProcessId>);

// Trivially-copyable class containing a thread id. It may be left unspecified.
class BaseProfilerThreadId {
 public:
  using NativeType = detail::ThreadIdType;

  using NumberType =
      std::conditional_t<(sizeof(NativeType) <= 4), uint32_t, uint64_t>;
  static_assert(sizeof(NativeType) <= sizeof(NumberType));

  // Unspecified thread id.
  constexpr BaseProfilerThreadId() = default;

  [[nodiscard]] constexpr bool IsSpecified() const {
    return mThreadId != scUnspecified;
  }

  // Construct from a native type.
  [[nodiscard]] static BaseProfilerThreadId FromNativeId(
      const NativeType& aNativeThreadId) {
    BaseProfilerThreadId id;
    // Convert trivially-copyable native id by copying its bits.
    static_assert(std::is_trivially_copyable_v<NativeType>);
    memcpy(&id.mThreadId, &aNativeThreadId, sizeof(NativeType));
    return id;
  }

  // Get the thread id as a number, which may be unspecified.
  // This should only be used for serialization or logging.
  [[nodiscard]] constexpr NumberType ToNumber() const { return mThreadId; }

  // BaseProfilerThreadId from given number (which may be unspecified).
  constexpr static BaseProfilerThreadId FromNumber(
      const NumberType& aThreadId) {
    BaseProfilerThreadId id;
    id.mThreadId = aThreadId;
    return id;
  }

  [[nodiscard]] constexpr bool operator==(
      const BaseProfilerThreadId& aOther) const {
    return mThreadId == aOther.mThreadId;
  }
  [[nodiscard]] constexpr bool operator!=(
      const BaseProfilerThreadId& aOther) const {
    return mThreadId != aOther.mThreadId;
  }

 private:
  static constexpr NumberType scUnspecified = 0;
  NumberType mThreadId = scUnspecified;
};

// Check traits. These should satisfy usage in std::atomic.
static_assert(std::is_trivially_copyable_v<BaseProfilerThreadId>);
static_assert(std::is_copy_constructible_v<BaseProfilerThreadId>);
static_assert(std::is_move_constructible_v<BaseProfilerThreadId>);
static_assert(std::is_copy_assignable_v<BaseProfilerThreadId>);
static_assert(std::is_move_assignable_v<BaseProfilerThreadId>);

}  // namespace mozilla::baseprofiler

#include "mozilla/Types.h"

namespace mozilla::baseprofiler {

// Get the current process's ID.
[[nodiscard]] MFBT_API BaseProfilerProcessId profiler_current_process_id();

// Get the current thread's ID.
[[nodiscard]] MFBT_API BaseProfilerThreadId profiler_current_thread_id();

// Must be called at least once from the main thread, before any other main-
// thread id function.
MFBT_API void profiler_init_main_thread_id();

[[nodiscard]] MFBT_API BaseProfilerThreadId profiler_main_thread_id();

[[nodiscard]] MFBT_API bool profiler_is_main_thread();

}  // namespace mozilla::baseprofiler

#endif  // BaseProfilerUtils_h

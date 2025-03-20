/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_ProtectedData_h
#define threading_ProtectedData_h

#include "mozilla/Atomics.h"
#include "jstypes.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "threading/ThreadId.h"

struct JS_PUBLIC_API JSContext;

namespace js {

// This file provides classes for encapsulating pieces of data with a check
// that ensures the data is only accessed if certain conditions are met.
// Checking is only done in debug builds; in release builds these classes
// have no space or time overhead. These classes are mainly used for ensuring
// that data is used in threadsafe ways.
//
// ProtectedData does not by itself ensure that data is threadsafe: it only
// documents and checks synchronization constraints that need to be established
// by the code using the data. If a mutex can be created and directly
// associated with the data, consider using the ExclusiveData class instead.
// Otherwise, ProtectedData should be used to document whatever synchronization
// method is used.

// Protected data checks are enabled in debug builds, except on android where
// they cause some permatimeouts in automation.
#if defined(DEBUG) && !defined(ANDROID)
#  define JS_HAS_PROTECTED_DATA_CHECKS
#endif

#define DECLARE_ONE_BOOL_OPERATOR(OP, T)     \
  template <typename U>                      \
  bool operator OP(const U& other) const {   \
    if constexpr (std::is_integral_v<T>) {   \
      return ref() OP static_cast<T>(other); \
    } else {                                 \
      return ref() OP other;                 \
    }                                        \
  }

#define DECLARE_BOOL_OPERATORS(T)  \
  DECLARE_ONE_BOOL_OPERATOR(==, T) \
  DECLARE_ONE_BOOL_OPERATOR(!=, T) \
  DECLARE_ONE_BOOL_OPERATOR(<=, T) \
  DECLARE_ONE_BOOL_OPERATOR(>=, T) \
  DECLARE_ONE_BOOL_OPERATOR(<, T)  \
  DECLARE_ONE_BOOL_OPERATOR(>, T)

// Mark a region of code that should be treated as single threaded and suppress
// any ProtectedData checks.
//
// Note that in practice there may be multiple threads running when this class
// is used, due to the presence of multiple runtimes in the process. When each
// process has only a single runtime this will no longer be a concern.
class MOZ_RAII AutoNoteSingleThreadedRegion {
 public:
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
  static mozilla::Atomic<size_t, mozilla::SequentiallyConsistent> count;
  AutoNoteSingleThreadedRegion() { count++; }
  ~AutoNoteSingleThreadedRegion() { count--; }
#else
  AutoNoteSingleThreadedRegion() {}
#endif
};

// Class for protected data that may be written to any number of times. Checks
// occur when the data is both read from and written to.
template <typename Check, typename T>
class ProtectedData {
  typedef ProtectedData<Check, T> ThisType;

 public:
  template <typename... Args>
  explicit ProtectedData(const Check& check, Args&&... args)
      : value(std::forward<Args>(args)...)
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
        ,
        check(check)
#endif
  {
  }

  DECLARE_BOOL_OPERATORS(T)

  operator const T&() const { return ref(); }
  const T& operator->() const { return ref(); }

  template <typename U>
  ThisType& operator=(const U& p) {
    this->ref() = p;
    return *this;
  }

  template <typename U>
  ThisType& operator=(U&& p) {
    this->ref() = std::move(p);
    return *this;
  }

  template <typename U>
  T& operator+=(const U& rhs) {
    return ref() += rhs;
  }
  template <typename U>
  T& operator-=(const U& rhs) {
    return ref() -= rhs;
  }
  template <typename U>
  T& operator*=(const U& rhs) {
    return ref() *= rhs;
  }
  template <typename U>
  T& operator/=(const U& rhs) {
    return ref() /= rhs;
  }
  template <typename U>
  T& operator&=(const U& rhs) {
    return ref() &= rhs;
  }
  template <typename U>
  T& operator|=(const U& rhs) {
    return ref() |= rhs;
  }
  T& operator++() { return ++ref(); }
  T& operator--() { return --ref(); }
  T operator++(int) { return ref()++; }
  T operator--(int) { return ref()--; }

  T& ref() {
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
    if (!AutoNoteSingleThreadedRegion::count) {
      check.check();
    }
#endif
    return value;
  }

  const T& ref() const {
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
    if (!AutoNoteSingleThreadedRegion::count) {
      check.check();
    }
#endif
    return value;
  }

  T& refNoCheck() { return value; }
  const T& refNoCheck() const { return value; }

  static size_t offsetOfValue() { return offsetof(ThisType, value); }

 private:
  T value;
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
  Check check;
#endif
};

// Intermediate class for protected data whose checks take no constructor
// arguments.
template <typename Check, typename T>
class ProtectedDataNoCheckArgs : public ProtectedData<Check, T> {
  using Base = ProtectedData<Check, T>;

 public:
  template <typename... Args>
  explicit ProtectedDataNoCheckArgs(Args&&... args)
      : ProtectedData<Check, T>(Check(), std::forward<Args>(args)...) {}

  using Base::operator=;
};

// Intermediate class for protected data whose checks take a JSContext.
template <typename Check, typename T>
class ProtectedDataContextArg : public ProtectedData<Check, T> {
  using Base = ProtectedData<Check, T>;

 public:
  template <typename... Args>
  explicit ProtectedDataContextArg(JSContext* cx, Args&&... args)
      : ProtectedData<Check, T>(Check(cx), std::forward<Args>(args)...) {}

  using Base::operator=;
};

class CheckUnprotected {
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
 public:
  inline void check() const {}
#endif
};

// Data with a no-op check that permits all accesses. This is tantamount to not
// using ProtectedData at all, but is in place to document points which need
// to be fixed in order for runtimes to be multithreaded (see bug 1323066).
template <typename T>
using UnprotectedData = ProtectedDataNoCheckArgs<CheckUnprotected, T>;

class CheckThreadLocal {
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
  ThreadId id;

 public:
  CheckThreadLocal() : id(ThreadId::ThisThreadId()) {}

  void check() const;
#endif
};

class CheckContextLocal {
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
  JSContext* cx_;

 public:
  explicit CheckContextLocal(JSContext* cx) : cx_(cx) {}

  void check() const;
#else
 public:
  explicit CheckContextLocal(JSContext* cx) {}
#endif
};

// Data which may only be accessed by the thread on which it is created.
template <typename T>
using ThreadData = ProtectedDataNoCheckArgs<CheckThreadLocal, T>;

// Data which belongs to a JSContext and should only be accessed from that
// JSContext's thread. Note that a JSContext may not have a thread currently
// associated with it and any associated thread may change over time.
template <typename T>
using ContextData = ProtectedDataContextArg<CheckContextLocal, T>;

// Enum describing which helper threads (GC tasks or Ion compilations) may
// access data.
enum class AllowedHelperThread {
  None,
  GCTask,
  IonCompile,
  GCTaskOrIonCompile,
};

template <AllowedHelperThread Helper>
class CheckMainThread {
 public:
  void check() const;
};

// Data which may only be accessed by the runtime's main thread.
template <typename T>
using MainThreadData =
    ProtectedDataNoCheckArgs<CheckMainThread<AllowedHelperThread::None>, T>;

// Data which may only be accessed by the runtime's main thread or by various
// helper thread tasks.
template <typename T>
using MainThreadOrGCTaskData =
    ProtectedDataNoCheckArgs<CheckMainThread<AllowedHelperThread::GCTask>, T>;
template <typename T>
using MainThreadOrIonCompileData =
    ProtectedDataNoCheckArgs<CheckMainThread<AllowedHelperThread::IonCompile>,
                             T>;
template <typename T>
using MainThreadOrGCTaskOrIonCompileData = ProtectedDataNoCheckArgs<
    CheckMainThread<AllowedHelperThread::GCTaskOrIonCompile>, T>;

// Runtime wide locks which might protect some data.
enum class GlobalLock { GCLock, HelperThreadLock };

template <GlobalLock Lock, AllowedHelperThread Helper>
class CheckGlobalLock {
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
 public:
  void check() const;
#endif
};

// Data which may only be accessed while holding the GC lock.
template <typename T>
using GCLockData = ProtectedDataNoCheckArgs<
    CheckGlobalLock<GlobalLock::GCLock, AllowedHelperThread::None>, T>;

// Data which may only be accessed while holding the helper thread lock.
template <typename T>
using HelperThreadLockData = ProtectedDataNoCheckArgs<
    CheckGlobalLock<GlobalLock::HelperThreadLock, AllowedHelperThread::None>,
    T>;

// Class for protected data that is only written to once. 'const' may sometimes
// be usable instead of this class, but in cases where the data cannot be set
// to its final value in its constructor this class is helpful. Protected data
// checking only occurs when writes are performed, not reads. Steps may need to
// be taken to ensure that reads do not occur until the written value is fully
// initialized, as such guarantees are not provided by this class.
template <typename Check, typename T>
class ProtectedDataWriteOnce {
  typedef ProtectedDataWriteOnce<Check, T> ThisType;

 public:
  template <typename... Args>
  explicit ProtectedDataWriteOnce(Args&&... args)
      : value(std::forward<Args>(args)...)
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
        ,
        nwrites(0)
#endif
  {
  }

  DECLARE_BOOL_OPERATORS(T)

  operator const T&() const { return ref(); }
  const T& operator->() const { return ref(); }

  template <typename U>
  ThisType& operator=(const U& p) {
    if (ref() != p) {
      this->writeRef() = p;
    }
    return *this;
  }

  const T& ref() const { return value; }

  T& writeRef() {
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
    if (!AutoNoteSingleThreadedRegion::count) {
      check.check();
    }
    // Despite the WriteOnce name, actually allow two writes to accommodate
    // data that is cleared during teardown.
    MOZ_ASSERT(++nwrites <= 2);
#endif
    return value;
  }

 private:
  T value;
#ifdef JS_HAS_PROTECTED_DATA_CHECKS
  Check check;
  size_t nwrites;
#endif
};

// Data that is written once with no requirements for exclusive access when
// that write occurs.
template <typename T>
using WriteOnceData = ProtectedDataWriteOnce<CheckUnprotected, T>;

#undef DECLARE_ASSIGNMENT_OPERATOR
#undef DECLARE_ONE_BOOL_OPERATOR
#undef DECLARE_BOOL_OPERATORS

}  // namespace js

#endif  // threading_ProtectedData_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_ExclusiveData_h
#define threading_ExclusiveData_h

#include "mozilla/Maybe.h"
#include "mozilla/OperatorNewExtensions.h"

#include <utility>

#include "threading/ConditionVariable.h"
#include "threading/Mutex.h"

namespace js {

/**
 * [SMDOC] ExclusiveData API
 *
 * A mutual exclusion lock class.
 *
 * `ExclusiveData` provides an RAII guard to automatically lock and unlock when
 * accessing the protected inner value.
 *
 * Unlike the STL's `std::mutex`, the protected value is internal to this
 * class. This is a huge win: one no longer has to rely on documentation to
 * explain the relationship between a lock and its protected data, and the type
 * system can enforce[0] it.
 *
 * For example, suppose we have a counter class:
 *
 *     class Counter
 *     {
 *         int32_t i;
 *
 *       public:
 *         void inc(int32_t n) { i += n; }
 *     };
 *
 * If we share a counter across threads with `std::mutex`, we rely solely on
 * comments to document the relationship between the lock and its data, like
 * this:
 *
 *     class SharedCounter
 *     {
 *         // Remember to acquire `counter_lock` when accessing `counter`,
 *         // pretty please!
 *         Counter counter;
 *         std::mutex counter_lock;
 *
 *       public:
 *         void inc(size_t n) {
 *             // Whoops, forgot to acquire the lock! Off to the races!
 *             counter.inc(n);
 *         }
 *     };
 *
 * In contrast, `ExclusiveData` wraps the protected value, enabling the type
 * system to enforce that we acquire the lock before accessing the value:
 *
 *     class SharedCounter
 *     {
 *         ExclusiveData<Counter> counter;
 *
 *       public:
 *         void inc(size_t n) {
 *             auto guard = counter.lock();
 *             guard->inc(n);
 *         }
 *     };
 *
 * The API design is based on Rust's `std::sync::Mutex<T>` type.
 *
 * [0]: Of course, we don't have a borrow checker in C++, so the type system
 *      cannot guarantee that you don't stash references received from
 *      `ExclusiveData<T>::Guard` somewhere such that the reference outlives the
 *      guard's lifetime and therefore becomes invalid. To help avoid this last
 *      foot-gun, prefer using the guard directly! Do not store raw references
 *      to the protected value in other structures!
 */
template <typename T>
class ExclusiveData {
 protected:
  mutable Mutex lock_ MOZ_UNANNOTATED;
  mutable T value_;

  ExclusiveData(const ExclusiveData&) = delete;
  ExclusiveData& operator=(const ExclusiveData&) = delete;

  void acquire() const { lock_.lock(); }
  void release() const { lock_.unlock(); }

 public:
  /**
   * Create a new `ExclusiveData`, with perfect forwarding of the protected
   * value.
   */
  template <typename U>
  explicit ExclusiveData(const MutexId& id, U&& u)
      : lock_(id), value_(std::forward<U>(u)) {}

  /**
   * Create a new `ExclusiveData`, constructing the protected value in place.
   */
  template <typename... Args>
  explicit ExclusiveData(const MutexId& id, Args&&... args)
      : lock_(id), value_(std::forward<Args>(args)...) {}

  ExclusiveData& operator=(ExclusiveData&& rhs) {
    this->~ExclusiveData();
    new (mozilla::KnownNotNull, this) ExclusiveData(std::move(rhs));
    return *this;
  }

  /**
   * An RAII class that provides exclusive access to a `ExclusiveData<T>`'s
   * protected inner `T` value.
   *
   * Note that this is intentionally marked MOZ_STACK_CLASS instead of
   * MOZ_RAII_CLASS, as the latter disallows moves and returning by value, but
   * Guard utilizes both.
   */
  class MOZ_STACK_CLASS Guard {
   protected:
    const ExclusiveData* parent_;
    explicit Guard(std::nullptr_t) : parent_(nullptr) {}

   private:
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   public:
    explicit Guard(const ExclusiveData& parent) : parent_(&parent) {
      parent_->acquire();
    }

    Guard(Guard&& rhs) : parent_(rhs.parent_) {
      MOZ_ASSERT(&rhs != this, "self-move disallowed!");
      rhs.parent_ = nullptr;
    }

    Guard& operator=(Guard&& rhs) {
      this->~Guard();
      new (this) Guard(std::move(rhs));
      return *this;
    }

    T& get() const {
      MOZ_ASSERT(parent_);
      return parent_->value_;
    }

    operator T&() const { return get(); }
    T* operator->() const { return &get(); }

    const ExclusiveData<T>* parent() const {
      MOZ_ASSERT(parent_);
      return parent_;
    }

    ~Guard() {
      if (parent_) {
        parent_->release();
      }
    }
  };

  /**
   * NullableGuard are similar to Guard, except that one the access to the
   * ExclusiveData might not always be granted. This is useful when contextual
   * information is enough to prevent useless use of Mutex.
   *
   * The NullableGuard can be manipulated as follows:
   *
   *     if (NullableGuard guard = data.mightAccess()) {
   *         // NullableGuard is acquired.
   *         guard->...
   *     }
   *     // NullableGuard was either not acquired or released.
   *
   * Where mightAccess returns either a NullableGuard from `noAccess()` or a
   * Guard from `lock()`.
   */
  class MOZ_STACK_CLASS NullableGuard : public Guard {
   public:
    explicit NullableGuard(std::nullptr_t) : Guard((std::nullptr_t) nullptr) {}
    explicit NullableGuard(const ExclusiveData& parent) : Guard(parent) {}
    explicit NullableGuard(Guard&& rhs) : Guard(std::move(rhs)) {}

    NullableGuard& operator=(Guard&& rhs) {
      this->~NullableGuard();
      new (this) NullableGuard(std::move(rhs));
      return *this;
    }

    /**
     * Returns whether this NullableGuard has access to the exclusive data.
     */
    bool hasAccess() const { return this->parent_; }
    explicit operator bool() const { return hasAccess(); }
  };

  /**
   * Access the protected inner `T` value for exclusive reading and writing.
   */
  Guard lock() const { return Guard(*this); }

  /**
   * Provide a no-access guard, which coerces to false when tested. This value
   * can be returned if the guard access is conditioned on external factors.
   *
   * See NullableGuard.
   */
  NullableGuard noAccess() const {
    return NullableGuard((std::nullptr_t) nullptr);
  }
};

template <class T>
class ExclusiveWaitableData : public ExclusiveData<T> {
  using Base = ExclusiveData<T>;

  mutable ConditionVariable condVar_;

 public:
  template <typename U>
  explicit ExclusiveWaitableData(const MutexId& id, U&& u)
      : Base(id, std::forward<U>(u)) {}

  template <typename... Args>
  explicit ExclusiveWaitableData(const MutexId& id, Args&&... args)
      : Base(id, std::forward<Args>(args)...) {}

  class MOZ_STACK_CLASS Guard : public ExclusiveData<T>::Guard {
    using Base = typename ExclusiveData<T>::Guard;

   public:
    explicit Guard(const ExclusiveWaitableData& parent) : Base(parent) {}

    Guard(Guard&& guard) : Base(std::move(guard)) {}

    Guard& operator=(Guard&& rhs) { return Base::operator=(std::move(rhs)); }

    void wait() {
      auto* parent = static_cast<const ExclusiveWaitableData*>(this->parent());
      parent->condVar_.wait(parent->lock_);
    }

    void notify_one() {
      auto* parent = static_cast<const ExclusiveWaitableData*>(this->parent());
      parent->condVar_.notify_one();
    }

    void notify_all() {
      auto* parent = static_cast<const ExclusiveWaitableData*>(this->parent());
      parent->condVar_.notify_all();
    }
  };

  Guard lock() const { return Guard(*this); }
};

/**
 * Multiple-readers / single-writer variant of ExclusiveData.
 *
 * Readers call readLock() to obtain a stack-only RAII reader lock, which will
 * allow other readers to read concurrently but block writers; the yielded value
 * is const.  Writers call writeLock() to obtain a ditto writer lock, which
 * yields exclusive access to non-const data.
 *
 * See ExclusiveData and its implementation for more documentation.
 */
template <typename T>
class RWExclusiveData {
  mutable Mutex lock_ MOZ_UNANNOTATED;
  mutable ConditionVariable cond_;
  mutable T value_;
  mutable int readers_;

  // We maintain a count of active readers.  Writers may enter the critical
  // section only when the reader count is zero, so the reader that decrements
  // the count to zero must wake up any waiting writers.
  //
  // There can be multiple writers waiting, so a writer leaving the critical
  // section must also wake up any other waiting writers.

  void acquireReaderLock() const {
    lock_.lock();
    readers_++;
    lock_.unlock();
  }

  void releaseReaderLock() const {
    lock_.lock();
    MOZ_ASSERT(readers_ > 0);
    if (--readers_ == 0) {
      cond_.notify_all();
    }
    lock_.unlock();
  }

  void acquireWriterLock() const {
    lock_.lock();
    while (readers_ > 0) {
      cond_.wait(lock_);
    }
  }

  void releaseWriterLock() const {
    cond_.notify_all();
    lock_.unlock();
  }

 public:
  RWExclusiveData(const RWExclusiveData&) = delete;
  RWExclusiveData& operator=(const RWExclusiveData&) = delete;

  /**
   * Create a new `RWExclusiveData`, constructing the protected value in place.
   */
  template <typename... Args>
  explicit RWExclusiveData(const MutexId& id, Args&&... args)
      : lock_(id), value_(std::forward<Args>(args)...), readers_(0) {}

  class MOZ_STACK_CLASS ReadGuard {
    const RWExclusiveData* parent_;
    explicit ReadGuard(std::nullptr_t) : parent_(nullptr) {}

   public:
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;

    explicit ReadGuard(const RWExclusiveData& parent) : parent_(&parent) {
      parent_->acquireReaderLock();
    }

    ReadGuard(ReadGuard&& rhs) : parent_(rhs.parent_) {
      MOZ_ASSERT(&rhs != this, "self-move disallowed!");
      rhs.parent_ = nullptr;
    }

    ReadGuard& operator=(ReadGuard&& rhs) {
      this->~ReadGuard();
      new (this) ReadGuard(std::move(rhs));
      return *this;
    }

    const T& get() const {
      MOZ_ASSERT(parent_);
      return parent_->value_;
    }

    operator const T&() const { return get(); }
    const T* operator->() const { return &get(); }

    const RWExclusiveData<T>* parent() const {
      MOZ_ASSERT(parent_);
      return parent_;
    }

    ~ReadGuard() {
      if (parent_) {
        parent_->releaseReaderLock();
      }
    }
  };

  class MOZ_STACK_CLASS WriteGuard {
    const RWExclusiveData* parent_;
    explicit WriteGuard(std::nullptr_t) : parent_(nullptr) {}

   public:
    WriteGuard(const WriteGuard&) = delete;
    WriteGuard& operator=(const WriteGuard&) = delete;

    explicit WriteGuard(const RWExclusiveData& parent) : parent_(&parent) {
      parent_->acquireWriterLock();
    }

    WriteGuard(WriteGuard&& rhs) : parent_(rhs.parent_) {
      MOZ_ASSERT(&rhs != this, "self-move disallowed!");
      rhs.parent_ = nullptr;
    }

    WriteGuard& operator=(WriteGuard&& rhs) {
      this->~WriteGuard();
      new (this) WriteGuard(std::move(rhs));
      return *this;
    }

    T& get() const {
      MOZ_ASSERT(parent_);
      return parent_->value_;
    }

    operator T&() const { return get(); }
    T* operator->() const { return &get(); }

    const RWExclusiveData<T>* parent() const {
      MOZ_ASSERT(parent_);
      return parent_;
    }

    ~WriteGuard() {
      if (parent_) {
        parent_->releaseWriterLock();
      }
    }
  };

  ReadGuard readLock() const { return ReadGuard(*this); }
  WriteGuard writeLock() const { return WriteGuard(*this); }
};

}  // namespace js

#endif  // threading_ExclusiveData_h

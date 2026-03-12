/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Multiple Producer Single Consumer lock-free queue.
 * Allocation-free is guaranteed outside of the constructor.
 *
 * This is a direct C++ port from
 * https://docs.rs/signal-hook/0.3.17/src/signal_hook/low_level/channel.rs.html#1-235
 * with the exception we are using atomic uint64t to have 15 slots in the ring
 * buffer (Rust implem is 5 slots, we want a bit more).
 * */

#ifndef mozilla_MPSCQueue_h
#define mozilla_MPSCQueue_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/PodOperations.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <optional>
#include <inttypes.h>

namespace mozilla {

namespace detail {
template <typename T, bool IsPod = std::is_trivial<T>::value>
struct MemoryOperations {
  /**
   * This allows either moving (if T supports it) or copying a number of
   * elements from a `aSource` pointer to a `aDestination` pointer.
   * If it is safe to do so and this call copies, this uses PodCopy. Otherwise,
   * constructors and destructors are called in a loop.
   */
  static void MoveOrCopy(T* aDestination, T* aSource, size_t aCount);
};

template <typename T>
struct MemoryOperations<T, true> {
  static void MoveOrCopy(T* aDestination, T* aSource, size_t aCount) {
    PodCopy(aDestination, aSource, aCount);
  }
};

template <typename T>
struct MemoryOperations<T, false> {
  static void MoveOrCopy(T* aDestination, T* aSource, size_t aCount) {
    std::move(aSource, aSource + aCount, aDestination);
  }
};
}  // namespace detail

static const bool MPSC_DEBUG = false;

static const size_t kMaxCapacity = 16;

/**
 * This data structure allows producing data from several threads, and consuming
 * it on one thread, safely and without performing memory allocations or
 * locking.
 *
 * The role for the producers and the consumer must be constant, i.e., the
 * producer should always be on one thread and the consumer should always be on
 * another thread.
 *
 * Some words about the inner workings of this class:
 * - Capacity is fixed. Only one allocation is performed, in the constructor.
 * - Maximum capacity is 15 elements, with 0 being used to denote an empty set.
 *   This is a hard limitation from encoding indexes within the atomic uint64_t.
 * - This is lock-free but not wait-free, it might spin a little until
 *   compare/exchange succeeds.
 * - There is no guarantee of forward progression for individual threads.
 * - This should be safe to use from a signal handler context.
 */
template <typename T>
class MPSCRingBufferBase {
 public:
  explicit MPSCRingBufferBase(size_t aCapacity)
      : mFree(0), mOccupied(0), mCapacity(aCapacity + 1) {
    MOZ_RELEASE_ASSERT(aCapacity < kMaxCapacity);

    if constexpr (MPSC_DEBUG) {
      fprintf(stderr,
              "[%s] this=%p { mCapacity=%zu, mBits=%" PRIu64
              ", mMask=0x%" PRIx64 " }\n",
              __PRETTY_FUNCTION__, this, mCapacity, mBits, mMask);
    }

    // Leave one empty space in the queue, used to distinguish an empty queue
    // from a full one, as in the SPSCQueue.
    // https://docs.rs/signal-hook/0.3.17/src/signal_hook/low_level/channel.rs.html#126
    for (uint64_t i = 1; i < StorageCapacity(); ++i) {
      MarkSlot(mFree, i);
    }

    // This should be the only allocation performed, thus it cannot be performed
    // in a restricted context (e.g., signal handler, real-time thread)
    mData = std::make_unique<T[]>(Capacity());

    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  /**
   * @brief Put an element in the queue. The caller MUST check the return value
   * and maybe loop to try again (or drop if acceptable).
   *
   * First it attempts to acuire a slot (storage index) that is known to be
   * non used. If that is not successfull then 0 is returned. If that is
   * successfull, the slot is ours (it has been exclusively acquired) and data
   * can be copied into the ring buffer at that index.
   *
   * @param aElement The element to put in the queue.
   *
   * @return 0 if insertion could not be performed, inserted index otherwise
   */
  [[nodiscard]] int Send(T& aElement) {
    std::optional<uint64_t> empty_idx = UnmarkSlot(mFree);
    if (empty_idx.has_value()) {
      detail::MemoryOperations<T>::MoveOrCopy(&mData[*empty_idx - 1], &aElement,
                                              1);
      MarkSlot(mOccupied, *empty_idx);
      return *empty_idx;
    }
    return 0;
  }

  /**
   * Retrieve one element from the ring buffer, and copy it to
   * `aElement`, if non-null.
   *
   * It attempts to acquire a slot from the list of used ones. If that is not
   * successfull, then 0 is returned. Once a slot has been exclusively acquired,
   * data is copied from it into the non-null pointer passed in parameter.
   *
   * @param aElement A pointer to a `T` where data will be copied.
   *
   * @return The index from which data was copied, 0 if there was nothing in the
   * ring buffer.
   */
  [[nodiscard]] int Recv(T* aElement) {
    std::optional<uint64_t> idx = UnmarkSlot(mOccupied);
    if (idx.has_value()) {
      if (aElement) {
        detail::MemoryOperations<T>::MoveOrCopy(aElement, &mData[*idx - 1], 1);
      }
      MarkSlot(mFree, *idx);
      return *idx;
    }
    return 0;
  }

  size_t Capacity() const { return StorageCapacity() - 1; }

 private:
  /*
   * Get/Set manipulates the encoding within `aNumber` by storing the index as a
   * number and shifting it to the left (set) or right (get).
   *
   * Initial `aNumber` value is 0.
   *
   * Set() with first index value (1), we store the index on mBits and we shift
   * it to the left, e.g., as follows:
   *
   * aNumber=0b00000000000000000000000000000000000000000000000000000000000000
   * aIndex=0 aValue=1
   * aNumber=0b00000000000000000000000000000000000000000000000000000000000001
   * aIndex=1 aValue=33
   * aNumber=0b00000000000000000000000000000000000000000000000000000000100001
   * aIndex=2 aValue=801
   * aNumber=0b00000000000000000000000000000000000000000000000000001100100001
   * aIndex=3 aValue=17185
   * aNumber=0b00000000000000000000000000000000000000000000000100001100100001
   * aIndex=4 aValue=344865
   * aNumber=0b00000000000000000000000000000000000000000001010100001100100001
   * aIndex=5 aValue=6636321
   * aNumber=0b00000000000000000000000000000000000000011001010100001100100001
   * aIndex=6 aValue=124076833
   * aNumber=0b00000000000000000000000000000000000111011001010100001100100001
   * aIndex=7 aValue=2271560481
   * aNumber=0b00000000000000000000000000000010000111011001010100001100100001
   * aIndex=8 aValue=40926266145
   * aNumber=0b00000000000000000000000000100110000111011001010100001100100001
   * aIndex=9 aValue=728121033505
   * aNumber=0b00000000000000000000001010100110000111011001010100001100100001
   * aIndex=10 aValue=12822748939041
   * aNumber=0b00000000000000000010111010100110000111011001010100001100100001
   * aIndex=11 aValue=223928981472033
   * aNumber=0b00000000000000110010111010100110000111011001010100001100100001
   * aIndex=12 aValue=3883103678710561
   * aNumber=0b00000000001101110010111010100110000111011001010100001100100001
   * aIndex=13 aValue=66933498461897505
   * aNumber=0b00000011101101110010111010100110000111011001010100001100100001
   * aIndex=14 aValue=1147797409030816545
   */
  [[nodiscard]] uint64_t Get(uint64_t aNumber, uint64_t aIndex) {
    return (aNumber >> (mBits * aIndex)) & mMask;
  }

  [[nodiscard]] uint64_t Set(uint64_t aNumber, uint64_t aIndex,
                             uint64_t aValue) {
    return (aNumber & ~(mMask << (mBits * aIndex))) |
           (aValue << (mBits * aIndex));
  }

  /*
   * Enqueue a value in the ring buffer at aIndex.
   *
   * Takes the current uint64_t value from the atomic and try to acquire a non
   * used slot in the ring buffer. If unsucessfull, 0 is returned, otherwise
   * compute the new atomic value that holds the new state of usage of the
   * slots, and use compare/exchange to perform lock-free synchronization:
   * compare/exchanges succeeds when the current value and the modified one are
   * equal, reflecting an acquired lock. If another thread was concurrent to
   * this one, then it would fail to that operation, and go into the next
   * iteration of the loop to read the new state value from the atomic, and
   * acquire a different slot.
   *
   * @param aSlotStatus a uint64_t atomic that is used to perform lock-free
   * thread exclusions
   *
   * @param aIndex the index where we want to enqueue. It should come from the
   * empty queue
   * */
  void MarkSlot(std::atomic<uint64_t>& aSlotStatus, uint64_t aIndex) {
    uint64_t current = aSlotStatus.load(std::memory_order_relaxed);
    do {
      // Attempts to find a slot that is available to enqueue, without
      // cross-thread synchronization
      auto empty = [&]() -> std::optional<uint64_t> {
        for (uint64_t i = 0; i < Capacity(); ++i) {
          if (Get(current, i) == 0) {
            return i;
          }
        }
        return {};
      }();
      if (!empty.has_value()) {
        // Rust does expect() which would panic:
        // https://docs.rs/signal-hook/0.3.17/src/signal_hook/low_level/channel.rs.html#62
        // If there's no empty place, then it would be up to the caller to deal
        // with that
        MOZ_CRASH("No empty slot available");
      }
      uint64_t modified = Set(current, *empty, aIndex);
      // This is where the lock-free synchronization happens ; if `current`
      // matches the content of `aSlotStatus`, then store `modified` in
      // aSlotStatus and succeeds. Upon success it means no other thread has
      // tried to change the same value at the same time, so the lock was safely
      // acquired.
      //
      // Upon failure, it means another thread tried at the same time to use the
      // same slot, so a new iteration of the loop needs to be executed to try
      // another slot.
      //
      // In case of success (`aSlotStatus`'s content is equal to `current`), we
      // require memory_order_release for the read-modify-write operation
      // because we want to make sure when acquiring a slot that any concurrent
      // thread performing a write had a chance to do it.
      //
      // In case of failure we require memory_order_relaxed for the load
      // operation because we dont need synchronization at that point.
      if (aSlotStatus.compare_exchange_weak(current, modified,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
        if constexpr (MPSC_DEBUG) {
          fprintf(stderr,
                  "[enqueue] modified=0x%" PRIx64 " => index=%" PRIu64 "\n",
                  modified, aIndex);
        }
        return;
      }
    } while (true);
  }

  /*
   * Dequeue a value from the ring buffer.
   *
   * Takes the current value from the uint64_t atomic and read the current index
   * out of it. If that index is 0 then we are facing a lack of slots and we
   * return, the caller MUST check this and deal with the situation. If the
   * index is non null we can try to acquire the matching slot in the ring
   * buffer thanks to the compare/exchange loop. When the compare/exchange call
   * succeeds, then the slot was acquired.
   *
   * @param aSlotStatus a uint64_t atomic that is used to perform lock-free
   * thread exclusions
   * */
  [[nodiscard]] std::optional<uint64_t> UnmarkSlot(
      std::atomic<uint64_t>& aSlotStatus) {
    uint64_t current = aSlotStatus.load(std::memory_order_relaxed);
    do {
      uint64_t index = current & mMask;
      if (index == 0) {
        // Return a None
        // https://docs.rs/signal-hook/0.3.17/src/signal_hook/low_level/channel.rs.html#77
        // If we return None while dequeuing on mFree then we are full and the
        // caller needs to deal with that.
        return {};
      }
      uint64_t modified = current >> mBits;
      // See the comment in MarkSlot for details
      //
      // In case of success (`aSlotStatus`'s content is equal to `current`), we
      // require memory_order_acquire for the read-modify-write operation
      // because we want to make sure when unmarking a slot that any concurrent
      // thread performing a read will see the value we are writing.
      //
      // In case of failure we require memory_order_relaxed for the load
      // operation because we dont need synchronization at that point.
      if (aSlotStatus.compare_exchange_weak(current, modified,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        if constexpr (MPSC_DEBUG) {
          fprintf(stderr,
                  "[dequeue] current=0x%" PRIx64 " => index=%" PRIu64 "\n",
                  current, index);
        }
        return index;
      }
    } while (true);
    return {};
  }

  // Return the number of elements we can store within the ring buffer, whereas
  // Capacity() will return the amount of elements in mData, including the 0
  // value.
  [[nodiscard]] size_t StorageCapacity() const { return mCapacity; }

  // For the atomics below they are manipulated by Get()/Set(), and we are using
  // them to store the IDs of the ring buffer usage (empty/full).
  //
  // We use mBits bits to store an ID (so we are limited to 16 and 0 is
  // reserved) and append each of them to the atomics.
  //
  // A 0 value in one of those denotes we are full for the atomic, i.e.,
  // mFree=0 means we are full and mOccupied=0 means we are empty.

  // Holds the IDs of the free slots in the ring buffer
  std::atomic<uint64_t> mFree;

  // Holds the IDs of the occupied slots in the ring buffer
  std::atomic<uint64_t> mOccupied;

  const size_t mCapacity;

  // The actual ring buffer
  std::unique_ptr<T[]> mData;

  // How we are using the uint64_t atomic above to store the IDs of the ring
  // buffer.
  static const uint64_t mBits = 4;
  static const uint64_t mMask = 0b1111;
};

/**
 * Instantiation of the `MPSCRingBufferBase` type. This is safe to use from
 * several producers threads and one one consumer (that never changes role),
 * without explicit synchronization nor allocation (outside of the constructor).
 */
template <typename T>
using MPSCQueue = MPSCRingBufferBase<T>;

}  // namespace mozilla

#endif  // mozilla_MPSCQueue_h

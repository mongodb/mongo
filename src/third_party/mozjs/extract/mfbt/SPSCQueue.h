/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Single producer single consumer lock-free and wait-free queue. */

#ifndef mozilla_LockFreeQueue_h
#define mozilla_LockFreeQueue_h

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

namespace mozilla {

namespace detail {
template <typename T, bool IsPod = std::is_trivial<T>::value>
struct MemoryOperations {
  /**
   * This allows zeroing (using memset) or default-constructing a number of
   * elements calling the constructors if necessary.
   */
  static void ConstructDefault(T* aDestination, size_t aCount);
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
  static void ConstructDefault(T* aDestination, size_t aCount) {
    PodZero(aDestination, aCount);
  }
  static void MoveOrCopy(T* aDestination, T* aSource, size_t aCount) {
    PodCopy(aDestination, aSource, aCount);
  }
};

template <typename T>
struct MemoryOperations<T, false> {
  static void ConstructDefault(T* aDestination, size_t aCount) {
    for (size_t i = 0; i < aCount; i++) {
      aDestination[i] = T();
    }
  }
  static void MoveOrCopy(T* aDestination, T* aSource, size_t aCount) {
    std::move(aSource, aSource + aCount, aDestination);
  }
};
}  // namespace detail

/**
 * This data structure allows producing data from one thread, and consuming it
 * on another thread, safely and without explicit synchronization.
 *
 * The role for the producer and the consumer must be constant, i.e., the
 * producer should always be on one thread and the consumer should always be on
 * another thread.
 *
 * Some words about the inner workings of this class:
 * - Capacity is fixed. Only one allocation is performed, in the constructor.
 *   When reading and writing, the return value of the method allows checking if
 *   the ring buffer is empty or full.
 * - We always keep the read index at least one element ahead of the write
 *   index, so we can distinguish between an empty and a full ring buffer: an
 *   empty ring buffer is when the write index is at the same position as the
 *   read index. A full buffer is when the write index is exactly one position
 *   before the read index.
 * - We synchronize updates to the read index after having read the data, and
 *   the write index after having written the data. This means that the each
 *   thread can only touch a portion of the buffer that is not touched by the
 *   other thread.
 * - Callers are expected to provide buffers. When writing to the queue,
 *   elements are copied into the internal storage from the buffer passed in.
 *   When reading from the queue, the user is expected to provide a buffer.
 *   Because this is a ring buffer, data might not be contiguous in memory;
 *   providing an external buffer to copy into is an easy way to have linear
 *   data for further processing.
 */
template <typename T>
class SPSCRingBufferBase {
 public:
  /**
   * Constructor for a ring buffer.
   *
   * This performs an allocation on the heap, but is the only allocation that
   * will happen for the life time of a `SPSCRingBufferBase`.
   *
   * @param Capacity The maximum number of element this ring buffer will hold.
   */
  explicit SPSCRingBufferBase(int aCapacity)
      : mReadIndex(0),
        mWriteIndex(0),
        /* One more element to distinguish from empty and full buffer. */
        mCapacity(aCapacity + 1) {
    MOZ_RELEASE_ASSERT(aCapacity != std::numeric_limits<int>::max());
    MOZ_RELEASE_ASSERT(mCapacity > 0);

    mData = std::make_unique<T[]>(StorageCapacity());

    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  /**
   * Push `aCount` zero or default constructed elements in the array.
   *
   * Only safely called on the producer thread.
   *
   * @param count The number of elements to enqueue.
   * @return The number of element enqueued.
   */
  [[nodiscard]] int EnqueueDefault(int aCount) {
    return Enqueue(nullptr, aCount);
  }
  /**
   * @brief Put an element in the queue.
   *
   * Only safely called on the producer thread.
   *
   * @param element The element to put in the queue.
   *
   * @return 1 if the element was inserted, 0 otherwise.
   */
  [[nodiscard]] int Enqueue(T& aElement) { return Enqueue(&aElement, 1); }
  /**
   * Push `aCount` elements in the ring buffer.
   *
   * Only safely called on the producer thread.
   *
   * @param elements a pointer to a buffer containing at least `count` elements.
   * If `elements` is nullptr, zero or default constructed elements are enqueud.
   * @param count The number of elements to read from `elements`
   * @return The number of elements successfully coped from `elements` and
   * inserted into the ring buffer.
   */
  [[nodiscard]] int Enqueue(T* aElements, int aCount) {
#ifdef DEBUG
    AssertCorrectThread(mProducerId);
#endif

    int rdIdx = mReadIndex.load(std::memory_order_acquire);
    int wrIdx = mWriteIndex.load(std::memory_order_relaxed);

    if (IsFull(rdIdx, wrIdx)) {
      return 0;
    }

    int toWrite = std::min(AvailableWriteInternal(rdIdx, wrIdx), aCount);

    /* First part, from the write index to the end of the array. */
    int firstPart = std::min(StorageCapacity() - wrIdx, toWrite);
    /* Second part, from the beginning of the array */
    int secondPart = toWrite - firstPart;

    if (aElements) {
      detail::MemoryOperations<T>::MoveOrCopy(mData.get() + wrIdx, aElements,
                                              firstPart);
      detail::MemoryOperations<T>::MoveOrCopy(
          mData.get(), aElements + firstPart, secondPart);
    } else {
      detail::MemoryOperations<T>::ConstructDefault(mData.get() + wrIdx,
                                                    firstPart);
      detail::MemoryOperations<T>::ConstructDefault(mData.get(), secondPart);
    }

    mWriteIndex.store(IncrementIndex(wrIdx, toWrite),
                      std::memory_order_release);

    return toWrite;
  }
  /**
   * Retrieve at most `count` elements from the ring buffer, and copy them to
   * `elements`, if non-null.
   *
   * Only safely called on the consumer side.
   *
   * @param elements A pointer to a buffer with space for at least `count`
   * elements. If `elements` is `nullptr`, `count` element will be discarded.
   * @param count The maximum number of elements to Dequeue.
   * @return The number of elements written to `elements`.
   */
  [[nodiscard]] int Dequeue(T* elements, int count) {
#ifdef DEBUG
    AssertCorrectThread(mConsumerId);
#endif

    int wrIdx = mWriteIndex.load(std::memory_order_acquire);
    int rdIdx = mReadIndex.load(std::memory_order_relaxed);

    if (IsEmpty(rdIdx, wrIdx)) {
      return 0;
    }

    int toRead = std::min(AvailableReadInternal(rdIdx, wrIdx), count);

    int firstPart = std::min(StorageCapacity() - rdIdx, toRead);
    int secondPart = toRead - firstPart;

    if (elements) {
      detail::MemoryOperations<T>::MoveOrCopy(elements, mData.get() + rdIdx,
                                              firstPart);
      detail::MemoryOperations<T>::MoveOrCopy(elements + firstPart, mData.get(),
                                              secondPart);
    }

    mReadIndex.store(IncrementIndex(rdIdx, toRead), std::memory_order_release);

    return toRead;
  }
  /**
   * Get the number of available elements for consuming.
   *
   * This can be less than the actual number of elements in the queue, since the
   * mWriteIndex is updated at the very end of the Enqueue method on the
   * producer thread, but consequently always returns a number of elements such
   * that a call to Dequeue return this number of elements.
   *
   * @return The number of available elements for reading.
   */
  int AvailableRead() const {
    return AvailableReadInternal(mReadIndex.load(std::memory_order_relaxed),
                                 mWriteIndex.load(std::memory_order_relaxed));
  }
  /**
   * Get the number of available elements for writing.
   *
   * This can be less than than the actual number of slots that are available,
   * because mReadIndex is updated at the very end of the Deque method. It
   * always returns a number such that a call to Enqueue with this number will
   * succeed in enqueuing this number of elements.
   *
   * @return The number of empty slots in the buffer, available for writing.
   */
  int AvailableWrite() const {
    return AvailableWriteInternal(mReadIndex.load(std::memory_order_relaxed),
                                  mWriteIndex.load(std::memory_order_relaxed));
  }
  /**
   * Get the total Capacity, for this ring buffer.
   *
   * Can be called safely on any thread.
   *
   * @return The maximum Capacity of this ring buffer.
   */
  int Capacity() const { return StorageCapacity() - 1; }

  /**
   * Reset the consumer thread id to the current thread. The caller must
   * guarantee that the last call to Dequeue() on the previous consumer thread
   * has completed, and subsequent calls to Dequeue() will only happen on the
   * current thread.
   */
  void ResetConsumerThreadId() {
#ifdef DEBUG
    mConsumerId = std::this_thread::get_id();
#endif

    // When changing consumer from thread A to B, the last Dequeue on A (synced
    // by mReadIndex.store with memory_order_release) must be picked up by B
    // through an acquire operation.
    std::ignore = mReadIndex.load(std::memory_order_acquire);
  }

  /**
   * Reset the producer thread id to the current thread. The caller must
   * guarantee that the last call to Enqueue() on the previous consumer thread
   * has completed, and subsequent calls to Dequeue() will only happen on the
   * current thread.
   */
  void ResetProducerThreadId() {
#ifdef DEBUG
    mProducerId = std::this_thread::get_id();
#endif

    // When changing producer from thread A to B, the last Enqueue on A (synced
    // by mWriteIndex.store with memory_order_release) must be picked up by B
    // through an acquire operation.
    std::ignore = mWriteIndex.load(std::memory_order_acquire);
  }

 private:
  /** Return true if the ring buffer is empty.
   *
   * This can be called from the consumer or the producer thread.
   *
   * @param aReadIndex the read index to consider
   * @param writeIndex the write index to consider
   * @return true if the ring buffer is empty, false otherwise.
   **/
  bool IsEmpty(int aReadIndex, int aWriteIndex) const {
    return aWriteIndex == aReadIndex;
  }
  /** Return true if the ring buffer is full.
   *
   * This happens if the write index is exactly one element behind the read
   * index.
   *
   * This can be called from the consummer or the producer thread.
   *
   * @param aReadIndex the read index to consider
   * @param writeIndex the write index to consider
   * @return true if the ring buffer is full, false otherwise.
   **/
  bool IsFull(int aReadIndex, int aWriteIndex) const {
    return (aWriteIndex + 1) % StorageCapacity() == aReadIndex;
  }
  /**
   * Return the size of the storage. It is one more than the number of elements
   * that can be stored in the buffer.
   *
   * This can be called from any thread.
   *
   * @return the number of elements that can be stored in the buffer.
   */
  int StorageCapacity() const { return mCapacity; }
  /**
   * Returns the number of elements available for reading.
   *
   * This can be called from the consummer or producer thread, but see the
   * comment in `AvailableRead`.
   *
   * @return the number of available elements for reading.
   */
  int AvailableReadInternal(int aReadIndex, int aWriteIndex) const {
    if (aWriteIndex >= aReadIndex) {
      return aWriteIndex - aReadIndex;
    } else {
      return aWriteIndex + StorageCapacity() - aReadIndex;
    }
  }
  /**
   * Returns the number of empty elements, available for writing.
   *
   * This can be called from the consummer or producer thread, but see the
   * comment in `AvailableWrite`.
   *
   * @return the number of elements that can be written into the array.
   */
  int AvailableWriteInternal(int aReadIndex, int aWriteIndex) const {
    /* We subtract one element here to always keep at least one sample
     * free in the buffer, to distinguish between full and empty array. */
    int rv = aReadIndex - aWriteIndex - 1;
    if (aWriteIndex >= aReadIndex) {
      rv += StorageCapacity();
    }
    return rv;
  }
  /**
   * Increments an index, wrapping it around the storage.
   *
   * Incrementing `mWriteIndex` can be done on the producer thread.
   * Incrementing `mReadIndex` can be done on the consummer thread.
   *
   * @param index a reference to the index to increment.
   * @param increment the number by which `index` is incremented.
   * @return the new index.
   */
  int IncrementIndex(int aIndex, int aIncrement) const {
    MOZ_ASSERT(aIncrement >= 0 && aIncrement < StorageCapacity() &&
               aIndex < StorageCapacity());
    return (aIndex + aIncrement) % StorageCapacity();
  }
  /**
   * @brief This allows checking that Enqueue (resp. Dequeue) are always
   * called by the right thread.
   *
   * The role of the thread are assigned the first time they call Enqueue or
   * Dequeue, and cannot change, except by a ResetThreadId method.
   *
   * @param id the id of the thread that has called the calling method first.
   */
#ifdef DEBUG
  static void AssertCorrectThread(std::thread::id& aId) {
    if (aId == std::thread::id()) {
      aId = std::this_thread::get_id();
      return;
    }
    MOZ_ASSERT(aId == std::this_thread::get_id());
  }
#endif
  /** Index at which the oldest element is. */
  std::atomic<int> mReadIndex;
  /** Index at which to write new elements. `mWriteIndex` is always at
   * least one element ahead of `mReadIndex`. */
  std::atomic<int> mWriteIndex;
  /** Maximum number of elements that can be stored in the ring buffer. */
  const int mCapacity;
  /** Data storage, of size `mCapacity + 1` */
  std::unique_ptr<T[]> mData;
#ifdef DEBUG
  /** The id of the only thread that is allowed to read from the queue. */
  mutable std::thread::id mConsumerId;
  /** The id of the only thread that is allowed to write from the queue. */
  mutable std::thread::id mProducerId;
#endif
};

/**
 * Instantiation of the `SPSCRingBufferBase` type. This is safe to use
 * from two threads, one producer, one consumer (that never change role),
 * without explicit synchronization.
 */
template <typename T>
using SPSCQueue = SPSCRingBufferBase<T>;

}  // namespace mozilla

#endif  // mozilla_LockFreeQueue_h

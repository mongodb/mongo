/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StringBuffer_h__
#define StringBuffer_h__

#include <atomic>
#include <cstring>
#include "mozilla/MemoryReporting.h"
#include "mozilla/Assertions.h"
#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/RefCounted.h"

namespace mozilla {

/**
 * This structure precedes the string buffers "we" allocate.  It may be the
 * case that nsTAString::mData does not point to one of these special
 * buffers.  The mDataFlags member variable distinguishes the buffer type.
 *
 * When this header is in use, it enables reference counting, and capacity
 * tracking.  NOTE: A string buffer can be modified only if its reference
 * count is 1.
 */
class StringBuffer {
 private:
  std::atomic<uint32_t> mRefCount;
  uint32_t mStorageSize;

 public:
  MOZ_DECLARE_REFCOUNTED_TYPENAME(StringBuffer)

  /**
   * Allocates a new string buffer, with given size in bytes and a
   * reference count of one.  When the string buffer is no longer needed,
   * it should be released via Release.
   *
   * It is up to the caller to set the bytes corresponding to the string
   * buffer by calling the Data method to fetch the raw data pointer.  Care
   * must be taken to properly null terminate the character array.  The
   * storage size can be greater than the length of the actual string
   * (i.e., it is not required that the null terminator appear in the last
   * storage unit of the string buffer's data).
   *
   * This guarantees that StorageSize() returns aSize if the returned
   * buffer is non-null. Some callers like nsAttrValue rely on it.
   *
   * @return new string buffer or null if out of memory.
   */
  static already_AddRefed<StringBuffer> Alloc(size_t aSize) {
    MOZ_ASSERT(aSize != 0, "zero capacity allocation not allowed");
    MOZ_ASSERT(sizeof(StringBuffer) + aSize <= size_t(uint32_t(-1)) &&
                   sizeof(StringBuffer) + aSize > aSize,
               "mStorageSize will truncate");

    auto* hdr = (StringBuffer*)malloc(sizeof(StringBuffer) + aSize);
    if (hdr) {
      hdr->mRefCount = 1;
      hdr->mStorageSize = aSize;
      detail::RefCountLogger::logAddRef(hdr, 1);
    }
    return already_AddRefed(hdr);
  }

  /**
   * Returns a string buffer initialized with the given string on it, or null on
   * OOM.
   * Note that this will allocate extra space for the trailing null byte, which
   * this method will add.
   */
  static already_AddRefed<StringBuffer> Create(const char16_t* aData,
                                               size_t aLength) {
    return DoCreate(aData, aLength);
  }
  static already_AddRefed<StringBuffer> Create(const char* aData,
                                               size_t aLength) {
    return DoCreate(aData, aLength);
  }

  /**
   * Resizes the given string buffer to the specified storage size.  This
   * method must not be called on a readonly string buffer.  Use this API
   * carefully!!
   *
   * This method behaves like the ANSI-C realloc function.  (i.e., If the
   * allocation fails, null will be returned and the given string buffer
   * will remain unmodified.)
   *
   * @see IsReadonly
   */
  static StringBuffer* Realloc(StringBuffer* aHdr, size_t aSize) {
    MOZ_ASSERT(aSize != 0, "zero capacity allocation not allowed");
    MOZ_ASSERT(sizeof(StringBuffer) + aSize <= size_t(uint32_t(-1)) &&
                   sizeof(StringBuffer) + aSize > aSize,
               "mStorageSize will truncate");

    // no point in trying to save ourselves if we hit this assertion
    MOZ_ASSERT(!aHdr->IsReadonly(), "|Realloc| attempted on readonly string");

    // Treat this as a release and addref for refcounting purposes, since we
    // just asserted that the refcount is 1.  If we don't do that, refcount
    // logging will claim we've leaked all sorts of stuff.
    {
      detail::RefCountLogger::ReleaseLogger logger(aHdr);
      logger.logRelease(0);
    }

    aHdr = (StringBuffer*)realloc(aHdr, sizeof(StringBuffer) + aSize);
    if (aHdr) {
      detail::RefCountLogger::logAddRef(aHdr, 1);
      aHdr->mStorageSize = aSize;
    }

    return aHdr;
  }

  void AddRef() {
    // Memory synchronization is not required when incrementing a
    // reference count.  The first increment of a reference count on a
    // thread is not important, since the first use of the object on a
    // thread can happen before it.  What is important is the transfer
    // of the pointer to that thread, which may happen prior to the
    // first increment on that thread.  The necessary memory
    // synchronization is done by the mechanism that transfers the
    // pointer between threads.
    uint32_t count = mRefCount.fetch_add(1, std::memory_order_relaxed) + 1;
    detail::RefCountLogger::logAddRef(this, count);
  }

  void Release() {
    // Since this may be the last release on this thread, we need release
    // semantics so that prior writes on this thread are visible to the thread
    // that destroys the object when it reads mValue with acquire semantics.
    detail::RefCountLogger::ReleaseLogger logger(this);
    uint32_t count = mRefCount.fetch_sub(1, std::memory_order_release) - 1;
    logger.logRelease(count);
    if (count == 0) {
      // We're going to destroy the object on this thread, so we need acquire
      // semantics to synchronize with the memory released by the last release
      // on other threads, that is, to ensure that writes prior to that release
      // are now visible on this thread.
      count = mRefCount.load(std::memory_order_acquire);
      free(this);  // We were allocated with malloc.
    }
  }

  /**
   * This method returns the string buffer corresponding to the given data
   * pointer.  The data pointer must have been returned previously by a
   * call to the StringBuffer::Data method.
   */
  static StringBuffer* FromData(void* aData) {
    return reinterpret_cast<StringBuffer*>(aData) - 1;
  }

  /**
   * This method returns the data pointer for this string buffer.
   */
  void* Data() const {
    return const_cast<char*>(reinterpret_cast<const char*>(this + 1));
  }

  /**
   * This function returns the storage size of a string buffer in bytes.
   * This value is the same value that was originally passed to Alloc (or
   * Realloc).
   */
  uint32_t StorageSize() const { return mStorageSize; }

  /**
   * If this method returns false, then the caller can be sure that their
   * reference to the string buffer is the only reference to the string
   * buffer, and therefore it has exclusive access to the string buffer and
   * associated data.  However, if this function returns true, then other
   * consumers may rely on the data in this buffer being immutable and
   * other threads may access this buffer simultaneously.
   */
  bool IsReadonly() const {
    // This doesn't lead to the destruction of the buffer, so we don't
    // need to perform acquire memory synchronization for the normal
    // reason that a reference count needs acquire synchronization
    // (ensuring that all writes to the object made on other threads are
    // visible to the thread destroying the object).
    //
    // We then need to consider the possibility that there were prior
    // writes to the buffer on a different thread:  one that has either
    // since released its reference count, or one that also has access
    // to this buffer through the same reference.  There are two ways
    // for that to happen: either the buffer pointer or a data structure
    // (e.g., string object) pointing to the buffer was transferred from
    // one thread to another, or the data structure pointing to the
    // buffer was already visible on both threads.  In the first case
    // (transfer), the transfer of data from one thread to another would
    // have handled the memory synchronization.  In the latter case
    // (data structure visible on both threads), the caller needed some
    // sort of higher level memory synchronization to protect against
    // the string object being mutated at the same time on multiple
    // threads.

    // See bug 1603504. TSan might complain about a race when using
    // memory_order_relaxed, so use memory_order_acquire for making TSan
    // happy.
#if defined(MOZ_TSAN)
    return mRefCount.load(std::memory_order_acquire) > 1;
#else
    return mRefCount.load(std::memory_order_relaxed) > 1;
#endif
  }

  /**
   * This measures the size only if the StringBuffer is unshared.
   */
  size_t SizeOfIncludingThisIfUnshared(MallocSizeOf aMallocSizeOf) const {
    return IsReadonly() ? 0 : aMallocSizeOf(this);
  }

  /**
   * This measures the size regardless of whether the StringBuffer is
   * unshared.
   *
   * WARNING: Only use this if you really know what you are doing, because
   * it can easily lead to double-counting strings.  If you do use them,
   * please explain clearly in a comment why it's safe and won't lead to
   * double-counting.
   */
  size_t SizeOfIncludingThisEvenIfShared(MallocSizeOf aMallocSizeOf) const {
    return aMallocSizeOf(this);
  }

 private:
  template <typename CharT>
  static already_AddRefed<StringBuffer> DoCreate(const CharT* aData,
                                                 size_t aLength) {
    StringBuffer* buffer = Alloc((aLength + 1) * sizeof(CharT)).take();
    if (MOZ_LIKELY(buffer)) {
      auto* data = reinterpret_cast<CharT*>(buffer->Data());
      memcpy(data, aData, aLength * sizeof(CharT));
      data[aLength] = 0;
    }
    return already_AddRefed(buffer);
  }
};

}  // namespace mozilla

#endif

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Buffer_h
#define mozilla_Buffer_h

#include <cstddef>
#include <iterator>

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/UniquePtrExtensions.h"

namespace mozilla {

/**
 * A move-only type that wraps a mozilla::UniquePtr<T[]> and the length of
 * the T[].
 *
 * Unlike mozilla::Array, the length is a run-time property.
 * Unlike mozilla::Vector and nsTArray, does not have capacity and
 * assocatiated growth functionality.
 * Unlike mozilla::Span, mozilla::Buffer owns the allocation it points to.
 */
template <typename T>
class Buffer final {
 private:
  mozilla::UniquePtr<T[]> mData;
  size_t mLength;

 public:
  Buffer(const Buffer<T>& aOther) = delete;
  Buffer<T>& operator=(const Buffer<T>& aOther) = delete;

  /**
   * Construct zero-lenth Buffer (without actually pointing to a heap
   * allocation).
   */
  Buffer() : mData(nullptr), mLength(0){};

  /**
   * Construct from raw parts.
   *
   * aLength must not be greater than the actual length of the buffer pointed
   * to by aData.
   */
  Buffer(mozilla::UniquePtr<T[]>&& aData, size_t aLength)
      : mData(std::move(aData)), mLength(aLength) {}

  /**
   * Move constructor. Sets the moved-from Buffer to zero-length
   * state.
   */
  Buffer(Buffer<T>&& aOther)
      : mData(std::move(aOther.mData)), mLength(aOther.mLength) {
    aOther.mLength = 0;
  }

  /**
   * Move assignment. Sets the moved-from Buffer to zero-length
   * state.
   */
  Buffer<T>& operator=(Buffer<T>&& aOther) {
    mData = std::move(aOther.mData);
    mLength = aOther.mLength;
    aOther.mLength = 0;
    return *this;
  }

  /**
   * Construct by copying the elements of a Span.
   *
   * Allocates the internal buffer infallibly. Use CopyFrom for fallible
   * allocation.
   */
  explicit Buffer(mozilla::Span<const T> aSpan)
      : mData(mozilla::MakeUniqueForOverwrite<T[]>(aSpan.Length())),
        mLength(aSpan.Length()) {
    std::copy(aSpan.cbegin(), aSpan.cend(), mData.get());
  }

  /**
   * Create a new Buffer by copying the elements of a Span.
   *
   * Allocates the internal buffer fallibly.
   */
  static mozilla::Maybe<Buffer<T>> CopyFrom(mozilla::Span<const T> aSpan) {
    if (aSpan.IsEmpty()) {
      return Some(Buffer());
    }

    auto data = mozilla::MakeUniqueForOverwriteFallible<T[]>(aSpan.Length());
    if (!data) {
      return mozilla::Nothing();
    }
    std::copy(aSpan.cbegin(), aSpan.cend(), data.get());
    return mozilla::Some(Buffer(std::move(data), aSpan.Length()));
  }

  /**
   * Construct a buffer of requested length.
   *
   * The contents will be initialized or uninitialized according
   * to the behavior of mozilla::MakeUnique<T[]>(aLength) for T.
   *
   * Allocates the internal buffer infallibly. Use Alloc for fallible
   * allocation.
   */
  explicit Buffer(size_t aLength)
      : mData(mozilla::MakeUnique<T[]>(aLength)), mLength(aLength) {}

  /**
   * Create a new Buffer with an internal buffer of requested length.
   *
   * The contents will be initialized or uninitialized according to the
   * behavior of mozilla::MakeUnique<T[]>(aLength) for T.
   *
   * Allocates the internal buffer fallibly.
   */
  static mozilla::Maybe<Buffer<T>> Alloc(size_t aLength) {
    auto data = mozilla::MakeUniqueFallible<T[]>(aLength);
    if (!data) {
      return mozilla::Nothing();
    }
    return mozilla::Some(Buffer(std::move(data), aLength));
  }

  /**
   * Create a new Buffer with an internal buffer of requested length.
   *
   * This uses MakeUniqueFallibleForOverwrite so the contents will be
   * default-initialized.
   *
   * Allocates the internal buffer fallibly.
   */
  static Maybe<Buffer<T>> AllocForOverwrite(size_t aLength) {
    auto data = MakeUniqueForOverwriteFallible<T[]>(aLength);
    if (!data) {
      return Nothing();
    }
    return Some(Buffer(std::move(data), aLength));
  }

  auto AsSpan() const { return mozilla::Span<const T>{mData.get(), mLength}; }
  auto AsWritableSpan() { return mozilla::Span<T>{mData.get(), mLength}; }
  operator mozilla::Span<const T>() const { return AsSpan(); }
  operator mozilla::Span<T>() { return AsWritableSpan(); }

  /**
   * Guarantees a non-null and aligned pointer
   * even for the zero-length case.
   */
  T* Elements() { return AsWritableSpan().Elements(); }
  size_t Length() const { return mLength; }

  T& operator[](size_t aIndex) {
    MOZ_ASSERT(aIndex < mLength);
    return mData.get()[aIndex];
  }

  const T& operator[](size_t aIndex) const {
    MOZ_ASSERT(aIndex < mLength);
    return mData.get()[aIndex];
  }

  typedef T* iterator;
  typedef const T* const_iterator;
  typedef std::reverse_iterator<T*> reverse_iterator;
  typedef std::reverse_iterator<const T*> const_reverse_iterator;

  // Methods for range-based for loops.
  iterator begin() { return mData.get(); }
  const_iterator begin() const { return mData.get(); }
  const_iterator cbegin() const { return begin(); }
  iterator end() { return mData.get() + mLength; }
  const_iterator end() const { return mData.get() + mLength; }
  const_iterator cend() const { return end(); }

  // Methods for reverse iterating.
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator crbegin() const { return rbegin(); }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }
  const_reverse_iterator crend() const { return rend(); }
};

} /* namespace mozilla */

#endif /* mozilla_Buffer_h */

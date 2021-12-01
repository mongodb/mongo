/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A vector of pointers space-optimized for a small number of elements. */
#ifndef mozilla_SmallPointerArray_h
#define mozilla_SmallPointerArray_h

#include "mozilla/Assertions.h"
#include <algorithm>
#include <iterator>
#include <vector>

namespace mozilla {

// Array class for situations where a small number of elements (<= 2) is
// expected, a large number of elements must be accomodated if necessary,
// and the size of the class must be minimal. Typical vector implementations
// will fulfill the first two requirements by simply adding inline storage
// alongside the rest of their member variables. While this strategy works,
// it brings unnecessary storage overhead for vectors with an expected small
// number of elements. This class is intended to deal with that problem.
//
// This class is similar in performance to a vector class. Accessing its
// elements when it has not grown over a size of 2 does not require an extra
// level of indirection and will therefore be faster.
//
// The minimum (inline) size is 2 * sizeof(void*).
//
// Any modification of the array invalidates any outstanding iterators.
template<typename T>
class SmallPointerArray
{
public:
  SmallPointerArray()
  {
    mInlineElements[0] = mInlineElements[1] = nullptr;
    static_assert(sizeof(SmallPointerArray<T>) == (2 * sizeof(void*)),
      "SmallPointerArray must compile to the size of 2 pointers");
    static_assert(offsetof(SmallPointerArray<T>, mArray) ==
                  offsetof(SmallPointerArray<T>, mInlineElements) + sizeof(T*),
      "mArray and mInlineElements[1] are expected to overlap in memory");
    static_assert(offsetof(SmallPointerArray<T>, mPadding) ==
      offsetof(SmallPointerArray<T>, mInlineElements),
      "mPadding and mInlineElements[0] are expected to overlap in memory");
  }
  ~SmallPointerArray()
  {
    if (!mInlineElements[0] && mArray) {
      delete mArray;
    }
  }

  void Clear() {
    if (!mInlineElements[0] && mArray) {
      delete mArray;
      mArray = nullptr;
      return;
    }
    mInlineElements[0] = mInlineElements[1] = nullptr;
  }

  void AppendElement(T* aElement) {
    // Storing nullptr as an element is not permitted, but we do check for it
    // to avoid corruption issues in non-debug builds.

    // In addition to this we assert in debug builds to point out mistakes to
    // users of the class.
    MOZ_ASSERT(aElement != nullptr);
    if (!mInlineElements[0]) {
      if (!mArray) {
        mInlineElements[0] = aElement;
        // Harmless if aElement == nullptr;
        return;
      }

      if (!aElement) {
        return;
      }

      mArray->push_back(aElement);
      return;
    }

    if (!aElement) {
      return;
    }

    if (!mInlineElements[1]) {
      mInlineElements[1] = aElement;
      return;
    }

    mArray = new std::vector<T*>({ mInlineElements[0], mInlineElements[1], aElement });
    mInlineElements[0] = nullptr;
  }

  bool RemoveElement(T* aElement) {
    MOZ_ASSERT(aElement != nullptr);
    if (aElement == nullptr) {
      return false;
    }

    if (mInlineElements[0] == aElement) {
      // Expectected case.
      mInlineElements[0] = mInlineElements[1];
      mInlineElements[1] = nullptr;
      return true;
    }

    if (mInlineElements[0]) {
      if (mInlineElements[1] == aElement) {
        mInlineElements[1] = nullptr;
        return true;
      }
      return false;
    }

    if (mArray) {
      for (auto iter = mArray->begin(); iter != mArray->end(); iter++) {
        if (*iter == aElement) {
          mArray->erase(iter);
          return true;
        }
      }
    }
    return false;
  }

  bool Contains(T* aElement) const {
    MOZ_ASSERT(aElement != nullptr);
    if (aElement == nullptr) {
      return false;
    }

    if (mInlineElements[0] == aElement) {
      return true;
    }

    if (mInlineElements[0]) {
      if (mInlineElements[1] == aElement) {
        return true;
      }
      return false;
    }

    if (mArray) {
      return std::find(mArray->begin(), mArray->end(), aElement) != mArray->end();
    }
    return false;

  }

  size_t Length() const
  {
    if (mInlineElements[0]) {
      if (!mInlineElements[1]) {
        return 1;
      }
      return 2;
    }

    if (mArray) {
      return mArray->size();
    }

    return 0;
  }

  T* ElementAt(size_t aIndex) const {
    MOZ_ASSERT(aIndex < Length());
    if (mInlineElements[0]) {
      return mInlineElements[aIndex];
    }

    return (*mArray)[aIndex];
  }

  T* operator[](size_t aIndex) const
  {
    return ElementAt(aIndex);
  }

  typedef T**                        iterator;
  typedef const T**                  const_iterator;

  // Methods for range-based for loops. Manipulation invalidates these.
  iterator begin() {
    return beginInternal();
  }
  const_iterator begin() const {
    return beginInternal();
  }
  const_iterator cbegin() const { return begin(); }
  iterator end() {
    return beginInternal() + Length();
  }
  const_iterator end() const {
    return beginInternal() + Length();
  }
  const_iterator cend() const { return end(); }

private:
  T** beginInternal() const {
    if (mInlineElements[0] || !mArray) {
      return const_cast<T**>(&mInlineElements[0]);
    }

    if (mArray->empty()) {
      return nullptr;
    }

    return &(*mArray)[0];
  }

  // mArray and mInlineElements[1] share the same area in memory.
  //
  // When !mInlineElements[0] && !mInlineElements[1] the array is empty.
  //
  // When mInlineElements[0] && !mInlineElements[1], mInlineElements[0]
  // contains the first element. The array is of size 1.
  //
  // When mInlineElements[0] && mInlineElements[1], mInlineElements[0]
  // contains the first element and mInlineElements[1] the second. The
  // array is of size 2.
  //
  // When !mInlineElements[0] && mArray, mArray contains the full contents
  // of the array and is of arbitrary size.
  union {
    T* mInlineElements[2];
    struct {
      void* mPadding;
      std::vector<T*>* mArray;
    };
  };
};

} // namespace mozilla

#endif // mozilla_SmallPointerArray_h

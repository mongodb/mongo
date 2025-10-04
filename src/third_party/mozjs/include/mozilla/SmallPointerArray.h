/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A vector of pointers space-optimized for a small number of elements. */

#ifndef mozilla_SmallPointerArray_h
#define mozilla_SmallPointerArray_h

#include "mozilla/Assertions.h"
#include "mozilla/PodOperations.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <vector>

namespace mozilla {

// Array class for situations where a small number of NON-NULL elements (<= 2)
// is expected, a large number of elements must be accommodated if necessary,
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
template <typename T>
class SmallPointerArray {
 public:
  SmallPointerArray() {
    // List-initialization would be nicer, but it only lets you initialize the
    // first union member.
    mArray[0].mValue = nullptr;
    mArray[1].mVector = nullptr;
  }

  ~SmallPointerArray() {
    if (!first()) {
      delete maybeVector();
    }
  }

  SmallPointerArray(SmallPointerArray&& aOther) {
    PodCopy(mArray, aOther.mArray, 2);
    aOther.mArray[0].mValue = nullptr;
    aOther.mArray[1].mVector = nullptr;
  }

  SmallPointerArray& operator=(SmallPointerArray&& aOther) {
    std::swap(mArray, aOther.mArray);
    return *this;
  }

  void Clear() {
    if (first()) {
      first() = nullptr;
      new (&mArray[1].mValue) std::vector<T*>*(nullptr);
      return;
    }

    delete maybeVector();
    mArray[1].mVector = nullptr;
  }

  void AppendElement(T* aElement) {
    // Storing nullptr as an element is not permitted, but we do check for it
    // to avoid corruption issues in non-debug builds.

    // In addition to this we assert in debug builds to point out mistakes to
    // users of the class.
    MOZ_ASSERT(aElement != nullptr);
    if (aElement == nullptr) {
      return;
    }

    if (!first()) {
      auto* vec = maybeVector();
      if (!vec) {
        first() = aElement;
        new (&mArray[1].mValue) T*(nullptr);
        return;
      }

      vec->push_back(aElement);
      return;
    }

    if (!second()) {
      second() = aElement;
      return;
    }

    auto* vec = new std::vector<T*>({first(), second(), aElement});
    first() = nullptr;
    new (&mArray[1].mVector) std::vector<T*>*(vec);
  }

  bool RemoveElement(T* aElement) {
    MOZ_ASSERT(aElement != nullptr);
    if (aElement == nullptr) {
      return false;
    }

    if (first() == aElement) {
      // Expected case.
      T* maybeSecond = second();
      first() = maybeSecond;
      if (maybeSecond) {
        second() = nullptr;
      } else {
        new (&mArray[1].mVector) std::vector<T*>*(nullptr);
      }

      return true;
    }

    if (first()) {
      if (second() == aElement) {
        second() = nullptr;
        return true;
      }
      return false;
    }

    if (auto* vec = maybeVector()) {
      for (auto iter = vec->begin(); iter != vec->end(); iter++) {
        if (*iter == aElement) {
          vec->erase(iter);
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

    if (T* v = first()) {
      return v == aElement || second() == aElement;
    }

    if (auto* vec = maybeVector()) {
      return std::find(vec->begin(), vec->end(), aElement) != vec->end();
    }

    return false;
  }

  size_t Length() const {
    if (first()) {
      return second() ? 2 : 1;
    }

    if (auto* vec = maybeVector()) {
      return vec->size();
    }

    return 0;
  }

  bool IsEmpty() const { return Length() == 0; }

  T* ElementAt(size_t aIndex) const {
    MOZ_ASSERT(aIndex < Length());
    if (first()) {
      return mArray[aIndex].mValue;
    }

    auto* vec = maybeVector();
    MOZ_ASSERT(vec, "must have backing vector if accessing an element");
    return (*vec)[aIndex];
  }

  T* operator[](size_t aIndex) const { return ElementAt(aIndex); }

  using iterator = T**;
  using const_iterator = const T**;

  // Methods for range-based for loops. Manipulation invalidates these.
  iterator begin() { return beginInternal(); }
  const_iterator begin() const { return beginInternal(); }
  const_iterator cbegin() const { return begin(); }
  iterator end() { return beginInternal() + Length(); }
  const_iterator end() const { return beginInternal() + Length(); }
  const_iterator cend() const { return end(); }

 private:
  T** beginInternal() const {
    if (first()) {
      static_assert(sizeof(T*) == sizeof(Element),
                    "pointer ops on &first() must produce adjacent "
                    "Element::mValue arms");
      return &first();
    }

    auto* vec = maybeVector();
    if (!vec) {
      return &first();
    }

    if (vec->empty()) {
      return nullptr;
    }

    return &(*vec)[0];
  }

  // Accessors for |mArray| element union arms.

  T*& first() const { return const_cast<T*&>(mArray[0].mValue); }

  T*& second() const {
    MOZ_ASSERT(first(), "first() must be non-null to have a T* second pointer");
    return const_cast<T*&>(mArray[1].mValue);
  }

  std::vector<T*>* maybeVector() const {
    MOZ_ASSERT(!first(),
               "function must only be called when this is either empty or has "
               "std::vector-backed elements");
    return mArray[1].mVector;
  }

  // In C++ active-union-arm terms:
  //
  //   - mArray[0].mValue is always active: a possibly null T*;
  //   - if mArray[0].mValue is null, mArray[1].mVector is active: a possibly
  //     null std::vector<T*>*; if mArray[0].mValue isn't null, mArray[1].mValue
  //     is active: a possibly null T*.
  //
  // SmallPointerArray begins empty, with mArray[1].mVector active and null.
  // Code that makes mArray[0].mValue non-null, i.e. assignments to first(),
  // must placement-new mArray[1].mValue with the proper value; code that goes
  // the opposite direction, making mArray[0].mValue null, must placement-new
  // mArray[1].mVector with the proper value.
  //
  // When !mArray[0].mValue && !mArray[1].mVector, the array is empty.
  //
  // When mArray[0].mValue && !mArray[1].mValue, the array has size 1 and
  // contains mArray[0].mValue.
  //
  // When mArray[0] && mArray[1], the array has size 2 and contains
  // mArray[0].mValue and mArray[1].mValue.
  //
  // When !mArray[0].mValue && mArray[1].mVector, mArray[1].mVector contains
  // the contents of an array of arbitrary size (even less than two if it ever
  // contained three elements and elements were removed).
  union Element {
    T* mValue;
    std::vector<T*>* mVector;
  } mArray[2];
};

}  // namespace mozilla

#endif  // mozilla_SmallPointerArray_h

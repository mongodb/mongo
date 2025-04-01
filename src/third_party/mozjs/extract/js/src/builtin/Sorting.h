/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Sorting_h
#define builtin_Sorting_h

#include "mozilla/Attributes.h"

#include "js/GCVector.h"
#include "vm/JSObject.h"

// Code used by Array.prototype.sort and %TypedArray%.prototype.sort to sort
// objects based on a user-defined comparator function.

namespace js {

// Note: we use uint32_t because the JIT code uses branch32.
enum class ArraySortResult : uint32_t {
  Failure,
  Done,
  CallJS,
  CallJSSameRealmNoRectifier
};

enum class ArraySortKind {
  Array,
  TypedArray,
};

// We use a JIT trampoline to optimize sorting with a comparator function. The
// trampoline frame has an ArraySortData instance that contains all state used
// by the sorting algorithm. The sorting algorithm is implemented as a C++
// "generator" that can yield to the trampoline to perform a fast JIT => JIT
// call to the comparator function. When the comparator function returns, the
// trampoline calls back into C++ to resume the sorting algorithm.
//
// ArraySortData stores the JS Values in a js::Vector. To ensure we don't leak
// its memory, we have debug assertions to check that for each C++ constructor
// call we call |freeMallocData| exactly once. C++ code calls |freeMallocData|
// when it's done sorting and the JIT exception handler calls it when unwinding
// the trampoline frame.
class ArraySortData {
 public:
  enum class ComparatorKind : uint8_t {
    Unoptimized,
    JS,
    JSSameRealmNoRectifier,
  };

  // Insertion sort is used if the length is <= InsertionSortMaxLength.
  static constexpr size_t InsertionSortMaxLength = 8;

  static constexpr size_t ComparatorActualArgs = 2;

  using ValueVector = GCVector<Value, 8, SystemAllocPolicy>;

 protected:  // Silence Clang warning about unused private fields.
  // Data for the comparator call. These fields must match the JitFrameLayout
  // to let us perform efficient calls to the comparator from JIT code.
  // This is asserted in the JIT trampoline code.
  // callArgs[0] is also used to store the return value of the sort function and
  // the comparator.
  uintptr_t descriptor_;
  JSObject* comparator_ = nullptr;
  Value thisv;
  Value callArgs[ComparatorActualArgs];

 private:
  ValueVector vec;
  Value item;
  JSContext* cx_;
  JSObject* obj_ = nullptr;

  Value* list;
  Value* out;

  // The value of the .length property.
  uint32_t length;

  // The number of items to sort. Can be less than |length| if the object has
  // holes.
  uint32_t denseLen;

  uint32_t windowSize;
  uint32_t start;
  uint32_t mid;
  uint32_t end;
  uint32_t i, j, k;

  // The state value determines where we resume in sortWithComparator.
  enum class State : uint8_t {
    Initial,
    InsertionSortCall1,
    InsertionSortCall2,
    MergeSortCall1,
    MergeSortCall2
  };
  State state = State::Initial;
  ComparatorKind comparatorKind_;

  // Optional padding to ensure proper alignment of the comparator JIT frame.
#if !defined(JS_64BIT) && !defined(DEBUG)
 protected:  // Silence Clang warning about unused private field.
  size_t padding;
#endif

 private:
  // Merge sort requires extra scratch space in the vector. Insertion sort
  // should be used for short arrays that fit in the vector's inline storage, to
  // avoid extra malloc calls.
  static_assert(decltype(vec)::InlineLength <= InsertionSortMaxLength);

  template <ArraySortKind Kind>
  static MOZ_ALWAYS_INLINE ArraySortResult
  sortWithComparatorShared(ArraySortData* d);

 public:
  explicit inline ArraySortData(JSContext* cx);

  void MOZ_ALWAYS_INLINE init(JSObject* obj, JSObject* comparator,
                              ValueVector&& vec, uint32_t length,
                              uint32_t denseLen);

  JSContext* cx() const { return cx_; }

  JSObject* comparator() const {
    MOZ_ASSERT(comparator_);
    return comparator_;
  }

  Value returnValue() const { return callArgs[0]; }
  void setReturnValue(JSObject* obj) { callArgs[0].setObject(*obj); }

  Value comparatorArg(size_t index) {
    MOZ_ASSERT(index < ComparatorActualArgs);
    return callArgs[index];
  }
  Value comparatorThisValue() const { return thisv; }
  Value comparatorReturnValue() const { return callArgs[0]; }
  void setComparatorArgs(const Value& x, const Value& y) {
    callArgs[0] = x;
    callArgs[1] = y;
  }
  void setComparatorReturnValue(const Value& v) { callArgs[0] = v; }

  ComparatorKind comparatorKind() const { return comparatorKind_; }

  static ArraySortResult sortArrayWithComparator(ArraySortData* d);
  static ArraySortResult sortTypedArrayWithComparator(ArraySortData* d);

  inline void freeMallocData();
  void trace(JSTracer* trc);

  static constexpr int32_t offsetOfDescriptor() {
    return offsetof(ArraySortData, descriptor_);
  }
  static constexpr int32_t offsetOfComparator() {
    return offsetof(ArraySortData, comparator_);
  }
  static constexpr int32_t offsetOfComparatorReturnValue() {
    return offsetof(ArraySortData, callArgs[0]);
  }
  static constexpr int32_t offsetOfComparatorThis() {
    return offsetof(ArraySortData, thisv);
  }
  static constexpr int32_t offsetOfComparatorArgs() {
    return offsetof(ArraySortData, callArgs);
  }
};

ArraySortResult CallComparatorSlow(ArraySortData* d, const Value& x,
                                   const Value& y);

}  // namespace js

#endif /* builtin_Sorting_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayObject_h
#define vm_ArrayObject_h

#include "vm/NativeObject.h"

namespace js {

class AutoSetNewObjectMetadata;

class ArrayObject : public NativeObject {
 public:
  // Array(x) eagerly allocates dense elements if x <= this value. Without
  // the subtraction the max would roll over to the next power-of-two (4096)
  // due to the way that growElements() and goodAllocated() work.
  static const uint32_t EagerAllocationMaxLength =
      2048 - ObjectElements::VALUES_PER_HEADER;

  static const JSClass class_;

  bool lengthIsWritable() const {
    return !getElementsHeader()->hasNonwritableArrayLength();
  }

  uint32_t length() const { return getElementsHeader()->length; }

  void setNonWritableLength(JSContext* cx) {
    shrinkCapacityToInitializedLength(cx);
    getElementsHeader()->setNonwritableArrayLength();
  }

  void setLength(uint32_t length) {
    MOZ_ASSERT(lengthIsWritable());
    MOZ_ASSERT_IF(length != getElementsHeader()->length,
                  !denseElementsAreFrozen());
    getElementsHeader()->length = length;
  }

  // Try to add a new dense element to this array. The array must be extensible.
  //
  // Returns DenseElementResult::Incomplete if `index >= length`, if the array
  // has sparse elements, if we're adding a sparse element, or if the array
  // already contains a dense element at this index.
  inline DenseElementResult addDenseElementNoLengthChange(JSContext* cx,
                                                          uint32_t index,
                                                          const Value& val);

  // Make an array object with the specified initial state.
  static MOZ_ALWAYS_INLINE ArrayObject* create(
      JSContext* cx, gc::AllocKind kind, gc::Heap heap,
      Handle<SharedShape*> shape, uint32_t length, uint32_t slotSpan,
      AutoSetNewObjectMetadata& metadata, gc::AllocSite* site = nullptr);
};

}  // namespace js

#endif  // vm_ArrayObject_h

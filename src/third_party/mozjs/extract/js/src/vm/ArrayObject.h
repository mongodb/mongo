/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayObject_h
#define vm_ArrayObject_h

#include "vm/JSContext.h"
#include "vm/NativeObject.h"

namespace js {

class AutoSetNewObjectMetadata;

class ArrayObject : public NativeObject {
 public:
  // Array(x) eagerly allocates dense elements if x <= this value.
  // This number was chosen so that the elements, the elements header,
  // and the MediumBuffer header all fit within MaxMediumAllocSize.
  static const uint32_t EagerAllocationMaxLength =
      (1 << 16) - ObjectElements::VALUES_PER_HEADER - 1;

  static const JSClass class_;

  bool lengthIsWritable() const {
    return !getElementsHeader()->hasNonwritableArrayLength();
  }

  uint32_t length() const { return getElementsHeader()->length; }

  void setNonWritableLength(JSContext* cx) {
    shrinkCapacityToInitializedLength(cx);
    assertInt32LengthFuse(cx);
    getElementsHeader()->setNonwritableArrayLength();
  }

  void setLengthToInitializedLength() {
    MOZ_ASSERT(lengthIsWritable());
    MOZ_ASSERT_IF(length() != getElementsHeader()->length,
                  !denseElementsAreFrozen());
    getElementsHeader()->length = getDenseInitializedLength();
    static_assert(MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX,
                  "No need to check HasSeenArrayExceedsInt32LengthFuse");
  }

  void setLength(JSContext* cx, uint32_t length) {
    MOZ_ASSERT(lengthIsWritable());
    MOZ_ASSERT_IF(length != getElementsHeader()->length,
                  !denseElementsAreFrozen());
    assertInt32LengthFuse(cx);
    NativeObject::elementsSizeMustNotOverflow();
    if (MOZ_UNLIKELY(length > INT32_MAX)) {
      cx->runtime()->hasSeenArrayExceedsInt32LengthFuse.ref().popFuse(cx);
    }
    getElementsHeader()->length = length;
  }

  void assertInt32LengthFuse(JSContext* cx) {
    MOZ_ASSERT_IF(
        length() > INT32_MAX,
        !cx->runtime()->hasSeenArrayExceedsInt32LengthFuse.ref().intact());
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

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TemplateObject_inl_h
#define jit_TemplateObject_inl_h

#include "jit/TemplateObject.h"

#include "vm/EnvironmentObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/RegExpObject.h"

namespace js {
namespace jit {

inline gc::AllocKind TemplateObject::getAllocKind() const {
  return obj_->asTenured().getAllocKind();
}

inline bool TemplateObject::isNativeObject() const {
  return obj_->is<NativeObject>();
}

inline bool TemplateObject::isArrayObject() const {
  return obj_->is<ArrayObject>();
}

inline bool TemplateObject::isArgumentsObject() const {
  return obj_->is<ArgumentsObject>();
}

inline bool TemplateObject::isTypedArrayObject() const {
  return obj_->is<TypedArrayObject>();
}

inline bool TemplateObject::isRegExpObject() const {
  return obj_->is<RegExpObject>();
}

inline bool TemplateObject::isCallObject() const {
  return obj_->is<CallObject>();
}

inline bool TemplateObject::isBlockLexicalEnvironmentObject() const {
  return obj_->is<BlockLexicalEnvironmentObject>();
}

inline bool TemplateObject::isPlainObject() const {
  return obj_->is<PlainObject>();
}

inline gc::Cell* TemplateObject::shape() const {
  Shape* shape = obj_->shape();
  MOZ_ASSERT(!shape->isDictionary());
  return shape;
}

inline const TemplateNativeObject& TemplateObject::asTemplateNativeObject()
    const {
  MOZ_ASSERT(isNativeObject());
  return *static_cast<const TemplateNativeObject*>(this);
}

inline bool TemplateNativeObject::hasDynamicSlots() const {
  return asNativeObject().hasDynamicSlots();
}

inline uint32_t TemplateNativeObject::numDynamicSlots() const {
  return asNativeObject().numDynamicSlots();
}

inline uint32_t TemplateNativeObject::numUsedFixedSlots() const {
  return asNativeObject().numUsedFixedSlots();
}

inline uint32_t TemplateNativeObject::numFixedSlots() const {
  return asNativeObject().numFixedSlots();
}

inline uint32_t TemplateNativeObject::slotSpan() const {
  return asNativeObject().sharedShape()->slotSpan();
}

inline Value TemplateNativeObject::getSlot(uint32_t i) const {
  return asNativeObject().getSlot(i);
}

inline const Value* TemplateNativeObject::getDenseElements() const {
  return asNativeObject().getDenseElements();
}

#ifdef DEBUG
inline bool TemplateNativeObject::isSharedMemory() const {
  return asNativeObject().isSharedMemory();
}
#endif

inline uint32_t TemplateNativeObject::getDenseCapacity() const {
  return asNativeObject().getDenseCapacity();
}

inline uint32_t TemplateNativeObject::getDenseInitializedLength() const {
  return asNativeObject().getDenseInitializedLength();
}

inline uint32_t TemplateNativeObject::getArrayLength() const {
  return obj_->as<ArrayObject>().length();
}

inline bool TemplateNativeObject::hasDynamicElements() const {
  return asNativeObject().hasDynamicElements();
}

inline gc::Cell* TemplateNativeObject::regExpShared() const {
  RegExpObject* regexp = &obj_->as<RegExpObject>();
  MOZ_ASSERT(regexp->hasShared());
  return regexp->getShared();
}

}  // namespace jit
}  // namespace js

#endif /* jit_TemplateObject_inl_h */

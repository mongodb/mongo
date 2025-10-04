/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TemplateObject_h
#define jit_TemplateObject_h

#include "vm/NativeObject.h"
#include "vm/Shape.h"

namespace js {
namespace jit {

class TemplateNativeObject;

// Wrapper for template objects. This should only expose methods that can be
// safely called off-thread without racing with the main thread.
class TemplateObject {
 protected:
  JSObject* obj_;

 public:
  explicit TemplateObject(JSObject* obj) : obj_(obj) {}

  inline gc::AllocKind getAllocKind() const;

  // The following methods rely on the object's group->clasp. This is safe
  // to read off-thread for template objects.
  inline bool isNativeObject() const;
  inline const TemplateNativeObject& asTemplateNativeObject() const;
  inline bool isArrayObject() const;
  inline bool isArgumentsObject() const;
  inline bool isTypedArrayObject() const;
  inline bool isRegExpObject() const;
  inline bool isCallObject() const;
  inline bool isBlockLexicalEnvironmentObject() const;
  inline bool isPlainObject() const;

  // The shape should not change. This is true for template objects because
  // they're never exposed to arbitrary script.
  inline gc::Cell* shape() const;
};

class TemplateNativeObject : public TemplateObject {
 protected:
  NativeObject& asNativeObject() const { return obj_->as<NativeObject>(); }

 public:
  // Reading slot counts and object slots is safe, as long as we don't touch
  // the BaseShape (it can change when we create a ShapeTable for the shape).
  inline bool hasDynamicSlots() const;
  inline uint32_t numDynamicSlots() const;
  inline uint32_t numUsedFixedSlots() const;
  inline uint32_t numFixedSlots() const;
  inline uint32_t slotSpan() const;
  inline Value getSlot(uint32_t i) const;

  // Reading ObjectElements fields is safe, except for the flags.
  // isSharedMemory is an exception: it's debug-only and not called on arrays.
#ifdef DEBUG
  inline bool isSharedMemory() const;
#endif
  inline uint32_t getDenseCapacity() const;
  inline uint32_t getDenseInitializedLength() const;
  inline uint32_t getArrayLength() const;
  inline bool hasDynamicElements() const;
  inline const Value* getDenseElements() const;

  inline gc::Cell* regExpShared() const;
};

}  // namespace jit
}  // namespace js

#endif /* jit_TemplateObject_h */

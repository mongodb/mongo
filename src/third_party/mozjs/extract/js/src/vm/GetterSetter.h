/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GetterSetter_h
#define vm_GetterSetter_h

#include "gc/Barrier.h"  // js::GCPtr<JSObject*>
#include "gc/Cell.h"     // js::gc::TenuredCellWithGCPointer

#include "js/TypeDecls.h"  // JS::HandleObject
#include "js/UbiNode.h"    // JS::ubi::TracerConcrete

namespace js {

// [SMDOC] Getter/Setter Properties
//
// Getter/setter properties are implemented similar to plain data properties:
// the shape contains the property's key, attributes, and slot number, but the
// getter/setter objects are stored separately as part of the object.
//
// To simplify the NativeObject and Shape code, a single slot is allocated for
// each getter/setter property (again similar to data properties). This slot
// contains a PrivateGCThingValue pointing to a js::GetterSetter instance.
//
// js::GetterSetter
// ================
// js::GetterSetter is an immutable type that stores the getter/setter objects.
// Because accessor properties can be defined with only a getter or only a
// setter, a GetterSetter's objects can be nullptr.
//
// JIT/IC Guards
// =============
// An object's shape implies a certain property is an accessor, but it does not
// imply the identity of the getter/setter objects. This means IC code needs to
// guard on the slot value (the GetterSetter*) when optimizing a call to a
// particular getter/setter function.
//
// See EmitGuardGetterSetterSlot in jit/CacheIR.cpp.
//
// HadGetterSetterChange Optimization
// ==================================
// Some getters and setters defined on the prototype chain are very hot, for
// example the 'length' getter for typed arrays. To avoid the GetterSetter guard
// in the common case, when attaching a stub for a known 'holder' object, we
// use the HadGetterSetterChange object flag.
//
// When this flag is not set, the object is guaranteed to get a different shape
// when an accessor property is either deleted or mutated, because when that
// happens the HadGetterSetterChange will be set which triggers a shape change.
//
// This means CacheIR does not have to guard on the GetterSetter slot for
// accessors on the prototype chain until the first time an accessor property is
// mutated or deleted.
class GetterSetter : public gc::TenuredCellWithGCPointer<JSObject> {
  friend class gc::CellAllocator;

 public:
  // Getter object, stored in the cell header.
  JSObject* getter() const { return headerPtr(); }

  GCPtr<JSObject*> setter_;

#ifndef JS_64BIT
  // Ensure size >= MinCellSize on 32-bit platforms.
  uint64_t padding_ = 0;
#endif

 private:
  GetterSetter(HandleObject getter, HandleObject setter);

 public:
  static GetterSetter* create(JSContext* cx, HandleObject getter,
                              HandleObject setter);

  JSObject* setter() const { return setter_; }

  static const JS::TraceKind TraceKind = JS::TraceKind::GetterSetter;

  void traceChildren(JSTracer* trc);

  void finalize(JS::GCContext* gcx) {
    // Nothing to do.
  }
};

}  // namespace js

// JS::ubi::Nodes can point to GetterSetters; they're js::gc::Cell instances
// with no associated compartment.
namespace JS {
namespace ubi {

template <>
class Concrete<js::GetterSetter> : TracerConcrete<js::GetterSetter> {
 protected:
  explicit Concrete(js::GetterSetter* ptr)
      : TracerConcrete<js::GetterSetter>(ptr) {}

 public:
  static void construct(void* storage, js::GetterSetter* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif  // vm_GetterSetter_h

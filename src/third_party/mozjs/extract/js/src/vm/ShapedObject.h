/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ShapedObject_h
#define vm_ShapedObject_h

#include "vm/JSObject.h"

namespace js {

/*
 * Shaped objects are a variant of JSObject that use a GCPtrShape for their
 * |shapeOrExpando_| field. All objects that point to a js::Shape as their
 * |shapeOrExpando_| field should use this as their subclass.
 *
 * NOTE: shape()->getObjectClass() must equal getClass().
 */
class ShapedObject : public JSObject
{
  protected:
    // ShapedObjects treat the |shapeOrExpando_| field as a GCPtrShape to
    // ensure barriers are called. Use these instead of accessing
    // |shapeOrExpando_| directly.
    MOZ_ALWAYS_INLINE const GCPtrShape& shapeRef() const {
        return *reinterpret_cast<const GCPtrShape*>(&(this->shapeOrExpando_));
    }
    MOZ_ALWAYS_INLINE GCPtrShape& shapeRef() {
        return *reinterpret_cast<GCPtrShape*>(&(this->shapeOrExpando_));
    }

    // Used for GC tracing and Shape::listp
    MOZ_ALWAYS_INLINE GCPtrShape* shapePtr() {
        return reinterpret_cast<GCPtrShape*>(&(this->shapeOrExpando_));
    }

  public:
    // Set the shape of an object. This pointer is valid for native objects and
    // some non-native objects. After creating an object, the objects for which
    // the shape pointer is invalid need to overwrite this pointer before a GC
    // can occur.
    void initShape(Shape* shape) { shapeRef().init(shape); }

    void setShape(Shape* shape) { shapeRef() = shape; }
    Shape* shape() const { return shapeRef(); }

    void traceShape(JSTracer* trc) {
        TraceEdge(trc, shapePtr(), "shape");
    }

    static JSObject* fromShapeFieldPointer(uintptr_t p) {
        return reinterpret_cast<JSObject*>(p - ShapedObject::offsetOfShape());
    }

  private:
    // See JSObject::offsetOfGroup() comment.
    friend class js::jit::MacroAssembler;

    static constexpr size_t offsetOfShape() {
        static_assert(offsetOfShapeOrExpando() == offsetof(shadow::Object, shape),
                      "shadow shape must match actual shape");
        return offsetOfShapeOrExpando();
    }
};

} // namespace js

#endif /* vm_ShapedObject_h */

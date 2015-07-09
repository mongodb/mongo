/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_UnboxedObject_h
#define vm_UnboxedObject_h

#include "jsgc.h"
#include "jsobj.h"

#include "vm/TypeInference.h"

namespace js {

// Memory required for an unboxed value of a given type. Returns zero for types
// which can't be used for unboxed objects.
static inline size_t
UnboxedTypeSize(JSValueType type)
{
    switch (type) {
      case JSVAL_TYPE_BOOLEAN: return 1;
      case JSVAL_TYPE_INT32:   return 4;
      case JSVAL_TYPE_DOUBLE:  return 8;
      case JSVAL_TYPE_STRING:  return sizeof(void*);
      case JSVAL_TYPE_OBJECT:  return sizeof(void*);
      default:                 return 0;
    }
}

static inline bool
UnboxedTypeNeedsPreBarrier(JSValueType type)
{
    return type == JSVAL_TYPE_STRING || type == JSVAL_TYPE_OBJECT;
}

// Class describing the layout of an UnboxedPlainObject.
class UnboxedLayout : public mozilla::LinkedListElement<UnboxedLayout>
{
  public:
    struct Property {
        PropertyName* name;
        uint32_t offset;
        JSValueType type;

        Property()
          : name(nullptr), offset(UINT32_MAX), type(JSVAL_TYPE_MAGIC)
        {}
    };

    typedef Vector<Property, 0, SystemAllocPolicy> PropertyVector;

  private:
    // All properties on objects with this layout, in enumeration order.
    PropertyVector properties_;

    // Byte size of the data for objects with this layout.
    size_t size_;

    // Any 'new' script information associated with this layout.
    TypeNewScript* newScript_;

    // List for use in tracing objects with this layout. This has the same
    // structure as the trace list on a TypeDescr.
    int32_t* traceList_;

    // If objects in this group have ever been converted to native objects,
    // these store the corresponding native group and initial shape for such
    // objects. Type information for this object is reflected in nativeGroup.
    HeapPtrObjectGroup nativeGroup_;
    HeapPtrShape nativeShape_;

  public:
    UnboxedLayout(const PropertyVector& properties, size_t size)
      : size_(size), newScript_(nullptr), traceList_(nullptr),
        nativeGroup_(nullptr), nativeShape_(nullptr)
    {
        properties_.appendAll(properties);
    }

    ~UnboxedLayout() {
        js_delete(newScript_);
        js_free(traceList_);
    }

    const PropertyVector& properties() const {
        return properties_;
    }

    TypeNewScript* newScript() const {
        return newScript_;
    }

    void setNewScript(TypeNewScript* newScript, bool writeBarrier = true);

    const int32_t* traceList() const {
        return traceList_;
    }

    void setTraceList(int32_t* traceList) {
        traceList_ = traceList;
    }

    const Property* lookup(JSAtom* atom) const {
        for (size_t i = 0; i < properties_.length(); i++) {
            if (properties_[i].name == atom)
                return &properties_[i];
        }
        return nullptr;
    }

    const Property* lookup(jsid id) const {
        if (JSID_IS_STRING(id))
            return lookup(JSID_TO_ATOM(id));
        return nullptr;
    }

    size_t size() const {
        return size_;
    }

    ObjectGroup* nativeGroup() const {
        return nativeGroup_;
    }

    Shape* nativeShape() const {
        return nativeShape_;
    }

    inline gc::AllocKind getAllocKind() const;

    void trace(JSTracer* trc);

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf);

    static bool makeNativeGroup(JSContext* cx, ObjectGroup* group);
};

// Class for a plain object using an unboxed representation. The physical
// layout of these objects is identical to that of an InlineTypedObject, though
// these objects use an UnboxedLayout instead of a TypeDescr to keep track of
// how their properties are stored.
class UnboxedPlainObject : public JSObject
{
    // Start of the inline data, which immediately follows the shape and type.
    uint8_t data_[1];

  public:
    static const Class class_;

    static bool obj_lookupProperty(JSContext* cx, HandleObject obj,
                                   HandleId id, MutableHandleObject objp,
                                   MutableHandleShape propp);

    static bool obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                   PropertyOp getter, StrictPropertyOp setter, unsigned attrs);

    static bool obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);

    static bool obj_getProperty(JSContext* cx, HandleObject obj, HandleObject receiver,
                                HandleId id, MutableHandleValue vp);

    static bool obj_setProperty(JSContext* cx, HandleObject obj, HandleObject receiver,
                                HandleId id, MutableHandleValue vp, bool strict);

    static bool obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                             MutableHandle<JSPropertyDescriptor> desc);

    static bool obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id, bool* succeeded);

    static bool obj_enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties);
    static bool obj_watch(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable);

    const UnboxedLayout& layout() const {
        return group()->unboxedLayout();
    }

    uint8_t* data() {
        return &data_[0];
    }

    bool setValue(JSContext* cx, const UnboxedLayout::Property& property, const Value& v);
    Value getValue(const UnboxedLayout::Property& property);

    static bool convertToNative(JSContext* cx, JSObject* obj);
    static UnboxedPlainObject* create(JSContext* cx, HandleObjectGroup group, NewObjectKind newKind);

    static void trace(JSTracer* trc, JSObject* object);

    static size_t offsetOfData() {
        return offsetof(UnboxedPlainObject, data_[0]);
    }
};

// Try to construct an UnboxedLayout for each of the preliminary objects,
// provided they all match the template shape. If successful, converts the
// preliminary objects and their group to the new unboxed representation.
bool
TryConvertToUnboxedLayout(JSContext* cx, Shape* templateShape,
                          ObjectGroup* group, PreliminaryObjectArray* objects);

inline gc::AllocKind
UnboxedLayout::getAllocKind() const
{
    return gc::GetGCObjectKindForBytes(UnboxedPlainObject::offsetOfData() + size());
}

} // namespace js

#endif /* vm_UnboxedObject_h */

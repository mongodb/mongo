/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_UnboxedObject_h
#define vm_UnboxedObject_h

#include "gc/DeletePolicy.h"
#include "gc/Zone.h"
#include "vm/JSObject.h"
#include "vm/Runtime.h"
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

static inline bool
UnboxedTypeNeedsPostBarrier(JSValueType type)
{
    return type == JSVAL_TYPE_STRING || type == JSVAL_TYPE_OBJECT;
}

// Class tracking information specific to unboxed objects.
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
    Zone* zone_;

    // If objects in this group have ever been converted to native objects,
    // these store the corresponding native group and initial shape for such
    // objects. Type information for this object is reflected in nativeGroup.
    GCPtrObjectGroup nativeGroup_;
    GCPtrShape nativeShape_;

    // Any script/pc which the associated group is created for.
    GCPtrScript allocationScript_;
    jsbytecode* allocationPc_;

    // If nativeGroup is set and this object originally had a TypeNewScript or
    // was keyed to an allocation site, this points to the group which replaced
    // this one. This link is only needed to keep the replacement group from
    // being GC'ed. If it were GC'ed and a new one regenerated later, that new
    // group might have a different allocation kind from this group.
    GCPtrObjectGroup replacementGroup_;

    // The following members are only used for unboxed plain objects.

    // All properties on objects with this layout, in enumeration order.
    PropertyVector properties_;

    // Byte size of the data for objects with this layout.
    size_t size_;

    // Any 'new' script information associated with this layout.
    TypeNewScript* newScript_;

    // List for use in tracing objects with this layout. This has the same
    // structure as the trace list on a TypeDescr.
    int32_t* traceList_;

    // If this layout has been used to construct script or JSON constant
    // objects, this code might be filled in to more quickly fill in objects
    // from an array of values.
    GCPtrJitCode constructorCode_;

  public:
    explicit UnboxedLayout(Zone* zone)
      : zone_(zone), nativeGroup_(nullptr), nativeShape_(nullptr),
        allocationScript_(nullptr), allocationPc_(nullptr), replacementGroup_(nullptr),
        size_(0), newScript_(nullptr), traceList_(nullptr), constructorCode_(nullptr)
    {}

    Zone* zone() const { return zone_; }

    bool initProperties(const PropertyVector& properties, size_t size) {
        size_ = size;
        return properties_.appendAll(properties);
    }

    ~UnboxedLayout() {
        if (newScript_)
            newScript_->clear();
        js_delete(newScript_);
        js_free(traceList_);

        nativeGroup_.init(nullptr);
        nativeShape_.init(nullptr);
        replacementGroup_.init(nullptr);
        constructorCode_.init(nullptr);
    }

    void detachFromCompartment();

    const PropertyVector& properties() const {
        return properties_;
    }

    TypeNewScript* newScript() const {
        return newScript_;
    }

    void setNewScript(TypeNewScript* newScript, bool writeBarrier = true);

    JSScript* allocationScript() const {
        return allocationScript_;
    }

    jsbytecode* allocationPc() const {
        return allocationPc_;
    }

    void setAllocationSite(JSScript* script, jsbytecode* pc) {
        allocationScript_ = script;
        allocationPc_ = pc;
    }

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

    jit::JitCode* constructorCode() const {
        return constructorCode_;
    }

    void setConstructorCode(jit::JitCode* code) {
        constructorCode_ = code;
    }

    inline gc::AllocKind getAllocKind() const;

    void trace(JSTracer* trc);

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf);

    static bool makeNativeGroup(JSContext* cx, ObjectGroup* group);
    static bool makeConstructorCode(JSContext* cx, HandleObjectGroup group);
};

class UnboxedObject : public JSObject
{
  protected:
    static JS::Result<UnboxedObject*, JS::OOM&>
    createInternal(JSContext* cx, js::gc::AllocKind kind, js::gc::InitialHeap heap,
                   js::HandleObjectGroup group);
};

// Class for expando objects holding extra properties given to an unboxed plain
// object. These objects behave identically to normal native plain objects, and
// have a separate Class to distinguish them for memory usage reporting.
class UnboxedExpandoObject : public NativeObject
{
  public:
    static const Class class_;
};

// Class for a plain object using an unboxed representation. The physical
// layout of these objects is identical to that of an InlineTypedObject, though
// these objects use an UnboxedLayout instead of a TypeDescr to keep track of
// how their properties are stored.
class UnboxedPlainObject : public UnboxedObject
{
    // The |JSObject::shapeOrExpando_| field can optionally refer to an object
    // which stores extra properties on this object. This is not automatically
    // barriered to avoid problems if the object is converted to a native. See
    // ensureExpando(). This object must be an UnboxedExpandoObject.
    //
    // NOTE: The JIT should not assume that seeing the same expando pointer
    //       means the object is even an UnboxedObject. Always check |group_|.

    // Start of the inline data, which immediately follows the group and extra properties.
    uint8_t data_[1];

  public:
    static const Class class_;

    static bool obj_lookupProperty(JSContext* cx, HandleObject obj,
                                   HandleId id, MutableHandleObject objp,
                                   MutableHandle<PropertyResult> propp);

    static bool obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   Handle<PropertyDescriptor> desc,
                                   ObjectOpResult& result);

    static bool obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);

    static bool obj_getProperty(JSContext* cx, HandleObject obj, HandleValue receiver,
                                HandleId id, MutableHandleValue vp);

    static bool obj_setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                HandleValue receiver, ObjectOpResult& result);

    static bool obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                             MutableHandle<PropertyDescriptor> desc);

    static bool obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   ObjectOpResult& result);

    static bool newEnumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                             bool enumerableOnly);

    inline const UnboxedLayout& layout() const;

    const UnboxedLayout& layoutDontCheckGeneration() const {
        return group()->unboxedLayoutDontCheckGeneration();
    }

    uint8_t* data() {
        return &data_[0];
    }

    UnboxedExpandoObject* maybeExpando() const {
        return static_cast<UnboxedExpandoObject*>(shapeOrExpando_);
    }

    void setExpandoUnsafe(UnboxedExpandoObject* expando) {
        shapeOrExpando_ = expando;
    }

    void initExpando() {
        shapeOrExpando_ = nullptr;
    }

    // For use during GC.
    JSObject** addressOfExpando() {
        return reinterpret_cast<JSObject**>(&shapeOrExpando_);
    }

    bool containsUnboxedOrExpandoProperty(JSContext* cx, jsid id) const;

    static UnboxedExpandoObject* ensureExpando(JSContext* cx, Handle<UnboxedPlainObject*> obj);

    bool setValue(JSContext* cx, const UnboxedLayout::Property& property, const Value& v);
    Value getValue(const UnboxedLayout::Property& property, bool maybeUninitialized = false);

    static NativeObject* convertToNative(JSContext* cx, JSObject* obj);
    static UnboxedPlainObject* create(JSContext* cx, HandleObjectGroup group,
                                      NewObjectKind newKind);
    static JSObject* createWithProperties(JSContext* cx, HandleObjectGroup group,
                                          NewObjectKind newKind, IdValuePair* properties);

    void fillAfterConvert(JSContext* cx,
                          Handle<GCVector<Value>> values, size_t* valueCursor);

    static void trace(JSTracer* trc, JSObject* object);

    static size_t offsetOfExpando() {
        return offsetOfShapeOrExpando();
    }

    static size_t offsetOfData() {
        return offsetof(UnboxedPlainObject, data_[0]);
    }
};

inline bool
IsUnboxedObjectClass(const Class* class_)
{
    return class_ == &UnboxedPlainObject::class_;
}


// Try to construct an UnboxedLayout for each of the preliminary objects,
// provided they all match the template shape. If successful, converts the
// preliminary objects and their group to the new unboxed representation.
bool
TryConvertToUnboxedLayout(JSContext* cx, AutoEnterAnalysis& enter, Shape* templateShape,
                          ObjectGroup* group, PreliminaryObjectArray* objects);

} // namespace js

namespace JS {

template <>
struct DeletePolicy<js::UnboxedLayout> : public js::GCManagedDeletePolicy<js::UnboxedLayout>
{};

} /* namespace JS */

#endif /* vm_UnboxedObject_h */

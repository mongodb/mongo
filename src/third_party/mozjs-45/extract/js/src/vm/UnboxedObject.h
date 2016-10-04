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

static inline bool
UnboxedTypeNeedsPostBarrier(JSValueType type)
{
    return type == JSVAL_TYPE_OBJECT;
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
    // If objects in this group have ever been converted to native objects,
    // these store the corresponding native group and initial shape for such
    // objects. Type information for this object is reflected in nativeGroup.
    HeapPtrObjectGroup nativeGroup_;
    HeapPtrShape nativeShape_;

    // Any script/pc which the associated group is created for.
    HeapPtrScript allocationScript_;
    jsbytecode* allocationPc_;

    // If nativeGroup is set and this object originally had a TypeNewScript or
    // was keyed to an allocation site, this points to the group which replaced
    // this one. This link is only needed to keep the replacement group from
    // being GC'ed. If it were GC'ed and a new one regenerated later, that new
    // group might have a different allocation kind from this group.
    HeapPtrObjectGroup replacementGroup_;

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
    HeapPtrJitCode constructorCode_;

    // The following members are only used for unboxed arrays.

    // The type of array elements.
    JSValueType elementType_;

  public:
    UnboxedLayout()
      : nativeGroup_(nullptr), nativeShape_(nullptr),
        allocationScript_(nullptr), allocationPc_(nullptr), replacementGroup_(nullptr),
        size_(0), newScript_(nullptr), traceList_(nullptr), constructorCode_(nullptr),
        elementType_(JSVAL_TYPE_MAGIC)
    {}

    bool initProperties(const PropertyVector& properties, size_t size) {
        size_ = size;
        return properties_.appendAll(properties);
    }

    void initArray(JSValueType elementType) {
        elementType_ = elementType;
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

    bool isArray() const {
        return elementType_ != JSVAL_TYPE_MAGIC;
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

    JSValueType elementType() const {
        return elementType_;
    }

    inline gc::AllocKind getAllocKind() const;

    void trace(JSTracer* trc);

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf);

    static bool makeNativeGroup(JSContext* cx, ObjectGroup* group);
    static bool makeConstructorCode(JSContext* cx, HandleObjectGroup group);
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
class UnboxedPlainObject : public JSObject
{
    // Optional object which stores extra properties on this object. This is
    // not automatically barriered to avoid problems if the object is converted
    // to a native. See ensureExpando().
    UnboxedExpandoObject* expando_;

    // Start of the inline data, which immediately follows the group and extra properties.
    uint8_t data_[1];

  public:
    static const Class class_;

    static bool obj_lookupProperty(JSContext* cx, HandleObject obj,
                                   HandleId id, MutableHandleObject objp,
                                   MutableHandleShape propp);

    static bool obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   Handle<JSPropertyDescriptor> desc,
                                   ObjectOpResult& result);

    static bool obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);

    static bool obj_getProperty(JSContext* cx, HandleObject obj, HandleValue receiver,
                                HandleId id, MutableHandleValue vp);

    static bool obj_setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                HandleValue receiver, ObjectOpResult& result);

    static bool obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                             MutableHandle<JSPropertyDescriptor> desc);

    static bool obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   ObjectOpResult& result);

    static bool obj_enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                              bool enumerableOnly);
    static bool obj_watch(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable);

    inline const UnboxedLayout& layout() const;

    const UnboxedLayout& layoutDontCheckGeneration() const {
        return group()->unboxedLayoutDontCheckGeneration();
    }

    uint8_t* data() {
        return &data_[0];
    }

    UnboxedExpandoObject* maybeExpando() const {
        return expando_;
    }

    void initExpando() {
        expando_ = nullptr;
    }

    // For use during GC.
    JSObject** addressOfExpando() {
        return reinterpret_cast<JSObject**>(&expando_);
    }

    bool containsUnboxedOrExpandoProperty(ExclusiveContext* cx, jsid id) const;

    static UnboxedExpandoObject* ensureExpando(JSContext* cx, Handle<UnboxedPlainObject*> obj);

    bool setValue(ExclusiveContext* cx, const UnboxedLayout::Property& property, const Value& v);
    Value getValue(const UnboxedLayout::Property& property, bool maybeUninitialized = false);

    static bool convertToNative(JSContext* cx, JSObject* obj);
    static UnboxedPlainObject* create(ExclusiveContext* cx, HandleObjectGroup group,
                                      NewObjectKind newKind);
    static JSObject* createWithProperties(ExclusiveContext* cx, HandleObjectGroup group,
                                          NewObjectKind newKind, IdValuePair* properties);

    void fillAfterConvert(ExclusiveContext* cx,
                          const AutoValueVector& values, size_t* valueCursor);

    static void trace(JSTracer* trc, JSObject* object);

    static size_t offsetOfExpando() {
        return offsetof(UnboxedPlainObject, expando_);
    }

    static size_t offsetOfData() {
        return offsetof(UnboxedPlainObject, data_[0]);
    }
};

// Try to construct an UnboxedLayout for each of the preliminary objects,
// provided they all match the template shape. If successful, converts the
// preliminary objects and their group to the new unboxed representation.
bool
TryConvertToUnboxedLayout(ExclusiveContext* cx, Shape* templateShape,
                          ObjectGroup* group, PreliminaryObjectArray* objects);

inline gc::AllocKind
UnboxedLayout::getAllocKind() const
{
    MOZ_ASSERT(size());
    return gc::GetGCObjectKindForBytes(UnboxedPlainObject::offsetOfData() + size());
}

// Class for an array object using an unboxed representation.
class UnboxedArrayObject : public JSObject
{
    // Elements pointer for the object.
    uint8_t* elements_;

    // The nominal array length. This always fits in an int32_t.
    uint32_t length_;

    // Value indicating the allocated capacity and initialized length of the
    // array. The top CapacityBits bits are an index into CapacityArray, which
    // indicates the elements capacity. The low InitializedLengthBits store the
    // initialized length of the array.
    uint32_t capacityIndexAndInitializedLength_;

    // If the elements are inline, they will point here.
    uint8_t inlineElements_[1];

  public:
    static const uint32_t CapacityBits = 6;
    static const uint32_t CapacityShift = 26;

    static const uint32_t CapacityMask = uint32_t(-1) << CapacityShift;
    static const uint32_t InitializedLengthMask = (1 << CapacityShift) - 1;

    static const uint32_t MaximumCapacity = InitializedLengthMask;
    static const uint32_t MinimumDynamicCapacity = 8;

    static const uint32_t CapacityArray[];

    // Capacity index which indicates the array's length is also its capacity.
    static const uint32_t CapacityMatchesLengthIndex = 0;

  private:
    static inline uint32_t computeCapacity(uint32_t index, uint32_t length) {
        if (index == CapacityMatchesLengthIndex)
            return length;
        return CapacityArray[index];
    }

    static uint32_t chooseCapacityIndex(uint32_t capacity, uint32_t length);
    static uint32_t exactCapacityIndex(uint32_t capacity);

  public:
    static const Class class_;

    static bool obj_lookupProperty(JSContext* cx, HandleObject obj,
                                   HandleId id, MutableHandleObject objp,
                                   MutableHandleShape propp);

    static bool obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   Handle<JSPropertyDescriptor> desc,
                                   ObjectOpResult& result);

    static bool obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);

    static bool obj_getProperty(JSContext* cx, HandleObject obj, HandleValue receiver,
                                HandleId id, MutableHandleValue vp);

    static bool obj_setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                HandleValue receiver, ObjectOpResult& result);

    static bool obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                             MutableHandle<JSPropertyDescriptor> desc);

    static bool obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   ObjectOpResult& result);

    static bool obj_enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                              bool enumerableOnly);
    static bool obj_watch(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable);

    inline const UnboxedLayout& layout() const;

    const UnboxedLayout& layoutDontCheckGeneration() const {
        return group()->unboxedLayoutDontCheckGeneration();
    }

    JSValueType elementType() const {
        return layoutDontCheckGeneration().elementType();
    }

    uint32_t elementSize() const {
        return UnboxedTypeSize(elementType());
    }

    static bool convertToNative(JSContext* cx, JSObject* obj);
    static UnboxedArrayObject* create(ExclusiveContext* cx, HandleObjectGroup group,
                                      uint32_t length, NewObjectKind newKind,
                                      uint32_t maxLength = MaximumCapacity);

    static bool convertToNativeWithGroup(ExclusiveContext* cx, JSObject* obj,
                                         ObjectGroup* group, Shape* shape);
    bool convertInt32ToDouble(ExclusiveContext* cx, ObjectGroup* group);

    void fillAfterConvert(ExclusiveContext* cx,
                          const AutoValueVector& values, size_t* valueCursor);

    static void trace(JSTracer* trc, JSObject* object);
    static void objectMoved(JSObject* obj, const JSObject* old);
    static void finalize(FreeOp* fop, JSObject* obj);

    static size_t objectMovedDuringMinorGC(JSTracer* trc, JSObject* dst, JSObject* src,
                                           gc::AllocKind allocKind);

    uint8_t* elements() {
        return elements_;
    }

    bool hasInlineElements() const {
        return elements_ == &inlineElements_[0];
    }

    uint32_t length() const {
        return length_;
    }

    uint32_t initializedLength() const {
        return capacityIndexAndInitializedLength_ & InitializedLengthMask;
    }

    uint32_t capacityIndex() const {
        return (capacityIndexAndInitializedLength_ & CapacityMask) >> CapacityShift;
    }

    uint32_t capacity() const {
        return computeCapacity(capacityIndex(), length());
    }

    bool containsProperty(ExclusiveContext* cx, jsid id);

    bool setElement(ExclusiveContext* cx, size_t index, const Value& v);
    bool initElement(ExclusiveContext* cx, size_t index, const Value& v);
    void initElementNoTypeChange(size_t index, const Value& v);
    Value getElement(size_t index);

    template <JSValueType Type> inline bool setElementSpecific(ExclusiveContext* cx, size_t index,
                                                               const Value& v);
    template <JSValueType Type> inline void setElementNoTypeChangeSpecific(size_t index, const Value& v);
    template <JSValueType Type> inline bool initElementSpecific(ExclusiveContext* cx, size_t index,
                                                                const Value& v);
    template <JSValueType Type> inline void initElementNoTypeChangeSpecific(size_t index, const Value& v);
    template <JSValueType Type> inline Value getElementSpecific(size_t index);
    template <JSValueType Type> inline void triggerPreBarrier(size_t index);

    bool growElements(ExclusiveContext* cx, size_t cap);
    void shrinkElements(ExclusiveContext* cx, size_t cap);

    static uint32_t offsetOfElements() {
        return offsetof(UnboxedArrayObject, elements_);
    }
    static uint32_t offsetOfLength() {
        return offsetof(UnboxedArrayObject, length_);
    }
    static uint32_t offsetOfCapacityIndexAndInitializedLength() {
        return offsetof(UnboxedArrayObject, capacityIndexAndInitializedLength_);
    }
    static uint32_t offsetOfInlineElements() {
        return offsetof(UnboxedArrayObject, inlineElements_);
    }

    void setLengthInt32(uint32_t length) {
        MOZ_ASSERT(length <= INT32_MAX);
        length_ = length;
    }

    inline void setLength(ExclusiveContext* cx, uint32_t len);
    inline void setInitializedLength(uint32_t initlen);

    inline void setInitializedLengthNoBarrier(uint32_t initlen) {
        MOZ_ASSERT(initlen <= InitializedLengthMask);
        capacityIndexAndInitializedLength_ =
            (capacityIndexAndInitializedLength_ & CapacityMask) | initlen;
    }

  private:
    void setInlineElements() {
        elements_ = &inlineElements_[0];
    }

    void setCapacityIndex(uint32_t index) {
        MOZ_ASSERT(index <= (CapacityMask >> CapacityShift));
        capacityIndexAndInitializedLength_ =
            (index << CapacityShift) | initializedLength();
    }
};

} // namespace js

#endif /* vm_UnboxedObject_h */

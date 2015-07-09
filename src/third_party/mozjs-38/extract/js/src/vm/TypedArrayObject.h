/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_TypedArrayObject_h
#define vm_TypedArrayObject_h

#include "jsobj.h"

#include "gc/Barrier.h"
#include "js/Class.h"
#include "vm/ArrayBufferObject.h"

typedef struct JSProperty JSProperty;

namespace js {

/*
 * TypedArrayObject
 *
 * The non-templated base class for the specific typed implementations.
 * This class holds all the member variables that are used by
 * the subclasses.
 */

/*
 * TypedArrayObject and SharedTypedArrayObject are unrelated types in
 * both C++ and JS, and that is deliberate to avoid one substituting
 * for the other.  However, they share a fixed representation and have
 * some variable attributes, all of which are encapsulated in the
 * TypedArrayLayout class.  The sharing avoids a lot of pointless
 * duplication in the JITs: one code path can be used, with occasional
 * decision points based on the attributes.
 */

class TypedArrayLayout
{
    const bool isShared_;
    const bool isNeuterable_;
    const Class* firstClass_;
    const Class* maxClass_;

  public:
    TypedArrayLayout(bool isShared, bool isNeuterable, const Class* firstClass, const Class* maxClass);

    // Underlying (Shared)ArrayBufferObject.
    static const size_t BUFFER_SLOT = 0;

    // Slot containing length of the view in number of typed elements.
    static const size_t LENGTH_SLOT = 1;

    // Offset of view within underlying (Shared)ArrayBufferObject.
    static const size_t BYTEOFFSET_SLOT = 2;

    static const size_t RESERVED_SLOTS = 3;

    // The raw pointer to the buffer memory, the "private" value.
    //
    // This offset is exposed for performance reasons - so that it
    // need not be looked up on accesses.
    static const size_t DATA_SLOT = 3;

    static int lengthOffset();
    static int dataOffset();

    bool isSharedMemory() const { return isShared_; }
    bool isNeuterable() const { return isNeuterable_; }
    const Class* addressOfFirstClass() const { return firstClass_; }
    const Class* addressOfMaxClass() const { return maxClass_; }

  protected:
    static_assert(js::detail::TypedArrayLengthSlot == LENGTH_SLOT,
                  "bad inlined constant in jsfriendapi.h");
};

class TypedArrayObject : public NativeObject
{
  public:
    typedef TypedArrayObject SomeTypedArray;
    typedef ArrayBufferObject BufferType;

    template<typename T> struct OfType;

    static bool sameBuffer(Handle<TypedArrayObject*> a, Handle<TypedArrayObject*> b) {
        return a->buffer() == b->buffer();
    }

    static const Class classes[Scalar::MaxTypedArrayViewType];
    static const Class protoClasses[Scalar::MaxTypedArrayViewType];
    static const Class sharedTypedArrayPrototypeClass;

    static const Class* classForType(Scalar::Type type) {
        MOZ_ASSERT(type < Scalar::MaxTypedArrayViewType);
        return &classes[type];
    }

    static const Class* protoClassForType(Scalar::Type type) {
        MOZ_ASSERT(type < Scalar::MaxTypedArrayViewType);
        return &protoClasses[type];
    }

    static const size_t FIXED_DATA_START = TypedArrayLayout::DATA_SLOT + 1;

    // For typed arrays which can store their data inline, the array buffer
    // object is created lazily.
    static const uint32_t INLINE_BUFFER_LIMIT =
        (NativeObject::MAX_FIXED_SLOTS - FIXED_DATA_START) * sizeof(Value);

    static gc::AllocKind
    AllocKindForLazyBuffer(size_t nbytes)
    {
        MOZ_ASSERT(nbytes <= INLINE_BUFFER_LIMIT);
        /* For GGC we need at least one slot in which to store a forwarding pointer. */
        size_t dataSlots = Max(size_t(1), AlignBytes(nbytes, sizeof(Value)) / sizeof(Value));
        MOZ_ASSERT(nbytes <= dataSlots * sizeof(Value));
        return gc::GetGCObjectKind(FIXED_DATA_START + dataSlots);
    }

    inline Scalar::Type type() const;
    inline size_t bytesPerElement() const;

    static Value bufferValue(TypedArrayObject* tarr) {
        return tarr->getFixedSlot(TypedArrayLayout::BUFFER_SLOT);
    }
    static Value byteOffsetValue(TypedArrayObject* tarr) {
        return tarr->getFixedSlot(TypedArrayLayout::BYTEOFFSET_SLOT);
    }
    static Value byteLengthValue(TypedArrayObject* tarr) {
        return Int32Value(tarr->getFixedSlot(TypedArrayLayout::LENGTH_SLOT).toInt32() * tarr->bytesPerElement());
    }
    static Value lengthValue(TypedArrayObject* tarr) {
        return tarr->getFixedSlot(TypedArrayLayout::LENGTH_SLOT);
    }

    static bool
    ensureHasBuffer(JSContext* cx, Handle<TypedArrayObject*> tarray);

    bool hasBuffer() const {
        return bufferValue(const_cast<TypedArrayObject*>(this)).isObject();
    }
    ArrayBufferObject* buffer() const {
        JSObject* obj = bufferValue(const_cast<TypedArrayObject*>(this)).toObjectOrNull();
        if (!obj)
            return nullptr;
        return &obj->as<ArrayBufferObject>();
    }
    uint32_t byteOffset() const {
        return byteOffsetValue(const_cast<TypedArrayObject*>(this)).toInt32();
    }
    uint32_t byteLength() const {
        return byteLengthValue(const_cast<TypedArrayObject*>(this)).toInt32();
    }
    uint32_t length() const {
        return lengthValue(const_cast<TypedArrayObject*>(this)).toInt32();
    }

    void* viewData() const {
        // Keep synced with js::Get<Type>ArrayLengthAndData in jsfriendapi.h!
        return static_cast<void*>(getPrivate(TypedArrayLayout::DATA_SLOT));
    }

    Value getElement(uint32_t index);
    static void setElement(TypedArrayObject& obj, uint32_t index, double d);

    void neuter(void* newData);

    /*
     * Byte length above which created typed arrays and data views will have
     * singleton types regardless of the context in which they are created.
     */
    static const uint32_t SINGLETON_BYTE_LENGTH = 1024 * 1024 * 10;

    static bool isOriginalLengthGetter(Native native);

  private:
    static TypedArrayLayout layout_;

  public:
    static const TypedArrayLayout& layout() {
        return layout_;
    }

    static void trace(JSTracer* trc, JSObject* obj);

    /* Initialization bits */

    template<Value ValueGetter(TypedArrayObject* tarr)>
    static bool
    GetterImpl(JSContext* cx, CallArgs args)
    {
        MOZ_ASSERT(is(args.thisv()));
        args.rval().set(ValueGetter(&args.thisv().toObject().as<TypedArrayObject>()));
        return true;
    }

    // ValueGetter is a function that takes an unwrapped typed array object and
    // returns a Value. Given such a function, Getter<> is a native that
    // retrieves a given Value, probably from a slot on the object.
    template<Value ValueGetter(TypedArrayObject* tarr)>
    static bool
    Getter(JSContext* cx, unsigned argc, Value* vp)
    {
        CallArgs args = CallArgsFromVp(argc, vp);
        return CallNonGenericMethod<is, GetterImpl<ValueGetter>>(cx, args);
    }

    static const JSFunctionSpec protoFunctions[];
    static const JSPropertySpec protoAccessors[];
    static const JSFunctionSpec staticFunctions[];

    /* Accessors and functions */

    static bool is(HandleValue v);

    static bool copyWithin(JSContext* cx, unsigned argc, Value* vp);
    static bool set(JSContext* cx, unsigned argc, Value* vp);
    static bool subarray(JSContext* cx, unsigned argc, Value* vp);
};

inline bool
IsTypedArrayClass(const Class* clasp)
{
    return &TypedArrayObject::classes[0] <= clasp &&
           clasp < &TypedArrayObject::classes[Scalar::MaxTypedArrayViewType];
}

bool
IsTypedArrayConstructor(HandleValue v, uint32_t type);

inline Scalar::Type
TypedArrayObject::type() const
{
    MOZ_ASSERT(IsTypedArrayClass(getClass()));
    return static_cast<Scalar::Type>(getClass() - &classes[0]);
}

inline size_t
TypedArrayObject::bytesPerElement() const
{
    return Scalar::byteSize(type());
}

// Return value is whether the string is some integer. If the string is an
// integer which is not representable as a uint64_t, the return value is true
// and the resulting index is UINT64_MAX.
template <typename CharT>
bool
StringIsTypedArrayIndex(const CharT* s, size_t length, uint64_t* indexp);

inline bool
IsTypedArrayIndex(jsid id, uint64_t* indexp)
{
    if (JSID_IS_INT(id)) {
        int32_t i = JSID_TO_INT(id);
        MOZ_ASSERT(i >= 0);
        *indexp = (double)i;
        return true;
    }

    if (MOZ_UNLIKELY(!JSID_IS_STRING(id)))
        return false;

    JS::AutoCheckCannotGC nogc;
    JSAtom* atom = JSID_TO_ATOM(id);
    size_t length = atom->length();

    if (atom->hasLatin1Chars()) {
        const Latin1Char* s = atom->latin1Chars(nogc);
        if (!JS7_ISDEC(*s) && *s != '-')
            return false;
        return StringIsTypedArrayIndex(s, length, indexp);
    }

    const char16_t* s = atom->twoByteChars(nogc);
    if (!JS7_ISDEC(*s) && *s != '-')
        return false;
    return StringIsTypedArrayIndex(s, length, indexp);
}

static inline unsigned
TypedArrayShift(Scalar::Type viewType)
{
    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
        return 0;
      case Scalar::Int16:
      case Scalar::Uint16:
        return 1;
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::Float32:
        return 2;
      case Scalar::Float64:
        return 3;
      case Scalar::Float32x4:
      case Scalar::Int32x4:
        return 4;
      default:;
    }
    MOZ_CRASH("Unexpected array type");
}

static inline unsigned
TypedArrayElemSize(Scalar::Type viewType)
{
    return 1u << TypedArrayShift(viewType);
}

class DataViewObject : public NativeObject
{
  private:
    static const Class protoClass;

    static bool is(HandleValue v) {
        return v.isObject() && v.toObject().hasClass(&class_);
    }

    template <typename NativeType>
    static uint8_t*
    getDataPointer(JSContext* cx, Handle<DataViewObject*> obj, uint32_t offset);

    template<Value ValueGetter(DataViewObject* view)>
    static bool
    getterImpl(JSContext* cx, CallArgs args);

    template<Value ValueGetter(DataViewObject* view)>
    static bool
    getter(JSContext* cx, unsigned argc, Value* vp);

    template<Value ValueGetter(DataViewObject* view)>
    static bool
    defineGetter(JSContext* cx, PropertyName* name, HandleNativeObject proto);

  public:
    static const Class class_;

    static Value byteOffsetValue(DataViewObject* view) {
        Value v = view->getReservedSlot(TypedArrayLayout::BYTEOFFSET_SLOT);
        MOZ_ASSERT(v.toInt32() >= 0);
        return v;
    }

    static Value byteLengthValue(DataViewObject* view) {
        Value v = view->getReservedSlot(TypedArrayLayout::LENGTH_SLOT);
        MOZ_ASSERT(v.toInt32() >= 0);
        return v;
    }

    static Value bufferValue(DataViewObject* view) {
        return view->getReservedSlot(TypedArrayLayout::BUFFER_SLOT);
    }

    uint32_t byteOffset() const {
        return byteOffsetValue(const_cast<DataViewObject*>(this)).toInt32();
    }

    uint32_t byteLength() const {
        return byteLengthValue(const_cast<DataViewObject*>(this)).toInt32();
    }

    ArrayBufferObject& arrayBuffer() const {
        return bufferValue(const_cast<DataViewObject*>(this)).toObject().as<ArrayBufferObject>();
    }

    void* dataPointer() const {
        return getPrivate();
    }

    static bool class_constructor(JSContext* cx, unsigned argc, Value* vp);
    static bool constructWithProto(JSContext* cx, unsigned argc, Value* vp);
    static bool construct(JSContext* cx, JSObject* bufobj, const CallArgs& args,
                          HandleObject proto);

    static inline DataViewObject*
    create(JSContext* cx, uint32_t byteOffset, uint32_t byteLength,
           Handle<ArrayBufferObject*> arrayBuffer, JSObject* proto);

    static bool getInt8Impl(JSContext* cx, CallArgs args);
    static bool fun_getInt8(JSContext* cx, unsigned argc, Value* vp);

    static bool getUint8Impl(JSContext* cx, CallArgs args);
    static bool fun_getUint8(JSContext* cx, unsigned argc, Value* vp);

    static bool getInt16Impl(JSContext* cx, CallArgs args);
    static bool fun_getInt16(JSContext* cx, unsigned argc, Value* vp);

    static bool getUint16Impl(JSContext* cx, CallArgs args);
    static bool fun_getUint16(JSContext* cx, unsigned argc, Value* vp);

    static bool getInt32Impl(JSContext* cx, CallArgs args);
    static bool fun_getInt32(JSContext* cx, unsigned argc, Value* vp);

    static bool getUint32Impl(JSContext* cx, CallArgs args);
    static bool fun_getUint32(JSContext* cx, unsigned argc, Value* vp);

    static bool getFloat32Impl(JSContext* cx, CallArgs args);
    static bool fun_getFloat32(JSContext* cx, unsigned argc, Value* vp);

    static bool getFloat64Impl(JSContext* cx, CallArgs args);
    static bool fun_getFloat64(JSContext* cx, unsigned argc, Value* vp);

    static bool setInt8Impl(JSContext* cx, CallArgs args);
    static bool fun_setInt8(JSContext* cx, unsigned argc, Value* vp);

    static bool setUint8Impl(JSContext* cx, CallArgs args);
    static bool fun_setUint8(JSContext* cx, unsigned argc, Value* vp);

    static bool setInt16Impl(JSContext* cx, CallArgs args);
    static bool fun_setInt16(JSContext* cx, unsigned argc, Value* vp);

    static bool setUint16Impl(JSContext* cx, CallArgs args);
    static bool fun_setUint16(JSContext* cx, unsigned argc, Value* vp);

    static bool setInt32Impl(JSContext* cx, CallArgs args);
    static bool fun_setInt32(JSContext* cx, unsigned argc, Value* vp);

    static bool setUint32Impl(JSContext* cx, CallArgs args);
    static bool fun_setUint32(JSContext* cx, unsigned argc, Value* vp);

    static bool setFloat32Impl(JSContext* cx, CallArgs args);
    static bool fun_setFloat32(JSContext* cx, unsigned argc, Value* vp);

    static bool setFloat64Impl(JSContext* cx, CallArgs args);
    static bool fun_setFloat64(JSContext* cx, unsigned argc, Value* vp);

    static bool initClass(JSContext* cx);
    static void neuter(JSObject* view);
    template<typename NativeType>
    static bool read(JSContext* cx, Handle<DataViewObject*> obj,
                     CallArgs& args, NativeType* val, const char* method);
    template<typename NativeType>
    static bool write(JSContext* cx, Handle<DataViewObject*> obj,
                      CallArgs& args, const char* method);

    void neuter(void* newData);

  private:
    static const JSFunctionSpec jsfuncs[];
};

static inline int32_t
ClampIntForUint8Array(int32_t x)
{
    if (x < 0)
        return 0;
    if (x > 255)
        return 255;
    return x;
}

} // namespace js

template <>
inline bool
JSObject::is<js::TypedArrayObject>() const
{
    return js::IsTypedArrayClass(getClass());
}

#endif /* vm_TypedArrayObject_h */

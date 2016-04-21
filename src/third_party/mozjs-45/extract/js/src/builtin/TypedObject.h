/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_TypedObject_h
#define builtin_TypedObject_h

#include "jsobj.h"
#include "jsweakmap.h"

#include "builtin/TypedObjectConstants.h"
#include "js/Conversions.h"
#include "vm/ArrayBufferObject.h"

/*
 * -------------
 * Typed Objects
 * -------------
 *
 * Typed objects are a special kind of JS object where the data is
 * given well-structured form. To use a typed object, users first
 * create *type objects* (no relation to the type objects used in TI)
 * that define the type layout. For example, a statement like:
 *
 *    var PointType = new StructType({x: uint8, y: uint8});
 *
 * would create a type object PointType that is a struct with
 * two fields, each of uint8 type.
 *
 * This comment typically assumes familiary with the API.  For more
 * info on the API itself, see the Harmony wiki page at
 * http://wiki.ecmascript.org/doku.php?id=harmony:typed_objects or the
 * ES6 spec (not finalized at the time of this writing).
 *
 * - Initialization:
 *
 * Currently, all "globals" related to typed objects are packaged
 * within a single "module" object `TypedObject`. This module has its
 * own js::Class and when that class is initialized, we also create
 * and define all other values (in `js::InitTypedObjectModuleClass()`).
 *
 * - Type objects, meta type objects, and type representations:
 *
 * There are a number of pre-defined type objects, one for each
 * scalar type (`uint8` etc). Each of these has its own class_,
 * defined in `DefineNumericClass()`.
 *
 * There are also meta type objects (`ArrayType`, `StructType`).
 * These constructors are not themselves type objects but rather the
 * means for the *user* to construct new typed objects.
 *
 * Each type object is associated with a *type representation* (see
 * TypeRepresentation.h). Type representations are canonical versions
 * of type objects. We attach them to TI type objects and (eventually)
 * use them for shape guards etc. They are purely internal to the
 * engine and are not exposed to end users (though self-hosted code
 * sometimes accesses them).
 *
 * - Typed objects:
 *
 * A typed object is an instance of a *type object* (note the past participle).
 * Typed objects can be either transparent or opaque, depending on whether
 * their underlying buffer can be accessed. Transparent and opaque typed
 * objects have different classes, and can have different physical layouts.
 * The following layouts are possible:
 *
 * InlineTypedObject: Typed objects whose data immediately follows the object's
 *   header are inline typed objects. The buffer for these objects is created
 *   lazily and stored via the compartment's LazyArrayBufferTable, and points
 *   back into the object's internal data.
 *
 * OutlineTypedObject: Typed objects whose data is owned by another object,
 *   which can be either an array buffer or an inline typed object. Outline
 *   typed objects may be attached or unattached. An unattached typed object
 *   has no data associated with it. When first created, objects are always
 *   attached, but they can become unattached if their buffer is neutered.
 *
 * Note that whether a typed object is opaque is not directly
 * connected to its type. That is, opaque types are *always*
 * represented by opaque typed objects, but you may have opaque typed
 * objects for transparent types too. This can occur for two reasons:
 * (1) a transparent type may be embedded within an opaque type or (2)
 * users can choose to convert transparent typed objects into opaque
 * ones to avoid giving access to the buffer itself.
 *
 * Typed objects (no matter their class) are non-native objects that
 * fully override the property accessors etc. The overridden accessor
 * methods are the same in each and are defined in methods of
 * TypedObject.
 */

namespace js {

/*
 * Helper method for converting a double into other scalar
 * types in the same way that JavaScript would. In particular,
 * simple C casting from double to int32_t gets things wrong
 * for values like 0xF0000000.
 */
template <typename T>
static T ConvertScalar(double d)
{
    if (TypeIsFloatingPoint<T>()) {
        return T(d);
    } else if (TypeIsUnsigned<T>()) {
        uint32_t n = JS::ToUint32(d);
        return T(n);
    } else {
        int32_t n = JS::ToInt32(d);
        return T(n);
    }
}

namespace type {

enum Kind {
    Scalar = JS_TYPEREPR_SCALAR_KIND,
    Reference = JS_TYPEREPR_REFERENCE_KIND,
    Simd = JS_TYPEREPR_SIMD_KIND,
    Struct = JS_TYPEREPR_STRUCT_KIND,
    Array = JS_TYPEREPR_ARRAY_KIND
};

} // namespace type

///////////////////////////////////////////////////////////////////////////
// Typed Prototypes

class SimpleTypeDescr;
class ComplexTypeDescr;
class SimdTypeDescr;
class StructTypeDescr;
class TypedProto;

/*
 * The prototype for a typed object.
 */
class TypedProto : public NativeObject
{
  public:
    static const Class class_;
};

class TypeDescr : public NativeObject
{
  public:
    TypedProto& typedProto() const {
        return getReservedSlot(JS_DESCR_SLOT_TYPROTO).toObject().as<TypedProto>();
    }

    JSAtom& stringRepr() const {
        return getReservedSlot(JS_DESCR_SLOT_STRING_REPR).toString()->asAtom();
    }

    type::Kind kind() const {
        return (type::Kind) getReservedSlot(JS_DESCR_SLOT_KIND).toInt32();
    }

    bool opaque() const {
        return getReservedSlot(JS_DESCR_SLOT_OPAQUE).toBoolean();
    }

    bool transparent() const {
        return !opaque();
    }

    int32_t alignment() const {
        return getReservedSlot(JS_DESCR_SLOT_ALIGNMENT).toInt32();
    }

    int32_t size() const {
        return getReservedSlot(JS_DESCR_SLOT_SIZE).toInt32();
    }

    // Whether id is an 'own' property of objects with this descriptor.
    bool hasProperty(const JSAtomState& names, jsid id);

    // Type descriptors may contain a list of their references for use during
    // scanning. Marking code is optimized to use this list to mark inline
    // typed objects, rather than the slower trace hook. This list is only
    // specified when (a) the descriptor is short enough that it can fit in an
    // InlineTypedObject, and (b) the descriptor contains at least one
    // reference. Otherwise its value is undefined.
    //
    // The list is three consecutive arrays of int32_t offsets, with each array
    // terminated by -1. The arrays store offsets of string, object, and value
    // references in the descriptor, in that order.
    bool hasTraceList() const {
        return !getFixedSlot(JS_DESCR_SLOT_TRACE_LIST).isUndefined();
    }
    const int32_t* traceList() const {
        MOZ_ASSERT(hasTraceList());
        return reinterpret_cast<int32_t*>(getFixedSlot(JS_DESCR_SLOT_TRACE_LIST).toPrivate());
    }

    void initInstances(const JSRuntime* rt, uint8_t* mem, size_t length);
    void traceInstances(JSTracer* trace, uint8_t* mem, size_t length);

    static void finalize(FreeOp* fop, JSObject* obj);
};

typedef Handle<TypeDescr*> HandleTypeDescr;

class SimpleTypeDescr : public TypeDescr
{
};

// Type for scalar type constructors like `uint8`. All such type
// constructors share a common js::Class and JSFunctionSpec. Scalar
// types are non-opaque (their storage is visible unless combined with
// an opaque reference type.)
class ScalarTypeDescr : public SimpleTypeDescr
{
  public:
    typedef Scalar::Type Type;

    static const type::Kind Kind = type::Scalar;
    static const bool Opaque = false;
    static int32_t size(Type t);
    static int32_t alignment(Type t);
    static const char* typeName(Type type);

    static const Class class_;
    static const JSFunctionSpec typeObjectMethods[];

    Type type() const {
        // Make sure the values baked into TypedObjectConstants.h line up with
        // the Scalar::Type enum. We don't define Scalar::Type directly in
        // terms of these constants to avoid making TypedObjectConstants.h a
        // public header file.
        static_assert(Scalar::Int8 == JS_SCALARTYPEREPR_INT8,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Uint8 == JS_SCALARTYPEREPR_UINT8,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Int16 == JS_SCALARTYPEREPR_INT16,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Uint16 == JS_SCALARTYPEREPR_UINT16,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Int32 == JS_SCALARTYPEREPR_INT32,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Uint32 == JS_SCALARTYPEREPR_UINT32,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Float32 == JS_SCALARTYPEREPR_FLOAT32,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Float64 == JS_SCALARTYPEREPR_FLOAT64,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Uint8Clamped == JS_SCALARTYPEREPR_UINT8_CLAMPED,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Float32x4 == JS_SCALARTYPEREPR_FLOAT32X4,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");
        static_assert(Scalar::Int32x4 == JS_SCALARTYPEREPR_INT32X4,
                      "TypedObjectConstants.h must be consistent with Scalar::Type");

        return Type(getReservedSlot(JS_DESCR_SLOT_TYPE).toInt32());
    }

    static bool call(JSContext* cx, unsigned argc, Value* vp);
};

// Enumerates the cases of ScalarTypeDescr::Type which have
// unique C representation. In particular, omits Uint8Clamped since it
// is just a Uint8.
#define JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE(macro_)       \
    macro_(Scalar::Int8,    int8_t,   int8)                     \
    macro_(Scalar::Uint8,   uint8_t,  uint8)                    \
    macro_(Scalar::Int16,   int16_t,  int16)                    \
    macro_(Scalar::Uint16,  uint16_t, uint16)                   \
    macro_(Scalar::Int32,   int32_t,  int32)                    \
    macro_(Scalar::Uint32,  uint32_t, uint32)                   \
    macro_(Scalar::Float32, float,    float32)                  \
    macro_(Scalar::Float64, double,   float64)

// Must be in same order as the enum ScalarTypeDescr::Type:
#define JS_FOR_EACH_SCALAR_TYPE_REPR(macro_)                    \
    JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE(macro_)           \
    macro_(Scalar::Uint8Clamped, uint8_t, uint8Clamped)

// Type for reference type constructors like `Any`, `String`, and
// `Object`. All such type constructors share a common js::Class and
// JSFunctionSpec. All these types are opaque.
class ReferenceTypeDescr : public SimpleTypeDescr
{
  public:
    // Must match order of JS_FOR_EACH_REFERENCE_TYPE_REPR below
    enum Type {
        TYPE_ANY = JS_REFERENCETYPEREPR_ANY,
        TYPE_OBJECT = JS_REFERENCETYPEREPR_OBJECT,
        TYPE_STRING = JS_REFERENCETYPEREPR_STRING,
    };
    static const int32_t TYPE_MAX = TYPE_STRING + 1;
    static const char* typeName(Type type);

    static const type::Kind Kind = type::Reference;
    static const bool Opaque = true;
    static const Class class_;
    static int32_t size(Type t);
    static int32_t alignment(Type t);
    static const JSFunctionSpec typeObjectMethods[];

    ReferenceTypeDescr::Type type() const {
        return (ReferenceTypeDescr::Type) getReservedSlot(JS_DESCR_SLOT_TYPE).toInt32();
    }

    const char* typeName() const {
        return typeName(type());
    }

    static bool call(JSContext* cx, unsigned argc, Value* vp);
};

#define JS_FOR_EACH_REFERENCE_TYPE_REPR(macro_)                    \
    macro_(ReferenceTypeDescr::TYPE_ANY,    HeapValue, Any)        \
    macro_(ReferenceTypeDescr::TYPE_OBJECT, HeapPtrObject, Object) \
    macro_(ReferenceTypeDescr::TYPE_STRING, HeapPtrString, string)

// Type descriptors whose instances are objects and hence which have
// an associated `prototype` property.
class ComplexTypeDescr : public TypeDescr
{
  public:
    // Returns the prototype that instances of this type descriptor
    // will have.
    TypedProto& instancePrototype() const {
        return getReservedSlot(JS_DESCR_SLOT_TYPROTO).toObject().as<TypedProto>();
    }
};

/*
 * Type descriptors `int8x16`, `int16x8`, `int32x4`, `float32x4` and `float64x2`
 */
class SimdTypeDescr : public ComplexTypeDescr
{
  public:
    enum Type {
        Int8x16   = JS_SIMDTYPEREPR_INT8X16,
        Int16x8   = JS_SIMDTYPEREPR_INT16X8,
        Int32x4   = JS_SIMDTYPEREPR_INT32X4,
        Float32x4 = JS_SIMDTYPEREPR_FLOAT32X4,
        Float64x2 = JS_SIMDTYPEREPR_FLOAT64X2,
        LAST_TYPE = Float64x2
    };

    static const type::Kind Kind = type::Simd;
    static const bool Opaque = false;
    static const Class class_;
    static int32_t size(Type t);
    static int32_t alignment(Type t);

    SimdTypeDescr::Type type() const {
        uint32_t t = uint32_t(getReservedSlot(JS_DESCR_SLOT_TYPE).toInt32());
        MOZ_ASSERT(t <= LAST_TYPE);
        return SimdTypeDescr::Type(t);
    }

    static bool call(JSContext* cx, unsigned argc, Value* vp);
    static bool is(const Value& v);
};

bool IsTypedObjectClass(const Class* clasp); // Defined below
bool IsTypedObjectArray(JSObject& obj);

bool CreateUserSizeAndAlignmentProperties(JSContext* cx, HandleTypeDescr obj);

class ArrayTypeDescr;

/*
 * Properties and methods of the `ArrayType` meta type object. There
 * is no `class_` field because `ArrayType` is just a native
 * constructor function.
 */
class ArrayMetaTypeDescr : public NativeObject
{
  private:
    // Helper for creating a new ArrayType object.
    //
    // - `arrayTypePrototype` - prototype for the new object to be created
    // - `elementType` - type object for the elements in the array
    // - `stringRepr` - canonical string representation for the array
    // - `size` - length of the array
    static ArrayTypeDescr* create(JSContext* cx,
                                  HandleObject arrayTypePrototype,
                                  HandleTypeDescr elementType,
                                  HandleAtom stringRepr,
                                  int32_t size,
                                  int32_t length);

  public:
    // Properties and methods to be installed on ArrayType.prototype,
    // and hence inherited by all array type objects:
    static const JSPropertySpec typeObjectProperties[];
    static const JSFunctionSpec typeObjectMethods[];

    // Properties and methods to be installed on ArrayType.prototype.prototype,
    // and hence inherited by all array *typed* objects:
    static const JSPropertySpec typedObjectProperties[];
    static const JSFunctionSpec typedObjectMethods[];

    // This is the function that gets called when the user
    // does `new ArrayType(elem)`. It produces an array type object.
    static bool construct(JSContext* cx, unsigned argc, Value* vp);
};

/*
 * Type descriptor created by `new ArrayType(type, n)`
 */
class ArrayTypeDescr : public ComplexTypeDescr
{
  public:
    static const Class class_;
    static const type::Kind Kind = type::Array;

    TypeDescr& elementType() const {
        return getReservedSlot(JS_DESCR_SLOT_ARRAY_ELEM_TYPE).toObject().as<TypeDescr>();
    }

    int32_t length() const {
        return getReservedSlot(JS_DESCR_SLOT_ARRAY_LENGTH).toInt32();
    }

    static int32_t offsetOfLength() {
        return getFixedSlotOffset(JS_DESCR_SLOT_ARRAY_LENGTH);
    }
};

/*
 * Properties and methods of the `StructType` meta type object. There
 * is no `class_` field because `StructType` is just a native
 * constructor function.
 */
class StructMetaTypeDescr : public NativeObject
{
  private:
    static JSObject* create(JSContext* cx, HandleObject structTypeGlobal,
                            HandleObject fields);

  public:
    // Properties and methods to be installed on StructType.prototype,
    // and hence inherited by all struct type objects:
    static const JSPropertySpec typeObjectProperties[];
    static const JSFunctionSpec typeObjectMethods[];

    // Properties and methods to be installed on StructType.prototype.prototype,
    // and hence inherited by all struct *typed* objects:
    static const JSPropertySpec typedObjectProperties[];
    static const JSFunctionSpec typedObjectMethods[];

    // This is the function that gets called when the user
    // does `new StructType(...)`. It produces a struct type object.
    static bool construct(JSContext* cx, unsigned argc, Value* vp);
};

class StructTypeDescr : public ComplexTypeDescr
{
  public:
    static const Class class_;

    // Returns the number of fields defined in this struct.
    size_t fieldCount() const;
    size_t maybeForwardedFieldCount() const;

    // Set `*out` to the index of the field named `id` and returns true,
    // or return false if no such field exists.
    bool fieldIndex(jsid id, size_t* out) const;

    // Return the name of the field at index `index`.
    JSAtom& fieldName(size_t index) const;

    // Return the type descr of the field at index `index`.
    TypeDescr& fieldDescr(size_t index) const;
    TypeDescr& maybeForwardedFieldDescr(size_t index) const;

    // Return the offset of the field at index `index`.
    size_t fieldOffset(size_t index) const;
    size_t maybeForwardedFieldOffset(size_t index) const;

  private:
    ArrayObject& fieldInfoObject(size_t slot) const {
        return getReservedSlot(slot).toObject().as<ArrayObject>();
    }
};

typedef Handle<StructTypeDescr*> HandleStructTypeDescr;

/*
 * This object exists in order to encapsulate the typed object types
 * somewhat, rather than sticking them all into the global object.
 * Eventually it will go away and become a module.
 */
class TypedObjectModuleObject : public NativeObject {
  public:
    enum Slot {
        ArrayTypePrototype,
        StructTypePrototype,
        SlotCount
    };

    static const Class class_;
};

/* Base type for transparent and opaque typed objects. */
class TypedObject : public JSObject
{
    static const bool IsTypedObjectClass = true;

    static bool obj_getArrayElement(JSContext* cx,
                                    Handle<TypedObject*> typedObj,
                                    Handle<TypeDescr*> typeDescr,
                                    uint32_t index,
                                    MutableHandleValue vp);

  protected:
    HeapPtrShape shape_;

    static bool obj_lookupProperty(JSContext* cx, HandleObject obj,
                                   HandleId id, MutableHandleObject objp,
                                   MutableHandleShape propp);

    static bool obj_defineProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   Handle<JSPropertyDescriptor> desc,
                                   ObjectOpResult& result);

    static bool obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);

    static bool obj_getProperty(JSContext* cx, HandleObject obj, HandleValue receiver,
                                HandleId id, MutableHandleValue vp);

    static bool obj_getElement(JSContext* cx, HandleObject obj, HandleValue receiver,
                               uint32_t index, MutableHandleValue vp);

    static bool obj_setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                                HandleValue receiver, ObjectOpResult& result);

    static bool obj_getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                             MutableHandle<JSPropertyDescriptor> desc);

    static bool obj_deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                                   ObjectOpResult& result);

    static bool obj_enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                              bool enumerableOnly);

  public:
    TypedProto& typedProto() const {
        return getProto()->as<TypedProto>();
    }

    TypeDescr& typeDescr() const {
        return group()->typeDescr();
    }

    int32_t offset() const;
    int32_t length() const;
    uint8_t* typedMem() const;
    uint8_t* typedMemBase() const;
    bool isAttached() const;
    bool maybeForwardedIsAttached() const;

    int32_t size() const {
        return typeDescr().size();
    }

    uint8_t* typedMem(size_t offset) const {
        // It seems a bit surprising that one might request an offset
        // == size(), but it can happen when taking the "address of" a
        // 0-sized value. (In other words, we maintain the invariant
        // that `offset + size <= size()` -- this is always checked in
        // the caller's side.)
        MOZ_ASSERT(offset <= (size_t) size());
        return typedMem() + offset;
    }

    inline bool opaque() const;

    // Creates a new typed object whose memory is freshly allocated and
    // initialized with zeroes (or, in the case of references, an appropriate
    // default value).
    static TypedObject* createZeroed(JSContext* cx, HandleTypeDescr typeObj, int32_t length,
                                     gc::InitialHeap heap = gc::DefaultHeap);

    // User-accessible constructor (`new TypeDescriptor(...)`). Note that the
    // callee here is the type descriptor.
    static bool construct(JSContext* cx, unsigned argc, Value* vp);

    /* Accessors for self hosted code. */
    static bool GetBuffer(JSContext* cx, unsigned argc, Value* vp);
    static bool GetByteOffset(JSContext* cx, unsigned argc, Value* vp);

    Shape** addressOfShapeFromGC() { return shape_.unsafeUnbarrieredForTracing(); }
};

typedef Handle<TypedObject*> HandleTypedObject;

class OutlineTypedObject : public TypedObject
{
    // The object which owns the data this object points to. Because this
    // pointer is managed in tandem with |data|, this is not a HeapPtr and
    // barriers are managed directly.
    JSObject* owner_;

    // Data pointer to some offset in the owner's contents.
    uint8_t* data_;

    void setOwnerAndData(JSObject* owner, uint8_t* data);

  public:
    // JIT accessors.
    static size_t offsetOfData() { return offsetof(OutlineTypedObject, data_); }
    static size_t offsetOfOwner() { return offsetof(OutlineTypedObject, owner_); }

    JSObject& owner() const {
        MOZ_ASSERT(owner_);
        return *owner_;
    }

    JSObject* maybeOwner() const {
        return owner_;
    }

    uint8_t* outOfLineTypedMem() const {
        return data_;
    }

    void setData(uint8_t* data) {
        data_ = data;
    }

    // Helper for createUnattached()
    static OutlineTypedObject* createUnattachedWithClass(JSContext* cx,
                                                         const Class* clasp,
                                                         HandleTypeDescr type,
                                                         int32_t length,
                                                         gc::InitialHeap heap = gc::DefaultHeap);

    // Creates an unattached typed object or handle (depending on the
    // type parameter T). Note that it is only legal for unattached
    // handles to escape to the end user; for non-handles, the caller
    // should always invoke one of the `attach()` methods below.
    //
    // Arguments:
    // - type: type object for resulting object
    // - length: 0 unless this is an array, otherwise the length
    static OutlineTypedObject* createUnattached(JSContext* cx, HandleTypeDescr type,
                                                int32_t length, gc::InitialHeap heap = gc::DefaultHeap);

    // Creates a typedObj that aliases the memory pointed at by `owner`
    // at the given offset. The typedObj will be a handle iff type is a
    // handle and a typed object otherwise.
    static OutlineTypedObject* createDerived(JSContext* cx,
                                             HandleTypeDescr type,
                                             Handle<TypedObject*> typedContents,
                                             int32_t offset);

    // Use this method when `buffer` is the owner of the memory.
    void attach(JSContext* cx, ArrayBufferObject& buffer, int32_t offset);

    // Otherwise, use this to attach to memory referenced by another typedObj.
    void attach(JSContext* cx, TypedObject& typedObj, int32_t offset);

    // Invoked when array buffer is transferred elsewhere
    void neuter(void* newData);

    static void obj_trace(JSTracer* trace, JSObject* object);
};

// Class for a transparent typed object whose owner is an array buffer.
class OutlineTransparentTypedObject : public OutlineTypedObject
{
  public:
    static const Class class_;

    ArrayBufferObject* getOrCreateBuffer(JSContext* cx);
};

// Class for an opaque typed object whose owner may be either an array buffer
// or an opaque inlined typed object.
class OutlineOpaqueTypedObject : public OutlineTypedObject
{
  public:
    static const Class class_;
};

// Class for a typed object whose data is allocated inline.
class InlineTypedObject : public TypedObject
{
    // Start of the inline data, which immediately follows the shape and type.
    uint8_t data_[1];

  public:
    static const size_t MaximumSize = JSObject::MAX_BYTE_SIZE - sizeof(TypedObject);

    static gc::AllocKind allocKindForTypeDescriptor(TypeDescr* descr) {
        size_t nbytes = descr->size();
        MOZ_ASSERT(nbytes <= MaximumSize);

        return gc::GetGCObjectKindForBytes(nbytes + sizeof(TypedObject));
    }

    uint8_t* inlineTypedMem() const {
        return (uint8_t*) &data_;
    }

    static void obj_trace(JSTracer* trace, JSObject* object);
    static void objectMovedDuringMinorGC(JSTracer* trc, JSObject* dst, JSObject* src);

    static size_t offsetOfDataStart() {
        return offsetof(InlineTypedObject, data_);
    }

    static InlineTypedObject* create(JSContext* cx, HandleTypeDescr descr,
                                     gc::InitialHeap heap = gc::DefaultHeap);
    static InlineTypedObject* createCopy(JSContext* cx, Handle<InlineTypedObject*> templateObject,
                                         gc::InitialHeap heap);
};

// Class for a transparent typed object with inline data, which may have a
// lazily allocated array buffer.
class InlineTransparentTypedObject : public InlineTypedObject
{
  public:
    static const Class class_;

    ArrayBufferObject* getOrCreateBuffer(JSContext* cx);
};

// Class for an opaque typed object with inline data and no array buffer.
class InlineOpaqueTypedObject : public InlineTypedObject
{
  public:
    static const Class class_;
};

/*
 * Usage: NewOpaqueTypedObject(typeObj)
 *
 * Constructs a new, unattached instance of `Handle`.
 */
bool NewOpaqueTypedObject(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: NewDerivedTypedObject(typeObj, owner, offset)
 *
 * Constructs a new, unattached instance of `Handle`.
 */
bool NewDerivedTypedObject(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: AttachTypedObject(typedObj, newDatum, newOffset)
 *
 * Moves `typedObj` to point at the memory referenced by `newDatum` with
 * the offset `newOffset`.
 */
bool AttachTypedObject(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: SetTypedObjectOffset(typedObj, offset)
 *
 * Changes the offset for `typedObj` within its buffer to `offset`.
 * `typedObj` must already be attached.
 */
bool SetTypedObjectOffset(JSContext*, unsigned argc, Value* vp);

/*
 * Usage: ObjectIsTypeDescr(obj)
 *
 * True if `obj` is a type object.
 */
bool ObjectIsTypeDescr(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: ObjectIsTypedObject(obj)
 *
 * True if `obj` is a transparent or opaque typed object.
 */
bool ObjectIsTypedObject(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: ObjectIsOpaqueTypedObject(obj)
 *
 * True if `obj` is an opaque typed object.
 */
bool ObjectIsOpaqueTypedObject(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: ObjectIsTransparentTypedObject(obj)
 *
 * True if `obj` is a transparent typed object.
 */
bool ObjectIsTransparentTypedObject(JSContext* cx, unsigned argc, Value* vp);

/* Predicates on type descriptor objects.  In all cases, 'obj' must be a type descriptor. */

bool TypeDescrIsSimpleType(JSContext*, unsigned argc, Value* vp);

bool TypeDescrIsArrayType(JSContext*, unsigned argc, Value* vp);

/*
 * Usage: TypedObjectIsAttached(obj)
 *
 * Given a TypedObject `obj`, returns true if `obj` is
 * "attached" (i.e., its data pointer is nullptr).
 */
bool TypedObjectIsAttached(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: TypedObjectTypeDescr(obj)
 *
 * Given a TypedObject `obj`, returns the object's type descriptor.
 */
bool TypedObjectTypeDescr(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: ClampToUint8(v)
 *
 * Same as the C function ClampDoubleToUint8. `v` must be a number.
 */
bool ClampToUint8(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: GetTypedObjectModule()
 *
 * Returns the global "typed object" module, which provides access
 * to the various builtin type descriptors. These are currently
 * exported as immutable properties so it is safe for self-hosted code
 * to access them; eventually this should be linked into the module
 * system.
 */
bool GetTypedObjectModule(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: GetFloat32x4TypeDescr()
 *
 * Returns the float32x4 type object. SIMD pseudo-module must have
 * been initialized for this to be safe.
 */
bool GetFloat32x4TypeDescr(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: GetFloat64x2TypeDescr()
 *
 * Returns the float64x2 type object. SIMD pseudo-module must have
 * been initialized for this to be safe.
 */
bool GetFloat64x2TypeDescr(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: GetInt8x16TypeDescr()
 *
 * Returns the int8x16 type object. SIMD pseudo-module must have
 * been initialized for this to be safe.
 */
bool GetInt8x16TypeDescr(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: GetInt16x8TypeDescr()
 *
 * Returns the int16x8 type object. SIMD pseudo-module must have
 * been initialized for this to be safe.
 */
bool GetInt16x8TypeDescr(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: GetInt32x4TypeDescr()
 *
 * Returns the int32x4 type object. SIMD pseudo-module must have
 * been initialized for this to be safe.
 */
bool GetInt32x4TypeDescr(JSContext* cx, unsigned argc, Value* vp);

/*
 * Usage: Store_int8(targetDatum, targetOffset, value)
 *        ...
 *        Store_uint8(targetDatum, targetOffset, value)
 *        ...
 *        Store_float32(targetDatum, targetOffset, value)
 *        Store_float64(targetDatum, targetOffset, value)
 *
 * Intrinsic function. Stores `value` into the memory referenced by
 * `targetDatum` at the offset `targetOffset`.
 *
 * Assumes (and asserts) that:
 * - `targetDatum` is attached
 * - `targetOffset` is a valid offset within the bounds of `targetDatum`
 * - `value` is a number
 */
#define JS_STORE_SCALAR_CLASS_DEFN(_constant, T, _name)                       \
class StoreScalar##T {                                                        \
  public:                                                                     \
    static bool Func(JSContext* cx, unsigned argc, Value* vp);        \
    static const JSJitInfo JitInfo;                                           \
};

/*
 * Usage: Store_Any(targetDatum, targetOffset, fieldName, value)
 *        Store_Object(targetDatum, targetOffset, fieldName, value)
 *        Store_string(targetDatum, targetOffset, fieldName, value)
 *
 * Intrinsic function. Stores `value` into the memory referenced by
 * `targetDatum` at the offset `targetOffset`.
 *
 * Assumes (and asserts) that:
 * - `targetDatum` is attached
 * - `targetOffset` is a valid offset within the bounds of `targetDatum`
 * - `value` is an object or null (`Store_Object`) or string (`Store_string`).
 */
#define JS_STORE_REFERENCE_CLASS_DEFN(_constant, T, _name)                    \
class StoreReference##T {                                                     \
  private:                                                                    \
    static bool store(JSContext* cx, T* heap, const Value& v,         \
                      TypedObject* obj, jsid id);                             \
                                                                              \
  public:                                                                     \
    static bool Func(JSContext* cx, unsigned argc, Value* vp);        \
    static const JSJitInfo JitInfo;                                           \
};

/*
 * Usage: LoadScalar(targetDatum, targetOffset, value)
 *
 * Intrinsic function. Loads value (which must be an int32 or uint32)
 * by `scalarTypeRepr` (which must be a type repr obj) and loads the
 * value at the memory for `targetDatum` at offset `targetOffset`.
 * `targetDatum` must be attached.
 */
#define JS_LOAD_SCALAR_CLASS_DEFN(_constant, T, _name)                        \
class LoadScalar##T {                                                         \
  public:                                                                     \
    static bool Func(JSContext* cx, unsigned argc, Value* vp);        \
    static const JSJitInfo JitInfo;                                           \
};

/*
 * Usage: LoadReference(targetDatum, targetOffset, value)
 *
 * Intrinsic function. Stores value (which must be an int32 or uint32)
 * by `scalarTypeRepr` (which must be a type repr obj) and stores the
 * value at the memory for `targetDatum` at offset `targetOffset`.
 * `targetDatum` must be attached.
 */
#define JS_LOAD_REFERENCE_CLASS_DEFN(_constant, T, _name)                     \
class LoadReference##T {                                                      \
  private:                                                                    \
    static void load(T* heap, MutableHandleValue v);                          \
                                                                              \
  public:                                                                     \
    static bool Func(JSContext* cx, unsigned argc, Value* vp);        \
    static const JSJitInfo JitInfo;                                           \
};

// I was using templates for this stuff instead of macros, but ran
// into problems with the Unagi compiler.
JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE(JS_STORE_SCALAR_CLASS_DEFN)
JS_FOR_EACH_UNIQUE_SCALAR_TYPE_REPR_CTYPE(JS_LOAD_SCALAR_CLASS_DEFN)
JS_FOR_EACH_REFERENCE_TYPE_REPR(JS_STORE_REFERENCE_CLASS_DEFN)
JS_FOR_EACH_REFERENCE_TYPE_REPR(JS_LOAD_REFERENCE_CLASS_DEFN)

inline bool
IsTypedObjectClass(const Class* class_)
{
    return class_ == &OutlineTransparentTypedObject::class_ ||
           class_ == &InlineTransparentTypedObject::class_ ||
           class_ == &OutlineOpaqueTypedObject::class_ ||
           class_ == &InlineOpaqueTypedObject::class_;
}

inline bool
IsOpaqueTypedObjectClass(const Class* class_)
{
    return class_ == &OutlineOpaqueTypedObject::class_ ||
           class_ == &InlineOpaqueTypedObject::class_;
}

inline bool
IsOutlineTypedObjectClass(const Class* class_)
{
    return class_ == &OutlineOpaqueTypedObject::class_ ||
           class_ == &OutlineTransparentTypedObject::class_;
}

inline bool
IsInlineTypedObjectClass(const Class* class_)
{
    return class_ == &InlineOpaqueTypedObject::class_ ||
           class_ == &InlineTransparentTypedObject::class_;
}

inline const Class*
GetOutlineTypedObjectClass(bool opaque)
{
    return opaque ? &OutlineOpaqueTypedObject::class_ : &OutlineTransparentTypedObject::class_;
}

inline bool
IsSimpleTypeDescrClass(const Class* clasp)
{
    return clasp == &ScalarTypeDescr::class_ ||
           clasp == &ReferenceTypeDescr::class_;
}

inline bool
IsComplexTypeDescrClass(const Class* clasp)
{
    return clasp == &StructTypeDescr::class_ ||
           clasp == &ArrayTypeDescr::class_ ||
           clasp == &SimdTypeDescr::class_;
}

inline bool
IsTypeDescrClass(const Class* clasp)
{
    return IsSimpleTypeDescrClass(clasp) ||
           IsComplexTypeDescrClass(clasp);
}

inline bool
TypedObject::opaque() const
{
    return IsOpaqueTypedObjectClass(getClass());
}

JSObject*
InitTypedObjectModuleObject(JSContext* cx, JS::HandleObject obj);

} // namespace js

template <>
inline bool
JSObject::is<js::SimpleTypeDescr>() const
{
    return IsSimpleTypeDescrClass(getClass());
}

template <>
inline bool
JSObject::is<js::ComplexTypeDescr>() const
{
    return IsComplexTypeDescrClass(getClass());
}

template <>
inline bool
JSObject::is<js::TypeDescr>() const
{
    return IsTypeDescrClass(getClass());
}

template <>
inline bool
JSObject::is<js::TypedObject>() const
{
    return IsTypedObjectClass(getClass());
}

template <>
inline bool
JSObject::is<js::OutlineTypedObject>() const
{
    return getClass() == &js::OutlineTransparentTypedObject::class_ ||
           getClass() == &js::OutlineOpaqueTypedObject::class_;
}

template <>
inline bool
JSObject::is<js::InlineTypedObject>() const
{
    return getClass() == &js::InlineTransparentTypedObject::class_ ||
           getClass() == &js::InlineOpaqueTypedObject::class_;
}

#endif /* builtin_TypedObject_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ArrayBufferObject_h
#define vm_ArrayBufferObject_h

#include "jsobj.h"

#include "builtin/TypedObjectConstants.h"
#include "js/GCHashTable.h"
#include "vm/Runtime.h"
#include "vm/SharedMem.h"

typedef struct JSProperty JSProperty;

namespace js {

class ArrayBufferViewObject;

// The inheritance hierarchy for the various classes relating to typed arrays
// is as follows.
//
// - NativeObject
//   - ArrayBufferObjectMaybeShared
//     - ArrayBufferObject
//     - SharedArrayBufferObject
//   - DataViewObject
//   - TypedArrayObject (declared in vm/TypedArrayObject.h)
//     - TypedArrayObjectTemplate
//       - Int8ArrayObject
//       - Uint8ArrayObject
//       - ...
// - JSObject
//   - ArrayBufferViewObject
//   - TypedObject (declared in builtin/TypedObject.h)
//
// Note that |TypedArrayObjectTemplate| is just an implementation
// detail that makes implementing its various subclasses easier.
//
// ArrayBufferObject and SharedArrayBufferObject are unrelated data types:
// the racy memory of the latter cannot substitute for the non-racy memory of
// the former; the non-racy memory of the former cannot be used with the atomics;
// the former can be neutered and the latter not.  Hence they have been separated
// completely.
//
// Most APIs will only accept ArrayBufferObject.  ArrayBufferObjectMaybeShared
// exists as a join point to allow APIs that can take or use either, notably AsmJS.
//
// In contrast with the separation of ArrayBufferObject and
// SharedArrayBufferObject, the TypedArray types can map either.
//
// The possible data ownership and reference relationships with ArrayBuffers
// and related classes are enumerated below. These are the possible locations
// for typed data:
//
// (1) malloc'ed or mmap'ed data owned by an ArrayBufferObject.
// (2) Data allocated inline with an ArrayBufferObject.
// (3) Data allocated inline with a TypedArrayObject.
// (4) Data allocated inline with an InlineTypedObject.
//
// An ArrayBufferObject may point to any of these sources of data, except (3).
// All array buffer views may point to any of these sources of data, except
// that (3) may only be pointed to by the typed array the data is inline with.
//
// During a minor GC, (3) and (4) may move. During a compacting GC, (2), (3),
// and (4) may move.

class ArrayBufferObjectMaybeShared;

uint32_t AnyArrayBufferByteLength(const ArrayBufferObjectMaybeShared* buf);
ArrayBufferObjectMaybeShared& AsAnyArrayBuffer(HandleValue val);

class ArrayBufferObjectMaybeShared : public NativeObject
{
  public:
    uint32_t byteLength() {
        return AnyArrayBufferByteLength(this);
    }

    inline SharedMem<uint8_t*> dataPointerEither();
};

/*
 * ArrayBufferObject
 *
 * This class holds the underlying raw buffer that the various ArrayBufferViews
 * (eg DataViewObject, the TypedArrays, TypedObjects) access. It can be created
 * explicitly and used to construct an ArrayBufferView, or can be created
 * lazily when it is first accessed for a TypedArrayObject or TypedObject that
 * doesn't have an explicit buffer.
 *
 * ArrayBufferObject (or really the underlying memory) /is not racy/: the
 * memory is private to a single worker.
 */
class ArrayBufferObject : public ArrayBufferObjectMaybeShared
{
    static bool byteLengthGetterImpl(JSContext* cx, const CallArgs& args);
    static bool fun_slice_impl(JSContext* cx, const CallArgs& args);

  public:
    static const uint8_t DATA_SLOT = 0;
    static const uint8_t BYTE_LENGTH_SLOT = 1;
    static const uint8_t FIRST_VIEW_SLOT = 2;
    static const uint8_t FLAGS_SLOT = 3;

    static const uint8_t RESERVED_SLOTS = 4;

    static const size_t ARRAY_BUFFER_ALIGNMENT = 8;

    static_assert(FLAGS_SLOT == JS_ARRAYBUFFER_FLAGS_SLOT,
                  "self-hosted code with burned-in constants must get the "
                  "right flags slot");

  public:

    enum OwnsState {
        DoesntOwnData = 0,
        OwnsData = 1,
    };

    enum BufferKind {
        PLAIN               = 0, // malloced or inline data
        ASMJS_MALLOCED      = 1,
        ASMJS_MAPPED        = 2,
        MAPPED              = 3,

        KIND_MASK           = 0x3
    };

  protected:

    enum ArrayBufferFlags {
        // The flags also store the BufferKind
        BUFFER_KIND_MASK    = BufferKind::KIND_MASK,

        NEUTERED            = 0x4,

        // The dataPointer() is owned by this buffer and should be released
        // when no longer in use. Releasing the pointer may be done by either
        // freeing or unmapping it, and how to do this is determined by the
        // buffer's other flags.
        //
        // Array buffers which do not own their data include buffers that
        // allocate their data inline, and buffers that are created lazily for
        // typed objects with inline storage, in which case the buffer points
        // directly to the typed object's storage.
        OWNS_DATA           = 0x8,

        // This array buffer was created lazily for a typed object with inline
        // data. This implies both that the typed object owns the buffer's data
        // and that the list of views sharing this buffer's data might be
        // incomplete. Any missing views will be typed objects.
        FOR_INLINE_TYPED_OBJECT = 0x10,

        // Views of this buffer might include typed objects.
        TYPED_OBJECT_VIEWS  = 0x20
    };

    static_assert(JS_ARRAYBUFFER_NEUTERED_FLAG == NEUTERED,
                  "self-hosted code with burned-in constants must use the "
                  "correct NEUTERED bit value");
  public:

    class BufferContents {
        uint8_t* data_;
        BufferKind kind_;

        friend class ArrayBufferObject;

        BufferContents(uint8_t* data, BufferKind kind) : data_(data), kind_(kind) {
            MOZ_ASSERT((kind_ & ~KIND_MASK) == 0);
        }

      public:

        template<BufferKind Kind>
        static BufferContents create(void* data)
        {
            return BufferContents(static_cast<uint8_t*>(data), Kind);
        }

        static BufferContents createPlain(void* data)
        {
            return BufferContents(static_cast<uint8_t*>(data), PLAIN);
        }

        uint8_t* data() const { return data_; }
        BufferKind kind() const { return kind_; }

        explicit operator bool() const { return data_ != nullptr; }
    };

    static const Class class_;

    static const Class protoClass;
    static const JSFunctionSpec jsfuncs[];
    static const JSFunctionSpec jsstaticfuncs[];

    static bool byteLengthGetter(JSContext* cx, unsigned argc, Value* vp);

    static bool fun_slice(JSContext* cx, unsigned argc, Value* vp);

    static bool fun_isView(JSContext* cx, unsigned argc, Value* vp);
#ifdef NIGHTLY_BUILD
    static bool fun_transfer(JSContext* cx, unsigned argc, Value* vp);
#endif

    static bool class_constructor(JSContext* cx, unsigned argc, Value* vp);

    static ArrayBufferObject* create(JSContext* cx, uint32_t nbytes,
                                     BufferContents contents,
                                     OwnsState ownsState = OwnsData,
                                     HandleObject proto = nullptr,
                                     NewObjectKind newKind = GenericObject);
    static ArrayBufferObject* create(JSContext* cx, uint32_t nbytes,
                                     HandleObject proto = nullptr,
                                     NewObjectKind newKind = GenericObject);

    static JSObject* createSlice(JSContext* cx, Handle<ArrayBufferObject*> arrayBuffer,
                                 uint32_t begin, uint32_t end);

    static bool createDataViewForThisImpl(JSContext* cx, const CallArgs& args);
    static bool createDataViewForThis(JSContext* cx, unsigned argc, Value* vp);

    template<typename T>
    static bool createTypedArrayFromBufferImpl(JSContext* cx, const CallArgs& args);

    template<typename T>
    static bool createTypedArrayFromBuffer(JSContext* cx, unsigned argc, Value* vp);

    static void trace(JSTracer* trc, JSObject* obj);
    static void objectMoved(JSObject* obj, const JSObject* old);

    static BufferContents stealContents(JSContext* cx,
                                        Handle<ArrayBufferObject*> buffer,
                                        bool hasStealableContents);

    bool hasStealableContents() const {
        // Inline elements strictly adhere to the corresponding buffer.
        if (!ownsData())
            return false;

        // Neutered contents aren't transferrable because we want a neutered
        // array's contents to be backed by zeroed memory equal in length to
        // the original buffer contents.  Transferring these contents would
        // allocate new ones based on the current byteLength, which is 0 for a
        // neutered array -- not the original byteLength.
        return !isNeutered();
    }

    // Return whether the buffer is allocated by js_malloc and should be freed
    // with js_free.
    bool hasMallocedContents() const {
        return (ownsData() && isPlain()) || isAsmJSMalloced();
    }

    static void addSizeOfExcludingThis(JSObject* obj, mozilla::MallocSizeOf mallocSizeOf,
                                       JS::ClassInfo* info);

    // ArrayBufferObjects (strongly) store the first view added to them, while
    // later views are (weakly) stored in the compartment's InnerViewTable
    // below. Buffers usually only have one view, so this slot optimizes for
    // the common case. Avoiding entries in the InnerViewTable saves memory and
    // non-incrementalized sweep time.
    ArrayBufferViewObject* firstView();

    bool addView(JSContext* cx, JSObject* view);

    void setNewOwnedData(FreeOp* fop, BufferContents newContents);
    void changeContents(JSContext* cx, BufferContents newContents);

    /*
     * Ensure data is not stored inline in the object. Used when handing back a
     * GC-safe pointer.
     */
    static bool ensureNonInline(JSContext* cx, Handle<ArrayBufferObject*> buffer);

    /* Neuter this buffer and all its views. */
    static MOZ_WARN_UNUSED_RESULT bool
    neuter(JSContext* cx, Handle<ArrayBufferObject*> buffer, BufferContents newContents);

  private:
    void neuterView(JSContext* cx, ArrayBufferViewObject* view,
                    BufferContents newContents);
    void changeViewContents(JSContext* cx, ArrayBufferViewObject* view,
                            uint8_t* oldDataPointer, BufferContents newContents);
    void setFirstView(ArrayBufferViewObject* view);

    uint8_t* inlineDataPointer() const;

  public:
    uint8_t* dataPointer() const;
    SharedMem<uint8_t*> dataPointerShared() const;
    size_t byteLength() const;
    BufferContents contents() const {
        return BufferContents(dataPointer(), bufferKind());
    }
    bool hasInlineData() const {
        return dataPointer() == inlineDataPointer();
    }

    void releaseData(FreeOp* fop);

    /*
     * Check if the arrayBuffer contains any data. This will return false for
     * ArrayBuffer.prototype and neutered ArrayBuffers.
     */
    bool hasData() const {
        return getClass() == &class_;
    }

    BufferKind bufferKind() const { return BufferKind(flags() & BUFFER_KIND_MASK); }
    bool isPlain() const { return bufferKind() == PLAIN; }
    bool isAsmJSMapped() const { return bufferKind() == ASMJS_MAPPED; }
    bool isAsmJSMalloced() const { return bufferKind() == ASMJS_MALLOCED; }
    bool isAsmJS() const { return isAsmJSMapped() || isAsmJSMalloced(); }
    bool isMapped() const { return bufferKind() == MAPPED; }
    bool isNeutered() const { return flags() & NEUTERED; }

    static bool prepareForAsmJS(JSContext* cx, Handle<ArrayBufferObject*> buffer,
                                bool usesSignalHandlers);
    static bool prepareForAsmJSNoSignals(JSContext* cx, Handle<ArrayBufferObject*> buffer);

    static void finalize(FreeOp* fop, JSObject* obj);

    static BufferContents createMappedContents(int fd, size_t offset, size_t length);

    static size_t offsetOfFlagsSlot() {
        return getFixedSlotOffset(FLAGS_SLOT);
    }
    static size_t offsetOfDataSlot() {
        return getFixedSlotOffset(DATA_SLOT);
    }

    static uint32_t neuteredFlag() { return NEUTERED; }

    void setForInlineTypedObject() {
        setFlags(flags() | FOR_INLINE_TYPED_OBJECT);
    }
    void setHasTypedObjectViews() {
        setFlags(flags() | TYPED_OBJECT_VIEWS);
    }

    bool forInlineTypedObject() const { return flags() & FOR_INLINE_TYPED_OBJECT; }

  protected:
    void setDataPointer(BufferContents contents, OwnsState ownsState);
    void setByteLength(size_t length);

    uint32_t flags() const;
    void setFlags(uint32_t flags);

    bool ownsData() const { return flags() & OWNS_DATA; }
    void setOwnsData(OwnsState owns) {
        setFlags(owns ? (flags() | OWNS_DATA) : (flags() & ~OWNS_DATA));
    }

    bool hasTypedObjectViews() const { return flags() & TYPED_OBJECT_VIEWS; }

    void setIsAsmJSMalloced() { setFlags((flags() & ~KIND_MASK) | ASMJS_MALLOCED); }
    void setIsNeutered() { setFlags(flags() | NEUTERED); }

    void initialize(size_t byteLength, BufferContents contents, OwnsState ownsState) {
        setByteLength(byteLength);
        setFlags(0);
        setFirstView(nullptr);
        setDataPointer(contents, ownsState);
    }
};

/*
 * ArrayBufferViewObject
 *
 * Common definitions shared by all array buffer views.
 */

class ArrayBufferViewObject : public JSObject
{
  public:
    static ArrayBufferObjectMaybeShared* bufferObject(JSContext* cx, Handle<ArrayBufferViewObject*> obj);

    void neuter(void* newData);

#ifdef DEBUG
    bool isSharedMemory();
#endif

    // By construction we only need unshared variants here.  See
    // comments in ArrayBufferObject.cpp.
    uint8_t* dataPointerUnshared();
    void setDataPointerUnshared(uint8_t* data);

    static void trace(JSTracer* trc, JSObject* obj);
};

bool
ToClampedIndex(JSContext* cx, HandleValue v, uint32_t length, uint32_t* out);

/*
 * Tests for ArrayBufferObject, like obj->is<ArrayBufferObject>().
 */
bool IsArrayBuffer(HandleValue v);
bool IsArrayBuffer(HandleObject obj);
bool IsArrayBuffer(JSObject* obj);
ArrayBufferObject& AsArrayBuffer(HandleObject obj);
ArrayBufferObject& AsArrayBuffer(JSObject* obj);

extern uint32_t JS_FASTCALL
ClampDoubleToUint8(const double x);

struct uint8_clamped {
    uint8_t val;

    uint8_clamped() { }
    uint8_clamped(const uint8_clamped& other) : val(other.val) { }

    // invoke our assignment helpers for constructor conversion
    explicit uint8_clamped(uint8_t x)    { *this = x; }
    explicit uint8_clamped(uint16_t x)   { *this = x; }
    explicit uint8_clamped(uint32_t x)   { *this = x; }
    explicit uint8_clamped(int8_t x)     { *this = x; }
    explicit uint8_clamped(int16_t x)    { *this = x; }
    explicit uint8_clamped(int32_t x)    { *this = x; }
    explicit uint8_clamped(double x)     { *this = x; }

    uint8_clamped& operator=(const uint8_clamped& x) {
        val = x.val;
        return *this;
    }

    uint8_clamped& operator=(uint8_t x) {
        val = x;
        return *this;
    }

    uint8_clamped& operator=(uint16_t x) {
        val = (x > 255) ? 255 : uint8_t(x);
        return *this;
    }

    uint8_clamped& operator=(uint32_t x) {
        val = (x > 255) ? 255 : uint8_t(x);
        return *this;
    }

    uint8_clamped& operator=(int8_t x) {
        val = (x >= 0) ? uint8_t(x) : 0;
        return *this;
    }

    uint8_clamped& operator=(int16_t x) {
        val = (x >= 0)
              ? ((x < 255)
                 ? uint8_t(x)
                 : 255)
              : 0;
        return *this;
    }

    uint8_clamped& operator=(int32_t x) {
        val = (x >= 0)
              ? ((x < 255)
                 ? uint8_t(x)
                 : 255)
              : 0;
        return *this;
    }

    uint8_clamped& operator=(const double x) {
        val = uint8_t(ClampDoubleToUint8(x));
        return *this;
    }

    operator uint8_t() const {
        return val;
    }

    void staticAsserts() {
        static_assert(sizeof(uint8_clamped) == 1,
                      "uint8_clamped must be layout-compatible with uint8_t");
    }
};

/* Note that we can't use std::numeric_limits here due to uint8_clamped. */
template<typename T> inline bool TypeIsFloatingPoint() { return false; }
template<> inline bool TypeIsFloatingPoint<float>() { return true; }
template<> inline bool TypeIsFloatingPoint<double>() { return true; }

template<typename T> inline bool TypeIsUnsigned() { return false; }
template<> inline bool TypeIsUnsigned<uint8_t>() { return true; }
template<> inline bool TypeIsUnsigned<uint16_t>() { return true; }
template<> inline bool TypeIsUnsigned<uint32_t>() { return true; }

// Per-compartment table that manages the relationship between array buffers
// and the views that use their storage.
class InnerViewTable
{
  public:
    typedef Vector<ArrayBufferViewObject*, 1, SystemAllocPolicy> ViewVector;

    friend class ArrayBufferObject;

  private:
    struct MapGCPolicy {
        static bool needsSweep(JSObject** key, ViewVector* value) {
            return InnerViewTable::sweepEntry(key, *value);
        }
    };

    // This key is a raw pointer and not a ReadBarriered because the post-
    // barrier would hold nursery-allocated entries live unconditionally. It is
    // a very common pattern in low-level and performance-oriented JavaScript
    // to create hundreds or thousands of very short lived temporary views on a
    // larger buffer; having to tenured all of these would be a catastrophic
    // performance regression. Thus, it is vital that nursery pointers in this
    // map not be held live. Special support is required in the minor GC,
    // implemented in sweepAfterMinorGC.
    typedef GCHashMap<JSObject*,
                      ViewVector,
                      MovableCellHasher<JSObject*>,
                      SystemAllocPolicy,
                      MapGCPolicy> Map;

    // For all objects sharing their storage with some other view, this maps
    // the object to the list of such views. All entries in this map are weak.
    Map map;

    // List of keys from innerViews where either the source or at least one
    // target is in the nursery. The raw pointer to a JSObject is allowed here
    // because this vector is cleared after every minor collection. Users in
    // sweepAfterMinorCollection must be careful to use MaybeForwarded before
    // touching these pointers.
    Vector<JSObject*, 0, SystemAllocPolicy> nurseryKeys;

    // Whether nurseryKeys is a complete list.
    bool nurseryKeysValid;

    // Sweep an entry during GC, returning whether the entry should be removed.
    static bool sweepEntry(JSObject** pkey, ViewVector& views);

    bool addView(JSContext* cx, ArrayBufferObject* obj, ArrayBufferViewObject* view);
    ViewVector* maybeViewsUnbarriered(ArrayBufferObject* obj);
    void removeViews(ArrayBufferObject* obj);

  public:
    InnerViewTable()
      : nurseryKeysValid(true)
    {}

    // Remove references to dead objects in the table and update table entries
    // to reflect moved objects.
    void sweep();
    void sweepAfterMinorGC();

    bool needsSweepAfterMinorGC() {
        return !nurseryKeys.empty() || !nurseryKeysValid;
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);
};

extern JSObject*
InitArrayBufferClass(JSContext* cx, HandleObject obj);

} // namespace js

template <>
bool
JSObject::is<js::ArrayBufferViewObject>() const;

template <>
bool
JSObject::is<js::ArrayBufferObjectMaybeShared>() const;

#endif // vm_ArrayBufferObject_h

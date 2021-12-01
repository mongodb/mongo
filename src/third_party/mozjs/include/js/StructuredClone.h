/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_StructuredClone_h
#define js_StructuredClone_h

#include "mozilla/Attributes.h"
#include "mozilla/BufferList.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Move.h"

#include <stdint.h>

#include "jstypes.h"

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "js/Vector.h"

/*
 * API for safe passing of structured data, HTML 2018 Feb 21 section 2.7.
 * <https://html.spec.whatwg.org/multipage/structured-data.html>
 *
 * This is a serialization scheme for JS values, somewhat like JSON. It
 * preserves some aspects of JS objects (strings, numbers, own data properties
 * with string keys, array elements) but not others (methods, getters and
 * setters, prototype chains). Unlike JSON, structured data:
 *
 * -   can contain cyclic references.
 *
 * -   handles Maps, Sets, and some other object types.
 *
 * -   supports *transferring* objects of certain types from one realm to
 *     another, rather than cloning them.
 *
 * -   is specified by a living standard, and continues to evolve.
 *
 * -   is encoded in a nonstandard binary format, and is never exposed to Web
 *     content in its serialized form. It's used internally by the browser to
 *     send data from one thread/realm/domain to another, not across the
 *     network.
 */

struct JSStructuredCloneReader;
struct JSStructuredCloneWriter;

/**
 * The structured-clone serialization format version number.
 *
 * When serialized data is stored as bytes, e.g. in your Firefox profile, later
 * versions of the engine may have to read it. When you upgrade Firefox, we
 * don't crawl through your whole profile converting all saved data from the
 * previous version of the serialization format to the latest version. So it is
 * normal to have data in old formats stored in your profile.
 *
 * The JS engine can *write* data only in the current format version.
 *
 * It can *read* any data written in the current version, and data written for
 * DifferentProcess scope in earlier versions.
 *
 *
 * ## When to bump this version number
 *
 * When making a change so drastic that the JS engine needs to know whether
 * it's reading old or new serialized data in order to handle both correctly,
 * increment this version number. Make sure the engine can still read all
 * old data written with previous versions.
 *
 * If StructuredClone.cpp doesn't contain code that distinguishes between
 * version 8 and version 9, there should not be a version 9.
 *
 * Do not increment for changes that only affect SameProcess encoding.
 *
 * Increment only for changes that would otherwise break old serialized data.
 * Do not increment for new data types. (Rationale: Modulo bugs, older versions
 * of the JS engine can already correctly throw errors when they encounter new,
 * unrecognized features. A version number bump does not actually help them.)
 */
#define JS_STRUCTURED_CLONE_VERSION 8

namespace JS {

/**
 * Indicates the "scope of validity" of serialized data.
 *
 * Writing plain JS data produces an array of bytes that can be copied and
 * read in another process or whatever. The serialized data is Plain Old Data.
 * However, HTML also supports `Transferable` objects, which, when cloned, can
 * be moved from the source object into the clone, like when you take a
 * photograph of someone and it steals their soul.
 * See <https://developer.mozilla.org/en-US/docs/Web/API/Transferable>.
 * We support cloning and transferring objects of many types.
 *
 * For example, when we transfer an ArrayBuffer (within a process), we "detach"
 * the ArrayBuffer, embed the raw buffer pointer in the serialized data, and
 * later install it in a new ArrayBuffer in the destination realm. Ownership
 * of that buffer memory is transferred from the original ArrayBuffer to the
 * serialized data and then to the clone.
 *
 * This only makes sense within a single address space. When we transfer an
 * ArrayBuffer to another process, the contents of the buffer must be copied
 * into the serialized data. (The original ArrayBuffer is still detached,
 * though, for consistency; in some cases the caller shouldn't know or care if
 * the recipient is in the same process.)
 *
 * ArrayBuffers are actually a lucky case; some objects (like MessagePorts)
 * can't reasonably be stored by value in serialized data -- it's pointers or
 * nothing.
 *
 * So there is a tradeoff between scope of validity -- how far away the
 * serialized data may be sent and still make sense -- and efficiency or
 * features. The read and write algorithms therefore take an argument of this
 * type, allowing the user to control those trade-offs.
 */
enum class StructuredCloneScope : uint32_t {
    /**
     * The most restrictive scope, with greatest efficiency and features.
     *
     * When writing, this means we're writing for an audience in the same
     * process and same thread. The caller promises that the serialized data
     * will **not** be shipped off to a different thread/process or stored in a
     * database. It's OK to produce serialized data that contains pointers.  In
     * Rust terms, the serialized data will be treated as `!Send`.
     *
     * When reading, this means: Accept transferred objects and buffers
     * (pointers). The caller promises that the serialized data was written
     * using this API (otherwise, the serialized data may contain bogus
     * pointers, leading to undefined behavior).
     */
    SameProcessSameThread,

    /**
     * When writing, this means: The caller promises that the serialized data
     * will **not** be shipped off to a different process or stored in a
     * database. However, it may be shipped to another thread. It's OK to
     * produce serialized data that contains pointers to data that is safe to
     * send across threads, such as array buffers. In Rust terms, the
     * serialized data will be treated as `Send` but not `Copy`.
     *
     * When reading, this means the same thing as SameProcessSameThread;
     * the distinction only matters when writing.
     */
    SameProcessDifferentThread,

    /**
     * When writing, this means we're writing for an audience in a different
     * process. Produce serialized data that can be sent to other processes,
     * bitwise copied, or even stored as bytes in a database and read by later
     * versions of Firefox years from now. The HTML5 spec refers to this as
     * "ForStorage" as in StructuredSerializeForStorage, though we use
     * DifferentProcess for IPC as well as storage.
     *
     * Transferable objects are limited to ArrayBuffers, whose contents are
     * copied into the serialized data (rather than just writing a pointer).
     *
     * When reading, this means: Do not accept pointers.
     */
    DifferentProcess,

    /**
     * Handle a backwards-compatibility case with IndexedDB (bug 1434308): when
     * reading, this means to treat legacy SameProcessSameThread data as if it
     * were DifferentProcess.
     *
     * Do not use this for writing; use DifferentProcess instead.
     */
    DifferentProcessForIndexedDB,

    /**
     * Existing code wants to be able to create an uninitialized
     * JSStructuredCloneData without knowing the scope, then populate it with
     * data (at which point the scope *is* known.)
     */
    Unassigned
};

enum TransferableOwnership {
    /** Transferable data has not been filled in yet */
    SCTAG_TMO_UNFILLED = 0,

    /** Structured clone buffer does not yet own the data */
    SCTAG_TMO_UNOWNED = 1,

    /** All values at least this large are owned by the clone buffer */
    SCTAG_TMO_FIRST_OWNED = 2,

    /** Data is a pointer that can be freed */
    SCTAG_TMO_ALLOC_DATA = 2,

    /** Data is a memory mapped pointer */
    SCTAG_TMO_MAPPED_DATA = 3,

    /**
     * Data is embedding-specific. The engine can free it by calling the
     * freeTransfer op. The embedding can also use SCTAG_TMO_USER_MIN and
     * greater, up to 32 bits, to distinguish specific ownership variants.
     */
    SCTAG_TMO_CUSTOM = 4,

    SCTAG_TMO_USER_MIN
};

class CloneDataPolicy
{
    bool sharedArrayBuffer_;

  public:
    // The default is to allow all policy-controlled aspects.

    CloneDataPolicy() :
      sharedArrayBuffer_(true)
    {}

    // In the JS engine, SharedArrayBuffers can only be cloned intra-process
    // because the shared memory areas are allocated in process-private memory.
    // Clients should therefore deny SharedArrayBuffers when cloning data that
    // are to be transmitted inter-process.
    //
    // Clients should also deny SharedArrayBuffers when cloning data that are to
    // be transmitted intra-process if policy needs dictate such denial.

    CloneDataPolicy& denySharedArrayBuffer() {
        sharedArrayBuffer_ = false;
        return *this;
    }

    bool isSharedArrayBufferAllowed() const {
        return sharedArrayBuffer_;
    }
};

} /* namespace JS */

/**
 * Read structured data from the reader r. This hook is used to read a value
 * previously serialized by a call to the WriteStructuredCloneOp hook.
 *
 * tag and data are the pair of uint32_t values from the header. The callback
 * may use the JS_Read* APIs to read any other relevant parts of the object
 * from the reader r. closure is any value passed to the JS_ReadStructuredClone
 * function. Return the new object on success, nullptr on error/exception.
 */
typedef JSObject* (*ReadStructuredCloneOp)(JSContext* cx, JSStructuredCloneReader* r,
                                           uint32_t tag, uint32_t data, void* closure);

/**
 * Structured data serialization hook. The engine can write primitive values,
 * Objects, Arrays, Dates, RegExps, TypedArrays, ArrayBuffers, Sets, Maps,
 * and SharedTypedArrays. Any other type of object requires application support.
 * This callback must first use the JS_WriteUint32Pair API to write an object
 * header, passing a value greater than JS_SCTAG_USER to the tag parameter.
 * Then it can use the JS_Write* APIs to write any other relevant parts of
 * the value v to the writer w. closure is any value passed to the
 * JS_WriteStructuredClone function.
 *
 * Return true on success, false on error/exception.
 */
typedef bool (*WriteStructuredCloneOp)(JSContext* cx, JSStructuredCloneWriter* w,
                                       JS::HandleObject obj, void* closure);

/**
 * This is called when JS_WriteStructuredClone is given an invalid transferable.
 * To follow HTML5, the application must throw a DATA_CLONE_ERR DOMException
 * with error set to one of the JS_SCERR_* values.
 */
typedef void (*StructuredCloneErrorOp)(JSContext* cx, uint32_t errorid);

/**
 * This is called when JS_ReadStructuredClone receives a transferable object
 * not known to the engine. If this hook does not exist or returns false, the
 * JS engine calls the reportError op if set, otherwise it throws a
 * DATA_CLONE_ERR DOM Exception. This method is called before any other
 * callback and must return a non-null object in returnObject on success.
 */
typedef bool (*ReadTransferStructuredCloneOp)(JSContext* cx, JSStructuredCloneReader* r,
                                              uint32_t tag, void* content, uint64_t extraData,
                                              void* closure,
                                              JS::MutableHandleObject returnObject);

/**
 * Called when JS_WriteStructuredClone receives a transferable object not
 * handled by the engine. If this hook does not exist or returns false, the JS
 * engine will call the reportError hook or fall back to throwing a
 * DATA_CLONE_ERR DOM Exception. This method is called before any other
 * callback.
 *
 *  tag: indicates what type of transferable this is. Must be greater than
 *       0xFFFF0201 (value of the internal SCTAG_TRANSFER_MAP_PENDING_ENTRY)
 *
 *  ownership: see TransferableOwnership, above. Used to communicate any needed
 *       ownership info to the FreeTransferStructuredCloneOp.
 *
 *  content, extraData: what the ReadTransferStructuredCloneOp will receive
 */
typedef bool (*TransferStructuredCloneOp)(JSContext* cx,
                                          JS::Handle<JSObject*> obj,
                                          void* closure,
                                          // Output:
                                          uint32_t* tag,
                                          JS::TransferableOwnership* ownership,
                                          void** content,
                                          uint64_t* extraData);

/**
 * Called when freeing an unknown transferable object. Note that it
 * should never trigger a garbage collection (and will assert in a
 * debug build if it does.)
 */
typedef void (*FreeTransferStructuredCloneOp)(uint32_t tag, JS::TransferableOwnership ownership,
                                              void* content, uint64_t extraData, void* closure);

struct JSStructuredCloneCallbacks {
    ReadStructuredCloneOp read;
    WriteStructuredCloneOp write;
    StructuredCloneErrorOp reportError;
    ReadTransferStructuredCloneOp readTransfer;
    TransferStructuredCloneOp writeTransfer;
    FreeTransferStructuredCloneOp freeTransfer;
};

enum OwnTransferablePolicy {
    OwnsTransferablesIfAny,
    IgnoreTransferablesIfAny,
    NoTransferables
};

namespace js
{
    class SharedArrayRawBuffer;

    class SharedArrayRawBufferRefs
    {
      public:
        SharedArrayRawBufferRefs() = default;
        SharedArrayRawBufferRefs(SharedArrayRawBufferRefs&& other) = default;
        SharedArrayRawBufferRefs& operator=(SharedArrayRawBufferRefs&& other);
        ~SharedArrayRawBufferRefs();

        MOZ_MUST_USE bool acquire(JSContext* cx, SharedArrayRawBuffer* rawbuf);
        MOZ_MUST_USE bool acquireAll(JSContext* cx, const SharedArrayRawBufferRefs& that);
        void takeOwnership(SharedArrayRawBufferRefs&&);
        void releaseAll();

      private:
        js::Vector<js::SharedArrayRawBuffer*, 0, js::SystemAllocPolicy> refs_;
    };

    template <typename T, typename AllocPolicy> struct BufferIterator;
}

/**
 * JSStructuredCloneData represents structured clone data together with the
 * information needed to read/write/transfer/free the records within it, in the
 * form of a set of callbacks.
 */
class MOZ_NON_MEMMOVABLE JS_PUBLIC_API(JSStructuredCloneData) {
  public:
    using BufferList = mozilla::BufferList<js::SystemAllocPolicy>;
    using Iterator = BufferList::IterImpl;

  private:
    static const size_t kStandardCapacity = 4096;

    BufferList bufList_;

    // The (address space, thread) scope within which this clone is valid. Note
    // that this must be either set during construction, or start out as
    // Unassigned and transition once to something else.
    JS::StructuredCloneScope scope_;

    const JSStructuredCloneCallbacks* callbacks_ = nullptr;
    void* closure_ = nullptr;
    OwnTransferablePolicy ownTransferables_ = OwnTransferablePolicy::NoTransferables;
    js::SharedArrayRawBufferRefs refsHeld_;

    friend struct JSStructuredCloneWriter;
    friend class JS_PUBLIC_API(JSAutoStructuredCloneBuffer);
    template <typename T, typename AllocPolicy> friend struct js::BufferIterator;

  public:
    // The constructor must be infallible but SystemAllocPolicy is not, so both
    // the initial size and initial capacity of the BufferList must be zero.
    explicit JSStructuredCloneData(JS::StructuredCloneScope scope)
        : bufList_(0, 0, kStandardCapacity, js::SystemAllocPolicy())
        , scope_(scope)
        , callbacks_(nullptr)
        , closure_(nullptr)
        , ownTransferables_(OwnTransferablePolicy::NoTransferables)
    {}

    // Steal the raw data from a BufferList. In this case, we don't know the
    // scope and none of the callback info is assigned yet.
    JSStructuredCloneData(BufferList&& buffers, JS::StructuredCloneScope scope)
        : bufList_(mozilla::Move(buffers))
        , scope_(scope)
        , callbacks_(nullptr)
        , closure_(nullptr)
        , ownTransferables_(OwnTransferablePolicy::NoTransferables)
    {}
    MOZ_IMPLICIT JSStructuredCloneData(BufferList&& buffers)
        : JSStructuredCloneData(mozilla::Move(buffers), JS::StructuredCloneScope::Unassigned)
    {}
    JSStructuredCloneData(JSStructuredCloneData&& other) = default;
    JSStructuredCloneData& operator=(JSStructuredCloneData&& other) = default;
    ~JSStructuredCloneData() { discardTransferables(); }

    void setCallbacks(const JSStructuredCloneCallbacks* callbacks,
                      void* closure,
                      OwnTransferablePolicy policy)
    {
        callbacks_ = callbacks;
        closure_ = closure;
        ownTransferables_ = policy;
    }

    bool Init(size_t initialCapacity = 0) { return bufList_.Init(0, initialCapacity); }

    JS::StructuredCloneScope scope() const { return scope_; }

    void initScope(JS::StructuredCloneScope scope) {
        MOZ_ASSERT(Size() == 0, "initScope() of nonempty JSStructuredCloneData");
        if (scope_ != JS::StructuredCloneScope::Unassigned)
            MOZ_ASSERT(scope_ == scope, "Cannot change scope after it has been initialized");
        scope_ = scope;
    }

    size_t Size() const { return bufList_.Size(); }

    const Iterator Start() const { return bufList_.Iter(); }

    bool Advance(Iterator& iter, size_t distance) const {
        return iter.AdvanceAcrossSegments(bufList_, distance);
    }

    bool ReadBytes(Iterator& iter, char* buffer, size_t size) const {
        return bufList_.ReadBytes(iter, buffer, size);
    }

    // Append new data to the end of the buffer.
    bool AppendBytes(const char* data, size_t size) {
        MOZ_ASSERT(scope_ != JS::StructuredCloneScope::Unassigned);
        return bufList_.WriteBytes(data, size);
    }

    // Update data stored within the existing buffer. There must be at least
    // 'size' bytes between the position of 'iter' and the end of the buffer.
    bool UpdateBytes(Iterator& iter, const char* data, size_t size) const {
        MOZ_ASSERT(scope_ != JS::StructuredCloneScope::Unassigned);
        while (size > 0) {
            size_t remaining = iter.RemainingInSegment();
            size_t nbytes = std::min(remaining, size);
            memcpy(iter.Data(), data, nbytes);
            data += nbytes;
            size -= nbytes;
            iter.Advance(bufList_, nbytes);
        }
        return true;
    }

    char* AllocateBytes(size_t maxSize, size_t* size) {
        return bufList_.AllocateBytes(maxSize, size);
    }

    void Clear() {
        discardTransferables();
        bufList_.Clear();
    }

    // Return a new read-only JSStructuredCloneData that "borrows" the contents
    // of |this|. Its lifetime should not exceed the donor's. This is only
    // allowed for DifferentProcess clones, so finalization of the borrowing
    // clone will do nothing.
    JSStructuredCloneData Borrow(Iterator& iter, size_t size, bool* success) const
    {
        MOZ_ASSERT(scope_ == JS::StructuredCloneScope::DifferentProcess);
        return JSStructuredCloneData(bufList_.Borrow<js::SystemAllocPolicy>(iter, size, success),
                                     scope_);
    }

    // Iterate over all contained data, one BufferList segment's worth at a
    // time, and invoke the given FunctionToApply with the data pointer and
    // size. The function should return a bool value, and this loop will exit
    // with false if the function ever returns false.
    template <typename FunctionToApply>
    bool ForEachDataChunk(FunctionToApply&& function) const {
        Iterator iter = bufList_.Iter();
        while (!iter.Done()) {
            if (!function(iter.Data(), iter.RemainingInSegment()))
                return false;
            iter.Advance(bufList_, iter.RemainingInSegment());
        }
        return true;
    }

    // Append the entire contents of other's bufList_ to our own.
    bool Append(const JSStructuredCloneData& other) {
        MOZ_ASSERT(scope_ == other.scope());
        return other.ForEachDataChunk([&](const char* data, size_t size) {
            return AppendBytes(data, size);
        });
    }

    size_t SizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
        return bufList_.SizeOfExcludingThis(mallocSizeOf);
    }

    void discardTransferables();
};

/**
 * Implements StructuredDeserialize and StructuredDeserializeWithTransfer.
 *
 * Note: If `data` contains transferable objects, it can be read only once.
 */
JS_PUBLIC_API(bool)
JS_ReadStructuredClone(JSContext* cx, JSStructuredCloneData& data, uint32_t version,
                       JS::StructuredCloneScope scope,
                       JS::MutableHandleValue vp,
                       const JSStructuredCloneCallbacks* optionalCallbacks, void* closure);

/**
 * Implements StructuredSerialize, StructuredSerializeForStorage, and
 * StructuredSerializeWithTransfer.
 *
 * Note: If the scope is DifferentProcess then the cloneDataPolicy must deny
 * shared-memory objects, or an error will be signaled if a shared memory object
 * is seen.
 */
JS_PUBLIC_API(bool)
JS_WriteStructuredClone(JSContext* cx, JS::HandleValue v, JSStructuredCloneData* data,
                        JS::StructuredCloneScope scope,
                        JS::CloneDataPolicy cloneDataPolicy,
                        const JSStructuredCloneCallbacks* optionalCallbacks,
                        void* closure, JS::HandleValue transferable);

JS_PUBLIC_API(bool)
JS_StructuredCloneHasTransferables(JSStructuredCloneData& data, bool* hasTransferable);

JS_PUBLIC_API(bool)
JS_StructuredClone(JSContext* cx, JS::HandleValue v, JS::MutableHandleValue vp,
                   const JSStructuredCloneCallbacks* optionalCallbacks, void* closure);

/**
 * The C-style API calls to read and write structured clones are fragile --
 * they rely on the caller to properly handle ownership of the clone data, and
 * the handling of the input data as well as the interpretation of the contents
 * of the clone buffer are dependent on the callbacks passed in. If you
 * serialize and deserialize with different callbacks, the results are
 * questionable.
 *
 * JSAutoStructuredCloneBuffer wraps things up in an RAII class for data
 * management, and uses the same callbacks for both writing and reading
 * (serializing and deserializing).
 */
class JS_PUBLIC_API(JSAutoStructuredCloneBuffer) {
    const JS::StructuredCloneScope scope_;
    JSStructuredCloneData data_;
    uint32_t version_;

  public:
    JSAutoStructuredCloneBuffer(JS::StructuredCloneScope scope,
                                const JSStructuredCloneCallbacks* callbacks, void* closure)
        : scope_(scope), data_(scope), version_(JS_STRUCTURED_CLONE_VERSION)
    {
        data_.setCallbacks(callbacks, closure, OwnTransferablePolicy::NoTransferables);
    }

    JSAutoStructuredCloneBuffer(JSAutoStructuredCloneBuffer&& other);
    JSAutoStructuredCloneBuffer& operator=(JSAutoStructuredCloneBuffer&& other);

    ~JSAutoStructuredCloneBuffer() { clear(); }

    JSStructuredCloneData& data() { return data_; }
    bool empty() const { return !data_.Size(); }

    void clear();

    JS::StructuredCloneScope scope() const { return scope_; }

    /**
     * Adopt some memory. It will be automatically freed by the destructor.
     * data must have been allocated by the JS engine (e.g., extracted via
     * JSAutoStructuredCloneBuffer::steal).
     */
    void adopt(JSStructuredCloneData&& data, uint32_t version=JS_STRUCTURED_CLONE_VERSION,
               const JSStructuredCloneCallbacks* callbacks=nullptr, void* closure=nullptr);

    /**
     * Release the buffer and transfer ownership to the caller.
     */
    void steal(JSStructuredCloneData* data, uint32_t* versionp=nullptr,
               const JSStructuredCloneCallbacks** callbacks=nullptr, void** closure=nullptr);

    /**
     * Abandon ownership of any transferable objects stored in the buffer,
     * without freeing the buffer itself. Useful when copying the data out into
     * an external container, though note that you will need to use adopt() to
     * properly release that data eventually.
     */
    void abandon() { data_.ownTransferables_ = OwnTransferablePolicy::IgnoreTransferablesIfAny; }

    bool read(JSContext* cx, JS::MutableHandleValue vp,
              const JSStructuredCloneCallbacks* optionalCallbacks=nullptr, void* closure=nullptr);

    bool write(JSContext* cx, JS::HandleValue v,
               const JSStructuredCloneCallbacks* optionalCallbacks=nullptr, void* closure=nullptr);

    bool write(JSContext* cx, JS::HandleValue v, JS::HandleValue transferable,
               JS::CloneDataPolicy cloneDataPolicy,
               const JSStructuredCloneCallbacks* optionalCallbacks=nullptr, void* closure=nullptr);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
    {
        return data_.SizeOfExcludingThis(mallocSizeOf);
    }

    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf)
    {
        return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
    }

  private:
    // Copy and assignment are not supported.
    JSAutoStructuredCloneBuffer(const JSAutoStructuredCloneBuffer& other) = delete;
    JSAutoStructuredCloneBuffer& operator=(const JSAutoStructuredCloneBuffer& other) = delete;
};

// The range of tag values the application may use for its own custom object types.
#define JS_SCTAG_USER_MIN  ((uint32_t) 0xFFFF8000)
#define JS_SCTAG_USER_MAX  ((uint32_t) 0xFFFFFFFF)

#define JS_SCERR_RECURSION 0
#define JS_SCERR_TRANSFERABLE 1
#define JS_SCERR_DUP_TRANSFERABLE 2
#define JS_SCERR_UNSUPPORTED_TYPE 3
#define JS_SCERR_SHMEM_TRANSFERABLE 4

JS_PUBLIC_API(bool)
JS_ReadUint32Pair(JSStructuredCloneReader* r, uint32_t* p1, uint32_t* p2);

JS_PUBLIC_API(bool)
JS_ReadBytes(JSStructuredCloneReader* r, void* p, size_t len);

JS_PUBLIC_API(bool)
JS_ReadTypedArray(JSStructuredCloneReader* r, JS::MutableHandleValue vp);

JS_PUBLIC_API(bool)
JS_WriteUint32Pair(JSStructuredCloneWriter* w, uint32_t tag, uint32_t data);

JS_PUBLIC_API(bool)
JS_WriteBytes(JSStructuredCloneWriter* w, const void* p, size_t len);

JS_PUBLIC_API(bool)
JS_WriteString(JSStructuredCloneWriter* w, JS::HandleString str);

JS_PUBLIC_API(bool)
JS_WriteTypedArray(JSStructuredCloneWriter* w, JS::HandleValue v);

JS_PUBLIC_API(bool)
JS_ObjectNotWritten(JSStructuredCloneWriter* w, JS::HandleObject obj);

JS_PUBLIC_API(JS::StructuredCloneScope)
JS_GetStructuredCloneScope(JSStructuredCloneWriter* w);

#endif  /* js_StructuredClone_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SharedArrayObject.h"

#include "mozilla/Atomics.h"

#include "jsfriendapi.h"
#include "jsprf.h"

#ifdef XP_WIN
# include "jswin.h"
#endif
#include "jswrapper.h"
#ifndef XP_WIN
# include <sys/mman.h>
#endif
#ifdef MOZ_VALGRIND
# include <valgrind/memcheck.h>
#endif

#include "asmjs/AsmJSValidate.h"
#include "vm/SharedMem.h"
#include "vm/TypedArrayCommon.h"

#include "jsobjinlines.h"

using namespace js;

static inline void*
MapMemory(size_t length, bool commit)
{
#ifdef XP_WIN
    int prot = (commit ? MEM_COMMIT : MEM_RESERVE);
    int flags = (commit ? PAGE_READWRITE : PAGE_NOACCESS);
    return VirtualAlloc(nullptr, length, prot, flags);
#else
    int prot = (commit ? (PROT_READ | PROT_WRITE) : PROT_NONE);
    void* p = mmap(nullptr, length, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    return p;
#endif
}

static inline void
UnmapMemory(void* addr, size_t len)
{
#ifdef XP_WIN
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    munmap(addr, len);
#endif
}

static inline bool
MarkValidRegion(void* addr, size_t len)
{
#ifdef XP_WIN
    if (!VirtualAlloc(addr, len, MEM_COMMIT, PAGE_READWRITE))
        return false;
    return true;
#else
    if (mprotect(addr, len, PROT_READ | PROT_WRITE))
        return false;
    return true;
#endif
}

#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
// Since this SharedArrayBuffer will likely be used for asm.js code, prepare it
// for asm.js by mapping the 4gb protected zone described in AsmJSValidate.h.
// Since we want to put the SharedArrayBuffer header immediately before the
// heap but keep the heap page-aligned, allocate an extra page before the heap.
static const uint64_t SharedArrayMappedSize = AsmJSMappedSize + AsmJSPageSize;
static_assert(sizeof(SharedArrayRawBuffer) < AsmJSPageSize, "Page size not big enough");

// If there are too many 4GB buffers live we run up against system resource
// exhaustion (address space or number of memory map descriptors), see
// bug 1068684, bug 1073934 for details.  The limiting case seems to be
// Windows Vista Home 64-bit, where the per-process address space is limited
// to 8TB.  Thus we track the number of live objects, and set a limit of
// 1000 live objects per process; we run synchronous GC if necessary; and
// we throw an OOM error if the per-process limit is exceeded.
static mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> numLive;
static const uint32_t maxLive = 1000;
#endif

SharedArrayRawBuffer*
SharedArrayRawBuffer::New(JSContext* cx, uint32_t length)
{
    // The value (uint32_t)-1 is used as a signal in various places,
    // so guard against it on principle.
    MOZ_ASSERT(length != (uint32_t)-1);

    // Add a page for the header and round to a page boundary.
    uint32_t allocSize = (length + 2*AsmJSPageSize - 1) & ~(AsmJSPageSize - 1);
    if (allocSize <= length)
        return nullptr;
#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
    void* p = nullptr;
    if (!IsValidAsmJSHeapLength(length)) {
        p = MapMemory(allocSize, true);
        if (!p)
            return nullptr;
    } else {
        // Test >= to guard against the case where multiple extant runtimes
        // race to allocate.
        if (++numLive >= maxLive) {
            JSRuntime* rt = cx->runtime();
            if (rt->largeAllocationFailureCallback)
                rt->largeAllocationFailureCallback(rt->largeAllocationFailureCallbackData);
            if (numLive >= maxLive) {
                numLive--;
                return nullptr;
            }
        }
        // Get the entire reserved region (with all pages inaccessible)
        p = MapMemory(SharedArrayMappedSize, false);
        if (!p) {
            numLive--;
            return nullptr;
        }

        if (!MarkValidRegion(p, allocSize)) {
            UnmapMemory(p, SharedArrayMappedSize);
            numLive--;
            return nullptr;
        }
#   if defined(MOZ_VALGRIND) && defined(VALGRIND_DISABLE_ADDR_ERROR_REPORTING_IN_RANGE)
        // Tell Valgrind/Memcheck to not report accesses in the inaccessible region.
        VALGRIND_DISABLE_ADDR_ERROR_REPORTING_IN_RANGE((unsigned char*)p + allocSize,
                                                       SharedArrayMappedSize - allocSize);
#   endif
    }
#else
    void* p = MapMemory(allocSize, true);
    if (!p)
        return nullptr;
#endif
    uint8_t* buffer = reinterpret_cast<uint8_t*>(p) + AsmJSPageSize;
    uint8_t* base = buffer - sizeof(SharedArrayRawBuffer);
    SharedArrayRawBuffer* rawbuf = new (base) SharedArrayRawBuffer(buffer, length);
    MOZ_ASSERT(rawbuf->length == length); // Deallocation needs this
    return rawbuf;
}

void
SharedArrayRawBuffer::addReference()
{
    MOZ_ASSERT(this->refcount > 0);
    ++this->refcount; // Atomic.
}

void
SharedArrayRawBuffer::dropReference()
{
    // Drop the reference to the buffer.
    uint32_t refcount = --this->refcount; // Atomic.

    // If this was the final reference, release the buffer.
    if (refcount == 0) {
        SharedMem<uint8_t*> p = this->dataPointerShared() - AsmJSPageSize;

        MOZ_ASSERT(p.asValue() % AsmJSPageSize == 0);

        uint8_t* address = p.unwrap(/*safe - only reference*/);
        uint32_t allocSize = (this->length + 2*AsmJSPageSize - 1) & ~(AsmJSPageSize - 1);
#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
        if (!IsValidAsmJSHeapLength(this->length)) {
            UnmapMemory(address, allocSize);
        } else {
            numLive--;
            UnmapMemory(address, SharedArrayMappedSize);
#       if defined(MOZ_VALGRIND) \
           && defined(VALGRIND_ENABLE_ADDR_ERROR_REPORTING_IN_RANGE)
            // Tell Valgrind/Memcheck to recommence reporting accesses in the
            // previously-inaccessible region.
            VALGRIND_ENABLE_ADDR_ERROR_REPORTING_IN_RANGE(address,
                                                          SharedArrayMappedSize);
#       endif
        }
#else
        UnmapMemory(address, allocSize);
#endif
    }
}

const JSFunctionSpec SharedArrayBufferObject::jsfuncs[] = {
    /* Nothing yet */
    JS_FS_END
};

const JSFunctionSpec SharedArrayBufferObject::jsstaticfuncs[] = {
    JS_FN("isView", SharedArrayBufferObject::fun_isView, 1, 0),
    JS_FS_END
};

MOZ_ALWAYS_INLINE bool
SharedArrayBufferObject::byteLengthGetterImpl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(IsSharedArrayBuffer(args.thisv()));
    args.rval().setInt32(args.thisv().toObject().as<SharedArrayBufferObject>().byteLength());
    return true;
}

bool
SharedArrayBufferObject::byteLengthGetter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsSharedArrayBuffer, byteLengthGetterImpl>(cx, args);
}

bool
SharedArrayBufferObject::fun_isView(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(args.get(0).isObject() &&
                           JS_IsTypedArrayObject(&args.get(0).toObject()));
    return true;
}

bool
SharedArrayBufferObject::class_constructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!args.isConstructing()) {
        if (args.hasDefined(0)) {
            ESClassValue cls;
            if (!GetClassOfValue(cx, args[0], &cls))
                return false;
            if (cls == ESClass_SharedArrayBuffer) {
                args.rval().set(args[0]);
                return true;
            }
        }
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_SHARED_ARRAY_BAD_OBJECT);
        return false;
    }

    // Bugs 1068458, 1161298: Limit length to 2^31-1.
    uint32_t length;
    bool overflow_unused;
    if (!ToLengthClamped(cx, args.get(0), &length, &overflow_unused) || length > INT32_MAX) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_SHARED_ARRAY_BAD_LENGTH);
        return false;
    }

    JSObject* bufobj = New(cx, length);
    if (!bufobj)
        return false;
    args.rval().setObject(*bufobj);
    return true;
}

SharedArrayBufferObject*
SharedArrayBufferObject::New(JSContext* cx, uint32_t length)
{
    SharedArrayRawBuffer* buffer = SharedArrayRawBuffer::New(cx, length);
    if (!buffer)
        return nullptr;

    return New(cx, buffer);
}

SharedArrayBufferObject*
SharedArrayBufferObject::New(JSContext* cx, SharedArrayRawBuffer* buffer)
{
    AutoSetNewObjectMetadata metadata(cx);
    Rooted<SharedArrayBufferObject*> obj(cx, NewBuiltinClassInstance<SharedArrayBufferObject>(cx));
    if (!obj)
        return nullptr;

    MOZ_ASSERT(obj->getClass() == &class_);

    obj->acceptRawBuffer(buffer);

    return obj;
}

void
SharedArrayBufferObject::acceptRawBuffer(SharedArrayRawBuffer* buffer)
{
    setReservedSlot(RAWBUF_SLOT, PrivateValue(buffer));
}

void
SharedArrayBufferObject::dropRawBuffer()
{
    setReservedSlot(RAWBUF_SLOT, UndefinedValue());
}

SharedArrayRawBuffer*
SharedArrayBufferObject::rawBufferObject() const
{
    Value v = getReservedSlot(RAWBUF_SLOT);
    MOZ_ASSERT(!v.isUndefined());
    return reinterpret_cast<SharedArrayRawBuffer*>(v.toPrivate());
}

void
SharedArrayBufferObject::Finalize(FreeOp* fop, JSObject* obj)
{
    SharedArrayBufferObject& buf = obj->as<SharedArrayBufferObject>();

    // Detect the case of failure during SharedArrayBufferObject creation,
    // which causes a SharedArrayRawBuffer to never be attached.
    Value v = buf.getReservedSlot(RAWBUF_SLOT);
    if (!v.isUndefined()) {
        buf.rawBufferObject()->dropReference();
        buf.dropRawBuffer();
    }
}

/* static */ void
SharedArrayBufferObject::addSizeOfExcludingThis(JSObject* obj, mozilla::MallocSizeOf mallocSizeOf,
                                                JS::ClassInfo* info)
{
    info->objectsNonHeapElementsMapped += obj->as<SharedArrayBufferObject>().byteLength();
}

const Class SharedArrayBufferObject::protoClass = {
    "SharedArrayBufferPrototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_SharedArrayBuffer)
};

const Class SharedArrayBufferObject::class_ = {
    "SharedArrayBuffer",
    JSCLASS_DELAY_METADATA_CALLBACK |
    JSCLASS_HAS_RESERVED_SLOTS(SharedArrayBufferObject::RESERVED_SLOTS) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_SharedArrayBuffer),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    SharedArrayBufferObject::Finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    nullptr, /* trace */
    JS_NULL_CLASS_SPEC,
    JS_NULL_CLASS_EXT
};

JSObject*
js::InitSharedArrayBufferClass(JSContext* cx, HandleObject obj)
{
    MOZ_ASSERT(obj->isNative());
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    RootedNativeObject proto(cx, global->createBlankPrototype(cx, &SharedArrayBufferObject::protoClass));
    if (!proto)
        return nullptr;

    RootedFunction ctor(cx, global->createConstructor(cx, SharedArrayBufferObject::class_constructor,
                                                      cx->names().SharedArrayBuffer, 1));
    if (!ctor)
        return nullptr;

    if (!GlobalObject::initBuiltinConstructor(cx, global, JSProto_SharedArrayBuffer, ctor, proto))
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return nullptr;

    RootedId byteLengthId(cx, NameToId(cx->names().byteLength));
    unsigned attrs = JSPROP_SHARED | JSPROP_GETTER | JSPROP_PERMANENT;
    JSObject* getter =
        NewNativeFunction(cx, SharedArrayBufferObject::byteLengthGetter, 0, nullptr);
    if (!getter)
        return nullptr;

    if (!NativeDefineProperty(cx, proto, byteLengthId, UndefinedHandleValue,
                              JS_DATA_TO_FUNC_PTR(GetterOp, getter), nullptr, attrs))
        return nullptr;

    if (!JS_DefineFunctions(cx, ctor, SharedArrayBufferObject::jsstaticfuncs))
        return nullptr;

    if (!JS_DefineFunctions(cx, proto, SharedArrayBufferObject::jsfuncs))
        return nullptr;

    return proto;
}

bool
js::IsSharedArrayBuffer(HandleValue v)
{
    return v.isObject() && v.toObject().is<SharedArrayBufferObject>();
}

bool
js::IsSharedArrayBuffer(HandleObject o)
{
    return o->is<SharedArrayBufferObject>();
}

bool
js::IsSharedArrayBuffer(JSObject* o)
{
    return o->is<SharedArrayBufferObject>();
}

SharedArrayBufferObject&
js::AsSharedArrayBuffer(HandleObject obj)
{
    MOZ_ASSERT(IsSharedArrayBuffer(obj));
    return obj->as<SharedArrayBufferObject>();
}

JS_FRIEND_API(uint32_t)
JS_GetSharedArrayBufferByteLength(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    return obj ? obj->as<SharedArrayBufferObject>().byteLength() : 0;
}

JS_FRIEND_API(void)
js::GetSharedArrayBufferLengthAndData(JSObject* obj, uint32_t* length, bool* isSharedMemory, uint8_t** data)
{
    MOZ_ASSERT(obj->is<SharedArrayBufferObject>());
    *length = obj->as<SharedArrayBufferObject>().byteLength();
    *data = obj->as<SharedArrayBufferObject>().dataPointerShared().unwrap(/*safe - caller knows*/);
    *isSharedMemory = true;
}

JS_FRIEND_API(JSObject*)
JS_NewSharedArrayBuffer(JSContext* cx, uint32_t nbytes)
{
    MOZ_ASSERT(nbytes <= INT32_MAX);
    return SharedArrayBufferObject::New(cx, nbytes);
}

JS_FRIEND_API(bool)
JS_IsSharedArrayBufferObject(JSObject* obj)
{
    obj = CheckedUnwrap(obj);
    return obj ? obj->is<SharedArrayBufferObject>() : false;
}

JS_FRIEND_API(uint8_t*)
JS_GetSharedArrayBufferData(JSObject* obj, bool* isSharedMemory, const JS::AutoCheckCannotGC&)
{
    obj = CheckedUnwrap(obj);
    if (!obj)
        return nullptr;
    *isSharedMemory = true;
    return obj->as<SharedArrayBufferObject>().dataPointerShared().unwrap(/*safe - caller knows*/);
}

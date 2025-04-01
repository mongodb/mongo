/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/ArrayBufferObject-inl.h"
#include "vm/ArrayBufferObject.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/TaggedAnonymousMemory.h"

#include <algorithm>  // std::max, std::min
#include <memory>     // std::uninitialized_copy_n
#include <string.h>
#if !defined(XP_WIN) && !defined(__wasi__)
#  include <sys/mman.h>
#endif
#include <tuple>  // std::tuple
#include <type_traits>
#ifdef MOZ_VALGRIND
#  include <valgrind/memcheck.h>
#endif
#include "jsnum.h"
#include "jstypes.h"

#include "gc/Barrier.h"
#include "gc/Memory.h"
#include "js/ArrayBuffer.h"
#include "js/Conversions.h"
#include "js/experimental/TypedData.h"  // JS_IsArrayBufferViewObject
#include "js/friend/ErrorMessages.h"    // js::GetErrorMessage, JSMSG_*
#include "js/MemoryMetrics.h"
#include "js/Prefs.h"
#include "js/PropertySpec.h"
#include "js/SharedArrayBuffer.h"
#include "js/Wrapper.h"
#include "util/WindowsWrapper.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/JSONPrinter.h"  // js::JSONPrinter
#include "vm/SharedArrayObject.h"
#include "vm/Warnings.h"  // js::WarnNumberASCII
#include "wasm/WasmConstants.h"
#include "wasm/WasmLog.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmProcess.h"

#include "gc/GCContext-inl.h"
#include "gc/Marking-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Realm-inl.h"  // js::AutoRealm

using JS::ToInt32;

using js::wasm::IndexType;
using js::wasm::Pages;
using mozilla::Atomic;
using mozilla::CheckedInt;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

using namespace js;

// Wasm allows large amounts of memory to be reserved at a time. On 64-bit
// platforms (with "huge memories") we reserve around 4GB of virtual address
// space for every wasm memory; on 32-bit platforms we usually do not, but users
// often initialize memories in the hundreds of megabytes.
//
// If too many wasm memories remain live, we run up against system resource
// exhaustion (address space or number of memory map descriptors) - see bug
// 1068684, bug 1073934, bug 1517412, bug 1502733 for details. The limiting case
// seems to be Android on ARM64, where the per-process address space is limited
// to 4TB (39 bits) by the organization of the page tables. An earlier problem
// was Windows Vista Home 64-bit, where the per-process address space is limited
// to 8TB (40 bits). And 32-bit platforms only have 4GB of address space anyway.
//
// Thus we track the amount of memory reserved for wasm, and set a limit per
// process. We trigger GC work when we approach the limit and we throw an OOM
// error if the per-process limit is exceeded. The limit (WasmReservedBytesMax)
// is specific to architecture, OS, and OS configuration.
//
// Since the WasmReservedBytesMax limit is not generally accounted for by
// any existing GC-trigger heuristics, we need an extra heuristic for triggering
// GCs when the caller is allocating memories rapidly without other garbage
// (e.g. bug 1773225). Thus, once the reserved memory crosses the threshold
// WasmReservedBytesStartTriggering, we start triggering GCs every
// WasmReservedBytesPerTrigger bytes. Once we reach
// WasmReservedBytesStartSyncFullGC bytes reserved, we perform expensive
// non-incremental full GCs as a last-ditch effort to avoid unnecessary failure.
// Once we reach WasmReservedBytesMax, we perform further full GCs before giving
// up.
//
// (History: The original implementation only tracked the number of "huge
// memories" allocated by WASM, but this was found to be insufficient because
// 32-bit platforms have similar resource exhaustion issues. We now track
// reserved bytes directly.)
//
// (We also used to reserve significantly more than 4GB for huge memories, but
// this was reduced in bug 1442544.)

// ASAN and TSAN use a ton of vmem for bookkeeping leaving a lot less for the
// program so use a lower limit.
#if defined(MOZ_TSAN) || defined(MOZ_ASAN)
static const uint64_t WasmMemAsanOverhead = 2;
#else
static const uint64_t WasmMemAsanOverhead = 1;
#endif

// WasmReservedStartTriggering + WasmReservedPerTrigger must be well below
// WasmReservedStartSyncFullGC in order to provide enough time for incremental
// GC to do its job.

#if defined(JS_CODEGEN_ARM64) && defined(ANDROID)

static const uint64_t WasmReservedBytesMax =
    75 * wasm::HugeMappedSize / WasmMemAsanOverhead;
static const uint64_t WasmReservedBytesStartTriggering =
    15 * wasm::HugeMappedSize;
static const uint64_t WasmReservedBytesStartSyncFullGC =
    WasmReservedBytesMax - 15 * wasm::HugeMappedSize;
static const uint64_t WasmReservedBytesPerTrigger = 15 * wasm::HugeMappedSize;

#elif defined(WASM_SUPPORTS_HUGE_MEMORY)

static const uint64_t WasmReservedBytesMax =
    1000 * wasm::HugeMappedSize / WasmMemAsanOverhead;
static const uint64_t WasmReservedBytesStartTriggering =
    100 * wasm::HugeMappedSize;
static const uint64_t WasmReservedBytesStartSyncFullGC =
    WasmReservedBytesMax - 100 * wasm::HugeMappedSize;
static const uint64_t WasmReservedBytesPerTrigger = 100 * wasm::HugeMappedSize;

#else  // 32-bit (and weird 64-bit platforms without huge memory)

static const uint64_t GiB = 1024 * 1024 * 1024;

static const uint64_t WasmReservedBytesMax =
    (4 * GiB) / 2 / WasmMemAsanOverhead;
static const uint64_t WasmReservedBytesStartTriggering = (4 * GiB) / 8;
static const uint64_t WasmReservedBytesStartSyncFullGC =
    WasmReservedBytesMax - (4 * GiB) / 8;
static const uint64_t WasmReservedBytesPerTrigger = (4 * GiB) / 8;

#endif

// The total number of bytes reserved for wasm memories.
static Atomic<uint64_t, mozilla::ReleaseAcquire> wasmReservedBytes(0);
// The number of bytes of wasm memory reserved since the last GC trigger.
static Atomic<uint64_t, mozilla::ReleaseAcquire> wasmReservedBytesSinceLast(0);

uint64_t js::WasmReservedBytes() { return wasmReservedBytes; }

[[nodiscard]] static bool CheckArrayBufferTooLarge(JSContext* cx,
                                                   uint64_t nbytes) {
  // Refuse to allocate too large buffers.
  if (MOZ_UNLIKELY(nbytes > ArrayBufferObject::ByteLengthLimit)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BAD_ARRAY_LENGTH);
    return false;
  }

  return true;
}

void* js::MapBufferMemory(wasm::IndexType t, size_t mappedSize,
                          size_t initialCommittedSize) {
  MOZ_ASSERT(mappedSize % gc::SystemPageSize() == 0);
  MOZ_ASSERT(initialCommittedSize % gc::SystemPageSize() == 0);
  MOZ_ASSERT(initialCommittedSize <= mappedSize);

  auto failed = mozilla::MakeScopeExit(
      [&] { wasmReservedBytes -= uint64_t(mappedSize); });
  wasmReservedBytes += uint64_t(mappedSize);

  // Test >= to guard against the case where multiple extant runtimes
  // race to allocate.
  if (wasmReservedBytes >= WasmReservedBytesMax) {
    if (OnLargeAllocationFailure) {
      OnLargeAllocationFailure();
    }
    if (wasmReservedBytes >= WasmReservedBytesMax) {
      return nullptr;
    }
  }

#ifdef XP_WIN
  void* data = VirtualAlloc(nullptr, mappedSize, MEM_RESERVE, PAGE_NOACCESS);
  if (!data) {
    return nullptr;
  }

  if (!VirtualAlloc(data, initialCommittedSize, MEM_COMMIT, PAGE_READWRITE)) {
    VirtualFree(data, 0, MEM_RELEASE);
    return nullptr;
  }
#elif defined(__wasi__)
  void* data = nullptr;
  if (int err = posix_memalign(&data, gc::SystemPageSize(), mappedSize)) {
    MOZ_ASSERT(err == ENOMEM);
    (void)err;
    return nullptr;
  }
  MOZ_ASSERT(data);
  memset(data, 0, mappedSize);
#else   // !XP_WIN && !__wasi__
  void* data =
      MozTaggedAnonymousMmap(nullptr, mappedSize, PROT_NONE,
                             MAP_PRIVATE | MAP_ANON, -1, 0, "wasm-reserved");
  if (data == MAP_FAILED) {
    return nullptr;
  }

  // Note we will waste a page on zero-sized memories here
  if (mprotect(data, initialCommittedSize, PROT_READ | PROT_WRITE)) {
    munmap(data, mappedSize);
    return nullptr;
  }
#endif  // !XP_WIN && !__wasi__

#if defined(MOZ_VALGRIND) && \
    defined(VALGRIND_DISABLE_ADDR_ERROR_REPORTING_IN_RANGE)
  VALGRIND_DISABLE_ADDR_ERROR_REPORTING_IN_RANGE(
      (unsigned char*)data + initialCommittedSize,
      mappedSize - initialCommittedSize);
#endif

  failed.release();
  return data;
}

bool js::CommitBufferMemory(void* dataEnd, size_t delta) {
  MOZ_ASSERT(delta);
  MOZ_ASSERT(delta % gc::SystemPageSize() == 0);

#ifdef XP_WIN
  if (!VirtualAlloc(dataEnd, delta, MEM_COMMIT, PAGE_READWRITE)) {
    return false;
  }
#elif defined(__wasi__)
  // posix_memalign'd memory is already committed
  return true;
#else
  if (mprotect(dataEnd, delta, PROT_READ | PROT_WRITE)) {
    return false;
  }
#endif  // XP_WIN

#if defined(MOZ_VALGRIND) && \
    defined(VALGRIND_DISABLE_ADDR_ERROR_REPORTING_IN_RANGE)
  VALGRIND_ENABLE_ADDR_ERROR_REPORTING_IN_RANGE((unsigned char*)dataEnd, delta);
#endif

  return true;
}

bool js::ExtendBufferMapping(void* dataPointer, size_t mappedSize,
                             size_t newMappedSize) {
  MOZ_ASSERT(mappedSize % gc::SystemPageSize() == 0);
  MOZ_ASSERT(newMappedSize % gc::SystemPageSize() == 0);
  MOZ_ASSERT(newMappedSize >= mappedSize);

#ifdef XP_WIN
  void* mappedEnd = (char*)dataPointer + mappedSize;
  uint32_t delta = newMappedSize - mappedSize;
  if (!VirtualAlloc(mappedEnd, delta, MEM_RESERVE, PAGE_NOACCESS)) {
    return false;
  }
  return true;
#elif defined(__wasi__)
  return false;
#elif defined(XP_LINUX)
  // Note this will not move memory (no MREMAP_MAYMOVE specified)
  if (MAP_FAILED == mremap(dataPointer, mappedSize, newMappedSize, 0)) {
    return false;
  }
  return true;
#else
  // No mechanism for remapping on MacOS and other Unices. Luckily
  // shouldn't need it here as most of these are 64-bit.
  return false;
#endif
}

void js::UnmapBufferMemory(wasm::IndexType t, void* base, size_t mappedSize) {
  MOZ_ASSERT(mappedSize % gc::SystemPageSize() == 0);

#ifdef XP_WIN
  VirtualFree(base, 0, MEM_RELEASE);
#elif defined(__wasi__)
  free(base);
#else
  munmap(base, mappedSize);
#endif  // XP_WIN

#if defined(MOZ_VALGRIND) && \
    defined(VALGRIND_ENABLE_ADDR_ERROR_REPORTING_IN_RANGE)
  VALGRIND_ENABLE_ADDR_ERROR_REPORTING_IN_RANGE((unsigned char*)base,
                                                mappedSize);
#endif

  // Untrack reserved memory *after* releasing memory -- otherwise, a race
  // condition could enable the creation of unlimited buffers.
  wasmReservedBytes -= uint64_t(mappedSize);
}

/*
 * ArrayBufferObject
 *
 * This class holds the underlying raw buffer that the TypedArrayObject classes
 * access.  It can be created explicitly and passed to a TypedArrayObject, or
 * can be created implicitly by constructing a TypedArrayObject with a size.
 */

/*
 * ArrayBufferObject (base)
 */

static const JSClassOps ArrayBufferObjectClassOps = {
    nullptr,                      // addProperty
    nullptr,                      // delProperty
    nullptr,                      // enumerate
    nullptr,                      // newEnumerate
    nullptr,                      // resolve
    nullptr,                      // mayResolve
    ArrayBufferObject::finalize,  // finalize
    nullptr,                      // call
    nullptr,                      // construct
    nullptr,                      // trace
};

static const JSFunctionSpec arraybuffer_functions[] = {
    JS_FN("isView", ArrayBufferObject::fun_isView, 1, 0),
    JS_FS_END,
};

static const JSPropertySpec arraybuffer_properties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "$ArrayBufferSpecies", 0),
    JS_PS_END,
};

static const JSFunctionSpec arraybuffer_proto_functions[] = {
    JS_SELF_HOSTED_FN("slice", "ArrayBufferSlice", 2, 0),
    JS_FN("resize", ArrayBufferObject::resize, 1, 0),
    JS_FN("transfer", ArrayBufferObject::transfer, 0, 0),
    JS_FN("transferToFixedLength", ArrayBufferObject::transferToFixedLength, 0,
          0),
    JS_FS_END,
};

static const JSPropertySpec arraybuffer_proto_properties[] = {
    JS_PSG("byteLength", ArrayBufferObject::byteLengthGetter, 0),
    JS_PSG("maxByteLength", ArrayBufferObject::maxByteLengthGetter, 0),
    JS_PSG("resizable", ArrayBufferObject::resizableGetter, 0),
    JS_PSG("detached", ArrayBufferObject::detachedGetter, 0),
    JS_STRING_SYM_PS(toStringTag, "ArrayBuffer", JSPROP_READONLY),
    JS_PS_END,
};

static JSObject* CreateArrayBufferPrototype(JSContext* cx, JSProtoKey key) {
  return GlobalObject::createBlankPrototype(cx, cx->global(),
                                            &ArrayBufferObject::protoClass_);
}

static const ClassSpec ArrayBufferObjectClassSpec = {
    GenericCreateConstructor<ArrayBufferObject::class_constructor, 1,
                             gc::AllocKind::FUNCTION>,
    CreateArrayBufferPrototype,
    arraybuffer_functions,
    arraybuffer_properties,
    arraybuffer_proto_functions,
    arraybuffer_proto_properties,
};

static const ClassExtension FixedLengthArrayBufferObjectClassExtension = {
    ArrayBufferObject::objectMoved<
        FixedLengthArrayBufferObject>,  // objectMovedOp
};

static const ClassExtension ResizableArrayBufferObjectClassExtension = {
    ArrayBufferObject::objectMoved<
        ResizableArrayBufferObject>,  // objectMovedOp
};

const JSClass ArrayBufferObject::protoClass_ = {
    "ArrayBuffer.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_ArrayBuffer),
    JS_NULL_CLASS_OPS,
    &ArrayBufferObjectClassSpec,
};

const JSClass FixedLengthArrayBufferObject::class_ = {
    "ArrayBuffer",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_ArrayBuffer) |
        JSCLASS_BACKGROUND_FINALIZE,
    &ArrayBufferObjectClassOps,
    &ArrayBufferObjectClassSpec,
    &FixedLengthArrayBufferObjectClassExtension,
};

const JSClass ResizableArrayBufferObject::class_ = {
    "ArrayBuffer",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_ArrayBuffer) |
        JSCLASS_BACKGROUND_FINALIZE,
    &ArrayBufferObjectClassOps,
    &ArrayBufferObjectClassSpec,
    &ResizableArrayBufferObjectClassExtension,
};

static bool IsArrayBuffer(HandleValue v) {
  return v.isObject() && v.toObject().is<ArrayBufferObject>();
}

static bool IsResizableArrayBuffer(HandleValue v) {
  return v.isObject() && v.toObject().is<ResizableArrayBufferObject>();
}

MOZ_ALWAYS_INLINE bool ArrayBufferObject::byteLengthGetterImpl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsArrayBuffer(args.thisv()));
  auto* buffer = &args.thisv().toObject().as<ArrayBufferObject>();
  args.rval().setNumber(buffer->byteLength());
  return true;
}

bool ArrayBufferObject::byteLengthGetter(JSContext* cx, unsigned argc,
                                         Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsArrayBuffer, byteLengthGetterImpl>(cx, args);
}

enum class PreserveResizability : bool { No, Yes };

/**
 * ArrayBufferCopyAndDetach ( arrayBuffer, newLength, preserveResizability )
 *
 * https://tc39.es/proposal-arraybuffer-transfer/#sec-arraybuffercopyanddetach
 */
static ArrayBufferObject* ArrayBufferCopyAndDetach(
    JSContext* cx, Handle<ArrayBufferObject*> arrayBuffer,
    Handle<Value> newLength, PreserveResizability preserveResizability) {
  // Steps 1-2. (Not applicable in our implementation.)

  // Steps 3-4.
  uint64_t newByteLength;
  if (newLength.isUndefined()) {
    // Step 3.a.
    newByteLength = arrayBuffer->byteLength();
  } else {
    // Step 4.a.
    if (!ToIndex(cx, newLength, &newByteLength)) {
      return nullptr;
    }
  }

  // Step 5.
  if (arrayBuffer->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }
  if (arrayBuffer->isLengthPinned()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    return nullptr;
  }

  // Steps 6-7.
  mozilla::Maybe<size_t> maxByteLength;
  if (preserveResizability == PreserveResizability::Yes &&
      arrayBuffer->isResizable()) {
    auto* resizableBuffer = &arrayBuffer->as<ResizableArrayBufferObject>();
    maxByteLength = mozilla::Some(resizableBuffer->maxByteLength());
  }

  // Step 8.
  if (arrayBuffer->hasDefinedDetachKey()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_NO_TRANSFER);
    return nullptr;
  }

  // Steps 9-16.
  //
  // 25.1.2.1 AllocateArrayBuffer, step 2.
  // 6.2.9.1 CreateByteDataBlock, step 2.
  if (!CheckArrayBufferTooLarge(cx, newByteLength)) {
    return nullptr;
  }

  if (maxByteLength) {
    if (size_t(newByteLength) > *maxByteLength) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_ARRAYBUFFER_LENGTH_LARGER_THAN_MAXIMUM);
      return nullptr;
    }

    Rooted<ResizableArrayBufferObject*> resizableBuffer(
        cx, &arrayBuffer->as<ResizableArrayBufferObject>());
    return ResizableArrayBufferObject::copyAndDetach(cx, size_t(newByteLength),
                                                     resizableBuffer);
  }

  return ArrayBufferObject::copyAndDetach(cx, size_t(newByteLength),
                                          arrayBuffer);
}

/**
 * get ArrayBuffer.prototype.maxByteLength
 *
 * https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.maxbytelength
 */
bool ArrayBufferObject::maxByteLengthGetterImpl(JSContext* cx,
                                                const CallArgs& args) {
  MOZ_ASSERT(IsArrayBuffer(args.thisv()));

  auto* buffer = &args.thisv().toObject().as<ArrayBufferObject>();

  // Steps 4-6.
  size_t maxByteLength = buffer->maxByteLength();
  MOZ_ASSERT_IF(buffer->isDetached(), maxByteLength == 0);

  // Step 7.
  args.rval().setNumber(maxByteLength);
  return true;
}

/**
 * get ArrayBuffer.prototype.maxByteLength
 *
 * https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.maxbytelength
 */
bool ArrayBufferObject::maxByteLengthGetter(JSContext* cx, unsigned argc,
                                            Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsArrayBuffer, maxByteLengthGetterImpl>(cx, args);
}

/**
 * get ArrayBuffer.prototype.resizable
 *
 * https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.resizable
 */
bool ArrayBufferObject::resizableGetterImpl(JSContext* cx,
                                            const CallArgs& args) {
  MOZ_ASSERT(IsArrayBuffer(args.thisv()));

  // Step 4.
  auto* buffer = &args.thisv().toObject().as<ArrayBufferObject>();
  args.rval().setBoolean(buffer->isResizable());
  return true;
}

/**
 * get ArrayBuffer.prototype.resizable
 *
 * https://tc39.es/ecma262/#sec-get-arraybuffer.prototype.resizable
 */
bool ArrayBufferObject::resizableGetter(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsArrayBuffer, resizableGetterImpl>(cx, args);
}

/**
 * get ArrayBuffer.prototype.detached
 *
 * https://tc39.es/proposal-arraybuffer-transfer/#sec-get-arraybuffer.prototype.detached
 */
bool ArrayBufferObject::detachedGetterImpl(JSContext* cx,
                                           const CallArgs& args) {
  MOZ_ASSERT(IsArrayBuffer(args.thisv()));

  // Step 4.
  auto* buffer = &args.thisv().toObject().as<ArrayBufferObject>();
  args.rval().setBoolean(buffer->isDetached());
  return true;
}

/**
 * get ArrayBuffer.prototype.detached
 *
 * https://tc39.es/proposal-arraybuffer-transfer/#sec-get-arraybuffer.prototype.detached
 */
bool ArrayBufferObject::detachedGetter(JSContext* cx, unsigned argc,
                                       Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsArrayBuffer, detachedGetterImpl>(cx, args);
}

/**
 * ArrayBuffer.prototype.transfer ( [ newLength ] )
 *
 * https://tc39.es/proposal-arraybuffer-transfer/#sec-arraybuffer.prototype.transfer
 */
bool ArrayBufferObject::transferImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsArrayBuffer(args.thisv()));

  // Steps 1-2.
  Rooted<ArrayBufferObject*> buffer(
      cx, &args.thisv().toObject().as<ArrayBufferObject>());
  auto* newBuffer = ArrayBufferCopyAndDetach(cx, buffer, args.get(0),
                                             PreserveResizability::Yes);
  if (!newBuffer) {
    return false;
  }

  args.rval().setObject(*newBuffer);
  return true;
}

/**
 * ArrayBuffer.prototype.transfer ( [ newLength ] )
 *
 * https://tc39.es/proposal-arraybuffer-transfer/#sec-arraybuffer.prototype.transfer
 */
bool ArrayBufferObject::transfer(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsArrayBuffer, transferImpl>(cx, args);
}

/**
 * ArrayBuffer.prototype.transferToFixedLength ( [ newLength ] )
 *
 * https://tc39.es/proposal-arraybuffer-transfer/#sec-arraybuffer.prototype.transfertofixedlength
 */
bool ArrayBufferObject::transferToFixedLengthImpl(JSContext* cx,
                                                  const CallArgs& args) {
  MOZ_ASSERT(IsArrayBuffer(args.thisv()));

  // Steps 1-2.
  Rooted<ArrayBufferObject*> buffer(
      cx, &args.thisv().toObject().as<ArrayBufferObject>());
  auto* newBuffer = ArrayBufferCopyAndDetach(cx, buffer, args.get(0),
                                             PreserveResizability::No);
  if (!newBuffer) {
    return false;
  }

  args.rval().setObject(*newBuffer);
  return true;
}

/**
 * ArrayBuffer.prototype.transferToFixedLength ( [ newLength ] )
 *
 * https://tc39.es/proposal-arraybuffer-transfer/#sec-arraybuffer.prototype.transfertofixedlength
 */
bool ArrayBufferObject::transferToFixedLength(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsArrayBuffer, transferToFixedLengthImpl>(cx,
                                                                        args);
}

/**
 * ArrayBuffer.prototype.resize ( newLength )
 *
 * https://tc39.es/ecma262/#sec-arraybuffer.prototype.resize
 */
bool ArrayBufferObject::resizeImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsResizableArrayBuffer(args.thisv()));

  Rooted<ResizableArrayBufferObject*> obj(
      cx, &args.thisv().toObject().as<ResizableArrayBufferObject>());

  // Step 4.
  uint64_t newByteLength;
  if (!ToIndex(cx, args.get(0), &newByteLength)) {
    return false;
  }

  // Step 5.
  if (obj->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }
  if (obj->isLengthPinned()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    return false;
  }

  // Step 6.
  if (newByteLength > obj->maxByteLength()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_LARGER_THAN_MAXIMUM);
    return false;
  }

  // Steps 7-15.
  obj->resize(size_t(newByteLength));

  // Step 16.
  args.rval().setUndefined();
  return true;
}

/**
 * ArrayBuffer.prototype.resize ( newLength )
 *
 * https://tc39.es/ecma262/#sec-arraybuffer.prototype.resize
 */
bool ArrayBufferObject::resize(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsResizableArrayBuffer, resizeImpl>(cx, args);
}

/*
 * ArrayBuffer.isView(obj); ES6 (Dec 2013 draft) 24.1.3.1
 */
bool ArrayBufferObject::fun_isView(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setBoolean(args.get(0).isObject() &&
                         JS_IsArrayBufferViewObject(&args.get(0).toObject()));
  return true;
}

// ES2024 draft rev 3a773fc9fae58be023228b13dbbd402ac18eeb6b
// 25.1.4.1 ArrayBuffer ( length [ , options ] )
bool ArrayBufferObject::class_constructor(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "ArrayBuffer")) {
    return false;
  }

  // Step 2.
  uint64_t byteLength;
  if (!ToIndex(cx, args.get(0), &byteLength)) {
    return false;
  }

  // Step 3.
  mozilla::Maybe<uint64_t> maxByteLength;
  if (JS::Prefs::experimental_arraybuffer_resizable()) {
    // Inline call to GetArrayBufferMaxByteLengthOption.
    if (args.get(1).isObject()) {
      Rooted<JSObject*> options(cx, &args[1].toObject());

      Rooted<Value> val(cx);
      if (!GetProperty(cx, options, options, cx->names().maxByteLength, &val)) {
        return false;
      }
      if (!val.isUndefined()) {
        uint64_t maxByteLengthInt;
        if (!ToIndex(cx, val, &maxByteLengthInt)) {
          return false;
        }

        // 25.1.3.1 AllocateArrayBuffer, step 3.a.
        if (byteLength > maxByteLengthInt) {
          JS_ReportErrorNumberASCII(
              cx, GetErrorMessage, nullptr,
              JSMSG_ARRAYBUFFER_LENGTH_LARGER_THAN_MAXIMUM);
          return false;
        }
        maxByteLength = mozilla::Some(maxByteLengthInt);
      }
    }
  }

  // Step 4 (Inlined 25.1.3.1 AllocateArrayBuffer).
  // 25.1.3.1, step 4 (Inlined 10.1.13 OrdinaryCreateFromConstructor, step 2).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_ArrayBuffer,
                                          &proto)) {
    return false;
  }

  // 25.1.3.1, step 5 (Inlined 6.2.9.1 CreateByteDataBlock, step 2).
  if (!CheckArrayBufferTooLarge(cx, byteLength)) {
    return false;
  }

  if (maxByteLength) {
    // 25.1.3.1, step 8.a.
    if (!CheckArrayBufferTooLarge(cx, *maxByteLength)) {
      return false;
    }

    // 25.1.3.1, remaining steps.
    auto* bufobj = ResizableArrayBufferObject::createZeroed(
        cx, byteLength, *maxByteLength, proto);
    if (!bufobj) {
      return false;
    }
    args.rval().setObject(*bufobj);
    return true;
  }

  // 25.1.3.1, remaining steps.
  JSObject* bufobj = createZeroed(cx, byteLength, proto);
  if (!bufobj) {
    return false;
  }
  args.rval().setObject(*bufobj);
  return true;
}

using ArrayBufferContents = UniquePtr<uint8_t[], JS::FreePolicy>;

static ArrayBufferContents AllocateUninitializedArrayBufferContents(
    JSContext* cx, size_t nbytes) {
  // First attempt a normal allocation.
  uint8_t* p =
      cx->maybe_pod_arena_malloc<uint8_t>(js::ArrayBufferContentsArena, nbytes);
  if (MOZ_UNLIKELY(!p)) {
    // Otherwise attempt a large allocation, calling the
    // large-allocation-failure callback if necessary.
    p = static_cast<uint8_t*>(cx->runtime()->onOutOfMemoryCanGC(
        js::AllocFunction::Malloc, js::ArrayBufferContentsArena, nbytes));
    if (!p) {
      MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode,
                            "OOM in AllocateUninitializedArrayBufferContents");
      ReportOutOfMemory(cx);
    }
  }

  return ArrayBufferContents(p);
}

static ArrayBufferContents AllocateArrayBufferContents(JSContext* cx,
                                                       size_t nbytes) {
  // First attempt a normal allocation.
  uint8_t* p =
      cx->maybe_pod_arena_calloc<uint8_t>(js::ArrayBufferContentsArena, nbytes);
  if (MOZ_UNLIKELY(!p)) {
    // Otherwise attempt a large allocation, calling the
    // large-allocation-failure callback if necessary.
    p = static_cast<uint8_t*>(cx->runtime()->onOutOfMemoryCanGC(
        js::AllocFunction::Calloc, js::ArrayBufferContentsArena, nbytes));
    if (!p) {
      ReportOutOfMemory(cx);
    }
  }

  return ArrayBufferContents(p);
}

static ArrayBufferContents ReallocateArrayBufferContents(JSContext* cx,
                                                         uint8_t* old,
                                                         size_t oldSize,
                                                         size_t newSize) {
  // First attempt a normal reallocation.
  uint8_t* p = cx->maybe_pod_arena_realloc<uint8_t>(
      js::ArrayBufferContentsArena, old, oldSize, newSize);
  if (MOZ_UNLIKELY(!p)) {
    // Otherwise attempt a large allocation, calling the
    // large-allocation-failure callback if necessary.
    p = static_cast<uint8_t*>(cx->runtime()->onOutOfMemoryCanGC(
        js::AllocFunction::Realloc, js::ArrayBufferContentsArena, newSize,
        old));
    if (!p) {
      ReportOutOfMemory(cx);
    }
  }

  return ArrayBufferContents(p);
}

static ArrayBufferContents NewCopiedBufferContents(
    JSContext* cx, Handle<ArrayBufferObject*> buffer) {
  ArrayBufferContents dataCopy =
      AllocateUninitializedArrayBufferContents(cx, buffer->byteLength());
  if (dataCopy) {
    if (auto count = buffer->byteLength()) {
      memcpy(dataCopy.get(), buffer->dataPointer(), count);
    }
  }
  return dataCopy;
}

/* static */
void ArrayBufferObject::detach(JSContext* cx,
                               Handle<ArrayBufferObject*> buffer) {
  cx->check(buffer);
  MOZ_ASSERT(!buffer->isPreparedForAsmJS());
  MOZ_ASSERT(!buffer->isLengthPinned());

  // Update all views of the buffer to account for the buffer having been
  // detached, and clear the buffer's data and list of views.

  auto& innerViews = ObjectRealm::get(buffer).innerViews.get();
  if (InnerViewTable::ViewVector* views =
          innerViews.maybeViewsUnbarriered(buffer)) {
    for (size_t i = 0; i < views->length(); i++) {
      JSObject* view = (*views)[i];
      view->as<ArrayBufferViewObject>().notifyBufferDetached();
    }
    innerViews.removeViews(buffer);
  }
  if (JSObject* view = buffer->firstView()) {
    view->as<ArrayBufferViewObject>().notifyBufferDetached();
    buffer->setFirstView(nullptr);
  }

  if (buffer->dataPointer()) {
    buffer->releaseData(cx->gcContext());
    buffer->setDataPointer(BufferContents::createNoData());
  }

  buffer->setByteLength(0);
  buffer->setIsDetached();
  if (buffer->isResizable()) {
    buffer->as<ResizableArrayBufferObject>().setMaxByteLength(0);
  }
}

void ResizableArrayBufferObject::resize(size_t newByteLength) {
  MOZ_ASSERT(!isPreparedForAsmJS());
  MOZ_ASSERT(!isWasm());
  MOZ_ASSERT(!isDetached());
  MOZ_ASSERT(!isLengthPinned());
  MOZ_ASSERT(isResizable());
  MOZ_ASSERT(newByteLength <= maxByteLength());

  // Clear the bytes between `data[newByteLength..oldByteLength]` when
  // shrinking the buffer. We don't need to clear any bytes when growing the
  // buffer, because the new space was either initialized to zero when creating
  // the buffer, or a prior shrink zeroed it out here.
  size_t oldByteLength = byteLength();
  if (newByteLength < oldByteLength) {
    size_t nbytes = oldByteLength - newByteLength;
    memset(dataPointer() + newByteLength, 0, nbytes);
  }

  setByteLength(newByteLength);

  // Update all views of the buffer to account for the buffer having been
  // resized.

  auto& innerViews = ObjectRealm::get(this).innerViews.get();
  if (InnerViewTable::ViewVector* views =
          innerViews.maybeViewsUnbarriered(this)) {
    for (auto& view : *views) {
      view->notifyBufferResized();
    }
  }
  if (auto* view = firstView()) {
    view->as<ArrayBufferViewObject>().notifyBufferResized();
  }
}

/* clang-format off */
/*
 * [SMDOC] WASM Linear Memory structure
 *
 * Wasm Raw Buf Linear Memory Structure
 *
 * The linear heap in Wasm is an mmaped array buffer. Several constants manage
 * its lifetime:
 *
 *  - byteLength - the wasm-visible current length of the buffer in
 *    bytes. Accesses in the range [0, byteLength] succeed. May only increase.
 *
 *  - boundsCheckLimit - the size against which we perform bounds checks.  The
 *    value of this depends on the bounds checking strategy chosen for the array
 *    buffer and the specific bounds checking semantics.  For asm.js code and
 *    for wasm code running with explicit bounds checking, it is the always the
 *    same as the byteLength.  For wasm code using the huge-memory trick, it is
 *    always wasm::GuardSize smaller than mappedSize.
 *
 *    See also "Linear memory addresses and bounds checking" in
 *    wasm/WasmMemory.cpp.
 *
 *    See also WasmMemoryObject::boundsCheckLimit().
 *
 *  - sourceMaxSize - the optional declared limit on how far byteLength can grow
 *    in pages. This is the unmodified maximum size from the source module or
 *    JS-API invocation. This may not be representable in byte lengths, nor
 *    feasible for a module to actually grow to due to implementation limits.
 *    It is used for correct linking checks and js-types reflection.
 *
 *  - clampedMaxSize - the maximum size on how far the byteLength can grow in
 *    pages. This value respects implementation limits and is always
 *    representable as a byte length. Every memory has a clampedMaxSize, even if
 *    no maximum was specified in source. When a memory has no sourceMaxSize,
 *    the clampedMaxSize will be the maximum amount of memory that can be grown
 *    to while still respecting implementation limits.
 *
 *  - mappedSize - the actual mmapped size. Access in the range [0, mappedSize]
 *    will either succeed, or be handled by the wasm signal handlers. If
 *    sourceMaxSize is present at initialization, then we attempt to map the
 *    whole clampedMaxSize. Otherwise we only map the region needed for the
 *    initial size.
 *
 * The below diagram shows the layout of the wasm heap. The wasm-visible portion
 * of the heap starts at 0. There is one extra page prior to the start of the
 * wasm heap which contains the WasmArrayRawBuffer struct at its end (i.e. right
 * before the start of the WASM heap).
 *
 *  WasmArrayRawBuffer
 *      \    ArrayBufferObject::dataPointer()
 *       \  /
 *        \ |
 *  ______|_|______________________________________________________
 * |______|_|______________|___________________|___________________|
 *          0          byteLength          clampedMaxSize     mappedSize
 *
 * \_______________________/
 *          COMMITED
 *                          \_____________________________________/
 *                                           SLOP
 * \______________________________________________________________/
 *                         MAPPED
 *
 * Invariants on byteLength, clampedMaxSize, and mappedSize:
 *  - byteLength only increases
 *  - 0 <= byteLength <= clampedMaxSize <= mappedSize
 *  - if sourceMaxSize is not specified, mappedSize may grow.
 *    It is otherwise constant.
 *  - initialLength <= clampedMaxSize <= sourceMaxSize (if present)
 *  - clampedMaxSize <= wasm::MaxMemoryPages()
 *
 * Invariants on boundsCheckLimit:
 *  - for wasm code with the huge-memory trick,
 *      clampedMaxSize <= boundsCheckLimit <= mappedSize
 *  - for asm.js code or wasm with explicit bounds checking,
 *      byteLength == boundsCheckLimit <= clampedMaxSize
 *  - on ARM, boundsCheckLimit must be a valid ARM immediate.
 *  - if sourceMaxSize is not specified, boundsCheckLimit may grow as
 *    mappedSize grows. They are otherwise constant.

 * NOTE: For asm.js on 32-bit platforms and on all platforms when running with
 * explicit bounds checking, we guarantee that
 *
 *   byteLength == maxSize == boundsCheckLimit == mappedSize
 *
 * That is, signal handlers will not be invoked.
 *
 * The region between byteLength and mappedSize is the SLOP - an area where we use
 * signal handlers to catch things that slip by bounds checks. Logically it has
 * two parts:
 *
 *  - from byteLength to boundsCheckLimit - this part of the SLOP serves to catch
 *    accesses to memory we have reserved but not yet grown into. This allows us
 *    to grow memory up to max (when present) without having to patch/update the
 *    bounds checks.
 *
 *  - from boundsCheckLimit to mappedSize - this part of the SLOP allows us to
 *    bounds check against base pointers and fold some constant offsets inside
 *    loads. This enables better Bounds Check Elimination.  See "Linear memory
 *    addresses and bounds checking" in wasm/WasmMemory.cpp.
 *
 */
/* clang-format on */

[[nodiscard]] bool WasmArrayRawBuffer::growToPagesInPlace(Pages newPages) {
  size_t newSize = newPages.byteLength();
  size_t oldSize = byteLength();

  MOZ_ASSERT(newSize >= oldSize);
  MOZ_ASSERT(newPages <= clampedMaxPages());
  MOZ_ASSERT(newSize <= mappedSize());

  size_t delta = newSize - oldSize;
  MOZ_ASSERT(delta % wasm::PageSize == 0);

  uint8_t* dataEnd = dataPointer() + oldSize;
  MOZ_ASSERT(uintptr_t(dataEnd) % gc::SystemPageSize() == 0);

  if (delta && !CommitBufferMemory(dataEnd, delta)) {
    return false;
  }

  length_ = newSize;

  return true;
}

bool WasmArrayRawBuffer::extendMappedSize(Pages maxPages) {
  size_t newMappedSize = wasm::ComputeMappedSize(maxPages);
  MOZ_ASSERT(mappedSize_ <= newMappedSize);
  if (mappedSize_ == newMappedSize) {
    return true;
  }

  if (!ExtendBufferMapping(dataPointer(), mappedSize_, newMappedSize)) {
    return false;
  }

  mappedSize_ = newMappedSize;
  return true;
}

void WasmArrayRawBuffer::tryGrowMaxPagesInPlace(Pages deltaMaxPages) {
  Pages newMaxPages = clampedMaxPages_;

  DebugOnly<bool> valid = newMaxPages.checkedIncrement(deltaMaxPages);
  // Caller must ensure increment does not overflow or increase over the
  // specified maximum pages.
  MOZ_ASSERT(valid);
  MOZ_ASSERT_IF(sourceMaxPages_.isSome(), newMaxPages <= *sourceMaxPages_);

  if (!extendMappedSize(newMaxPages)) {
    return;
  }
  clampedMaxPages_ = newMaxPages;
}

void WasmArrayRawBuffer::discard(size_t byteOffset, size_t byteLen) {
  uint8_t* memBase = dataPointer();

  // The caller is responsible for ensuring these conditions are met; see this
  // function's comment in ArrayBufferObject.h.
  MOZ_ASSERT(byteOffset % wasm::PageSize == 0);
  MOZ_ASSERT(byteLen % wasm::PageSize == 0);
  MOZ_ASSERT(wasm::MemoryBoundsCheck(uint64_t(byteOffset), uint64_t(byteLen),
                                     byteLength()));

  // Discarding zero bytes "succeeds" with no effect.
  if (byteLen == 0) {
    return;
  }

  void* addr = memBase + uintptr_t(byteOffset);

  // On POSIX-ish platforms, we discard memory by overwriting previously-mapped
  // pages with freshly-mapped pages (which are all zeroed). The operating
  // system recognizes this and decreases the process RSS, and eventually
  // collects the abandoned physical pages.
  //
  // On Windows, committing over previously-committed pages has no effect, and
  // the memory must be explicitly decommitted first. This is not the same as an
  // munmap; the address space is still reserved.

#ifdef XP_WIN
  if (!VirtualFree(addr, byteLen, MEM_DECOMMIT)) {
    MOZ_CRASH("wasm discard: failed to decommit memory");
  }
  if (!VirtualAlloc(addr, byteLen, MEM_COMMIT, PAGE_READWRITE)) {
    MOZ_CRASH("wasm discard: decommitted memory but failed to recommit");
  };
#elif defined(__wasi__)
  memset(addr, 0, byteLen);
#else  // !XP_WIN
  void* data = MozTaggedAnonymousMmap(addr, byteLen, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0,
                                      "wasm-reserved");
  if (data == MAP_FAILED) {
    MOZ_CRASH("failed to discard wasm memory; memory mappings may be broken");
  }
#endif
}

/* static */
WasmArrayRawBuffer* WasmArrayRawBuffer::AllocateWasm(
    IndexType indexType, Pages initialPages, Pages clampedMaxPages,
    const Maybe<Pages>& sourceMaxPages, const Maybe<size_t>& mapped) {
  // Prior code has asserted that initial pages is within our implementation
  // limits (wasm::MaxMemoryPages) and we can assume it is a valid size_t.
  MOZ_ASSERT(initialPages.hasByteLength());
  size_t numBytes = initialPages.byteLength();

  // If there is a specified maximum, attempt to map the whole range for
  // clampedMaxPages. Or else map only what's required for initialPages.
  Pages initialMappedPages =
      sourceMaxPages.isSome() ? clampedMaxPages : initialPages;

  // Use an override mapped size, or else compute the mapped size from
  // initialMappedPages.
  size_t mappedSize =
      mapped.isSome() ? *mapped : wasm::ComputeMappedSize(initialMappedPages);

  MOZ_RELEASE_ASSERT(mappedSize <= SIZE_MAX - gc::SystemPageSize());
  MOZ_RELEASE_ASSERT(numBytes <= SIZE_MAX - gc::SystemPageSize());
  MOZ_RELEASE_ASSERT(initialPages <= clampedMaxPages);
  MOZ_ASSERT(numBytes % gc::SystemPageSize() == 0);
  MOZ_ASSERT(mappedSize % gc::SystemPageSize() == 0);

  uint64_t mappedSizeWithHeader = mappedSize + gc::SystemPageSize();
  uint64_t numBytesWithHeader = numBytes + gc::SystemPageSize();

  void* data = MapBufferMemory(indexType, (size_t)mappedSizeWithHeader,
                               (size_t)numBytesWithHeader);
  if (!data) {
    return nullptr;
  }

  uint8_t* base = reinterpret_cast<uint8_t*>(data) + gc::SystemPageSize();
  uint8_t* header = base - sizeof(WasmArrayRawBuffer);

  auto rawBuf = new (header) WasmArrayRawBuffer(
      indexType, base, clampedMaxPages, sourceMaxPages, mappedSize, numBytes);
  return rawBuf;
}

/* static */
void WasmArrayRawBuffer::Release(void* mem) {
  WasmArrayRawBuffer* header =
      (WasmArrayRawBuffer*)((uint8_t*)mem - sizeof(WasmArrayRawBuffer));

  MOZ_RELEASE_ASSERT(header->mappedSize() <= SIZE_MAX - gc::SystemPageSize());
  size_t mappedSizeWithHeader = header->mappedSize() + gc::SystemPageSize();

  static_assert(std::is_trivially_destructible_v<WasmArrayRawBuffer>,
                "no need to call the destructor");

  UnmapBufferMemory(header->indexType(), header->basePointer(),
                    mappedSizeWithHeader);
}

WasmArrayRawBuffer* ArrayBufferObject::BufferContents::wasmBuffer() const {
  MOZ_RELEASE_ASSERT(kind_ == WASM);
  return (WasmArrayRawBuffer*)(data_ - sizeof(WasmArrayRawBuffer));
}

template <typename ObjT, typename RawbufT>
static ArrayBufferObjectMaybeShared* CreateSpecificWasmBuffer(
    JSContext* cx, const wasm::MemoryDesc& memory) {
  bool useHugeMemory = wasm::IsHugeMemoryEnabled(memory.indexType());
  Pages initialPages = memory.initialPages();
  Maybe<Pages> sourceMaxPages = memory.maximumPages();
  Pages clampedMaxPages = wasm::ClampedMaxPages(
      memory.indexType(), initialPages, sourceMaxPages, useHugeMemory);

  Maybe<size_t> mappedSize;
#ifdef WASM_SUPPORTS_HUGE_MEMORY
  // Override the mapped size if we are using huge memory. If we are not, then
  // it will be calculated by the raw buffer we are using.
  if (useHugeMemory) {
    mappedSize = Some(wasm::HugeMappedSize);
  }
#endif

  RawbufT* buffer =
      RawbufT::AllocateWasm(memory.limits.indexType, initialPages,
                            clampedMaxPages, sourceMaxPages, mappedSize);
  if (!buffer) {
    if (useHugeMemory) {
      WarnNumberASCII(cx, JSMSG_WASM_HUGE_MEMORY_FAILED);
      if (cx->isExceptionPending()) {
        cx->clearPendingException();
      }

      ReportOutOfMemory(cx);
      return nullptr;
    }

    // If we fail, and have a sourceMaxPages, try to reserve the biggest
    // chunk in the range [initialPages, clampedMaxPages) using log backoff.
    if (!sourceMaxPages) {
      wasm::Log(cx, "new Memory({initial=%" PRIu64 " pages}) failed",
                initialPages.value());
      ReportOutOfMemory(cx);
      return nullptr;
    }

    uint64_t cur = clampedMaxPages.value() / 2;
    for (; Pages(cur) > initialPages; cur /= 2) {
      buffer = RawbufT::AllocateWasm(memory.limits.indexType, initialPages,
                                     Pages(cur), sourceMaxPages, mappedSize);
      if (buffer) {
        break;
      }
    }

    if (!buffer) {
      wasm::Log(cx, "new Memory({initial=%" PRIu64 " pages}) failed",
                initialPages.value());
      ReportOutOfMemory(cx);
      return nullptr;
    }

    // Try to grow our chunk as much as possible.
    for (size_t d = cur / 2; d >= 1; d /= 2) {
      buffer->tryGrowMaxPagesInPlace(Pages(d));
    }
  }

  // ObjT::createFromNewRawBuffer assumes ownership of |buffer| even in case
  // of failure.
  Rooted<ArrayBufferObjectMaybeShared*> object(
      cx, ObjT::createFromNewRawBuffer(cx, buffer, initialPages.byteLength()));
  if (!object) {
    return nullptr;
  }

  // See MaximumLiveMappedBuffers comment above.
  if (wasmReservedBytes > WasmReservedBytesStartSyncFullGC) {
    JS::PrepareForFullGC(cx);
    JS::NonIncrementalGC(cx, JS::GCOptions::Normal,
                         JS::GCReason::TOO_MUCH_WASM_MEMORY);
    wasmReservedBytesSinceLast = 0;
  } else if (wasmReservedBytes > WasmReservedBytesStartTriggering) {
    wasmReservedBytesSinceLast += uint64_t(buffer->mappedSize());
    if (wasmReservedBytesSinceLast > WasmReservedBytesPerTrigger) {
      (void)cx->runtime()->gc.triggerGC(JS::GCReason::TOO_MUCH_WASM_MEMORY);
      wasmReservedBytesSinceLast = 0;
    }
  } else {
    wasmReservedBytesSinceLast = 0;
  }

  // Log the result with details on the memory allocation
  if (sourceMaxPages) {
    if (useHugeMemory) {
      wasm::Log(cx,
                "new Memory({initial:%" PRIu64 " pages, maximum:%" PRIu64
                " pages}) succeeded",
                initialPages.value(), sourceMaxPages->value());
    } else {
      wasm::Log(cx,
                "new Memory({initial:%" PRIu64 " pages, maximum:%" PRIu64
                " pages}) succeeded "
                "with internal maximum of %" PRIu64 " pages",
                initialPages.value(), sourceMaxPages->value(),
                object->wasmClampedMaxPages().value());
    }
  } else {
    wasm::Log(cx, "new Memory({initial:%" PRIu64 " pages}) succeeded",
              initialPages.value());
  }

  return object;
}

ArrayBufferObjectMaybeShared* js::CreateWasmBuffer(
    JSContext* cx, const wasm::MemoryDesc& memory) {
  MOZ_RELEASE_ASSERT(memory.initialPages() <=
                     wasm::MaxMemoryPages(memory.indexType()));
  MOZ_RELEASE_ASSERT(cx->wasm().haveSignalHandlers);

  if (memory.isShared()) {
    if (!cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_WASM_NO_SHMEM_LINK);
      return nullptr;
    }
    return CreateSpecificWasmBuffer<SharedArrayBufferObject,
                                    WasmSharedArrayRawBuffer>(cx, memory);
  }
  return CreateSpecificWasmBuffer<ArrayBufferObject, WasmArrayRawBuffer>(
      cx, memory);
}

bool ArrayBufferObject::prepareForAsmJS() {
  MOZ_ASSERT(byteLength() % wasm::PageSize == 0,
             "prior size checking should have guaranteed page-size multiple");
  MOZ_ASSERT(byteLength() > 0,
             "prior size checking should have excluded empty buffers");
  MOZ_ASSERT(!isResizable(),
             "prior checks should have excluded resizable buffers");

  switch (bufferKind()) {
    case MALLOCED_ARRAYBUFFER_CONTENTS_ARENA:
    case MALLOCED_UNKNOWN_ARENA:
    case MAPPED:
    case EXTERNAL:
      // It's okay if this uselessly sets the flag a second time.
      setIsPreparedForAsmJS();
      return true;

    case INLINE_DATA:
      static_assert(
          wasm::PageSize > FixedLengthArrayBufferObject::MaxInlineBytes,
          "inline data must be too small to be a page size multiple");
      MOZ_ASSERT_UNREACHABLE(
          "inline-data buffers should be implicitly excluded by size checks");
      return false;

    case NO_DATA:
      MOZ_ASSERT_UNREACHABLE(
          "size checking should have excluded detached or empty buffers");
      return false;

    // asm.js code and associated buffers are potentially long-lived.  Yet a
    // buffer of user-owned data *must* be detached by the user before the
    // user-owned data is disposed.  No caller wants to use a user-owned
    // ArrayBuffer with asm.js, so just don't support this and avoid a mess of
    // complexity.
    case USER_OWNED:
    // wasm buffers can be detached at any time.
    case WASM:
      MOZ_ASSERT(!isPreparedForAsmJS());
      return false;
  }

  MOZ_ASSERT_UNREACHABLE("non-exhaustive kind-handling switch?");
  return false;
}

ArrayBufferObject::BufferContents ArrayBufferObject::createMappedContents(
    int fd, size_t offset, size_t length) {
  void* data =
      gc::AllocateMappedContent(fd, offset, length, ARRAY_BUFFER_ALIGNMENT);
  return BufferContents::createMapped(data);
}

uint8_t* FixedLengthArrayBufferObject::inlineDataPointer() const {
  return static_cast<uint8_t*>(fixedData(JSCLASS_RESERVED_SLOTS(&class_)));
}

uint8_t* ResizableArrayBufferObject::inlineDataPointer() const {
  return static_cast<uint8_t*>(fixedData(JSCLASS_RESERVED_SLOTS(&class_)));
}

uint8_t* ArrayBufferObject::dataPointer() const {
  return static_cast<uint8_t*>(getFixedSlot(DATA_SLOT).toPrivate());
}

SharedMem<uint8_t*> ArrayBufferObject::dataPointerShared() const {
  return SharedMem<uint8_t*>::unshared(getFixedSlot(DATA_SLOT).toPrivate());
}

ArrayBufferObject::FreeInfo* ArrayBufferObject::freeInfo() const {
  MOZ_ASSERT(isExternal());
  MOZ_ASSERT(!isResizable());
  auto* data = as<FixedLengthArrayBufferObject>().inlineDataPointer();
  return reinterpret_cast<FreeInfo*>(data);
}

void ArrayBufferObject::releaseData(JS::GCContext* gcx) {
  switch (bufferKind()) {
    case INLINE_DATA:
      // Inline data doesn't require releasing.
      break;
    case MALLOCED_ARRAYBUFFER_CONTENTS_ARENA:
    case MALLOCED_UNKNOWN_ARENA:
      gcx->free_(this, dataPointer(), associatedBytes(),
                 MemoryUse::ArrayBufferContents);
      break;
    case NO_DATA:
      // There's nothing to release if there's no data.
      MOZ_ASSERT(dataPointer() == nullptr);
      break;
    case USER_OWNED:
      // User-owned data is released by, well, the user.
      break;
    case MAPPED:
      gc::DeallocateMappedContent(dataPointer(), byteLength());
      gcx->removeCellMemory(this, associatedBytes(),
                            MemoryUse::ArrayBufferContents);
      break;
    case WASM:
      WasmArrayRawBuffer::Release(dataPointer());
      gcx->removeCellMemory(this, byteLength(), MemoryUse::ArrayBufferContents);
      break;
    case EXTERNAL:
      MOZ_ASSERT(freeInfo()->freeFunc);
      {
        // The analyzer can't know for sure whether the embedder-supplied
        // free function will GC. We give the analyzer a hint here.
        // (Doing a GC in the free function is considered a programmer
        // error.)
        JS::AutoSuppressGCAnalysis nogc;
        freeInfo()->freeFunc(dataPointer(), freeInfo()->freeUserData);
      }
      break;
  }
}

void ArrayBufferObject::setDataPointer(BufferContents contents) {
  setFixedSlot(DATA_SLOT, PrivateValue(contents.data()));
  setFlags((flags() & ~KIND_MASK) | contents.kind());

  if (isExternal()) {
    auto info = freeInfo();
    info->freeFunc = contents.freeFunc();
    info->freeUserData = contents.freeUserData();
  }
}

size_t ArrayBufferObject::byteLength() const {
  return size_t(getFixedSlot(BYTE_LENGTH_SLOT).toPrivate());
}

inline size_t ArrayBufferObject::associatedBytes() const {
  if (isMalloced()) {
    return maxByteLength();
  }
  if (isMapped()) {
    return RoundUp(byteLength(), js::gc::SystemPageSize());
  }
  MOZ_CRASH("Unexpected buffer kind");
}

void ArrayBufferObject::setByteLength(size_t length) {
  MOZ_ASSERT(length <= ArrayBufferObject::ByteLengthLimit);
  setFixedSlot(BYTE_LENGTH_SLOT, PrivateValue(length));
}

size_t ArrayBufferObject::wasmMappedSize() const {
  if (isWasm()) {
    return contents().wasmBuffer()->mappedSize();
  }
  return byteLength();
}

IndexType ArrayBufferObject::wasmIndexType() const {
  if (isWasm()) {
    return contents().wasmBuffer()->indexType();
  }
  MOZ_ASSERT(isPreparedForAsmJS());
  return wasm::IndexType::I32;
}

Pages ArrayBufferObject::wasmPages() const {
  if (isWasm()) {
    return contents().wasmBuffer()->pages();
  }
  MOZ_ASSERT(isPreparedForAsmJS());
  return Pages::fromByteLengthExact(byteLength());
}

Pages ArrayBufferObject::wasmClampedMaxPages() const {
  if (isWasm()) {
    return contents().wasmBuffer()->clampedMaxPages();
  }
  MOZ_ASSERT(isPreparedForAsmJS());
  return Pages::fromByteLengthExact(byteLength());
}

Maybe<Pages> ArrayBufferObject::wasmSourceMaxPages() const {
  if (isWasm()) {
    return contents().wasmBuffer()->sourceMaxPages();
  }
  MOZ_ASSERT(isPreparedForAsmJS());
  return Some<Pages>(Pages::fromByteLengthExact(byteLength()));
}

size_t js::WasmArrayBufferMappedSize(const ArrayBufferObjectMaybeShared* buf) {
  if (buf->is<ArrayBufferObject>()) {
    return buf->as<ArrayBufferObject>().wasmMappedSize();
  }
  return buf->as<SharedArrayBufferObject>().wasmMappedSize();
}

IndexType js::WasmArrayBufferIndexType(
    const ArrayBufferObjectMaybeShared* buf) {
  if (buf->is<ArrayBufferObject>()) {
    return buf->as<ArrayBufferObject>().wasmIndexType();
  }
  return buf->as<SharedArrayBufferObject>().wasmIndexType();
}
Pages js::WasmArrayBufferPages(const ArrayBufferObjectMaybeShared* buf) {
  if (buf->is<ArrayBufferObject>()) {
    return buf->as<ArrayBufferObject>().wasmPages();
  }
  return buf->as<SharedArrayBufferObject>().volatileWasmPages();
}
Pages js::WasmArrayBufferClampedMaxPages(
    const ArrayBufferObjectMaybeShared* buf) {
  if (buf->is<ArrayBufferObject>()) {
    return buf->as<ArrayBufferObject>().wasmClampedMaxPages();
  }
  return buf->as<SharedArrayBufferObject>().wasmClampedMaxPages();
}
Maybe<Pages> js::WasmArrayBufferSourceMaxPages(
    const ArrayBufferObjectMaybeShared* buf) {
  if (buf->is<ArrayBufferObject>()) {
    return buf->as<ArrayBufferObject>().wasmSourceMaxPages();
  }
  return Some(buf->as<SharedArrayBufferObject>().wasmSourceMaxPages());
}

static void CheckStealPreconditions(Handle<ArrayBufferObject*> buffer,
                                    JSContext* cx) {
  cx->check(buffer);

  MOZ_ASSERT(!buffer->isDetached(), "can't steal from a detached buffer");
  MOZ_ASSERT(!buffer->isLengthPinned(),
             "can't steal from a buffer with a pinned length");
  MOZ_ASSERT(!buffer->isPreparedForAsmJS(),
             "asm.js-prepared buffers don't have detachable/stealable data");
}

/* static */
ArrayBufferObject* ArrayBufferObject::wasmGrowToPagesInPlace(
    wasm::IndexType t, Pages newPages, Handle<ArrayBufferObject*> oldBuf,
    JSContext* cx) {
  if (oldBuf->isLengthPinned()) {
    return nullptr;
  }

  CheckStealPreconditions(oldBuf, cx);

  MOZ_ASSERT(oldBuf->isWasm());

  // Check that the new pages is within our allowable range. This will
  // simultaneously check against the maximum specified in source and our
  // implementation limits.
  if (newPages > oldBuf->wasmClampedMaxPages()) {
    return nullptr;
  }
  MOZ_ASSERT(newPages <= wasm::MaxMemoryPages(t) &&
             newPages.byteLength() <= ArrayBufferObject::ByteLengthLimit);

  // We have checked against the clamped maximum and so we know we can convert
  // to byte lengths now.
  size_t newSize = newPages.byteLength();

  // On failure, do not throw and ensure that the original buffer is
  // unmodified and valid. After WasmArrayRawBuffer::growToPagesInPlace(), the
  // wasm-visible length of the buffer has been increased so it must be the
  // last fallible operation.

  auto* newBuf = ArrayBufferObject::createEmpty(cx);
  if (!newBuf) {
    cx->clearPendingException();
    return nullptr;
  }

  MOZ_ASSERT(newBuf->isNoData());

  if (!oldBuf->contents().wasmBuffer()->growToPagesInPlace(newPages)) {
    return nullptr;
  }

  // Extract the grown contents from |oldBuf|.
  BufferContents oldContents = oldBuf->contents();

  // Overwrite |oldBuf|'s data pointer *without* releasing old data.
  oldBuf->setDataPointer(BufferContents::createNoData());

  // Detach |oldBuf| now that doing so won't release |oldContents|.
  RemoveCellMemory(oldBuf, oldBuf->byteLength(),
                   MemoryUse::ArrayBufferContents);
  ArrayBufferObject::detach(cx, oldBuf);

  // Set |newBuf|'s contents to |oldBuf|'s original contents.
  newBuf->initialize(newSize, oldContents);
  AddCellMemory(newBuf, newSize, MemoryUse::ArrayBufferContents);

  return newBuf;
}

/* static */
ArrayBufferObject* ArrayBufferObject::wasmMovingGrowToPages(
    IndexType t, Pages newPages, Handle<ArrayBufferObject*> oldBuf,
    JSContext* cx) {
  // On failure, do not throw and ensure that the original buffer is
  // unmodified and valid.
  if (oldBuf->isLengthPinned()) {
    return nullptr;
  }

  // Check that the new pages is within our allowable range. This will
  // simultaneously check against the maximum specified in source and our
  // implementation limits.
  if (newPages > oldBuf->wasmClampedMaxPages()) {
    return nullptr;
  }
  MOZ_ASSERT(newPages <= wasm::MaxMemoryPages(t) &&
             newPages.byteLength() <= ArrayBufferObject::ByteLengthLimit);

  // We have checked against the clamped maximum and so we know we can convert
  // to byte lengths now.
  size_t newSize = newPages.byteLength();

  if (wasm::ComputeMappedSize(newPages) <= oldBuf->wasmMappedSize() ||
      oldBuf->contents().wasmBuffer()->extendMappedSize(newPages)) {
    return wasmGrowToPagesInPlace(t, newPages, oldBuf, cx);
  }

  Rooted<ArrayBufferObject*> newBuf(cx, ArrayBufferObject::createEmpty(cx));
  if (!newBuf) {
    cx->clearPendingException();
    return nullptr;
  }

  Pages clampedMaxPages =
      wasm::ClampedMaxPages(t, newPages, Nothing(), /* hugeMemory */ false);
  WasmArrayRawBuffer* newRawBuf = WasmArrayRawBuffer::AllocateWasm(
      oldBuf->wasmIndexType(), newPages, clampedMaxPages, Nothing(), Nothing());
  if (!newRawBuf) {
    return nullptr;
  }

  AddCellMemory(newBuf, newSize, MemoryUse::ArrayBufferContents);

  BufferContents contents =
      BufferContents::createWasm(newRawBuf->dataPointer());
  newBuf->initialize(newSize, contents);

  memcpy(newBuf->dataPointer(), oldBuf->dataPointer(), oldBuf->byteLength());
  ArrayBufferObject::detach(cx, oldBuf);

  return newBuf;
}

/* static */
void ArrayBufferObject::wasmDiscard(Handle<ArrayBufferObject*> buf,
                                    uint64_t byteOffset, uint64_t byteLen) {
  MOZ_ASSERT(buf->isWasm());
  buf->contents().wasmBuffer()->discard(byteOffset, byteLen);
}

uint32_t ArrayBufferObject::flags() const {
  return uint32_t(getFixedSlot(FLAGS_SLOT).toInt32());
}

void ArrayBufferObject::setFlags(uint32_t flags) {
  setFixedSlot(FLAGS_SLOT, Int32Value(flags));
}

static constexpr js::gc::AllocKind GetArrayBufferGCObjectKind(size_t numSlots) {
  if (numSlots <= 4) {
    return js::gc::AllocKind::ARRAYBUFFER4;
  }
  if (numSlots <= 8) {
    return js::gc::AllocKind::ARRAYBUFFER8;
  }
  if (numSlots <= 12) {
    return js::gc::AllocKind::ARRAYBUFFER12;
  }
  return js::gc::AllocKind::ARRAYBUFFER16;
}

template <class ArrayBufferType>
static ArrayBufferType* NewArrayBufferObject(JSContext* cx, HandleObject proto_,
                                             gc::AllocKind allocKind) {
  MOZ_ASSERT(allocKind == gc::AllocKind::ARRAYBUFFER4 ||
             allocKind == gc::AllocKind::ARRAYBUFFER8 ||
             allocKind == gc::AllocKind::ARRAYBUFFER12 ||
             allocKind == gc::AllocKind::ARRAYBUFFER16);

  static_assert(std::is_same_v<ArrayBufferType, FixedLengthArrayBufferObject> ||
                std::is_same_v<ArrayBufferType, ResizableArrayBufferObject>);

  RootedObject proto(cx, proto_);
  if (!proto) {
    proto = GlobalObject::getOrCreatePrototype(cx, JSProto_ArrayBuffer);
    if (!proto) {
      MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "creating ArrayBuffer proto");
      return nullptr;
    }
  }

  const JSClass* clasp = &ArrayBufferType::class_;

  // Array buffers can store data inline so we only use fixed slots to cover the
  // reserved slots, ignoring the AllocKind.
  MOZ_ASSERT(ClassCanHaveFixedData(clasp));
  constexpr size_t nfixed = ArrayBufferType::RESERVED_SLOTS;
  static_assert(nfixed <= NativeObject::MAX_FIXED_SLOTS);

  Rooted<SharedShape*> shape(
      cx,
      SharedShape::getInitialShape(cx, clasp, cx->realm(), AsTaggedProto(proto),
                                   nfixed, ObjectFlags()));
  if (!shape) {
    MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "get ArrayBuffer initial shape");
    return nullptr;
  }

  // Array buffers can't be nursery allocated but can be background-finalized.
  MOZ_ASSERT(IsBackgroundFinalized(allocKind));
  MOZ_ASSERT(!CanNurseryAllocateFinalizedClass(clasp));
  constexpr gc::Heap heap = gc::Heap::Tenured;

  auto* buffer =
      NativeObject::create<ArrayBufferType>(cx, allocKind, heap, shape);
  if (!buffer) {
    MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "create NativeObject failed");
  }
  return buffer;
}

// Creates a new ArrayBufferObject with %ArrayBuffer.prototype% as proto and no
// space for inline data.
static ArrayBufferObject* NewArrayBufferObject(JSContext* cx) {
  constexpr auto allocKind =
      GetArrayBufferGCObjectKind(FixedLengthArrayBufferObject::RESERVED_SLOTS);
  return NewArrayBufferObject<FixedLengthArrayBufferObject>(cx, nullptr,
                                                            allocKind);
}
static ResizableArrayBufferObject* NewResizableArrayBufferObject(
    JSContext* cx) {
  constexpr auto allocKind =
      GetArrayBufferGCObjectKind(ResizableArrayBufferObject::RESERVED_SLOTS);
  return NewArrayBufferObject<ResizableArrayBufferObject>(cx, nullptr,
                                                          allocKind);
}

ArrayBufferObject* ArrayBufferObject::createForContents(
    JSContext* cx, size_t nbytes, BufferContents contents) {
  MOZ_ASSERT(contents);
  MOZ_ASSERT(contents.kind() != INLINE_DATA);
  MOZ_ASSERT(contents.kind() != NO_DATA);
  MOZ_ASSERT(contents.kind() != WASM);

  // 24.1.1.1, step 3 (Inlined 6.2.6.1 CreateByteDataBlock, step 2).
  if (!CheckArrayBufferTooLarge(cx, nbytes)) {
    return nullptr;
  }

  // Some |contents| kinds need to store extra data in the ArrayBuffer beyond a
  // data pointer.  If needed for the particular kind, add extra fixed slots to
  // the ArrayBuffer for use as raw storage to store such information.
  constexpr size_t reservedSlots = FixedLengthArrayBufferObject::RESERVED_SLOTS;

  size_t nAllocated = 0;
  size_t nslots = reservedSlots;
  if (contents.kind() == USER_OWNED) {
    // No accounting to do in this case.
  } else if (contents.kind() == EXTERNAL) {
    // Store the FreeInfo in the inline data slots so that we
    // don't use up slots for it in non-refcounted array buffers.
    constexpr size_t freeInfoSlots = HowMany(sizeof(FreeInfo), sizeof(Value));
    static_assert(
        reservedSlots + freeInfoSlots <= NativeObject::MAX_FIXED_SLOTS,
        "FreeInfo must fit in inline slots");
    nslots += freeInfoSlots;
  } else {
    // The ABO is taking ownership, so account the bytes against the zone.
    nAllocated = nbytes;
    if (contents.kind() == MAPPED) {
      nAllocated = RoundUp(nbytes, js::gc::SystemPageSize());
    } else {
      MOZ_ASSERT(contents.kind() == MALLOCED_ARRAYBUFFER_CONTENTS_ARENA ||
                     contents.kind() == MALLOCED_UNKNOWN_ARENA,
                 "should have handled all possible callers' kinds");
    }
  }

  gc::AllocKind allocKind = GetArrayBufferGCObjectKind(nslots);

  AutoSetNewObjectMetadata metadata(cx);
  Rooted<ArrayBufferObject*> buffer(
      cx, NewArrayBufferObject<FixedLengthArrayBufferObject>(cx, nullptr,
                                                             allocKind));
  if (!buffer) {
    return nullptr;
  }

  MOZ_ASSERT(!gc::IsInsideNursery(buffer),
             "ArrayBufferObject has a finalizer that must be called to not "
             "leak in some cases, so it can't be nursery-allocated");

  buffer->initialize(nbytes, contents);

  if (contents.kind() == MAPPED ||
      contents.kind() == MALLOCED_ARRAYBUFFER_CONTENTS_ARENA ||
      contents.kind() == MALLOCED_UNKNOWN_ARENA) {
    AddCellMemory(buffer, nAllocated, MemoryUse::ArrayBufferContents);
  }

  return buffer;
}

template <class ArrayBufferType, ArrayBufferObject::FillContents FillType>
/* static */ std::tuple<ArrayBufferType*, uint8_t*>
ArrayBufferObject::createUninitializedBufferAndData(
    JSContext* cx, size_t nbytes, AutoSetNewObjectMetadata&,
    JS::Handle<JSObject*> proto) {
  MOZ_ASSERT(nbytes <= ArrayBufferObject::ByteLengthLimit,
             "caller must validate the byte count it passes");

  static_assert(std::is_same_v<ArrayBufferType, FixedLengthArrayBufferObject> ||
                std::is_same_v<ArrayBufferType, ResizableArrayBufferObject>);

  // Try fitting the data inline with the object by repurposing fixed-slot
  // storage.  Add extra fixed slots if necessary to accomplish this, but don't
  // exceed the maximum number of fixed slots!
  size_t nslots = ArrayBufferType::RESERVED_SLOTS;
  ArrayBufferContents data;
  if (nbytes <= ArrayBufferType::MaxInlineBytes) {
    int newSlots = HowMany(nbytes, sizeof(Value));
    MOZ_ASSERT(int(nbytes) <= newSlots * int(sizeof(Value)));

    nslots += newSlots;
  } else {
    data = FillType == FillContents::Uninitialized
               ? AllocateUninitializedArrayBufferContents(cx, nbytes)
               : AllocateArrayBufferContents(cx, nbytes);
    if (!data) {
      if (cx->brittleMode) {
        if (nbytes < INT32_MAX) {
          MOZ_DIAGNOSTIC_ASSERT(false, "ArrayBuffer allocation OOM < 2GB - 1");
        } else {
          MOZ_DIAGNOSTIC_ASSERT(
              false,
              "ArrayBuffer allocation OOM between 2GB and ByteLengthLimit");
        }
      }
      return {nullptr, nullptr};
    }
  }

  gc::AllocKind allocKind = GetArrayBufferGCObjectKind(nslots);

  auto* buffer = NewArrayBufferObject<ArrayBufferType>(cx, proto, allocKind);
  if (!buffer) {
    return {nullptr, nullptr};
  }

  MOZ_ASSERT(!gc::IsInsideNursery(buffer),
             "ArrayBufferObject has a finalizer that must be called to not "
             "leak in some cases, so it can't be nursery-allocated");

  if (data) {
    return {buffer, data.release()};
  }

  if constexpr (FillType == FillContents::Zero) {
    memset(buffer->inlineDataPointer(), 0, nbytes);
  }
  return {buffer, nullptr};
}

template <ArrayBufferObject::FillContents FillType>
/* static */ std::tuple<ArrayBufferObject*, uint8_t*>
ArrayBufferObject::createBufferAndData(
    JSContext* cx, size_t nbytes, AutoSetNewObjectMetadata& metadata,
    JS::Handle<JSObject*> proto /* = nullptr */) {
  MOZ_ASSERT(nbytes <= ArrayBufferObject::ByteLengthLimit,
             "caller must validate the byte count it passes");

  auto [buffer, data] =
      createUninitializedBufferAndData<FixedLengthArrayBufferObject, FillType>(
          cx, nbytes, metadata, proto);
  if (!buffer) {
    return {nullptr, nullptr};
  }

  if (data) {
    buffer->initialize(
        nbytes, BufferContents::createMallocedArrayBufferContentsArena(data));
    AddCellMemory(buffer, nbytes, MemoryUse::ArrayBufferContents);
  } else {
    data = buffer->inlineDataPointer();
    buffer->initialize(nbytes, BufferContents::createInlineData(data));
  }
  return {buffer, data};
}

template <ArrayBufferObject::FillContents FillType>
/* static */ std::tuple<ResizableArrayBufferObject*, uint8_t*>
ResizableArrayBufferObject::createBufferAndData(
    JSContext* cx, size_t byteLength, size_t maxByteLength,
    AutoSetNewObjectMetadata& metadata, Handle<JSObject*> proto) {
  MOZ_ASSERT(byteLength <= maxByteLength);
  MOZ_ASSERT(maxByteLength <= ArrayBufferObject::ByteLengthLimit,
             "caller must validate the byte count it passes");

  // NOTE: The spec proposal for resizable ArrayBuffers suggests to use a
  // virtual memory based approach to avoid eagerly allocating the maximum byte
  // length. We don't yet support this and instead are allocating the maximum
  // byte length direct from the start.
  size_t nbytes = maxByteLength;

  auto [buffer, data] =
      createUninitializedBufferAndData<ResizableArrayBufferObject, FillType>(
          cx, nbytes, metadata, proto);
  if (!buffer) {
    return {nullptr, nullptr};
  }

  if (data) {
    buffer->initialize(
        byteLength, maxByteLength,
        BufferContents::createMallocedArrayBufferContentsArena(data));
    AddCellMemory(buffer, nbytes, MemoryUse::ArrayBufferContents);
  } else {
    data = buffer->inlineDataPointer();
    buffer->initialize(byteLength, maxByteLength,
                       BufferContents::createInlineData(data));
  }
  return {buffer, data};
}

/* static */ ArrayBufferObject* ArrayBufferObject::copy(
    JSContext* cx, size_t newByteLength,
    JS::Handle<ArrayBufferObject*> source) {
  MOZ_ASSERT(!source->isDetached());
  MOZ_ASSERT(newByteLength <= ArrayBufferObject::ByteLengthLimit,
             "caller must validate the byte count it passes");

  size_t sourceByteLength = source->byteLength();

  if (newByteLength > sourceByteLength) {
    // Copy into a larger buffer.
    AutoSetNewObjectMetadata metadata(cx);
    auto [buffer, toFill] = createBufferAndData<FillContents::Zero>(
        cx, newByteLength, metadata, nullptr);
    if (!buffer) {
      return nullptr;
    }

    std::copy_n(source->dataPointer(), sourceByteLength, toFill);

    return buffer;
  }

  // Copy into a smaller or same size buffer.
  AutoSetNewObjectMetadata metadata(cx);
  auto [buffer, toFill] = createBufferAndData<FillContents::Uninitialized>(
      cx, newByteLength, metadata, nullptr);
  if (!buffer) {
    return nullptr;
  }

  std::uninitialized_copy_n(source->dataPointer(), newByteLength, toFill);

  return buffer;
}

/* static */ ResizableArrayBufferObject* ResizableArrayBufferObject::copy(
    JSContext* cx, size_t newByteLength,
    JS::Handle<ResizableArrayBufferObject*> source) {
  MOZ_ASSERT(!source->isDetached());
  MOZ_ASSERT(newByteLength <= source->maxByteLength());

  size_t sourceByteLength = source->byteLength();
  size_t newMaxByteLength = source->maxByteLength();

  if (newByteLength > sourceByteLength) {
    // Copy into a larger buffer.
    AutoSetNewObjectMetadata metadata(cx);
    auto [buffer, toFill] = createBufferAndData<FillContents::Zero>(
        cx, newByteLength, newMaxByteLength, metadata, nullptr);
    if (!buffer) {
      return nullptr;
    }

    // The `createBufferAndData()` call first zero-initializes the complete
    // buffer and then we copy over |sourceByteLength| bytes from |source|. It
    // seems prudent to only zero-initialize the trailing bytes of |toFill|
    // to avoid writing twice to `toFill[0..newByteLength]`. We don't yet
    // implement this optimization, because this method is only called for
    // small, inline buffers, so any write optimizations probably won't make
    // much of a difference.
    std::copy_n(source->dataPointer(), sourceByteLength, toFill);

    return buffer;
  }

  // Copy into a smaller or same size buffer.
  AutoSetNewObjectMetadata metadata(cx);
  auto [buffer, toFill] = createBufferAndData<FillContents::Uninitialized>(
      cx, newByteLength, newMaxByteLength, metadata, nullptr);
  if (!buffer) {
    return nullptr;
  }

  std::uninitialized_copy_n(source->dataPointer(), newByteLength, toFill);

  return buffer;
}

/* static */ ArrayBufferObject* ArrayBufferObject::copyAndDetach(
    JSContext* cx, size_t newByteLength,
    JS::Handle<ArrayBufferObject*> source) {
  MOZ_ASSERT(!source->isDetached());
  MOZ_ASSERT(!source->isLengthPinned());
  MOZ_ASSERT(newByteLength <= ArrayBufferObject::ByteLengthLimit,
             "caller must validate the byte count it passes");

  if (newByteLength > FixedLengthArrayBufferObject::MaxInlineBytes &&
      source->isMalloced()) {
    if (newByteLength == source->associatedBytes()) {
      return copyAndDetachSteal(cx, source);
    }
    if (source->bufferKind() ==
        ArrayBufferObject::MALLOCED_ARRAYBUFFER_CONTENTS_ARENA) {
      return copyAndDetachRealloc(cx, newByteLength, source);
    }
  }

  auto* newBuffer = ArrayBufferObject::copy(cx, newByteLength, source);
  if (!newBuffer) {
    return nullptr;
  }
  ArrayBufferObject::detach(cx, source);

  return newBuffer;
}

/* static */ ArrayBufferObject* ArrayBufferObject::copyAndDetachSteal(
    JSContext* cx, JS::Handle<ArrayBufferObject*> source) {
  MOZ_ASSERT(!source->isDetached());
  MOZ_ASSERT(!source->isLengthPinned());
  MOZ_ASSERT(source->isMalloced());

  size_t newByteLength = source->associatedBytes();
  MOZ_ASSERT(newByteLength > FixedLengthArrayBufferObject::MaxInlineBytes,
             "prefer copying small buffers");
  MOZ_ASSERT(source->byteLength() <= newByteLength,
             "source length is less-or-equal to |newByteLength|");

  auto* newBuffer = ArrayBufferObject::createEmpty(cx);
  if (!newBuffer) {
    return nullptr;
  }

  // Extract the contents from |source|.
  BufferContents contents = source->contents();
  MOZ_ASSERT(contents);
  MOZ_ASSERT(contents.kind() == MALLOCED_ARRAYBUFFER_CONTENTS_ARENA ||
             contents.kind() == MALLOCED_UNKNOWN_ARENA);

  // Overwrite |source|'s data pointer *without* releasing the data.
  source->setDataPointer(BufferContents::createNoData());

  // Detach |source| now that doing so won't release |contents|.
  RemoveCellMemory(source, newByteLength, MemoryUse::ArrayBufferContents);
  ArrayBufferObject::detach(cx, source);

  // Set |newBuffer|'s contents to |source|'s original contents.
  newBuffer->initialize(newByteLength, contents);
  AddCellMemory(newBuffer, newByteLength, MemoryUse::ArrayBufferContents);

  return newBuffer;
}

/* static */ ArrayBufferObject* ArrayBufferObject::copyAndDetachRealloc(
    JSContext* cx, size_t newByteLength,
    JS::Handle<ArrayBufferObject*> source) {
  MOZ_ASSERT(!source->isDetached());
  MOZ_ASSERT(!source->isLengthPinned());
  MOZ_ASSERT(source->bufferKind() == MALLOCED_ARRAYBUFFER_CONTENTS_ARENA);
  MOZ_ASSERT(newByteLength > FixedLengthArrayBufferObject::MaxInlineBytes,
             "prefer copying small buffers");
  MOZ_ASSERT(newByteLength <= ArrayBufferObject::ByteLengthLimit,
             "caller must validate the byte count it passes");

  size_t oldByteLength = source->associatedBytes();
  MOZ_ASSERT(oldByteLength != newByteLength,
             "steal instead of realloc same size buffers");
  MOZ_ASSERT(source->byteLength() <= oldByteLength,
             "source length is less-or-equal to |oldByteLength|");

  Rooted<ArrayBufferObject*> newBuffer(cx, ArrayBufferObject::createEmpty(cx));
  if (!newBuffer) {
    return nullptr;
  }

  // Extract the contents from |source|.
  BufferContents contents = source->contents();
  MOZ_ASSERT(contents);
  MOZ_ASSERT(contents.kind() == MALLOCED_ARRAYBUFFER_CONTENTS_ARENA);

  // Reallocate the data pointer.
  auto newData = ReallocateArrayBufferContents(cx, contents.data(),
                                               oldByteLength, newByteLength);
  if (!newData) {
    // If reallocation failed, the old pointer is still valid, so just return.
    return nullptr;
  }
  auto newContents =
      BufferContents::createMallocedArrayBufferContentsArena(newData.release());

  // Overwrite |source|'s data pointer *without* releasing the data.
  source->setDataPointer(BufferContents::createNoData());

  // Detach |source| now that doing so won't release |contents|.
  RemoveCellMemory(source, oldByteLength, MemoryUse::ArrayBufferContents);
  ArrayBufferObject::detach(cx, source);

  // Set |newBuffer|'s contents to |newContents|.
  newBuffer->initialize(newByteLength, newContents);
  AddCellMemory(newBuffer, newByteLength, MemoryUse::ArrayBufferContents);

  // Zero initialize the newly allocated memory, if necessary.
  if (newByteLength > oldByteLength) {
    size_t count = newByteLength - oldByteLength;
    std::uninitialized_fill_n(newContents.data() + oldByteLength, count, 0);
  }

  return newBuffer;
}

/* static */ ResizableArrayBufferObject*
ResizableArrayBufferObject::copyAndDetach(
    JSContext* cx, size_t newByteLength,
    JS::Handle<ResizableArrayBufferObject*> source) {
  MOZ_ASSERT(!source->isDetached());
  MOZ_ASSERT(!source->isLengthPinned());
  MOZ_ASSERT(newByteLength <= source->maxByteLength());

  if (source->maxByteLength() > ResizableArrayBufferObject::MaxInlineBytes &&
      source->isMalloced()) {
    return copyAndDetachSteal(cx, newByteLength, source);
  }

  auto* newBuffer = ResizableArrayBufferObject::copy(cx, newByteLength, source);
  if (!newBuffer) {
    return nullptr;
  }
  ArrayBufferObject::detach(cx, source);

  return newBuffer;
}

/* static */ ResizableArrayBufferObject*
ResizableArrayBufferObject::copyAndDetachSteal(
    JSContext* cx, size_t newByteLength,
    JS::Handle<ResizableArrayBufferObject*> source) {
  MOZ_ASSERT(!source->isDetached());
  MOZ_ASSERT(!source->isLengthPinned());
  MOZ_ASSERT(newByteLength <= source->maxByteLength());
  MOZ_ASSERT(source->isMalloced());

  size_t sourceByteLength = source->byteLength();
  size_t maxByteLength = source->maxByteLength();
  MOZ_ASSERT(maxByteLength > ResizableArrayBufferObject::MaxInlineBytes,
             "prefer copying small buffers");

  auto* newBuffer = ResizableArrayBufferObject::createEmpty(cx);
  if (!newBuffer) {
    return nullptr;
  }

  // Extract the contents from |source|.
  BufferContents contents = source->contents();
  MOZ_ASSERT(contents);
  MOZ_ASSERT(contents.kind() == MALLOCED_ARRAYBUFFER_CONTENTS_ARENA ||
             contents.kind() == MALLOCED_UNKNOWN_ARENA);

  // Overwrite |source|'s data pointer *without* releasing the data.
  source->setDataPointer(BufferContents::createNoData());

  // Detach |source| now that doing so won't release |contents|.
  RemoveCellMemory(source, maxByteLength, MemoryUse::ArrayBufferContents);
  ArrayBufferObject::detach(cx, source);

  // Set |newBuffer|'s contents to |source|'s original contents.
  newBuffer->initialize(newByteLength, maxByteLength, contents);
  AddCellMemory(newBuffer, maxByteLength, MemoryUse::ArrayBufferContents);

  // Clear the bytes between `data[newByteLength..sourceByteLength]`.
  if (newByteLength < sourceByteLength) {
    size_t nbytes = sourceByteLength - newByteLength;
    memset(newBuffer->dataPointer() + newByteLength, 0, nbytes);
  }

  return newBuffer;
}

ArrayBufferObject* ArrayBufferObject::createZeroed(
    JSContext* cx, size_t nbytes, HandleObject proto /* = nullptr */) {
  // 24.1.1.1, step 3 (Inlined 6.2.6.1 CreateByteDataBlock, step 2).
  if (!CheckArrayBufferTooLarge(cx, nbytes)) {
    MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "buffer too large");
    return nullptr;
  }

  AutoSetNewObjectMetadata metadata(cx);
  auto [buffer, toFill] =
      createBufferAndData<FillContents::Zero>(cx, nbytes, metadata, proto);
  (void)toFill;
  return buffer;
}

ResizableArrayBufferObject* ResizableArrayBufferObject::createZeroed(
    JSContext* cx, size_t byteLength, size_t maxByteLength,
    HandleObject proto /* = nullptr */) {
  // 24.1.1.1, step 3 (Inlined 6.2.6.1 CreateByteDataBlock, step 2).
  if (!CheckArrayBufferTooLarge(cx, byteLength) ||
      !CheckArrayBufferTooLarge(cx, maxByteLength)) {
    return nullptr;
  }
  if (byteLength > maxByteLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_LARGER_THAN_MAXIMUM);
    return nullptr;
  }

  AutoSetNewObjectMetadata metadata(cx);
  auto [buffer, toFill] = createBufferAndData<FillContents::Zero>(
      cx, byteLength, maxByteLength, metadata, proto);
  (void)toFill;
  return buffer;
}

ArrayBufferObject* ArrayBufferObject::createEmpty(JSContext* cx) {
  AutoSetNewObjectMetadata metadata(cx);
  ArrayBufferObject* obj = NewArrayBufferObject(cx);
  if (!obj) {
    return nullptr;
  }

  obj->initialize(0, BufferContents::createNoData());
  return obj;
}

ResizableArrayBufferObject* ResizableArrayBufferObject::createEmpty(
    JSContext* cx) {
  AutoSetNewObjectMetadata metadata(cx);
  auto* obj = NewResizableArrayBufferObject(cx);
  if (!obj) {
    return nullptr;
  }

  obj->initialize(0, 0, BufferContents::createNoData());
  return obj;
}

ArrayBufferObject* ArrayBufferObject::createFromNewRawBuffer(
    JSContext* cx, WasmArrayRawBuffer* rawBuffer, size_t initialSize) {
  AutoSetNewObjectMetadata metadata(cx);
  ArrayBufferObject* buffer = NewArrayBufferObject(cx);
  if (!buffer) {
    WasmArrayRawBuffer::Release(rawBuffer->dataPointer());
    return nullptr;
  }

  MOZ_ASSERT(initialSize == rawBuffer->byteLength());

  auto contents = BufferContents::createWasm(rawBuffer->dataPointer());
  buffer->initialize(initialSize, contents);

  AddCellMemory(buffer, initialSize, MemoryUse::ArrayBufferContents);

  return buffer;
}

/* static */ uint8_t* ArrayBufferObject::stealMallocedContents(
    JSContext* cx, Handle<ArrayBufferObject*> buffer) {
  CheckStealPreconditions(buffer, cx);

  switch (buffer->bufferKind()) {
    case MALLOCED_ARRAYBUFFER_CONTENTS_ARENA:
    case MALLOCED_UNKNOWN_ARENA: {
      uint8_t* stolenData = buffer->dataPointer();
      MOZ_ASSERT(stolenData);

      // Resizable buffers are initially allocated with their maximum
      // byte-length. When stealing the buffer contents shrink the allocated
      // memory to the actually used byte-length.
      if (buffer->isResizable()) {
        auto* resizableBuffer = &buffer->as<ResizableArrayBufferObject>();
        size_t byteLength = resizableBuffer->byteLength();
        size_t maxByteLength = resizableBuffer->maxByteLength();
        MOZ_ASSERT(byteLength <= maxByteLength);

        if (byteLength < maxByteLength) {
          auto newData = ReallocateArrayBufferContents(
              cx, stolenData, maxByteLength, byteLength);
          if (!newData) {
            // If reallocation failed, the old pointer is still valid. The
            // ArrayBuffer isn't detached and still owns the malloc'ed memory.
            return nullptr;
          }

          // The following code must be infallible, because the data pointer of
          // |buffer| is possibly no longer valid after the above realloc.

          stolenData = newData.release();
        }
      }

      RemoveCellMemory(buffer, buffer->associatedBytes(),
                       MemoryUse::ArrayBufferContents);

      // Overwrite the old data pointer *without* releasing the contents
      // being stolen.
      buffer->setDataPointer(BufferContents::createNoData());

      // Detach |buffer| now that doing so won't free |stolenData|.
      ArrayBufferObject::detach(cx, buffer);
      return stolenData;
    }

    case INLINE_DATA:
    case NO_DATA:
    case USER_OWNED:
    case MAPPED:
    case EXTERNAL: {
      // We can't use these data types directly.  Make a copy to return.
      ArrayBufferContents copiedData = NewCopiedBufferContents(cx, buffer);
      if (!copiedData) {
        return nullptr;
      }

      // Detach |buffer|.  This immediately releases the currently owned
      // contents, freeing or unmapping data in the MAPPED and EXTERNAL cases.
      ArrayBufferObject::detach(cx, buffer);
      return copiedData.release();
    }

    case WASM:
      MOZ_ASSERT_UNREACHABLE(
          "wasm buffers aren't stealable except by a "
          "memory.grow operation that shouldn't call this "
          "function");
      return nullptr;
  }

  MOZ_ASSERT_UNREACHABLE("garbage kind computed");
  return nullptr;
}

/* static */ ArrayBufferObject::BufferContents
ArrayBufferObject::extractStructuredCloneContents(
    JSContext* cx, Handle<ArrayBufferObject*> buffer) {
  if (buffer->isLengthPinned()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    return BufferContents::createFailed();
  }

  CheckStealPreconditions(buffer, cx);

  // We don't yet support extracting the contents of resizable buffers.
  MOZ_ASSERT(!buffer->isResizable(),
             "extracting the contents of resizable buffers not supported");

  BufferContents contents = buffer->contents();

  switch (contents.kind()) {
    case INLINE_DATA:
    case NO_DATA:
    case USER_OWNED: {
      ArrayBufferContents copiedData = NewCopiedBufferContents(cx, buffer);
      if (!copiedData) {
        return BufferContents::createFailed();
      }

      ArrayBufferObject::detach(cx, buffer);
      return BufferContents::createMallocedArrayBufferContentsArena(
          copiedData.release());
    }

    case MALLOCED_ARRAYBUFFER_CONTENTS_ARENA:
    case MALLOCED_UNKNOWN_ARENA:
    case MAPPED: {
      MOZ_ASSERT(contents);

      RemoveCellMemory(buffer, buffer->associatedBytes(),
                       MemoryUse::ArrayBufferContents);

      // Overwrite the old data pointer *without* releasing old data.
      buffer->setDataPointer(BufferContents::createNoData());

      // Detach |buffer| now that doing so won't release |oldContents|.
      ArrayBufferObject::detach(cx, buffer);
      return contents;
    }

    case WASM:
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_WASM_NO_TRANSFER);
      return BufferContents::createFailed();

    case EXTERNAL:
      MOZ_ASSERT_UNREACHABLE(
          "external ArrayBuffer shouldn't have passed the "
          "structured-clone preflighting");
      break;
  }

  MOZ_ASSERT_UNREACHABLE("garbage kind computed");
  return BufferContents::createFailed();
}

/* static */
bool ArrayBufferObject::ensureNonInline(JSContext* cx,
                                        Handle<ArrayBufferObject*> buffer) {
  if (buffer->isDetached() || buffer->isPreparedForAsmJS()) {
    return true;
  }

  if (buffer->isLengthPinned()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    MOZ_DIAGNOSTIC_ASSERT(!cx->brittleMode, "ArrayBuffer length pinned");
    return false;
  }

  BufferContents inlineContents = buffer->contents();
  if (inlineContents.kind() != INLINE_DATA) {
    return true;
  }

  size_t nbytes = buffer->maxByteLength();
  ArrayBufferContents copy = NewCopiedBufferContents(cx, buffer);
  if (!copy) {
    return false;
  }
  BufferContents outOfLineContents =
      BufferContents::createMallocedArrayBufferContentsArena(copy.release());
  buffer->setDataPointer(outOfLineContents);
  AddCellMemory(buffer, nbytes, MemoryUse::ArrayBufferContents);

  if (!buffer->firstView()) {
    return true;  // No views! Easy!
  }

  buffer->firstView()->as<ArrayBufferViewObject>().notifyBufferMoved(
      inlineContents.data(), outOfLineContents.data());

  auto& innerViews = ObjectRealm::get(buffer).innerViews.get();
  if (InnerViewTable::ViewVector* views =
          innerViews.maybeViewsUnbarriered(buffer)) {
    for (JSObject* view : *views) {
      view->as<ArrayBufferViewObject>().notifyBufferMoved(
          inlineContents.data(), outOfLineContents.data());
    }
  }

  return true;
}

/* static */
void ArrayBufferObject::addSizeOfExcludingThis(
    JSObject* obj, mozilla::MallocSizeOf mallocSizeOf, JS::ClassInfo* info,
    JS::RuntimeSizes* runtimeSizes) {
  auto& buffer = obj->as<ArrayBufferObject>();
  switch (buffer.bufferKind()) {
    case INLINE_DATA:
      // Inline data's size should be reported by this object's size-class
      // reporting.
      break;
    case MALLOCED_ARRAYBUFFER_CONTENTS_ARENA:
    case MALLOCED_UNKNOWN_ARENA:
      if (buffer.isPreparedForAsmJS()) {
        info->objectsMallocHeapElementsAsmJS +=
            mallocSizeOf(buffer.dataPointer());
      } else {
        info->objectsMallocHeapElementsNormal +=
            mallocSizeOf(buffer.dataPointer());
      }
      break;
    case NO_DATA:
      // No data is no memory.
      MOZ_ASSERT(buffer.dataPointer() == nullptr);
      break;
    case USER_OWNED:
      // User-owned data should be accounted for by the user.
      break;
    case EXTERNAL:
      // External data will be accounted for by the owner of the buffer,
      // not this view.
      break;
    case MAPPED:
      info->objectsNonHeapElementsNormal += buffer.byteLength();
      break;
    case WASM:
      if (!buffer.isDetached()) {
        info->objectsNonHeapElementsWasm += buffer.byteLength();
        if (runtimeSizes) {
          MOZ_ASSERT(buffer.wasmMappedSize() >= buffer.byteLength());
          runtimeSizes->wasmGuardPages +=
              buffer.wasmMappedSize() - buffer.byteLength();
        }
      }
      break;
  }
}

/* static */
void ArrayBufferObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  obj->as<ArrayBufferObject>().releaseData(gcx);
}

/* static */
void ArrayBufferObject::copyData(ArrayBufferObject* toBuffer, size_t toIndex,
                                 ArrayBufferObject* fromBuffer,
                                 size_t fromIndex, size_t count) {
  MOZ_ASSERT(!toBuffer->isDetached());
  MOZ_ASSERT(toBuffer->byteLength() >= count);
  MOZ_ASSERT(toBuffer->byteLength() >= toIndex + count);
  MOZ_ASSERT(!fromBuffer->isDetached());
  MOZ_ASSERT(fromBuffer->byteLength() >= fromIndex);
  MOZ_ASSERT(fromBuffer->byteLength() >= fromIndex + count);

  memcpy(toBuffer->dataPointer() + toIndex,
         fromBuffer->dataPointer() + fromIndex, count);
}

template <class ArrayBufferType>
/* static */
size_t ArrayBufferObject::objectMoved(JSObject* obj, JSObject* old) {
  auto& dst = obj->as<ArrayBufferType>();
  const auto& src = old->as<ArrayBufferType>();

#ifdef DEBUG
  // Check the data pointer is not inside the nursery, but take account of the
  // fact that inline data pointers for zero length buffers can point to the end
  // of a chunk which can abut the start of the nursery.
  if (src.byteLength() != 0 || (uintptr_t(src.dataPointer()) & gc::ChunkMask)) {
    Nursery& nursery = obj->runtimeFromMainThread()->gc.nursery();
    MOZ_ASSERT(!nursery.isInside(src.dataPointer()));
  }
#endif

  // Fix up possible inline data pointer.
  if (src.hasInlineData()) {
    dst.setFixedSlot(DATA_SLOT, PrivateValue(dst.inlineDataPointer()));
  }

  return 0;
}

JSObject* ArrayBufferObject::firstView() {
  return getFixedSlot(FIRST_VIEW_SLOT).isObject()
             ? &getFixedSlot(FIRST_VIEW_SLOT).toObject()
             : nullptr;
}

void ArrayBufferObject::setFirstView(ArrayBufferViewObject* view) {
  setFixedSlot(FIRST_VIEW_SLOT, ObjectOrNullValue(view));
}

bool ArrayBufferObject::addView(JSContext* cx, ArrayBufferViewObject* view) {
  if (!firstView()) {
    setFirstView(view);
    return true;
  }

  return ObjectRealm::get(this).innerViews.get().addView(cx, this, view);
}

#if defined(DEBUG) || defined(JS_JITSPEW)

template <typename KnownF, typename UnknownF>
void BufferKindToString(ArrayBufferObject::BufferKind kind, KnownF known,
                        UnknownF unknown) {
  switch (kind) {
    case ArrayBufferObject::BufferKind::INLINE_DATA:
      known("INLINE_DATA");
      break;
    case ArrayBufferObject::BufferKind::MALLOCED_ARRAYBUFFER_CONTENTS_ARENA:
      known("MALLOCED_ARRAYBUFFER_CONTENTS_ARENA");
      break;
    case ArrayBufferObject::BufferKind::NO_DATA:
      known("NO_DATA");
      break;
    case ArrayBufferObject::BufferKind::USER_OWNED:
      known("USER_OWNED");
      break;
    case ArrayBufferObject::BufferKind::WASM:
      known("WASM");
      break;
    case ArrayBufferObject::BufferKind::MAPPED:
      known("MAPPED");
      break;
    case ArrayBufferObject::BufferKind::EXTERNAL:
      known("EXTERNAL");
      break;
    case ArrayBufferObject::BufferKind::MALLOCED_UNKNOWN_ARENA:
      known("MALLOCED_UNKNOWN_ARENA");
      break;
    default:
      unknown(uint8_t(kind));
      break;
  }
}

template <typename KnownF, typename UnknownF>
void ForEachArrayBufferFlag(uint32_t flags, KnownF known, UnknownF unknown) {
  for (uint32_t i = ArrayBufferObject::ArrayBufferFlags::BUFFER_KIND_MASK + 1;
       i; i = i << 1) {
    if (!(flags & i)) {
      continue;
    }
    switch (ArrayBufferObject::ArrayBufferFlags(flags & i)) {
      case ArrayBufferObject::ArrayBufferFlags::DETACHED:
        known("DETACHED");
        break;
      case ArrayBufferObject::ArrayBufferFlags::FOR_ASMJS:
        known("FOR_ASMJS");
        break;
      default:
        unknown(i);
        break;
    }
  }
}

void ArrayBufferObject::dumpOwnFields(js::JSONPrinter& json) const {
  json.formatProperty("byteLength", "%zu",
                      size_t(getFixedSlot(BYTE_LENGTH_SLOT).toPrivate()));

  BufferKindToString(
      bufferKind(),
      [&](const char* name) { json.property("bufferKind", name); },
      [&](uint8_t value) {
        json.formatProperty("bufferKind", "Unknown(%02x)", value);
      });

  json.beginInlineListProperty("flags");
  ForEachArrayBufferFlag(
      flags(), [&](const char* name) { json.value("%s", name); },
      [&](uint32_t value) { json.value("Unknown(%08x)", value); });
  json.endInlineList();

  void* data = dataPointer();
  if (data) {
    json.formatProperty("data", "0x%p", data);
  } else {
    json.nullProperty("data");
  }
}

void ArrayBufferObject::dumpOwnStringContent(js::GenericPrinter& out) const {
  out.printf("byteLength=%zu, ",
             size_t(getFixedSlot(BYTE_LENGTH_SLOT).toPrivate()));

  BufferKindToString(
      bufferKind(),
      [&](const char* name) { out.printf("bufferKind=%s, ", name); },
      [&](uint8_t value) { out.printf("bufferKind=Unknown(%02x), ", value); });

  out.printf("flags=[");
  bool first = true;
  ForEachArrayBufferFlag(
      flags(),
      [&](const char* name) {
        if (!first) {
          out.put(",");
        }
        first = false;
        out.put(name);
      },
      [&](uint32_t value) {
        if (!first) {
          out.put(",");
        }
        first = false;
        out.printf("Unknown(%08x)", value);
      });
  out.put("], ");

  void* data = dataPointer();
  if (data) {
    out.printf("data=0x%p", data);
  } else {
    out.put("data=null");
  }
}
#endif

/*
 * InnerViewTable
 */

inline bool InnerViewTable::Views::empty() { return views.empty(); }

inline bool InnerViewTable::Views::hasNurseryViews() {
  return firstNurseryView < views.length();
}

bool InnerViewTable::Views::addView(ArrayBufferViewObject* view) {
  // Add the view to the list, ensuring that all nursery views are at end.

  if (!views.append(view)) {
    return false;
  }

  if (!gc::IsInsideNursery(view)) {
    // Move tenured views before |firstNurseryView|.
    if (firstNurseryView != views.length() - 1) {
      std::swap(views[firstNurseryView], views.back());
    }
    firstNurseryView++;
  }

  check();

  return true;
}

bool InnerViewTable::Views::sweepAfterMinorGC(JSTracer* trc) {
  return traceWeak(trc, firstNurseryView);
}

bool InnerViewTable::Views::traceWeak(JSTracer* trc, size_t startIndex) {
  // Use |trc| to trace the view vector from |startIndex| to the end, removing
  // dead views and updating |firstNurseryView|.

  size_t index = startIndex;
  bool sawNurseryView = false;
  views.mutableEraseIf(
      [&](auto& view) {
        if (!JS::GCPolicy<ViewVector::ElementType>::traceWeak(trc, &view)) {
          return true;
        }

        if (!sawNurseryView && gc::IsInsideNursery(view)) {
          sawNurseryView = true;
          firstNurseryView = index;
        }

        index++;
        return false;
      },
      startIndex);

  if (!sawNurseryView) {
    firstNurseryView = views.length();
  }

  check();

  return !views.empty();
}

inline void InnerViewTable::Views::check() {
#ifdef DEBUG
  MOZ_ASSERT(firstNurseryView <= views.length());
  if (views.length() < 100) {
    for (size_t i = 0; i < views.length(); i++) {
      MOZ_ASSERT(gc::IsInsideNursery(views[i]) == (i >= firstNurseryView));
    }
  }
#endif
}

bool InnerViewTable::addView(JSContext* cx, ArrayBufferObject* buffer,
                             ArrayBufferViewObject* view) {
  // ArrayBufferObject entries are only added when there are multiple views.
  MOZ_ASSERT(buffer->firstView());
  MOZ_ASSERT(!gc::IsInsideNursery(buffer));

  // Ensure the buffer is present in the map, getting the list of views.
  auto ptr = map.lookupForAdd(buffer);
  if (!ptr && !map.add(ptr, buffer, Views(cx->zone()))) {
    ReportOutOfMemory(cx);
    return false;
  }
  Views& views = ptr->value();

  bool isNurseryView = gc::IsInsideNursery(view);
  bool hadNurseryViews = views.hasNurseryViews();
  if (!views.addView(view)) {
    ReportOutOfMemory(cx);
    return false;
  }

  // If we added the first nursery view, add the buffer to the list of buffers
  // which have nursery views.
  if (isNurseryView && !hadNurseryViews && nurseryKeysValid) {
#ifdef DEBUG
    if (nurseryKeys.length() < 100) {
      for (const auto& key : nurseryKeys) {
        MOZ_ASSERT(key != buffer);
      }
    }
#endif
    if (!nurseryKeys.append(buffer)) {
      nurseryKeysValid = false;
    }
  }

  return true;
}

InnerViewTable::ViewVector* InnerViewTable::maybeViewsUnbarriered(
    ArrayBufferObject* buffer) {
  auto ptr = map.lookup(buffer);
  if (ptr) {
    return &ptr->value().views;
  }
  return nullptr;
}

void InnerViewTable::removeViews(ArrayBufferObject* buffer) {
  auto ptr = map.lookup(buffer);
  MOZ_ASSERT(ptr);

  map.remove(ptr);
}

bool InnerViewTable::traceWeak(JSTracer* trc) {
  nurseryKeys.traceWeak(trc);
  map.traceWeak(trc);
  return true;
}

void InnerViewTable::sweepAfterMinorGC(JSTracer* trc) {
  MOZ_ASSERT(needsSweepAfterMinorGC());

  NurseryKeysVector keys;
  bool valid = true;
  std::swap(nurseryKeys, keys);
  std::swap(nurseryKeysValid, valid);

  // Use nursery keys vector if possible.
  if (valid) {
    for (ArrayBufferObject* buffer : keys) {
      MOZ_ASSERT(!gc::IsInsideNursery(buffer));
      auto ptr = map.lookup(buffer);
      if (ptr && !sweepViewsAfterMinorGC(trc, buffer, ptr->value())) {
        map.remove(ptr);
      }
    }
    return;
  }

  // Otherwise look at every map entry.
  for (ArrayBufferViewMap::Enum e(map); !e.empty(); e.popFront()) {
    MOZ_ASSERT(!gc::IsInsideNursery(e.front().key()));
    if (!sweepViewsAfterMinorGC(trc, e.front().key(), e.front().value())) {
      e.removeFront();
    }
  }
}

bool InnerViewTable::sweepViewsAfterMinorGC(JSTracer* trc,
                                            ArrayBufferObject* buffer,
                                            Views& views) {
  if (!views.sweepAfterMinorGC(trc)) {
    return false;  // No more views.
  }

  if (views.hasNurseryViews() && !nurseryKeys.append(buffer)) {
    nurseryKeysValid = false;
  }

  return true;
}

size_t InnerViewTable::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  size_t vectorSize = 0;
  for (auto r = map.all(); !r.empty(); r.popFront()) {
    vectorSize += r.front().value().views.sizeOfExcludingThis(mallocSizeOf);
  }

  return vectorSize + map.shallowSizeOfExcludingThis(mallocSizeOf) +
         nurseryKeys.sizeOfExcludingThis(mallocSizeOf);
}

template <>
bool JSObject::is<js::ArrayBufferObjectMaybeShared>() const {
  return is<ArrayBufferObject>() || is<SharedArrayBufferObject>();
}

JS_PUBLIC_API size_t JS::GetArrayBufferByteLength(JSObject* obj) {
  ArrayBufferObject* aobj = obj->maybeUnwrapAs<ArrayBufferObject>();
  return aobj ? aobj->byteLength() : 0;
}

JS_PUBLIC_API uint8_t* JS::GetArrayBufferData(JSObject* obj,
                                              bool* isSharedMemory,
                                              const JS::AutoRequireNoGC&) {
  ArrayBufferObject* aobj = obj->maybeUnwrapIf<ArrayBufferObject>();
  if (!aobj) {
    return nullptr;
  }
  *isSharedMemory = false;
  return aobj->dataPointer();
}

static ArrayBufferObject* UnwrapOrReportArrayBuffer(
    JSContext* cx, JS::Handle<JSObject*> maybeArrayBuffer) {
  JSObject* obj = CheckedUnwrapStatic(maybeArrayBuffer);
  if (!obj) {
    ReportAccessDenied(cx);
    return nullptr;
  }

  if (!obj->is<ArrayBufferObject>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_REQUIRED);
    return nullptr;
  }

  return &obj->as<ArrayBufferObject>();
}

JS_PUBLIC_API bool JS::DetachArrayBuffer(JSContext* cx, HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  Rooted<ArrayBufferObject*> unwrappedBuffer(
      cx, UnwrapOrReportArrayBuffer(cx, obj));
  if (!unwrappedBuffer) {
    return false;
  }

  if (unwrappedBuffer->hasDefinedDetachKey()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_NO_TRANSFER);
    return false;
  }
  if (unwrappedBuffer->isLengthPinned()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    return false;
  }

  AutoRealm ar(cx, unwrappedBuffer);
  ArrayBufferObject::detach(cx, unwrappedBuffer);
  return true;
}

JS_PUBLIC_API bool JS::HasDefinedArrayBufferDetachKey(JSContext* cx,
                                                      HandleObject obj,
                                                      bool* isDefined) {
  Rooted<ArrayBufferObject*> unwrappedBuffer(
      cx, UnwrapOrReportArrayBuffer(cx, obj));
  if (!unwrappedBuffer) {
    return false;
  }

  *isDefined = unwrappedBuffer->hasDefinedDetachKey();
  return true;
}

JS_PUBLIC_API bool JS::IsDetachedArrayBufferObject(JSObject* obj) {
  ArrayBufferObject* aobj = obj->maybeUnwrapIf<ArrayBufferObject>();
  if (!aobj) {
    return false;
  }

  return aobj->isDetached();
}

JS_PUBLIC_API JSObject* JS::NewArrayBuffer(JSContext* cx, size_t nbytes) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return ArrayBufferObject::createZeroed(cx, nbytes);
}

JS_PUBLIC_API JSObject* JS::NewArrayBufferWithContents(
    JSContext* cx, size_t nbytes,
    mozilla::UniquePtr<void, JS::FreePolicy> contents) {
  auto* result = NewArrayBufferWithContents(
      cx, nbytes, contents.get(),
      JS::NewArrayBufferOutOfMemory::CallerMustFreeMemory);
  if (result) {
    // If and only if an ArrayBuffer is successfully created, ownership of
    // |contents| is transferred to the new ArrayBuffer.
    (void)contents.release();
  }
  return result;
}

JS_PUBLIC_API JSObject* JS::NewArrayBufferWithContents(
    JSContext* cx, size_t nbytes, void* data, NewArrayBufferOutOfMemory) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_ASSERT_IF(!data, nbytes == 0);

  if (!data) {
    // Don't pass nulled contents to |createForContents|.
    return ArrayBufferObject::createZeroed(cx, 0);
  }

  using BufferContents = ArrayBufferObject::BufferContents;

  BufferContents contents = BufferContents::createMallocedUnknownArena(data);
  return ArrayBufferObject::createForContents(cx, nbytes, contents);
}

JS_PUBLIC_API JSObject* JS::CopyArrayBuffer(JSContext* cx,
                                            Handle<JSObject*> arrayBuffer) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  MOZ_ASSERT(arrayBuffer != nullptr);

  Rooted<ArrayBufferObject*> unwrappedSource(
      cx, UnwrapOrReportArrayBuffer(cx, arrayBuffer));
  if (!unwrappedSource) {
    return nullptr;
  }

  if (unwrappedSource->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }

  return ArrayBufferObject::copy(cx, unwrappedSource->byteLength(),
                                 unwrappedSource);
}

JS_PUBLIC_API JSObject* JS::NewExternalArrayBuffer(
    JSContext* cx, size_t nbytes,
    mozilla::UniquePtr<void, JS::BufferContentsDeleter> contents) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  MOZ_ASSERT(contents);

  using BufferContents = ArrayBufferObject::BufferContents;

  BufferContents bufferContents = BufferContents::createExternal(
      contents.get(), contents.get_deleter().freeFunc(),
      contents.get_deleter().userData());
  auto* result =
      ArrayBufferObject::createForContents(cx, nbytes, bufferContents);
  if (result) {
    // If and only if an ArrayBuffer is successfully created, ownership of
    // |contents| is transferred to the new ArrayBuffer.
    (void)contents.release();
  }
  return result;
}

JS_PUBLIC_API JSObject* JS::NewArrayBufferWithUserOwnedContents(JSContext* cx,
                                                                size_t nbytes,
                                                                void* data) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  MOZ_ASSERT(data);

  using BufferContents = ArrayBufferObject::BufferContents;

  BufferContents contents = BufferContents::createUserOwned(data);
  return ArrayBufferObject::createForContents(cx, nbytes, contents);
}

JS_PUBLIC_API bool JS::IsArrayBufferObject(JSObject* obj) {
  return obj->canUnwrapAs<ArrayBufferObject>();
}

JS_PUBLIC_API bool JS::ArrayBufferHasData(JSObject* obj) {
  return !obj->unwrapAs<ArrayBufferObject>().isDetached();
}

JS_PUBLIC_API JSObject* JS::UnwrapArrayBuffer(JSObject* obj) {
  return obj->maybeUnwrapIf<ArrayBufferObject>();
}

JS_PUBLIC_API JSObject* JS::UnwrapSharedArrayBuffer(JSObject* obj) {
  return obj->maybeUnwrapIf<SharedArrayBufferObject>();
}

JS_PUBLIC_API void* JS::StealArrayBufferContents(JSContext* cx,
                                                 HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  Rooted<ArrayBufferObject*> unwrappedBuffer(
      cx, UnwrapOrReportArrayBuffer(cx, obj));
  if (!unwrappedBuffer) {
    return nullptr;
  }

  if (unwrappedBuffer->isDetached()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }
  if (unwrappedBuffer->isLengthPinned()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_PINNED);
    return nullptr;
  }

  if (unwrappedBuffer->hasDefinedDetachKey()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_NO_TRANSFER);
    return nullptr;
  }

  AutoRealm ar(cx, unwrappedBuffer);
  return ArrayBufferObject::stealMallocedContents(cx, unwrappedBuffer);
}

JS_PUBLIC_API JSObject* JS::NewMappedArrayBufferWithContents(JSContext* cx,
                                                             size_t nbytes,
                                                             void* data) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  MOZ_ASSERT(data);

  using BufferContents = ArrayBufferObject::BufferContents;

  BufferContents contents = BufferContents::createMapped(data);
  return ArrayBufferObject::createForContents(cx, nbytes, contents);
}

JS_PUBLIC_API void* JS::CreateMappedArrayBufferContents(int fd, size_t offset,
                                                        size_t length) {
  return ArrayBufferObject::createMappedContents(fd, offset, length).data();
}

JS_PUBLIC_API void JS::ReleaseMappedArrayBufferContents(void* contents,
                                                        size_t length) {
  gc::DeallocateMappedContent(contents, length);
}

JS_PUBLIC_API bool JS::IsMappedArrayBufferObject(JSObject* obj) {
  ArrayBufferObject* aobj = obj->maybeUnwrapIf<ArrayBufferObject>();
  if (!aobj) {
    return false;
  }

  return aobj->isMapped();
}

JS_PUBLIC_API JSObject* JS::GetObjectAsArrayBuffer(JSObject* obj,
                                                   size_t* length,
                                                   uint8_t** data) {
  ArrayBufferObject* aobj = obj->maybeUnwrapIf<ArrayBufferObject>();
  if (!aobj) {
    return nullptr;
  }

  *length = aobj->byteLength();
  *data = aobj->dataPointer();

  return aobj;
}

JS_PUBLIC_API void JS::GetArrayBufferLengthAndData(JSObject* obj,
                                                   size_t* length,
                                                   bool* isSharedMemory,
                                                   uint8_t** data) {
  auto& aobj = obj->as<ArrayBufferObject>();
  *length = aobj.byteLength();
  *data = aobj.dataPointer();
  *isSharedMemory = false;
}

const JSClass* const JS::ArrayBuffer::FixedLengthUnsharedClass =
    &FixedLengthArrayBufferObject::class_;
const JSClass* const JS::ArrayBuffer::ResizableUnsharedClass =
    &ResizableArrayBufferObject::class_;
const JSClass* const JS::ArrayBuffer::FixedLengthSharedClass =
    &FixedLengthSharedArrayBufferObject::class_;
const JSClass* const JS::ArrayBuffer::GrowableSharedClass =
    &GrowableSharedArrayBufferObject::class_;

/* static */ JS::ArrayBuffer JS::ArrayBuffer::create(JSContext* cx,
                                                     size_t nbytes) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return JS::ArrayBuffer(ArrayBufferObject::createZeroed(cx, nbytes));
}

mozilla::Span<uint8_t> JS::ArrayBuffer::getData(
    bool* isSharedMemory, const JS::AutoRequireNoGC& nogc) {
  auto* buffer = obj->maybeUnwrapAs<ArrayBufferObjectMaybeShared>();
  if (!buffer) {
    return nullptr;
  }
  size_t length = buffer->byteLength();
  if (buffer->is<SharedArrayBufferObject>()) {
    *isSharedMemory = true;
    return {buffer->dataPointerEither().unwrap(), length};
  }
  *isSharedMemory = false;
  return {buffer->as<ArrayBufferObject>().dataPointer(), length};
};

JS::ArrayBuffer JS::ArrayBuffer::unwrap(JSObject* maybeWrapped) {
  if (!maybeWrapped) {
    return JS::ArrayBuffer(nullptr);
  }
  auto* ab = maybeWrapped->maybeUnwrapIf<ArrayBufferObjectMaybeShared>();
  return fromObject(ab);
}

bool JS::ArrayBufferCopyData(JSContext* cx, Handle<JSObject*> toBlock,
                             size_t toIndex, Handle<JSObject*> fromBlock,
                             size_t fromIndex, size_t count) {
  Rooted<ArrayBufferObjectMaybeShared*> unwrappedToBlock(
      cx, toBlock->maybeUnwrapIf<ArrayBufferObjectMaybeShared>());
  if (!unwrappedToBlock) {
    ReportAccessDenied(cx);
    return false;
  }

  Rooted<ArrayBufferObjectMaybeShared*> unwrappedFromBlock(
      cx, fromBlock->maybeUnwrapIf<ArrayBufferObjectMaybeShared>());
  if (!unwrappedFromBlock) {
    ReportAccessDenied(cx);
    return false;
  }

  // Verify that lengths still make sense and throw otherwise.
  if (toIndex + count < toIndex ||      // size_t overflow
      fromIndex + count < fromIndex ||  // size_t overflow
      toIndex + count > unwrappedToBlock->byteLength() ||
      fromIndex + count > unwrappedFromBlock->byteLength()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_COPY_RANGE);
    return false;
  }

  // If both are array buffers, can use ArrayBufferCopyData
  if (unwrappedToBlock->is<ArrayBufferObject>() &&
      unwrappedFromBlock->is<ArrayBufferObject>()) {
    Rooted<ArrayBufferObject*> toArray(
        cx, &unwrappedToBlock->as<ArrayBufferObject>());
    Rooted<ArrayBufferObject*> fromArray(
        cx, &unwrappedFromBlock->as<ArrayBufferObject>());
    ArrayBufferObject::copyData(toArray, toIndex, fromArray, fromIndex, count);
    return true;
  }

  Rooted<ArrayBufferObjectMaybeShared*> toArray(
      cx, &unwrappedToBlock->as<ArrayBufferObjectMaybeShared>());
  Rooted<ArrayBufferObjectMaybeShared*> fromArray(
      cx, &unwrappedFromBlock->as<ArrayBufferObjectMaybeShared>());
  SharedArrayBufferObject::copyData(toArray, toIndex, fromArray, fromIndex,
                                    count);

  return true;
}

// https://tc39.es/ecma262/#sec-clonearraybuffer
// We only support the case where cloneConstructor is %ArrayBuffer%. Note,
// this means that cloning a SharedArrayBuffer will produce an ArrayBuffer
JSObject* JS::ArrayBufferClone(JSContext* cx, Handle<JSObject*> srcBuffer,
                               size_t srcByteOffset, size_t srcLength) {
  MOZ_ASSERT(srcBuffer->is<ArrayBufferObjectMaybeShared>());

  // 2. (reordered) If IsDetachedBuffer(srcBuffer) is true, throw a TypeError
  // exception.
  if (IsDetachedArrayBufferObject(srcBuffer)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return nullptr;
  }

  // 1. Let targetBuffer be ? AllocateArrayBuffer(cloneConstructor, srcLength).
  JS::RootedObject targetBuffer(cx, JS::NewArrayBuffer(cx, srcLength));
  if (!targetBuffer) {
    return nullptr;
  }

  // 3. Let srcBlock be srcBuffer.[[ArrayBufferData]].
  // 4. Let targetBlock be targetBuffer.[[ArrayBufferData]].
  // 5. Perform CopyDataBlockBytes(targetBlock, 0, srcBlock, srcByteOffset,
  // srcLength).
  if (!ArrayBufferCopyData(cx, targetBuffer, 0, srcBuffer, srcByteOffset,
                           srcLength)) {
    return nullptr;
  }

  // 6. Return targetBuffer.
  return targetBuffer;
}

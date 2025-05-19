/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SharedArrayObject.h"

#include "mozilla/Atomics.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/TaggedAnonymousMemory.h"

#include "gc/GCContext.h"
#include "gc/Memory.h"
#include "jit/AtomicOperations.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Prefs.h"
#include "js/PropertySpec.h"
#include "js/SharedArrayBuffer.h"
#include "util/Memory.h"
#include "util/WindowsWrapper.h"
#include "vm/SharedMem.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmMemory.h"

#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using js::wasm::Pages;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

using namespace js;
using namespace js::jit;

static size_t WasmSharedArrayAccessibleSize(size_t length) {
  return AlignBytes(length, gc::SystemPageSize());
}

static size_t NonWasmSharedArrayAllocSize(size_t length) {
  MOZ_ASSERT(length <= ArrayBufferObject::ByteLengthLimit);
  return sizeof(SharedArrayRawBuffer) + length;
}

// The mapped size for a plain shared array buffer, used only for tracking
// memory usage. This is incorrect for some WASM cases, and for hypothetical
// callers of js::SharedArrayBufferObject::createFromNewRawBuffer that do not
// currently exist, but it's fine as a signal of GC pressure.
static size_t SharedArrayMappedSize(bool isWasm, size_t length) {
  // Wasm buffers use MapBufferMemory and allocate a full page for the header.
  // Non-Wasm buffers use malloc.
  if (isWasm) {
    return WasmSharedArrayAccessibleSize(length) + gc::SystemPageSize();
  }
  return NonWasmSharedArrayAllocSize(length);
}

SharedArrayRawBuffer* SharedArrayRawBuffer::Allocate(bool isGrowable,
                                                     size_t length,
                                                     size_t maxLength) {
  MOZ_RELEASE_ASSERT(length <= ArrayBufferObject::ByteLengthLimit);
  MOZ_RELEASE_ASSERT(maxLength <= ArrayBufferObject::ByteLengthLimit);
  MOZ_ASSERT_IF(!isGrowable, length == maxLength);
  MOZ_ASSERT_IF(isGrowable, length <= maxLength);

  size_t allocSize = NonWasmSharedArrayAllocSize(maxLength);
  uint8_t* p = js_pod_calloc<uint8_t>(allocSize);
  if (!p) {
    return nullptr;
  }
  MOZ_ASSERT(reinterpret_cast<uintptr_t>(p) %
                     ArrayBufferObject::ARRAY_BUFFER_ALIGNMENT ==
                 0,
             "shared array buffer memory is aligned");

  // jemalloc tiny allocations can produce allocations not aligned to the
  // smallest std::malloc allocation. Ensure shared array buffer allocations
  // don't have to worry about this special case.
  static_assert(sizeof(SharedArrayRawBuffer) > sizeof(void*),
                "SharedArrayRawBuffer doesn't fit in jemalloc tiny allocation");

  static_assert(sizeof(SharedArrayRawBuffer) %
                        ArrayBufferObject::ARRAY_BUFFER_ALIGNMENT ==
                    0,
                "sizeof(SharedArrayRawBuffer) is a multiple of the array "
                "buffer alignment, so |p + sizeof(SharedArrayRawBuffer)| is "
                "also array buffer aligned");

  uint8_t* buffer = p + sizeof(SharedArrayRawBuffer);
  return new (p) SharedArrayRawBuffer(isGrowable, buffer, length);
}

WasmSharedArrayRawBuffer* WasmSharedArrayRawBuffer::AllocateWasm(
    wasm::IndexType indexType, Pages initialPages, wasm::Pages clampedMaxPages,
    const mozilla::Maybe<wasm::Pages>& sourceMaxPages,
    const mozilla::Maybe<size_t>& mappedSize) {
  // Prior code has asserted that initial pages is within our implementation
  // limits (wasm::MaxMemoryPages()) and we can assume it is a valid size_t.
  MOZ_ASSERT(initialPages.hasByteLength());
  size_t length = initialPages.byteLength();

  MOZ_RELEASE_ASSERT(length <= ArrayBufferObject::ByteLengthLimit);

  size_t accessibleSize = WasmSharedArrayAccessibleSize(length);
  if (accessibleSize < length) {
    return nullptr;
  }

  size_t computedMappedSize = mappedSize.isSome()
                                  ? *mappedSize
                                  : wasm::ComputeMappedSize(clampedMaxPages);
  MOZ_ASSERT(accessibleSize <= computedMappedSize);

  uint64_t mappedSizeWithHeader = computedMappedSize + gc::SystemPageSize();
  uint64_t accessibleSizeWithHeader = accessibleSize + gc::SystemPageSize();

  void* p = MapBufferMemory(indexType, mappedSizeWithHeader,
                            accessibleSizeWithHeader);
  if (!p) {
    return nullptr;
  }

  uint8_t* buffer = reinterpret_cast<uint8_t*>(p) + gc::SystemPageSize();
  uint8_t* base = buffer - sizeof(WasmSharedArrayRawBuffer);
  return new (base) WasmSharedArrayRawBuffer(
      buffer, length, indexType, clampedMaxPages,
      sourceMaxPages.valueOr(Pages(0)), computedMappedSize);
}

void WasmSharedArrayRawBuffer::tryGrowMaxPagesInPlace(Pages deltaMaxPages) {
  Pages newMaxPages = clampedMaxPages_;
  DebugOnly<bool> valid = newMaxPages.checkedIncrement(deltaMaxPages);
  // Caller must ensure increment does not overflow or increase over the
  // specified maximum pages.
  MOZ_ASSERT(valid);
  MOZ_ASSERT(newMaxPages <= sourceMaxPages_);

  size_t newMappedSize = wasm::ComputeMappedSize(newMaxPages);
  MOZ_ASSERT(mappedSize_ <= newMappedSize);
  if (mappedSize_ == newMappedSize) {
    return;
  }

  if (!ExtendBufferMapping(basePointer(), mappedSize_, newMappedSize)) {
    return;
  }

  mappedSize_ = newMappedSize;
  clampedMaxPages_ = newMaxPages;
}

bool WasmSharedArrayRawBuffer::wasmGrowToPagesInPlace(const Lock&,
                                                      wasm::IndexType t,
                                                      wasm::Pages newPages) {
  // Check that the new pages is within our allowable range. This will
  // simultaneously check against the maximum specified in source and our
  // implementation limits.
  if (newPages > clampedMaxPages_) {
    return false;
  }
  MOZ_ASSERT(newPages <= wasm::MaxMemoryPages(t) &&
             newPages.byteLength() <= ArrayBufferObject::ByteLengthLimit);

  // We have checked against the clamped maximum and so we know we can convert
  // to byte lengths now.
  size_t newLength = newPages.byteLength();

  MOZ_ASSERT(newLength >= length_);

  if (newLength == length_) {
    return true;
  }

  size_t delta = newLength - length_;
  MOZ_ASSERT(delta % wasm::PageSize == 0);

  uint8_t* dataEnd = dataPointerShared().unwrap(/* for resize */) + length_;
  MOZ_ASSERT(uintptr_t(dataEnd) % gc::SystemPageSize() == 0);

  if (!CommitBufferMemory(dataEnd, delta)) {
    return false;
  }

  // We rely on CommitBufferMemory (and therefore memmap/VirtualAlloc) to only
  // return once it has committed memory for all threads. We only update with a
  // new length once this has occurred.
  length_ = newLength;

  return true;
}

void WasmSharedArrayRawBuffer::discard(size_t byteOffset, size_t byteLen) {
  SharedMem<uint8_t*> memBase = dataPointerShared();

  // The caller is responsible for ensuring these conditions are met; see this
  // function's comment in SharedArrayObject.h.
  MOZ_ASSERT(byteOffset % wasm::PageSize == 0);
  MOZ_ASSERT(byteLen % wasm::PageSize == 0);
  MOZ_ASSERT(wasm::MemoryBoundsCheck(uint64_t(byteOffset), uint64_t(byteLen),
                                     volatileByteLength()));

  // Discarding zero bytes "succeeds" with no effect.
  if (byteLen == 0) {
    return;
  }

  SharedMem<uint8_t*> addr = memBase + uintptr_t(byteOffset);

  // On POSIX-ish platforms, we discard memory by overwriting previously-mapped
  // pages with freshly-mapped pages (which are all zeroed). The operating
  // system recognizes this and decreases the process RSS, and eventually
  // collects the abandoned physical pages.
  //
  // On Windows, committing over previously-committed pages has no effect. We
  // could decommit and recommit, but this doesn't work for shared memories
  // since other threads could access decommitted memory - causing a trap.
  // Instead, we simply zero memory (memset 0), and then VirtualUnlock(), which
  // for Historical Reasons immediately removes the pages from the working set.
  // And then, because the pages were zeroed, Windows will actually reclaim the
  // memory entirely instead of paging it out to disk. Naturally this behavior
  // is not officially documented, but a Raymond Chen blog post is basically as
  // good as MSDN, right?
  //
  // https://devblogs.microsoft.com/oldnewthing/20170113-00/?p=95185

#ifdef XP_WIN
  // Discarding the entire region at once causes us to page the entire region
  // into the working set, only to throw it out again. This can be actually
  // disastrous when discarding already-discarded memory. To mitigate this, we
  // discard a chunk of memory at a time - this comes at a small performance
  // cost from syscalls and potentially less-optimal memsets.
  size_t numPages = byteLen / wasm::PageSize;
  for (size_t i = 0; i < numPages; i++) {
    AtomicOperations::memsetSafeWhenRacy(addr + (i * wasm::PageSize), 0,
                                         wasm::PageSize);
    DebugOnly<bool> result =
        VirtualUnlock(addr.unwrap() + (i * wasm::PageSize), wasm::PageSize);
    MOZ_ASSERT(!result);  // this always "fails" when unlocking unlocked
                          // memory...which is the only case we care about
  }
#elif defined(__wasi__)
  AtomicOperations::memsetSafeWhenRacy(addr, 0, byteLen);
#else  // !XP_WIN
  void* data = MozTaggedAnonymousMmap(
      addr.unwrap(), byteLen, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0, "wasm-reserved");
  if (data == MAP_FAILED) {
    MOZ_CRASH("failed to discard wasm memory; memory mappings may be broken");
  }
#endif
}

bool SharedArrayRawBuffer::addReference() {
  MOZ_RELEASE_ASSERT(refcount_ > 0);

  // Be careful never to overflow the refcount field.
  for (;;) {
    uint32_t old_refcount = refcount_;
    uint32_t new_refcount = old_refcount + 1;
    if (new_refcount == 0) {
      return false;
    }
    if (refcount_.compareExchange(old_refcount, new_refcount)) {
      return true;
    }
  }
}

void SharedArrayRawBuffer::dropReference() {
  // Normally if the refcount is zero then the memory will have been unmapped
  // and this test may just crash, but if the memory has been retained for any
  // reason we will catch the underflow here.
  MOZ_RELEASE_ASSERT(refcount_ > 0);

  // Drop the reference to the buffer.
  uint32_t new_refcount = --refcount_;  // Atomic.
  if (new_refcount) {
    return;
  }

  // This was the final reference, so release the buffer.
  if (isWasm()) {
    WasmSharedArrayRawBuffer* wasmBuf = toWasmBuffer();
    wasm::IndexType indexType = wasmBuf->wasmIndexType();
    uint8_t* basePointer = wasmBuf->basePointer();
    size_t mappedSizeWithHeader = wasmBuf->mappedSize() + gc::SystemPageSize();
    // Call the destructor to destroy the growLock_ Mutex.
    wasmBuf->~WasmSharedArrayRawBuffer();
    UnmapBufferMemory(indexType, basePointer, mappedSizeWithHeader);
  } else {
    js_delete(this);
  }
}

bool SharedArrayRawBuffer::grow(size_t newByteLength) {
  MOZ_RELEASE_ASSERT(isGrowable());

  // The caller is responsible to ensure |newByteLength| doesn't exceed the
  // maximum allowed byte length.

  while (true) {
    // `mozilla::Atomic::compareExchange` doesn't return the current value, so
    // we need to perform a normal load here. (bug 1005335)
    size_t oldByteLength = length_;
    if (newByteLength == oldByteLength) {
      return true;
    }
    if (newByteLength < oldByteLength) {
      return false;
    }
    if (length_.compareExchange(oldByteLength, newByteLength)) {
      return true;
    }
  }
}

static bool IsSharedArrayBuffer(HandleValue v) {
  return v.isObject() && v.toObject().is<SharedArrayBufferObject>();
}

static bool IsGrowableSharedArrayBuffer(HandleValue v) {
  return v.isObject() && v.toObject().is<GrowableSharedArrayBufferObject>();
}

MOZ_ALWAYS_INLINE bool SharedArrayBufferObject::byteLengthGetterImpl(
    JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsSharedArrayBuffer(args.thisv()));
  auto* buffer = &args.thisv().toObject().as<SharedArrayBufferObject>();
  args.rval().setNumber(buffer->byteLength());
  return true;
}

bool SharedArrayBufferObject::byteLengthGetter(JSContext* cx, unsigned argc,
                                               Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsSharedArrayBuffer, byteLengthGetterImpl>(cx,
                                                                         args);
}

/**
 * get SharedArrayBuffer.prototype.maxByteLength
 */
bool SharedArrayBufferObject::maxByteLengthGetterImpl(JSContext* cx,
                                                      const CallArgs& args) {
  MOZ_ASSERT(IsSharedArrayBuffer(args.thisv()));
  auto* buffer = &args.thisv().toObject().as<SharedArrayBufferObject>();

  // Steps 4-6.
  args.rval().setNumber(buffer->byteLengthOrMaxByteLength());
  return true;
}

/**
 * get SharedArrayBuffer.prototype.maxByteLength
 */
bool SharedArrayBufferObject::maxByteLengthGetter(JSContext* cx, unsigned argc,
                                                  Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsSharedArrayBuffer, maxByteLengthGetterImpl>(
      cx, args);
}

/**
 * get SharedArrayBuffer.prototype.growable
 */
bool SharedArrayBufferObject::growableGetterImpl(JSContext* cx,
                                                 const CallArgs& args) {
  MOZ_ASSERT(IsSharedArrayBuffer(args.thisv()));
  auto* buffer = &args.thisv().toObject().as<SharedArrayBufferObject>();

  // Step 4.
  args.rval().setBoolean(buffer->isGrowable());
  return true;
}

/**
 * get SharedArrayBuffer.prototype.growable
 */
bool SharedArrayBufferObject::growableGetter(JSContext* cx, unsigned argc,
                                             Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsSharedArrayBuffer, growableGetterImpl>(cx,
                                                                       args);
}

/**
 * SharedArrayBuffer.prototype.grow ( newLength )
 */
bool SharedArrayBufferObject::growImpl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsGrowableSharedArrayBuffer(args.thisv()));
  Rooted<GrowableSharedArrayBufferObject*> buffer(
      cx, &args.thisv().toObject().as<GrowableSharedArrayBufferObject>());

  // Step 4.
  uint64_t newByteLength;
  if (!ToIndex(cx, args.get(0), &newByteLength)) {
    return false;
  }

  // Steps 5-11.
  if (newByteLength > buffer->maxByteLength()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_ARRAYBUFFER_LENGTH_LARGER_THAN_MAXIMUM);
    return false;
  }
  if (!buffer->rawBufferObject()->grow(newByteLength)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHARED_ARRAY_LENGTH_SMALLER_THAN_CURRENT);
    return false;
  }

  args.rval().setUndefined();
  return true;
}

/**
 * SharedArrayBuffer.prototype.grow ( newLength )
 */
bool SharedArrayBufferObject::grow(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-3.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsGrowableSharedArrayBuffer, growImpl>(cx, args);
}

// ES2024 draft rev 3a773fc9fae58be023228b13dbbd402ac18eeb6b
// 25.2.3.1 SharedArrayBuffer ( length [ , options ] )
bool SharedArrayBufferObject::class_constructor(JSContext* cx, unsigned argc,
                                                Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "SharedArrayBuffer")) {
    return false;
  }

  // Step 2.
  uint64_t byteLength;
  if (!ToIndex(cx, args.get(0), &byteLength)) {
    return false;
  }

  // Step 3.
  mozilla::Maybe<uint64_t> maxByteLength;
  if (JS::Prefs::experimental_sharedarraybuffer_growable()) {
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

        // 25.2.2.1 AllocateSharedArrayBuffer, step 3.a.
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

  // Step 4 (Inlined 25.2.2.1 AllocateSharedArrayBuffer).
  // 25.2.2.1, step 5 (Inlined 10.1.13 OrdinaryCreateFromConstructor, step 2).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_SharedArrayBuffer,
                                          &proto)) {
    return false;
  }

  // 25.2.2.1, step 6.
  uint64_t allocLength = maxByteLength.valueOr(byteLength);

  // 25.2.2.1, step 7 (Inlined 6.2.9.2 CreateSharedByteDataBlock, step 1).
  // Refuse to allocate too large buffers.
  if (allocLength > ArrayBufferObject::ByteLengthLimit) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHARED_ARRAY_BAD_LENGTH);
    return false;
  }

  if (maxByteLength) {
    // 25.2.2.1, remaining steps.
    auto* bufobj = NewGrowable(cx, byteLength, *maxByteLength, proto);
    if (!bufobj) {
      return false;
    }
    args.rval().setObject(*bufobj);
    return true;
  }

  // 25.2.2.1, remaining steps.
  JSObject* bufobj = New(cx, byteLength, proto);
  if (!bufobj) {
    return false;
  }
  args.rval().setObject(*bufobj);
  return true;
}

FixedLengthSharedArrayBufferObject* SharedArrayBufferObject::New(
    JSContext* cx, size_t length, HandleObject proto) {
  bool isGrowable = false;
  size_t maxLength = length;
  auto* buffer = SharedArrayRawBuffer::Allocate(isGrowable, length, maxLength);
  if (!buffer) {
    js::ReportOutOfMemory(cx);
    return nullptr;
  }

  auto* obj = New(cx, buffer, length, proto);
  if (!obj) {
    buffer->dropReference();
    return nullptr;
  }

  return obj;
}

FixedLengthSharedArrayBufferObject* SharedArrayBufferObject::New(
    JSContext* cx, SharedArrayRawBuffer* buffer, size_t length,
    HandleObject proto) {
  return NewWith<FixedLengthSharedArrayBufferObject>(cx, buffer, length, proto);
}

GrowableSharedArrayBufferObject* SharedArrayBufferObject::NewGrowable(
    JSContext* cx, size_t length, size_t maxLength, HandleObject proto) {
  bool isGrowable = true;
  auto* buffer = SharedArrayRawBuffer::Allocate(isGrowable, length, maxLength);
  if (!buffer) {
    js::ReportOutOfMemory(cx);
    return nullptr;
  }

  auto* obj = NewGrowable(cx, buffer, maxLength, proto);
  if (!obj) {
    buffer->dropReference();
    return nullptr;
  }

  return obj;
}

GrowableSharedArrayBufferObject* SharedArrayBufferObject::NewGrowable(
    JSContext* cx, SharedArrayRawBuffer* buffer, size_t maxLength,
    HandleObject proto) {
  return NewWith<GrowableSharedArrayBufferObject>(cx, buffer, maxLength, proto);
}

template <class SharedArrayBufferType>
SharedArrayBufferType* SharedArrayBufferObject::NewWith(
    JSContext* cx, SharedArrayRawBuffer* buffer, size_t length,
    HandleObject proto) {
  MOZ_ASSERT(cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled());

  static_assert(
      std::is_same_v<SharedArrayBufferType,
                     FixedLengthSharedArrayBufferObject> ||
      std::is_same_v<SharedArrayBufferType, GrowableSharedArrayBufferObject>);

  if constexpr (std::is_same_v<SharedArrayBufferType,
                               FixedLengthSharedArrayBufferObject>) {
    MOZ_ASSERT(!buffer->isGrowable());
  } else {
    MOZ_ASSERT(buffer->isGrowable());
  }

  AutoSetNewObjectMetadata metadata(cx);
  auto* obj = NewObjectWithClassProto<SharedArrayBufferType>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(obj->getClass() == &SharedArrayBufferType::class_);

  cx->runtime()->incSABCount();

  if (!obj->acceptRawBuffer(buffer, length)) {
    js::ReportOutOfMemory(cx);
    return nullptr;
  }

  return obj;
}

bool SharedArrayBufferObject::acceptRawBuffer(SharedArrayRawBuffer* buffer,
                                              size_t length) {
  MOZ_ASSERT(!isInitialized());
  if (!zone()->addSharedMemory(buffer,
                               SharedArrayMappedSize(buffer->isWasm(), length),
                               MemoryUse::SharedArrayRawBuffer)) {
    return false;
  }

  setFixedSlot(RAWBUF_SLOT, PrivateValue(buffer));
  setFixedSlot(LENGTH_SLOT, PrivateValue(length));
  MOZ_ASSERT(isInitialized());
  return true;
}

void SharedArrayBufferObject::dropRawBuffer() {
  size_t length = byteLengthOrMaxByteLength();
  size_t size = SharedArrayMappedSize(isWasm(), length);
  zoneFromAnyThread()->removeSharedMemory(rawBufferObject(), size,
                                          MemoryUse::SharedArrayRawBuffer);
  rawBufferObject()->dropReference();
  setFixedSlot(RAWBUF_SLOT, UndefinedValue());
  MOZ_ASSERT(!isInitialized());
}

SharedArrayRawBuffer* SharedArrayBufferObject::rawBufferObject() const {
  Value v = getFixedSlot(RAWBUF_SLOT);
  MOZ_ASSERT(!v.isUndefined());
  return reinterpret_cast<SharedArrayRawBuffer*>(v.toPrivate());
}

void SharedArrayBufferObject::Finalize(JS::GCContext* gcx, JSObject* obj) {
  // Must be foreground finalizable so that we can account for the object.
  MOZ_ASSERT(gcx->onMainThread());
  gcx->runtime()->decSABCount();

  SharedArrayBufferObject& buf = obj->as<SharedArrayBufferObject>();

  // Detect the case of failure during SharedArrayBufferObject creation,
  // which causes a SharedArrayRawBuffer to never be attached.
  Value v = buf.getFixedSlot(RAWBUF_SLOT);
  if (!v.isUndefined()) {
    buf.dropRawBuffer();
  }
}

/* static */
void SharedArrayBufferObject::addSizeOfExcludingThis(
    JSObject* obj, mozilla::MallocSizeOf mallocSizeOf, JS::ClassInfo* info,
    JS::RuntimeSizes* runtimeSizes) {
  // Divide the buffer size by the refcount to get the fraction of the buffer
  // owned by this thread. It's conceivable that the refcount might change in
  // the middle of memory reporting, in which case the amount reported for
  // some threads might be to high (if the refcount goes up) or too low (if
  // the refcount goes down). But that's unlikely and hard to avoid, so we
  // just live with the risk.
  const SharedArrayBufferObject& buf = obj->as<SharedArrayBufferObject>();

  if (MOZ_UNLIKELY(!buf.isInitialized())) {
    return;
  }

  size_t nbytes = buf.byteLengthOrMaxByteLength();
  size_t owned = nbytes / buf.rawBufferObject()->refcount();
  if (buf.isWasm()) {
    info->objectsNonHeapElementsWasmShared += owned;
    if (runtimeSizes) {
      size_t ownedGuardPages =
          (buf.wasmMappedSize() - nbytes) / buf.rawBufferObject()->refcount();
      runtimeSizes->wasmGuardPages += ownedGuardPages;
    }
  } else {
    info->objectsNonHeapElementsShared += owned;
  }
}

/* static */
void SharedArrayBufferObject::copyData(
    Handle<ArrayBufferObjectMaybeShared*> toBuffer, size_t toIndex,
    Handle<ArrayBufferObjectMaybeShared*> fromBuffer, size_t fromIndex,
    size_t count) {
  MOZ_ASSERT(toBuffer->byteLength() >= count);
  MOZ_ASSERT(toBuffer->byteLength() >= toIndex + count);
  MOZ_ASSERT(fromBuffer->byteLength() >= fromIndex);
  MOZ_ASSERT(fromBuffer->byteLength() >= fromIndex + count);

  jit::AtomicOperations::memcpySafeWhenRacy(
      toBuffer->dataPointerEither() + toIndex,
      fromBuffer->dataPointerEither() + fromIndex, count);
}

SharedArrayBufferObject* SharedArrayBufferObject::createFromNewRawBuffer(
    JSContext* cx, WasmSharedArrayRawBuffer* buffer, size_t initialSize) {
  MOZ_ASSERT(cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled());

  AutoSetNewObjectMetadata metadata(cx);
  auto* obj = NewBuiltinClassInstance<FixedLengthSharedArrayBufferObject>(cx);
  if (!obj) {
    buffer->dropReference();
    return nullptr;
  }

  cx->runtime()->incSABCount();

  if (!obj->acceptRawBuffer(buffer, initialSize)) {
    buffer->dropReference();
    js::ReportOutOfMemory(cx);
    return nullptr;
  }

  return obj;
}

/* static */
void SharedArrayBufferObject::wasmDiscard(Handle<SharedArrayBufferObject*> buf,
                                          uint64_t byteOffset,
                                          uint64_t byteLen) {
  MOZ_ASSERT(buf->isWasm());
  buf->rawWasmBufferObject()->discard(byteOffset, byteLen);
}

static const JSClassOps SharedArrayBufferObjectClassOps = {
    nullptr,                            // addProperty
    nullptr,                            // delProperty
    nullptr,                            // enumerate
    nullptr,                            // newEnumerate
    nullptr,                            // resolve
    nullptr,                            // mayResolve
    SharedArrayBufferObject::Finalize,  // finalize
    nullptr,                            // call
    nullptr,                            // construct
    nullptr,                            // trace
};

static const JSFunctionSpec sharedarray_functions[] = {
    JS_FS_END,
};

static const JSPropertySpec sharedarray_properties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "$SharedArrayBufferSpecies", 0),
    JS_PS_END,
};

static const JSFunctionSpec sharedarray_proto_functions[] = {
    JS_SELF_HOSTED_FN("slice", "SharedArrayBufferSlice", 2, 0),
    JS_FN("grow", SharedArrayBufferObject::grow, 1, 0),
    JS_FS_END,
};

static const JSPropertySpec sharedarray_proto_properties[] = {
    JS_PSG("byteLength", SharedArrayBufferObject::byteLengthGetter, 0),
    JS_PSG("maxByteLength", SharedArrayBufferObject::maxByteLengthGetter, 0),
    JS_PSG("growable", SharedArrayBufferObject::growableGetter, 0),
    JS_STRING_SYM_PS(toStringTag, "SharedArrayBuffer", JSPROP_READONLY),
    JS_PS_END,
};

static JSObject* CreateSharedArrayBufferPrototype(JSContext* cx,
                                                  JSProtoKey key) {
  return GlobalObject::createBlankPrototype(
      cx, cx->global(), &SharedArrayBufferObject::protoClass_);
}

static const ClassSpec SharedArrayBufferObjectClassSpec = {
    GenericCreateConstructor<SharedArrayBufferObject::class_constructor, 1,
                             gc::AllocKind::FUNCTION>,
    CreateSharedArrayBufferPrototype,
    sharedarray_functions,
    sharedarray_properties,
    sharedarray_proto_functions,
    sharedarray_proto_properties,
};

const JSClass SharedArrayBufferObject::protoClass_ = {
    "SharedArrayBuffer.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_SharedArrayBuffer),
    JS_NULL_CLASS_OPS,
    &SharedArrayBufferObjectClassSpec,
};

const JSClass FixedLengthSharedArrayBufferObject::class_ = {
    "SharedArrayBuffer",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(SharedArrayBufferObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_SharedArrayBuffer) |
        JSCLASS_FOREGROUND_FINALIZE,
    &SharedArrayBufferObjectClassOps,
    &SharedArrayBufferObjectClassSpec,
    JS_NULL_CLASS_EXT,
};

const JSClass GrowableSharedArrayBufferObject::class_ = {
    "SharedArrayBuffer",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(SharedArrayBufferObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_SharedArrayBuffer) |
        JSCLASS_FOREGROUND_FINALIZE,
    &SharedArrayBufferObjectClassOps,
    &SharedArrayBufferObjectClassSpec,
    JS_NULL_CLASS_EXT,
};

JS_PUBLIC_API size_t JS::GetSharedArrayBufferByteLength(JSObject* obj) {
  auto* aobj = obj->maybeUnwrapAs<SharedArrayBufferObject>();
  return aobj ? aobj->byteLength() : 0;
}

JS_PUBLIC_API void JS::GetSharedArrayBufferLengthAndData(JSObject* obj,
                                                         size_t* length,
                                                         bool* isSharedMemory,
                                                         uint8_t** data) {
  MOZ_ASSERT(obj->is<SharedArrayBufferObject>());
  *length = obj->as<SharedArrayBufferObject>().byteLength();
  *data = obj->as<SharedArrayBufferObject>().dataPointerShared().unwrap(
      /*safe - caller knows*/);
  *isSharedMemory = true;
}

JS_PUBLIC_API JSObject* JS::NewSharedArrayBuffer(JSContext* cx, size_t nbytes) {
  MOZ_ASSERT(cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled());

  if (nbytes > ArrayBufferObject::ByteLengthLimit) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_SHARED_ARRAY_BAD_LENGTH);
    return nullptr;
  }

  return SharedArrayBufferObject::New(cx, nbytes,
                                      /* proto = */ nullptr);
}

JS_PUBLIC_API bool JS::IsSharedArrayBufferObject(JSObject* obj) {
  return obj->canUnwrapAs<SharedArrayBufferObject>();
}

JS_PUBLIC_API uint8_t* JS::GetSharedArrayBufferData(
    JSObject* obj, bool* isSharedMemory, const JS::AutoRequireNoGC&) {
  auto* aobj = obj->maybeUnwrapAs<SharedArrayBufferObject>();
  if (!aobj) {
    return nullptr;
  }
  *isSharedMemory = true;
  return aobj->dataPointerShared().unwrap(/*safe - caller knows*/);
}

JS_PUBLIC_API bool JS::ContainsSharedArrayBuffer(JSContext* cx) {
  return cx->runtime()->hasLiveSABs();
}

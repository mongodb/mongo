/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmInstance-inl.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"

#include <algorithm>
#include <utility>

#include "jsmath.h"

#include "builtin/String.h"
#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "jit/AtomicOperations.h"
#include "jit/Disassemble.h"
#include "jit/JitCommon.h"
#include "jit/JitRuntime.h"
#include "jit/Registers.h"
#include "js/ForOfIterator.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Stack.h"                 // JS::NativeStackLimitMin
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "util/Unicode.h"
#include "vm/ArrayBufferObject.h"
#include "vm/BigIntType.h"
#include "vm/Compartment.h"
#include "vm/ErrorObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JitActivation.h"
#include "vm/JSFunction.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmDebugFrame.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmInitExpr.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmPI.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

#include "gc/Marking-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSObject-inl.h"
#include "wasm/WasmGcObject-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::BitwiseCast;
using mozilla::CheckedUint32;
using mozilla::DebugOnly;

// Instance must be aligned at least as much as any of the integer, float,
// or SIMD values that we'd like to store in it.
static_assert(alignof(Instance) >=
              std::max(sizeof(Registers::RegisterContent),
                       sizeof(FloatRegisters::RegisterContent)));

// The globalArea must be aligned at least as much as an instance. This is
// guaranteed to be sufficient for all data types we care about, including
// SIMD values. See the above assertion.
static_assert(Instance::offsetOfData() % alignof(Instance) == 0);

// We want the memory base to be the first field, and accessible with no
// offset. This incidentally is also an assertion that there is no superclass
// with fields.
static_assert(Instance::offsetOfMemory0Base() == 0);

// We want instance fields that are commonly accessed by the JIT to have
// compact encodings. A limit of less than 128 bytes is chosen to fit within
// the signed 8-bit mod r/m x86 encoding.
static_assert(Instance::offsetOfLastCommonJitField() < 128);

//////////////////////////////////////////////////////////////////////////////
//
// Functions and invocation.

TypeDefInstanceData* Instance::typeDefInstanceData(uint32_t typeIndex) const {
  TypeDefInstanceData* instanceData =
      (TypeDefInstanceData*)(data() + metadata().typeDefsOffsetStart);
  return &instanceData[typeIndex];
}

const void* Instance::addressOfGlobalCell(const GlobalDesc& global) const {
  const void* cell = data() + global.offset();
  // Indirect globals store a pointer to their cell in the instance global
  // data. Dereference it to find the real cell.
  if (global.isIndirect()) {
    cell = *(const void**)cell;
  }
  return cell;
}

FuncImportInstanceData& Instance::funcImportInstanceData(const FuncImport& fi) {
  return *(FuncImportInstanceData*)(data() + fi.instanceOffset());
}

MemoryInstanceData& Instance::memoryInstanceData(uint32_t memoryIndex) const {
  MemoryInstanceData* instanceData =
      (MemoryInstanceData*)(data() + metadata().memoriesOffsetStart);
  return instanceData[memoryIndex];
}

TableInstanceData& Instance::tableInstanceData(uint32_t tableIndex) const {
  TableInstanceData* instanceData =
      (TableInstanceData*)(data() + metadata().tablesOffsetStart);
  return instanceData[tableIndex];
}

TagInstanceData& Instance::tagInstanceData(uint32_t tagIndex) const {
  TagInstanceData* instanceData =
      (TagInstanceData*)(data() + metadata().tagsOffsetStart);
  return instanceData[tagIndex];
}

static bool UnpackResults(JSContext* cx, const ValTypeVector& resultTypes,
                          const Maybe<char*> stackResultsArea, uint64_t* argv,
                          MutableHandleValue rval) {
  if (!stackResultsArea) {
    MOZ_ASSERT(resultTypes.length() <= 1);
    // Result is either one scalar value to unpack to a wasm value, or
    // an ignored value for a zero-valued function.
    if (resultTypes.length() == 1) {
      return ToWebAssemblyValue(cx, rval, resultTypes[0], argv, true);
    }
    return true;
  }

  MOZ_ASSERT(stackResultsArea.isSome());
  Rooted<ArrayObject*> array(cx);
  if (!IterableToArray(cx, rval, &array)) {
    return false;
  }

  if (resultTypes.length() != array->length()) {
    UniqueChars expected(JS_smprintf("%zu", resultTypes.length()));
    UniqueChars got(JS_smprintf("%u", array->length()));
    if (!expected || !got) {
      ReportOutOfMemory(cx);
      return false;
    }

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_WRONG_NUMBER_OF_VALUES, expected.get(),
                             got.get());
    return false;
  }

  DebugOnly<uint64_t> previousOffset = ~(uint64_t)0;

  ABIResultIter iter(ResultType::Vector(resultTypes));
  // The values are converted in the order they are pushed on the
  // abstract WebAssembly stack; switch to iterate in push order.
  while (!iter.done()) {
    iter.next();
  }
  DebugOnly<bool> seenRegisterResult = false;
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    MOZ_ASSERT(!seenRegisterResult);
    // Use rval as a scratch area to hold the extracted result.
    rval.set(array->getDenseElement(iter.index()));
    if (result.inRegister()) {
      // Currently, if a function type has results, there can be only
      // one register result.  If there is only one result, it is
      // returned as a scalar and not an iterable, so we don't get here.
      // If there are multiple results, we extract the register result
      // and set `argv[0]` set to the extracted result, to be returned by
      // register in the stub.  The register result follows any stack
      // results, so this preserves conversion order.
      if (!ToWebAssemblyValue(cx, rval, result.type(), argv, true)) {
        return false;
      }
      seenRegisterResult = true;
      continue;
    }
    uint32_t result_size = result.size();
    MOZ_ASSERT(result_size == 4 || result_size == 8);
#ifdef DEBUG
    if (previousOffset == ~(uint64_t)0) {
      previousOffset = (uint64_t)result.stackOffset();
    } else {
      MOZ_ASSERT(previousOffset - (uint64_t)result_size ==
                 (uint64_t)result.stackOffset());
      previousOffset -= (uint64_t)result_size;
    }
#endif
    char* loc = stackResultsArea.value() + result.stackOffset();
    if (!ToWebAssemblyValue(cx, rval, result.type(), loc, result_size == 8)) {
      return false;
    }
  }

  return true;
}

bool Instance::callImport(JSContext* cx, uint32_t funcImportIndex,
                          unsigned argc, uint64_t* argv) {
  AssertRealmUnchanged aru(cx);

  Tier tier = code().bestTier();

  const FuncImport& fi = metadata(tier).funcImports[funcImportIndex];
  const FuncType& funcType = metadata().getFuncImportType(fi);

  ArgTypeVector argTypes(funcType);
  InvokeArgs args(cx);
  if (!args.init(cx, argTypes.lengthWithoutStackResults())) {
    return false;
  }

  if (funcType.hasUnexposableArgOrRet()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }

  MOZ_ASSERT(argTypes.lengthWithStackResults() == argc);
  Maybe<char*> stackResultPointer;
  size_t lastBoxIndexPlusOne = 0;
  {
    JS::AutoAssertNoGC nogc;
    for (size_t i = 0; i < argc; i++) {
      const void* rawArgLoc = &argv[i];
      if (argTypes.isSyntheticStackResultPointerArg(i)) {
        stackResultPointer = Some(*(char**)rawArgLoc);
        continue;
      }
      size_t naturalIndex = argTypes.naturalIndex(i);
      ValType type = funcType.args()[naturalIndex];
      // Avoid boxes creation not to trigger GC.
      if (ToJSValueMayGC(type)) {
        lastBoxIndexPlusOne = i + 1;
        continue;
      }
      MutableHandleValue argValue = args[naturalIndex];
      if (!ToJSValue(cx, rawArgLoc, type, argValue)) {
        return false;
      }
    }
  }

  // Visit arguments that need to perform allocation in a second loop
  // after the rest of arguments are converted.
  for (size_t i = 0; i < lastBoxIndexPlusOne; i++) {
    if (argTypes.isSyntheticStackResultPointerArg(i)) {
      continue;
    }
    const void* rawArgLoc = &argv[i];
    size_t naturalIndex = argTypes.naturalIndex(i);
    ValType type = funcType.args()[naturalIndex];
    if (!ToJSValueMayGC(type)) {
      continue;
    }
    MOZ_ASSERT(!type.isRefRepr());
    // The conversions are safe here because source values are not references
    // and will not be moved.
    MutableHandleValue argValue = args[naturalIndex];
    if (!ToJSValue(cx, rawArgLoc, type, argValue)) {
      return false;
    }
  }

  FuncImportInstanceData& import = funcImportInstanceData(fi);
  Rooted<JSObject*> importCallable(cx, import.callable);
  MOZ_ASSERT(cx->realm() == importCallable->nonCCWRealm());

  RootedValue fval(cx, ObjectValue(*importCallable));
  RootedValue thisv(cx, UndefinedValue());
  RootedValue rval(cx);
  if (!Call(cx, fval, thisv, args, &rval)) {
    return false;
  }

  if (!UnpackResults(cx, funcType.results(), stackResultPointer, argv, &rval)) {
    return false;
  }

  if (!JitOptions.enableWasmJitExit) {
    return true;
  }

#ifdef ENABLE_WASM_JSPI
  // Disable jit exit optimization when JSPI is enabled.
  if (JSPromiseIntegrationAvailable(cx)) {
    return true;
  }
#endif

  // The import may already have become optimized.
  for (auto t : code().tiers()) {
    void* jitExitCode = codeBase(t) + fi.jitExitCodeOffset();
    if (import.code == jitExitCode) {
      return true;
    }
  }

  void* jitExitCode = codeBase(tier) + fi.jitExitCodeOffset();

  if (!importCallable->is<JSFunction>()) {
    return true;
  }

  // Test if the function is JIT compiled.
  if (!importCallable->as<JSFunction>().hasBytecode()) {
    return true;
  }

  JSScript* script = importCallable->as<JSFunction>().nonLazyScript();
  if (!script->hasJitScript()) {
    return true;
  }

  // Skip if the function does not have a signature that allows for a JIT exit.
  if (!funcType.canHaveJitExit()) {
    return true;
  }

  // Let's optimize it!

  import.code = jitExitCode;
  return true;
}

/* static */ int32_t /* 0 to signal trap; 1 to signal OK */
Instance::callImport_general(Instance* instance, int32_t funcImportIndex,
                             int32_t argc, uint64_t* argv) {
  JSContext* cx = instance->cx();
#ifdef ENABLE_WASM_JSPI
  if (IsSuspendableStackActive(cx)) {
    return CallImportOnMainThread(cx, instance, funcImportIndex, argc, argv);
  }
#endif
  return instance->callImport(cx, funcImportIndex, argc, argv);
}

//////////////////////////////////////////////////////////////////////////////
//
// Atomic operations and shared memory.

template <typename ValT, typename PtrT>
static int32_t PerformWait(Instance* instance, uint32_t memoryIndex,
                           PtrT byteOffset, ValT value, int64_t timeout_ns) {
  JSContext* cx = instance->cx();

  if (!instance->memory(memoryIndex)->isShared()) {
    ReportTrapError(cx, JSMSG_WASM_NONSHARED_WAIT);
    return -1;
  }

  if (byteOffset & (sizeof(ValT) - 1)) {
    ReportTrapError(cx, JSMSG_WASM_UNALIGNED_ACCESS);
    return -1;
  }

  if (byteOffset + sizeof(ValT) >
      instance->memory(memoryIndex)->volatileMemoryLength()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  mozilla::Maybe<mozilla::TimeDuration> timeout;
  if (timeout_ns >= 0) {
    timeout = mozilla::Some(
        mozilla::TimeDuration::FromMicroseconds(double(timeout_ns) / 1000));
  }

  MOZ_ASSERT(byteOffset <= SIZE_MAX, "Bounds check is broken");
  switch (atomics_wait_impl(cx, instance->sharedMemoryBuffer(memoryIndex),
                            size_t(byteOffset), value, timeout)) {
    case FutexThread::WaitResult::OK:
      return 0;
    case FutexThread::WaitResult::NotEqual:
      return 1;
    case FutexThread::WaitResult::TimedOut:
      return 2;
    case FutexThread::WaitResult::Error:
      return -1;
    default:
      MOZ_CRASH();
  }
}

/* static */ int32_t Instance::wait_i32_m32(Instance* instance,
                                            uint32_t byteOffset, int32_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI32M32.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

/* static */ int32_t Instance::wait_i32_m64(Instance* instance,
                                            uint64_t byteOffset, int32_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI32M64.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

/* static */ int32_t Instance::wait_i64_m32(Instance* instance,
                                            uint32_t byteOffset, int64_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI64M32.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

/* static */ int32_t Instance::wait_i64_m64(Instance* instance,
                                            uint64_t byteOffset, int64_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI64M64.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

template <typename PtrT>
static int32_t PerformWake(Instance* instance, PtrT byteOffset, int32_t count,
                           uint32_t memoryIndex) {
  JSContext* cx = instance->cx();

  // The alignment guard is not in the wasm spec as of 2017-11-02, but is
  // considered likely to appear, as 4-byte alignment is required for WAKE by
  // the spec's validation algorithm.

  if (byteOffset & 3) {
    ReportTrapError(cx, JSMSG_WASM_UNALIGNED_ACCESS);
    return -1;
  }

  if (byteOffset >= instance->memory(memoryIndex)->volatileMemoryLength()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  if (!instance->memory(memoryIndex)->isShared()) {
    return 0;
  }

  MOZ_ASSERT(byteOffset <= SIZE_MAX, "Bounds check is broken");
  int64_t woken = atomics_notify_impl(instance->sharedMemoryBuffer(memoryIndex),
                                      size_t(byteOffset), int64_t(count));

  if (woken > INT32_MAX) {
    ReportTrapError(cx, JSMSG_WASM_WAKE_OVERFLOW);
    return -1;
  }

  return int32_t(woken);
}

/* static */ int32_t Instance::wake_m32(Instance* instance, uint32_t byteOffset,
                                        int32_t count, uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWakeM32.failureMode == FailureMode::FailOnNegI32);
  return PerformWake(instance, byteOffset, count, memoryIndex);
}

/* static */ int32_t Instance::wake_m64(Instance* instance, uint64_t byteOffset,
                                        int32_t count, uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWakeM32.failureMode == FailureMode::FailOnNegI32);
  return PerformWake(instance, byteOffset, count, memoryIndex);
}

//////////////////////////////////////////////////////////////////////////////
//
// Bulk memory operations.

/* static */ uint32_t Instance::memoryGrow_m32(Instance* instance,
                                               uint32_t delta,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemoryGrowM32.failureMode == FailureMode::Infallible);
  MOZ_ASSERT(!instance->isAsmJS());

  JSContext* cx = instance->cx();
  Rooted<WasmMemoryObject*> memory(cx, instance->memory(memoryIndex));

  // It is safe to cast to uint32_t, as all limits have been checked inside
  // grow() and will not have been exceeded for a 32-bit memory.
  uint32_t ret = uint32_t(WasmMemoryObject::grow(memory, uint64_t(delta), cx));

  // If there has been a moving grow, this Instance should have been notified.
  MOZ_RELEASE_ASSERT(
      instance->memoryBase(memoryIndex) ==
      instance->memory(memoryIndex)->buffer().dataPointerEither());

  return ret;
}

/* static */ uint64_t Instance::memoryGrow_m64(Instance* instance,
                                               uint64_t delta,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemoryGrowM64.failureMode == FailureMode::Infallible);
  MOZ_ASSERT(!instance->isAsmJS());

  JSContext* cx = instance->cx();
  Rooted<WasmMemoryObject*> memory(cx, instance->memory(memoryIndex));

  uint64_t ret = WasmMemoryObject::grow(memory, delta, cx);

  // If there has been a moving grow, this Instance should have been notified.
  MOZ_RELEASE_ASSERT(
      instance->memoryBase(memoryIndex) ==
      instance->memory(memoryIndex)->buffer().dataPointerEither());

  return ret;
}

/* static */ uint32_t Instance::memorySize_m32(Instance* instance,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemorySizeM32.failureMode == FailureMode::Infallible);

  // This invariant must hold when running Wasm code. Assert it here so we can
  // write tests for cross-realm calls.
  DebugOnly<JSContext*> cx = instance->cx();
  MOZ_ASSERT(cx->realm() == instance->realm());

  Pages pages = instance->memory(memoryIndex)->volatilePages();
#ifdef JS_64BIT
  // Ensure that the memory size is no more than 4GiB.
  MOZ_ASSERT(pages <= Pages(MaxMemory32LimitField));
#endif
  return uint32_t(pages.value());
}

/* static */ uint64_t Instance::memorySize_m64(Instance* instance,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemorySizeM64.failureMode == FailureMode::Infallible);

  // This invariant must hold when running Wasm code. Assert it here so we can
  // write tests for cross-realm calls.
  DebugOnly<JSContext*> cx = instance->cx();
  MOZ_ASSERT(cx->realm() == instance->realm());

  Pages pages = instance->memory(memoryIndex)->volatilePages();
#ifdef JS_64BIT
  MOZ_ASSERT(pages <= Pages(MaxMemory64LimitField));
#endif
  return pages.value();
}

template <typename PointerT, typename CopyFuncT, typename IndexT>
inline int32_t WasmMemoryCopy(JSContext* cx, PointerT dstMemBase,
                              PointerT srcMemBase, size_t dstMemLen,
                              size_t srcMemLen, IndexT dstByteOffset,
                              IndexT srcByteOffset, IndexT len,
                              CopyFuncT memMove) {
  if (!MemoryBoundsCheck(dstByteOffset, len, dstMemLen) ||
      !MemoryBoundsCheck(srcByteOffset, len, srcMemLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  memMove(dstMemBase + uintptr_t(dstByteOffset),
          srcMemBase + uintptr_t(srcByteOffset), size_t(len));
  return 0;
}

template <typename I>
inline int32_t MemoryCopy(JSContext* cx, I dstByteOffset, I srcByteOffset,
                          I len, uint8_t* memBase) {
  const WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();
  return WasmMemoryCopy(cx, memBase, memBase, memLen, memLen, dstByteOffset,
                        srcByteOffset, len, memmove);
}

template <typename I>
inline int32_t MemoryCopyShared(JSContext* cx, I dstByteOffset, I srcByteOffset,
                                I len, uint8_t* memBase) {
  using RacyMemMove =
      void (*)(SharedMem<uint8_t*>, SharedMem<uint8_t*>, size_t);

  const WasmSharedArrayRawBuffer* rawBuf =
      WasmSharedArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->volatileByteLength();

  SharedMem<uint8_t*> sharedMemBase = SharedMem<uint8_t*>::shared(memBase);
  return WasmMemoryCopy<SharedMem<uint8_t*>, RacyMemMove>(
      cx, sharedMemBase, sharedMemBase, memLen, memLen, dstByteOffset,
      srcByteOffset, len, AtomicOperations::memmoveSafeWhenRacy);
}

/* static */ int32_t Instance::memCopy_m32(Instance* instance,
                                           uint32_t dstByteOffset,
                                           uint32_t srcByteOffset, uint32_t len,
                                           uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopyM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopy(cx, dstByteOffset, srcByteOffset, len, memBase);
}

/* static */ int32_t Instance::memCopyShared_m32(Instance* instance,
                                                 uint32_t dstByteOffset,
                                                 uint32_t srcByteOffset,
                                                 uint32_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopySharedM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopyShared(cx, dstByteOffset, srcByteOffset, len, memBase);
}

/* static */ int32_t Instance::memCopy_m64(Instance* instance,
                                           uint64_t dstByteOffset,
                                           uint64_t srcByteOffset, uint64_t len,
                                           uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopyM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopy(cx, dstByteOffset, srcByteOffset, len, memBase);
}

/* static */ int32_t Instance::memCopyShared_m64(Instance* instance,
                                                 uint64_t dstByteOffset,
                                                 uint64_t srcByteOffset,
                                                 uint64_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopySharedM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopyShared(cx, dstByteOffset, srcByteOffset, len, memBase);
}

// Dynamic dispatch to get the length of a memory given just the base and
// whether it is shared or not. This is only used for memCopy_any, where being
// slower is okay.
static inline size_t GetVolatileByteLength(uint8_t* memBase, bool isShared) {
  if (isShared) {
    return WasmSharedArrayRawBuffer::fromDataPtr(memBase)->volatileByteLength();
  }
  return WasmArrayRawBuffer::fromDataPtr(memBase)->byteLength();
}

/* static */ int32_t Instance::memCopy_any(Instance* instance,
                                           uint64_t dstByteOffset,
                                           uint64_t srcByteOffset, uint64_t len,
                                           uint32_t dstMemIndex,
                                           uint32_t srcMemIndex) {
  MOZ_ASSERT(SASigMemCopyAny.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  using RacyMemMove =
      void (*)(SharedMem<uint8_t*>, SharedMem<uint8_t*>, size_t);

  const MemoryInstanceData& dstMemory =
      instance->memoryInstanceData(dstMemIndex);
  const MemoryInstanceData& srcMemory =
      instance->memoryInstanceData(srcMemIndex);

  uint8_t* dstMemBase = dstMemory.base;
  uint8_t* srcMemBase = srcMemory.base;

  size_t dstMemLen = GetVolatileByteLength(dstMemBase, dstMemory.isShared);
  size_t srcMemLen = GetVolatileByteLength(srcMemBase, srcMemory.isShared);

  return WasmMemoryCopy<SharedMem<uint8_t*>, RacyMemMove>(
      cx, SharedMem<uint8_t*>::shared(dstMemBase),
      SharedMem<uint8_t*>::shared(srcMemBase), dstMemLen, srcMemLen,
      dstByteOffset, srcByteOffset, len, AtomicOperations::memmoveSafeWhenRacy);
}

template <typename T, typename F, typename I>
inline int32_t WasmMemoryFill(JSContext* cx, T memBase, size_t memLen,
                              I byteOffset, uint32_t value, I len, F memSet) {
  if (!MemoryBoundsCheck(byteOffset, len, memLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // The required write direction is upward, but that is not currently
  // observable as there are no fences nor any read/write protect operation.
  memSet(memBase + uintptr_t(byteOffset), int(value), size_t(len));
  return 0;
}

template <typename I>
inline int32_t MemoryFill(JSContext* cx, I byteOffset, uint32_t value, I len,
                          uint8_t* memBase) {
  const WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();
  return WasmMemoryFill(cx, memBase, memLen, byteOffset, value, len, memset);
}

template <typename I>
inline int32_t MemoryFillShared(JSContext* cx, I byteOffset, uint32_t value,
                                I len, uint8_t* memBase) {
  const WasmSharedArrayRawBuffer* rawBuf =
      WasmSharedArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->volatileByteLength();
  return WasmMemoryFill(cx, SharedMem<uint8_t*>::shared(memBase), memLen,
                        byteOffset, value, len,
                        AtomicOperations::memsetSafeWhenRacy);
}

/* static */ int32_t Instance::memFill_m32(Instance* instance,
                                           uint32_t byteOffset, uint32_t value,
                                           uint32_t len, uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFill(cx, byteOffset, value, len, memBase);
}

/* static */ int32_t Instance::memFillShared_m32(Instance* instance,
                                                 uint32_t byteOffset,
                                                 uint32_t value, uint32_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillSharedM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFillShared(cx, byteOffset, value, len, memBase);
}

/* static */ int32_t Instance::memFill_m64(Instance* instance,
                                           uint64_t byteOffset, uint32_t value,
                                           uint64_t len, uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFill(cx, byteOffset, value, len, memBase);
}

/* static */ int32_t Instance::memFillShared_m64(Instance* instance,
                                                 uint64_t byteOffset,
                                                 uint32_t value, uint64_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillSharedM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFillShared(cx, byteOffset, value, len, memBase);
}

static bool BoundsCheckInit(uint32_t dstOffset, uint32_t srcOffset,
                            uint32_t len, size_t memLen, uint32_t segLen) {
  uint64_t dstOffsetLimit = uint64_t(dstOffset) + uint64_t(len);
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + uint64_t(len);

  return dstOffsetLimit > memLen || srcOffsetLimit > segLen;
}

static bool BoundsCheckInit(uint64_t dstOffset, uint32_t srcOffset,
                            uint32_t len, size_t memLen, uint32_t segLen) {
  uint64_t dstOffsetLimit = dstOffset + uint64_t(len);
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + uint64_t(len);

  return dstOffsetLimit < dstOffset || dstOffsetLimit > memLen ||
         srcOffsetLimit > segLen;
}

template <typename I>
static int32_t MemoryInit(JSContext* cx, Instance* instance,
                          uint32_t memoryIndex, I dstOffset, uint32_t srcOffset,
                          uint32_t len, const DataSegment* maybeSeg) {
  if (!maybeSeg) {
    if (len == 0 && srcOffset == 0) {
      return 0;
    }

    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  const DataSegment& seg = *maybeSeg;
  MOZ_RELEASE_ASSERT(!seg.active());

  const uint32_t segLen = seg.bytes.length();
  WasmMemoryObject* mem = instance->memory(memoryIndex);
  const size_t memLen = mem->volatileMemoryLength();

  // We are proposing to copy
  //
  //   seg.bytes.begin()[ srcOffset .. srcOffset + len - 1 ]
  // to
  //   memoryBase[ dstOffset .. dstOffset + len - 1 ]

  if (BoundsCheckInit(dstOffset, srcOffset, len, memLen, segLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // The required read/write direction is upward, but that is not currently
  // observable as there are no fences nor any read/write protect operation.
  SharedMem<uint8_t*> dataPtr = mem->buffer().dataPointerEither();
  if (mem->isShared()) {
    AtomicOperations::memcpySafeWhenRacy(
        dataPtr + uintptr_t(dstOffset), (uint8_t*)seg.bytes.begin() + srcOffset,
        len);
  } else {
    uint8_t* rawBuf = dataPtr.unwrap(/*Unshared*/);
    memcpy(rawBuf + uintptr_t(dstOffset),
           (const char*)seg.bytes.begin() + srcOffset, len);
  }
  return 0;
}

/* static */ int32_t Instance::memInit_m32(Instance* instance,
                                           uint32_t dstOffset,
                                           uint32_t srcOffset, uint32_t len,
                                           uint32_t segIndex,
                                           uint32_t memIndex) {
  MOZ_ASSERT(SASigMemInitM32.failureMode == FailureMode::FailOnNegI32);
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");

  JSContext* cx = instance->cx();
  return MemoryInit(cx, instance, memIndex, dstOffset, srcOffset, len,
                    instance->passiveDataSegments_[segIndex]);
}

/* static */ int32_t Instance::memInit_m64(Instance* instance,
                                           uint64_t dstOffset,
                                           uint32_t srcOffset, uint32_t len,
                                           uint32_t segIndex,
                                           uint32_t memIndex) {
  MOZ_ASSERT(SASigMemInitM64.failureMode == FailureMode::FailOnNegI32);
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");

  JSContext* cx = instance->cx();
  return MemoryInit(cx, instance, memIndex, dstOffset, srcOffset, len,
                    instance->passiveDataSegments_[segIndex]);
}

//////////////////////////////////////////////////////////////////////////////
//
// Bulk table operations.

/* static */ int32_t Instance::tableCopy(Instance* instance, uint32_t dstOffset,
                                         uint32_t srcOffset, uint32_t len,
                                         uint32_t dstTableIndex,
                                         uint32_t srcTableIndex) {
  MOZ_ASSERT(SASigTableCopy.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  const SharedTable& srcTable = instance->tables()[srcTableIndex];
  uint32_t srcTableLen = srcTable->length();

  const SharedTable& dstTable = instance->tables()[dstTableIndex];
  uint32_t dstTableLen = dstTable->length();

  // Bounds check and deal with arithmetic overflow.
  uint64_t dstOffsetLimit = uint64_t(dstOffset) + len;
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + len;

  if (dstOffsetLimit > dstTableLen || srcOffsetLimit > srcTableLen) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  bool isOOM = false;

  if (&srcTable == &dstTable && dstOffset > srcOffset) {
    for (uint32_t i = len; i > 0; i--) {
      if (!dstTable->copy(cx, *srcTable, dstOffset + (i - 1),
                          srcOffset + (i - 1))) {
        isOOM = true;
        break;
      }
    }
  } else if (&srcTable == &dstTable && dstOffset == srcOffset) {
    // No-op
  } else {
    for (uint32_t i = 0; i < len; i++) {
      if (!dstTable->copy(cx, *srcTable, dstOffset + i, srcOffset + i)) {
        isOOM = true;
        break;
      }
    }
  }

  if (isOOM) {
    return -1;
  }
  return 0;
}

#ifdef DEBUG
static bool AllSegmentsArePassive(const DataSegmentVector& vec) {
  for (const DataSegment* seg : vec) {
    if (seg->active()) {
      return false;
    }
  }
  return true;
}
#endif

bool Instance::initSegments(JSContext* cx,
                            const DataSegmentVector& dataSegments,
                            const ModuleElemSegmentVector& elemSegments) {
  MOZ_ASSERT_IF(metadata().memories.length() == 0,
                AllSegmentsArePassive(dataSegments));

  Rooted<WasmInstanceObject*> instanceObj(cx, object());

  // Write data/elem segments into memories/tables.

  for (const ModuleElemSegment& seg : elemSegments) {
    if (seg.active()) {
      RootedVal offsetVal(cx);
      if (!seg.offset().evaluate(cx, instanceObj, &offsetVal)) {
        return false;  // OOM
      }
      uint32_t offset = offsetVal.get().i32();

      uint32_t tableLength = tables()[seg.tableIndex]->length();
      if (offset > tableLength || tableLength - offset < seg.numElements()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_OUT_OF_BOUNDS);
        return false;
      }

      if (!initElems(seg.tableIndex, seg, offset)) {
        return false;  // OOM
      }
    }
  }

  for (const DataSegment* seg : dataSegments) {
    if (!seg->active()) {
      continue;
    }

    Rooted<const WasmMemoryObject*> memoryObj(cx, memory(seg->memoryIndex));
    size_t memoryLength = memoryObj->volatileMemoryLength();
    uint8_t* memoryBase =
        memoryObj->buffer().dataPointerEither().unwrap(/* memcpy */);

    RootedVal offsetVal(cx);
    if (!seg->offset().evaluate(cx, instanceObj, &offsetVal)) {
      return false;  // OOM
    }
    uint64_t offset = memoryObj->indexType() == IndexType::I32
                          ? offsetVal.get().i32()
                          : offsetVal.get().i64();
    uint32_t count = seg->bytes.length();

    if (offset > memoryLength || memoryLength - offset < count) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_OUT_OF_BOUNDS);
      return false;
    }
    memcpy(memoryBase + uintptr_t(offset), seg->bytes.begin(), count);
  }

  return true;
}

bool Instance::initElems(uint32_t tableIndex, const ModuleElemSegment& seg,
                         uint32_t dstOffset) {
  Table& table = *tables_[tableIndex];
  MOZ_ASSERT(dstOffset <= table.length());
  MOZ_ASSERT(seg.numElements() <= table.length() - dstOffset);

  if (seg.numElements() == 0) {
    return true;
  }

  Rooted<WasmInstanceObject*> instanceObj(cx(), object());

  if (table.isFunction() &&
      seg.encoding == ModuleElemSegment::Encoding::Indices) {
    // Initialize this table of functions without creating any intermediate
    // JSFunctions.
    bool ok = iterElemsFunctions(
        seg, [&](uint32_t i, void* code, Instance* instance) -> bool {
          table.setFuncRef(dstOffset + i, code, instance);
          return true;
        });
    if (!ok) {
      return false;
    }
  } else {
    bool ok = iterElemsAnyrefs(seg, [&](uint32_t i, AnyRef ref) -> bool {
      table.setRef(dstOffset + i, ref);
      return true;
    });
    if (!ok) {
      return false;
    }
  }

  return true;
}

template <typename F>
bool Instance::iterElemsFunctions(const ModuleElemSegment& seg,
                                  const F& onFunc) {
  // In the future, we could theoretically get function data (instance + code
  // pointer) from segments with the expression encoding without creating
  // JSFunctions. But that is not how it works today. We can only bypass the
  // creation of JSFunctions for the index encoding.
  MOZ_ASSERT(seg.encoding == ModuleElemSegment::Encoding::Indices);

  if (seg.numElements() == 0) {
    return true;
  }

  Tier tier = code().bestTier();
  const MetadataTier& metadataTier = metadata(tier);
  const FuncImportVector& funcImports = metadataTier.funcImports;
  const CodeRangeVector& codeRanges = metadataTier.codeRanges;
  const Uint32Vector& funcToCodeRange = metadataTier.funcToCodeRange;
  const Uint32Vector& elemIndices = seg.elemIndices;

  uint8_t* codeBaseTier = codeBase(tier);
  for (uint32_t i = 0; i < seg.numElements(); i++) {
    uint32_t elemIndex = elemIndices[i];
    if (elemIndex < metadataTier.funcImports.length()) {
      FuncImportInstanceData& import =
          funcImportInstanceData(funcImports[elemIndex]);
      MOZ_ASSERT(import.callable->isCallable());
      if (import.callable->is<JSFunction>()) {
        JSFunction* fun = &import.callable->as<JSFunction>();
        if (IsWasmExportedFunction(fun)) {
          // This element is a wasm function imported from another
          // instance. To preserve the === function identity required by
          // the JS embedding spec, we must get the imported function's
          // underlying CodeRange.funcCheckedCallEntry and Instance so that
          // future Table.get()s produce the same function object as was
          // imported.
          WasmInstanceObject* calleeInstanceObj =
              ExportedFunctionToInstanceObject(fun);
          Instance& calleeInstance = calleeInstanceObj->instance();
          Tier calleeTier = calleeInstance.code().bestTier();
          const CodeRange& calleeCodeRange =
              calleeInstanceObj->getExportedFunctionCodeRange(fun, calleeTier);
          void* code = calleeInstance.codeBase(calleeTier) +
                       calleeCodeRange.funcCheckedCallEntry();
          if (!onFunc(i, code, &calleeInstance)) {
            return false;
          }
          continue;
        }
      }
    }

    void* code = codeBaseTier +
                 codeRanges[funcToCodeRange[elemIndex]].funcCheckedCallEntry();
    if (!onFunc(i, code, this)) {
      return false;
    }
  }

  return true;
}

template <typename F>
bool Instance::iterElemsAnyrefs(const ModuleElemSegment& seg,
                                const F& onAnyRef) {
  if (seg.numElements() == 0) {
    return true;
  }

  switch (seg.encoding) {
    case ModuleElemSegment::Encoding::Indices: {
      // The only types of indices that exist right now are function indices, so
      // this code is specialized to functions.

      for (uint32_t i = 0; i < seg.numElements(); i++) {
        uint32_t funcIndex = seg.elemIndices[i];
        // Note, fnref must be rooted if we do anything more than just store it.
        void* fnref = Instance::refFunc(this, funcIndex);
        if (fnref == AnyRef::invalid().forCompiledCode()) {
          return false;  // OOM, which has already been reported.
        }
        if (!onAnyRef(i, AnyRef::fromCompiledCode(fnref))) {
          return false;
        }
      }
    } break;
    case ModuleElemSegment::Encoding::Expressions: {
      Rooted<WasmInstanceObject*> instanceObj(cx(), object());
      const ModuleElemSegment::Expressions& exprs = seg.elemExpressions;

      UniqueChars error;
      // The offset is a dummy because the expression has already been
      // validated.
      Decoder d(exprs.exprBytes.begin(), exprs.exprBytes.end(), 0, &error);
      for (uint32_t i = 0; i < seg.numElements(); i++) {
        RootedVal result(cx());
        if (!InitExpr::decodeAndEvaluate(cx(), instanceObj, d, seg.elemType,
                                         &result)) {
          MOZ_ASSERT(!error);  // The only possible failure should be OOM.
          return false;
        }
        // We would need to root this AnyRef if we were doing anything other
        // than storing it.
        AnyRef ref = result.get().ref();
        if (!onAnyRef(i, ref)) {
          return false;
        }
      }
    } break;
    default:
      MOZ_CRASH("unknown encoding type for element segment");
  }
  return true;
}

/* static */ int32_t Instance::tableInit(Instance* instance, uint32_t dstOffset,
                                         uint32_t srcOffset, uint32_t len,
                                         uint32_t segIndex,
                                         uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableInit.failureMode == FailureMode::FailOnNegI32);

  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");

  JSContext* cx = instance->cx();

  const InstanceElemSegment& seg = instance->passiveElemSegments_[segIndex];
  const uint32_t segLen = seg.length();

  Table& table = *instance->tables()[tableIndex];
  const uint32_t tableLen = table.length();

  // We are proposing to copy
  //
  //   seg[ srcOffset .. srcOffset + len - 1 ]
  // to
  //   tableBase[ dstOffset .. dstOffset + len - 1 ]

  // Bounds check and deal with arithmetic overflow.
  uint64_t dstOffsetLimit = uint64_t(dstOffset) + uint64_t(len);
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + uint64_t(len);

  if (dstOffsetLimit > tableLen || srcOffsetLimit > segLen) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  for (size_t i = 0; i < len; i++) {
    table.setRef(dstOffset + i, seg[srcOffset + i]);
  }

  return 0;
}

/* static */ int32_t Instance::tableFill(Instance* instance, uint32_t start,
                                         void* value, uint32_t len,
                                         uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableFill.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  Table& table = *instance->tables()[tableIndex];

  // Bounds check and deal with arithmetic overflow.
  uint64_t offsetLimit = uint64_t(start) + uint64_t(len);

  if (offsetLimit > table.length()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  switch (table.repr()) {
    case TableRepr::Ref:
      table.fillAnyRef(start, len, AnyRef::fromCompiledCode(value));
      break;
    case TableRepr::Func:
      MOZ_RELEASE_ASSERT(!table.isAsmJS());
      table.fillFuncRef(start, len, FuncRef::fromCompiledCode(value), cx);
      break;
  }

  return 0;
}

template <typename I>
static bool WasmDiscardCheck(Instance* instance, I byteOffset, I byteLen,
                             size_t memLen, bool shared) {
  JSContext* cx = instance->cx();

  if (byteOffset % wasm::PageSize != 0 || byteLen % wasm::PageSize != 0) {
    ReportTrapError(cx, JSMSG_WASM_UNALIGNED_ACCESS);
    return false;
  }

  if (!MemoryBoundsCheck(byteOffset, byteLen, memLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  return true;
}

template <typename I>
static int32_t MemDiscardNotShared(Instance* instance, I byteOffset, I byteLen,
                                   uint8_t* memBase) {
  WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();

  if (!WasmDiscardCheck(instance, byteOffset, byteLen, memLen, false)) {
    return -1;
  }
  rawBuf->discard(byteOffset, byteLen);

  return 0;
}

template <typename I>
static int32_t MemDiscardShared(Instance* instance, I byteOffset, I byteLen,
                                uint8_t* memBase) {
  WasmSharedArrayRawBuffer* rawBuf =
      WasmSharedArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->volatileByteLength();

  if (!WasmDiscardCheck(instance, byteOffset, byteLen, memLen, true)) {
    return -1;
  }
  rawBuf->discard(byteOffset, byteLen);

  return 0;
}

/* static */ int32_t Instance::memDiscard_m32(Instance* instance,
                                              uint32_t byteOffset,
                                              uint32_t byteLen,
                                              uint8_t* memBase) {
  return MemDiscardNotShared(instance, byteOffset, byteLen, memBase);
}

/* static */ int32_t Instance::memDiscard_m64(Instance* instance,
                                              uint64_t byteOffset,
                                              uint64_t byteLen,
                                              uint8_t* memBase) {
  return MemDiscardNotShared(instance, byteOffset, byteLen, memBase);
}

/* static */ int32_t Instance::memDiscardShared_m32(Instance* instance,
                                                    uint32_t byteOffset,
                                                    uint32_t byteLen,
                                                    uint8_t* memBase) {
  return MemDiscardShared(instance, byteOffset, byteLen, memBase);
}

/* static */ int32_t Instance::memDiscardShared_m64(Instance* instance,
                                                    uint64_t byteOffset,
                                                    uint64_t byteLen,
                                                    uint8_t* memBase) {
  return MemDiscardShared(instance, byteOffset, byteLen, memBase);
}

/* static */ void* Instance::tableGet(Instance* instance, uint32_t index,
                                      uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableGet.failureMode == FailureMode::FailOnInvalidRef);

  JSContext* cx = instance->cx();
  const Table& table = *instance->tables()[tableIndex];
  if (index >= table.length()) {
    ReportTrapError(cx, JSMSG_WASM_TABLE_OUT_OF_BOUNDS);
    return AnyRef::invalid().forCompiledCode();
  }

  switch (table.repr()) {
    case TableRepr::Ref:
      return table.getAnyRef(index).forCompiledCode();
    case TableRepr::Func: {
      MOZ_RELEASE_ASSERT(!table.isAsmJS());
      RootedFunction fun(cx);
      if (!table.getFuncRef(cx, index, &fun)) {
        return AnyRef::invalid().forCompiledCode();
      }
      return FuncRef::fromJSFunction(fun).forCompiledCode();
    }
  }
  MOZ_CRASH("Should not happen");
}

/* static */ uint32_t Instance::tableGrow(Instance* instance, void* initValue,
                                          uint32_t delta, uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableGrow.failureMode == FailureMode::Infallible);

  JSContext* cx = instance->cx();
  RootedAnyRef ref(cx, AnyRef::fromCompiledCode(initValue));
  Table& table = *instance->tables()[tableIndex];

  uint32_t oldSize = table.grow(delta);

  if (oldSize != uint32_t(-1) && initValue != nullptr) {
    table.fillUninitialized(oldSize, delta, ref, cx);
  }

#ifdef DEBUG
  if (!table.elemType().isNullable()) {
    table.assertRangeNotNull(oldSize, delta);
  }
#endif  // DEBUG
  return oldSize;
}

/* static */ int32_t Instance::tableSet(Instance* instance, uint32_t index,
                                        void* value, uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableSet.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  Table& table = *instance->tables()[tableIndex];

  if (index >= table.length()) {
    ReportTrapError(cx, JSMSG_WASM_TABLE_OUT_OF_BOUNDS);
    return -1;
  }

  switch (table.repr()) {
    case TableRepr::Ref:
      table.setAnyRef(index, AnyRef::fromCompiledCode(value));
      break;
    case TableRepr::Func:
      MOZ_RELEASE_ASSERT(!table.isAsmJS());
      table.fillFuncRef(index, 1, FuncRef::fromCompiledCode(value), cx);
      break;
  }

  return 0;
}

/* static */ uint32_t Instance::tableSize(Instance* instance,
                                          uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableSize.failureMode == FailureMode::Infallible);
  Table& table = *instance->tables()[tableIndex];
  return table.length();
}

/* static */ void* Instance::refFunc(Instance* instance, uint32_t funcIndex) {
  MOZ_ASSERT(SASigRefFunc.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();

  Tier tier = instance->code().bestTier();
  const MetadataTier& metadataTier = instance->metadata(tier);
  const FuncImportVector& funcImports = metadataTier.funcImports;

  // If this is an import, we need to recover the original function to maintain
  // reference equality between a re-exported function and 'ref.func'. The
  // identity of the imported function object is stable across tiers, which is
  // what we want.
  //
  // Use the imported function only if it is an exported function, otherwise
  // fall through to get a (possibly new) exported function.
  if (funcIndex < funcImports.length()) {
    FuncImportInstanceData& import =
        instance->funcImportInstanceData(funcImports[funcIndex]);
    if (import.callable->is<JSFunction>()) {
      JSFunction* fun = &import.callable->as<JSFunction>();
      if (IsWasmExportedFunction(fun)) {
        return FuncRef::fromJSFunction(fun).forCompiledCode();
      }
    }
  }

  RootedFunction fun(cx);
  Rooted<WasmInstanceObject*> instanceObj(cx, instance->object());
  if (!WasmInstanceObject::getExportedFunction(cx, instanceObj, funcIndex,
                                               &fun)) {
    // Validation ensures that we always have a valid funcIndex, so we must
    // have OOM'ed
    ReportOutOfMemory(cx);
    return AnyRef::invalid().forCompiledCode();
  }

  return FuncRef::fromJSFunction(fun).forCompiledCode();
}

//////////////////////////////////////////////////////////////////////////////
//
// Segment management.

/* static */ int32_t Instance::elemDrop(Instance* instance, uint32_t segIndex) {
  MOZ_ASSERT(SASigElemDrop.failureMode == FailureMode::FailOnNegI32);

  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");

  instance->passiveElemSegments_[segIndex].clearAndFree();
  return 0;
}

/* static */ int32_t Instance::dataDrop(Instance* instance, uint32_t segIndex) {
  MOZ_ASSERT(SASigDataDrop.failureMode == FailureMode::FailOnNegI32);

  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");

  if (!instance->passiveDataSegments_[segIndex]) {
    return 0;
  }

  SharedDataSegment& segRefPtr = instance->passiveDataSegments_[segIndex];
  MOZ_RELEASE_ASSERT(!segRefPtr->active());

  // Drop this instance's reference to the DataSegment so it can be released.
  segRefPtr = nullptr;
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// AnyRef support.

/* static */ void Instance::postBarrier(Instance* instance, void** location) {
  MOZ_ASSERT(SASigPostBarrier.failureMode == FailureMode::Infallible);
  MOZ_ASSERT(location);
  instance->storeBuffer_->putWasmAnyRef(
      reinterpret_cast<wasm::AnyRef*>(location));
}

/* static */ void Instance::postBarrierPrecise(Instance* instance,
                                               void** location, void* prev) {
  MOZ_ASSERT(SASigPostBarrierPrecise.failureMode == FailureMode::Infallible);
  postBarrierPreciseWithOffset(instance, location, /*offset=*/0, prev);
}

/* static */ void Instance::postBarrierPreciseWithOffset(Instance* instance,
                                                         void** base,
                                                         uint32_t offset,
                                                         void* prev) {
  MOZ_ASSERT(SASigPostBarrierPreciseWithOffset.failureMode ==
             FailureMode::Infallible);
  MOZ_ASSERT(base);
  wasm::AnyRef* location = (wasm::AnyRef*)(uintptr_t(base) + size_t(offset));
  wasm::AnyRef next = *location;
  InternalBarrierMethods<AnyRef>::postBarrier(
      location, wasm::AnyRef::fromCompiledCode(prev), next);
}

//////////////////////////////////////////////////////////////////////////////
//
// GC and exception handling support.

/* static */
template <bool ZeroFields>
void* Instance::structNewIL(Instance* instance,
                            TypeDefInstanceData* typeDefData) {
  MOZ_ASSERT((ZeroFields ? SASigStructNewIL_true : SASigStructNewIL_false)
                 .failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  // The new struct will be allocated in an initial heap as determined by
  // pretenuring logic as set up in `Instance::init`.
  return WasmStructObject::createStructIL<ZeroFields>(
      cx, typeDefData, typeDefData->allocSite.initialHeap());
}

template void* Instance::structNewIL<true>(Instance* instance,
                                           TypeDefInstanceData* typeDefData);
template void* Instance::structNewIL<false>(Instance* instance,
                                            TypeDefInstanceData* typeDefData);

/* static */
template <bool ZeroFields>
void* Instance::structNewOOL(Instance* instance,
                             TypeDefInstanceData* typeDefData) {
  MOZ_ASSERT((ZeroFields ? SASigStructNewOOL_true : SASigStructNewOOL_false)
                 .failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  // The new struct will be allocated in an initial heap as determined by
  // pretenuring logic as set up in `Instance::init`.
  return WasmStructObject::createStructOOL<ZeroFields>(
      cx, typeDefData, typeDefData->allocSite.initialHeap());
}

template void* Instance::structNewOOL<true>(Instance* instance,
                                            TypeDefInstanceData* typeDefData);
template void* Instance::structNewOOL<false>(Instance* instance,
                                             TypeDefInstanceData* typeDefData);

/* static */
template <bool ZeroFields>
void* Instance::arrayNew(Instance* instance, uint32_t numElements,
                         TypeDefInstanceData* typeDefData) {
  MOZ_ASSERT(
      (ZeroFields ? SASigArrayNew_true : SASigArrayNew_false).failureMode ==
      FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  // The new array will be allocated in an initial heap as determined by
  // pretenuring logic as set up in `Instance::init`.
  return WasmArrayObject::createArray<ZeroFields>(
      cx, typeDefData, typeDefData->allocSite.initialHeap(), numElements);
}

template void* Instance::arrayNew<true>(Instance* instance,
                                        uint32_t numElements,
                                        TypeDefInstanceData* typeDefData);
template void* Instance::arrayNew<false>(Instance* instance,
                                         uint32_t numElements,
                                         TypeDefInstanceData* typeDefData);

// Copies from a data segment into a wasm GC array. Performs the necessary
// bounds checks, accounting for the array's element size. If this function
// returns false, it has already reported a trap error.
static bool ArrayCopyFromData(JSContext* cx, Handle<WasmArrayObject*> arrayObj,
                              const TypeDef* typeDef, uint32_t arrayIndex,
                              const DataSegment* seg, uint32_t segByteOffset,
                              uint32_t numElements) {
  // Compute the number of bytes to copy, ensuring it's below 2^32.
  CheckedUint32 numBytesToCopy =
      CheckedUint32(numElements) *
      CheckedUint32(typeDef->arrayType().elementType().size());
  if (!numBytesToCopy.isValid()) {
    // Because the request implies that 2^32 or more bytes are to be copied.
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // Range-check the copy.  The obvious thing to do is to compute the offset
  // of the last byte to copy, but that would cause underflow in the
  // zero-length-and-zero-offset case.  Instead, compute that value plus one;
  // in other words the offset of the first byte *not* to copy.
  CheckedUint32 lastByteOffsetPlus1 =
      CheckedUint32(segByteOffset) + numBytesToCopy;

  CheckedUint32 numBytesAvailable(seg->bytes.length());
  if (!lastByteOffsetPlus1.isValid() || !numBytesAvailable.isValid() ||
      lastByteOffsetPlus1.value() > numBytesAvailable.value()) {
    // Because the last byte to copy doesn't exist inside `seg->bytes`.
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // Range check the destination array.
  uint64_t dstNumElements = uint64_t(arrayObj->numElements_);
  if (uint64_t(arrayIndex) + uint64_t(numElements) > dstNumElements) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // Because `numBytesToCopy` is an in-range `CheckedUint32`, the cast to
  // `size_t` is safe even on a 32-bit target.
  if (numElements != 0) {
    memcpy(arrayObj->data_, &seg->bytes[segByteOffset],
           size_t(numBytesToCopy.value()));
  }

  return true;
}

// Copies from an element segment into a wasm GC array. Performs the necessary
// bounds checks, accounting for the array's element size. If this function
// returns false, it has already reported a trap error.
static bool ArrayCopyFromElem(JSContext* cx, Handle<WasmArrayObject*> arrayObj,
                              uint32_t arrayIndex,
                              const InstanceElemSegment& seg,
                              uint32_t segOffset, uint32_t numElements) {
  // Range-check the copy. As in ArrayCopyFromData, compute the index of the
  // last element to copy, plus one.
  CheckedUint32 lastIndexPlus1 =
      CheckedUint32(segOffset) + CheckedUint32(numElements);
  CheckedUint32 numElemsAvailable(seg.length());
  if (!lastIndexPlus1.isValid() || !numElemsAvailable.isValid() ||
      lastIndexPlus1.value() > numElemsAvailable.value()) {
    // Because the last element to copy doesn't exist inside the segment.
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // Range check the destination array.
  uint64_t dstNumElements = uint64_t(arrayObj->numElements_);
  if (uint64_t(arrayIndex) + uint64_t(numElements) > dstNumElements) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  GCPtr<AnyRef>* dst = reinterpret_cast<GCPtr<AnyRef>*>(arrayObj->data_);
  for (uint32_t i = 0; i < numElements; i++) {
    dst[i] = seg[segOffset + i];
  }

  return true;
}

// Creates an array (WasmArrayObject) containing `numElements` of type
// described by `typeDef`.  Initialises it with data copied from the data
// segment whose index is `segIndex`, starting at byte offset `segByteOffset`
// in the segment.  Traps if the segment doesn't hold enough bytes to fill the
// array.
/* static */ void* Instance::arrayNewData(Instance* instance,
                                          uint32_t segByteOffset,
                                          uint32_t numElements,
                                          TypeDefInstanceData* typeDefData,
                                          uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayNewData.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();

  // Check that the data segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");
  const DataSegment* seg = instance->passiveDataSegments_[segIndex];

  // `seg` will be nullptr if the segment has already been 'data.drop'ed
  // (either implicitly in the case of 'active' segments during instantiation,
  // or explicitly by the data.drop instruction.)  In that case we can
  // continue only if there's no need to copy any data out of it.
  if (!seg && (numElements != 0 || segByteOffset != 0)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return nullptr;
  }
  // At this point, if `seg` is null then `numElements` and `segByteOffset`
  // are both zero.

  const TypeDef* typeDef = typeDefData->typeDef;
  Rooted<WasmArrayObject*> arrayObj(
      cx,
      WasmArrayObject::createArray<true>(
          cx, typeDefData, typeDefData->allocSite.initialHeap(), numElements));
  if (!arrayObj) {
    // WasmArrayObject::createArray will have reported OOM.
    return nullptr;
  }
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!seg) {
    // A zero-length array was requested and has been created, so we're done.
    return arrayObj;
  }

  if (!ArrayCopyFromData(cx, arrayObj, typeDef, 0, seg, segByteOffset,
                         numElements)) {
    // Trap errors will be reported by ArrayCopyFromData.
    return nullptr;
  }

  return arrayObj;
}

// This is almost identical to ::arrayNewData, apart from the final part that
// actually copies the data.  It creates an array (WasmArrayObject)
// containing `numElements` of type described by `typeDef`.  Initialises it
// with data copied from the element segment whose index is `segIndex`,
// starting at element number `srcOffset` in the segment.  Traps if the
// segment doesn't hold enough elements to fill the array.
/* static */ void* Instance::arrayNewElem(Instance* instance,
                                          uint32_t srcOffset,
                                          uint32_t numElements,
                                          TypeDefInstanceData* typeDefData,
                                          uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayNewElem.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();

  // Check that the element segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");
  const InstanceElemSegment& seg = instance->passiveElemSegments_[segIndex];

  const TypeDef* typeDef = typeDefData->typeDef;

  // Any data coming from an element segment will be an AnyRef. Writes into
  // array memory are done with raw pointers, so we must ensure here that the
  // destination size is correct.
  MOZ_RELEASE_ASSERT(typeDef->arrayType().elementType().size() ==
                     sizeof(AnyRef));

  Rooted<WasmArrayObject*> arrayObj(
      cx,
      WasmArrayObject::createArray<true>(
          cx, typeDefData, typeDefData->allocSite.initialHeap(), numElements));
  if (!arrayObj) {
    // WasmArrayObject::createArray will have reported OOM.
    return nullptr;
  }
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!ArrayCopyFromElem(cx, arrayObj, 0, seg, srcOffset, numElements)) {
    // Trap errors will be reported by ArrayCopyFromElems.
    return nullptr;
  }

  return arrayObj;
}

// Copies a range of the data segment `segIndex` into an array
// (WasmArrayObject), starting at offset `segByteOffset` in the data segment and
// index `index` in the array. `numElements` is the length of the copy in array
// elements, NOT bytes - the number of bytes will be computed based on the type
// of the array.
//
// Traps if accesses are out of bounds for either the data segment or the array,
// or if the array object is null.
/* static */ int32_t Instance::arrayInitData(
    Instance* instance, void* array, uint32_t index, uint32_t segByteOffset,
    uint32_t numElements, TypeDefInstanceData* typeDefData, uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayInitData.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Check that the data segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");
  const DataSegment* seg = instance->passiveDataSegments_[segIndex];

  // `seg` will be nullptr if the segment has already been 'data.drop'ed
  // (either implicitly in the case of 'active' segments during instantiation,
  // or explicitly by the data.drop instruction.)  In that case we can
  // continue only if there's no need to copy any data out of it.
  if (!seg && (numElements != 0 || segByteOffset != 0)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }
  // At this point, if `seg` is null then `numElements` and `segByteOffset`
  // are both zero.

  // Trap if the array is null.
  if (!array) {
    ReportTrapError(cx, JSMSG_WASM_DEREF_NULL);
    return -1;
  }

  if (!seg) {
    // A zero-length init was requested, so we're done.
    return 0;
  }

  // Get hold of the array.
  const TypeDef* typeDef = typeDefData->typeDef;
  Rooted<WasmArrayObject*> arrayObj(cx, static_cast<WasmArrayObject*>(array));
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!ArrayCopyFromData(cx, arrayObj, typeDef, index, seg, segByteOffset,
                         numElements)) {
    // Trap errors will be reported by ArrayCopyFromData.
    return -1;
  }

  return 0;
}

// Copies a range of the element segment `segIndex` into an array
// (WasmArrayObject), starting at offset `segOffset` in the elem segment and
// index `index` in the array. `numElements` is the length of the copy.
//
// Traps if accesses are out of bounds for either the elem segment or the array,
// or if the array object is null.
/* static */ int32_t Instance::arrayInitElem(Instance* instance, void* array,
                                             uint32_t index, uint32_t segOffset,
                                             uint32_t numElements,
                                             TypeDefInstanceData* typeDefData,
                                             uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayInitElem.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Check that the element segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");
  const InstanceElemSegment& seg = instance->passiveElemSegments_[segIndex];

  // Trap if the array is null.
  if (!array) {
    ReportTrapError(cx, JSMSG_WASM_DEREF_NULL);
    return -1;
  }

  const TypeDef* typeDef = typeDefData->typeDef;

  // Any data coming from an element segment will be an AnyRef. Writes into
  // array memory are done with raw pointers, so we must ensure here that the
  // destination size is correct.
  MOZ_RELEASE_ASSERT(typeDef->arrayType().elementType().size() ==
                     sizeof(AnyRef));

  // Get hold of the array.
  Rooted<WasmArrayObject*> arrayObj(cx, static_cast<WasmArrayObject*>(array));
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!ArrayCopyFromElem(cx, arrayObj, index, seg, segOffset, numElements)) {
    // Trap errors will be reported by ArrayCopyFromElems.
    return -1;
  }

  return 0;
}

/* static */ int32_t Instance::arrayCopy(Instance* instance, void* dstArray,
                                         uint32_t dstIndex, void* srcArray,
                                         uint32_t srcIndex,
                                         uint32_t numElements,
                                         uint32_t elementSize) {
  MOZ_ASSERT(SASigArrayCopy.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // At the entry point, `elementSize` may be negative to indicate
  // reftyped-ness of array elements.  That is done in order to avoid having
  // to pass yet another (boolean) parameter here.

  // "traps if either array is null"
  if (!srcArray || !dstArray) {
    ReportTrapError(cx, JSMSG_WASM_DEREF_NULL);
    return -1;
  }

  bool elemsAreRefTyped = false;
  if (int32_t(elementSize) < 0) {
    elemsAreRefTyped = true;
    elementSize = uint32_t(-int32_t(elementSize));
  }
  MOZ_ASSERT(elementSize >= 1 && elementSize <= 16);

  // Get hold of the two arrays.
  Rooted<WasmArrayObject*> dstArrayObj(cx,
                                       static_cast<WasmArrayObject*>(dstArray));
  MOZ_RELEASE_ASSERT(dstArrayObj->is<WasmArrayObject>());

  Rooted<WasmArrayObject*> srcArrayObj(cx,
                                       static_cast<WasmArrayObject*>(srcArray));
  MOZ_RELEASE_ASSERT(srcArrayObj->is<WasmArrayObject>());

  // If WasmArrayObject::numElements() is changed to return 64 bits, the
  // following checking logic will be incorrect.
  STATIC_ASSERT_WASMARRAYELEMENTS_NUMELEMENTS_IS_U32;

  // "traps if destination + length > len(array1)"
  uint64_t dstNumElements = uint64_t(dstArrayObj->numElements_);
  if (uint64_t(dstIndex) + uint64_t(numElements) > dstNumElements) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // "traps if source + length > len(array2)"
  uint64_t srcNumElements = uint64_t(srcArrayObj->numElements_);
  if (uint64_t(srcIndex) + uint64_t(numElements) > srcNumElements) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // trap if we're asked to copy 2^32 or more bytes on a 32-bit target.
  uint64_t numBytesToCopy = uint64_t(numElements) * uint64_t(elementSize);
#ifndef JS_64BIT
  if (numBytesToCopy > uint64_t(UINT32_MAX)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }
#endif
  // We're now assured that `numBytesToCopy` can be cast to `size_t` without
  // overflow.

  // Actually do the copy, taking care to handle cases where the src and dst
  // areas overlap.
  uint8_t* srcBase = srcArrayObj->data_;
  uint8_t* dstBase = dstArrayObj->data_;
  srcBase += size_t(srcIndex) * size_t(elementSize);
  dstBase += size_t(dstIndex) * size_t(elementSize);

  if (numBytesToCopy == 0 || srcBase == dstBase) {
    // Early exit if there's no work to do.
    return 0;
  }

  if (!elemsAreRefTyped) {
    // Hand off to memmove, which is presumably highly optimized.
    memmove(dstBase, srcBase, size_t(numBytesToCopy));
    return 0;
  }

  // We're copying refs; doing that needs suitable GC barrier-ing.
  uint8_t* nextSrc;
  uint8_t* nextDst;
  intptr_t step;
  if (dstBase < srcBase) {
    // Moving data backwards in the address space; so iterate forwards through
    // the array.
    step = intptr_t(elementSize);
    nextSrc = srcBase;
    nextDst = dstBase;
  } else {
    // Moving data forwards; so iterate backwards.
    step = -intptr_t(elementSize);
    nextSrc = srcBase + size_t(numBytesToCopy) - size_t(elementSize);
    nextDst = dstBase + size_t(numBytesToCopy) - size_t(elementSize);
  }
  // We don't know the type of the elems, only that they are refs.  No matter,
  // we can simply make up a type.
  RefType aRefType = RefType::eq();
  // Do the iteration
  for (size_t i = 0; i < size_t(numElements); i++) {
    // Copy `elementSize` bytes from `nextSrc` to `nextDst`.
    RootedVal value(cx, aRefType);
    value.get().readFromHeapLocation(nextSrc);
    value.get().writeToHeapLocation(nextDst);
    nextSrc += step;
    nextDst += step;
  }

  return 0;
}

/* static */ void* Instance::exceptionNew(Instance* instance, void* tagArg) {
  MOZ_ASSERT(SASigExceptionNew.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  AnyRef tag = AnyRef::fromCompiledCode(tagArg);
  Rooted<WasmTagObject*> tagObj(cx, &tag.toJSObject().as<WasmTagObject>());
  RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmException));
  RootedObject stack(cx, nullptr);
  // An OOM will result in null which will be caught on the wasm side.
  return AnyRef::fromJSObjectOrNull(
             WasmExceptionObject::create(cx, tagObj, stack, proto))
      .forCompiledCode();
}

/* static */ int32_t Instance::throwException(Instance* instance,
                                              void* exceptionArg) {
  MOZ_ASSERT(SASigThrowException.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  AnyRef exception = AnyRef::fromCompiledCode(exceptionArg);
  RootedValue exnVal(cx, exception.toJSValue());
  cx->setPendingException(exnVal, nullptr);

  // By always returning -1, we trigger a wasmTrap(Trap::ThrowReported),
  // and use that to trigger the stack walking for this exception.
  return -1;
}

/* static */ int32_t Instance::intrI8VecMul(Instance* instance, uint32_t dest,
                                            uint32_t src1, uint32_t src2,
                                            uint32_t len, uint8_t* memBase) {
  MOZ_ASSERT(SASigIntrI8VecMul.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  const WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();

  // Bounds check and deal with arithmetic overflow.
  uint64_t destLimit = uint64_t(dest) + uint64_t(len);
  uint64_t src1Limit = uint64_t(src1) + uint64_t(len);
  uint64_t src2Limit = uint64_t(src2) + uint64_t(len);
  if (destLimit > memLen || src1Limit > memLen || src2Limit > memLen) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // Basic dot product
  uint8_t* destPtr = &memBase[dest];
  uint8_t* src1Ptr = &memBase[src1];
  uint8_t* src2Ptr = &memBase[src2];
  while (len > 0) {
    *destPtr = (*src1Ptr) * (*src2Ptr);

    destPtr++;
    src1Ptr++;
    src2Ptr++;
    len--;
  }

  return 0;
}

// TODO: this cast is irregular and not representable in wasm, as it does not
// take into account the enclosing recursion group of the type. This is
// temporary until builtin module functions can specify a precise array type
// for params/results.
template <bool isMutable>
static WasmArrayObject* UncheckedCastToArrayI16(HandleAnyRef ref) {
  JSObject& object = ref.toJSObject();
  WasmArrayObject& array = object.as<WasmArrayObject>();
  DebugOnly<const ArrayType*> type(&array.typeDef().arrayType());
  MOZ_ASSERT(type->elementType() == StorageType::I16);
  MOZ_ASSERT(type->isMutable() == isMutable);
  return &array;
}

/* static */
int32_t Instance::stringTest(Instance* instance, void* stringArg) {
  AnyRef string = AnyRef::fromCompiledCode(stringArg);
  if (string.isNull() || !string.isJSString()) {
    return 0;
  }
  return 1;
}

/* static */
void* Instance::stringCast(Instance* instance, void* stringArg) {
  AnyRef string = AnyRef::fromCompiledCode(stringArg);
  if (string.isNull() || !string.isJSString()) {
    ReportTrapError(instance->cx(), JSMSG_WASM_BAD_CAST);
    return nullptr;
  }
  return string.forCompiledCode();
}

/* static */
void* Instance::stringFromCharCodeArray(Instance* instance, void* arrayArg,
                                        uint32_t arrayStart,
                                        uint32_t arrayCount) {
  JSContext* cx = instance->cx();
  RootedAnyRef arrayRef(cx, AnyRef::fromCompiledCode(arrayArg));
  Rooted<WasmArrayObject*> array(cx, UncheckedCastToArrayI16<true>(arrayRef));

  CheckedUint32 lastIndexPlus1 =
      CheckedUint32(arrayStart) + CheckedUint32(arrayCount);
  if (!lastIndexPlus1.isValid() ||
      lastIndexPlus1.value() > array->numElements_) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return nullptr;
  }

  // GC is disabled on this call since it can cause the array to move,
  // invalidating the data pointer we pass as a parameter
  JSLinearString* string = NewStringCopyN<NoGC, char16_t>(
      cx, (char16_t*)array->data_ + arrayStart, arrayCount);
  if (!string) {
    return nullptr;
  }
  return AnyRef::fromJSString(string).forCompiledCode();
}

/* static */
int32_t Instance::stringIntoCharCodeArray(Instance* instance, void* stringArg,
                                          void* arrayArg, uint32_t arrayStart) {
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }
  Rooted<JSString*> string(cx, stringRef.toJSString());
  size_t stringLength = string->length();

  RootedAnyRef arrayRef(cx, AnyRef::fromCompiledCode(arrayArg));
  Rooted<WasmArrayObject*> array(cx, UncheckedCastToArrayI16<true>(arrayRef));

  CheckedUint32 lastIndexPlus1 = CheckedUint32(arrayStart) + stringLength;
  if (!lastIndexPlus1.isValid() ||
      lastIndexPlus1.value() > array->numElements_) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  JSLinearString* linearStr = string->ensureLinear(cx);
  if (!linearStr) {
    return -1;
  }
  char16_t* arrayData = reinterpret_cast<char16_t*>(array->data_);
  CopyChars(arrayData + arrayStart, *linearStr);
  return stringLength;
}

void* Instance::stringFromCharCode(Instance* instance, uint32_t charCode) {
  JSContext* cx = instance->cx();

  JSString* str = StringFromCharCode(cx, int32_t(charCode));
  if (!str) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }

  return AnyRef::fromJSString(str).forCompiledCode();
}

void* Instance::stringFromCodePoint(Instance* instance, uint32_t codePoint) {
  JSContext* cx = instance->cx();

  // Check for any error conditions before calling fromCodePoint so we report
  // the correct error
  if (codePoint > unicode::NonBMPMax) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CODEPOINT);
    return nullptr;
  }

  JSString* str = StringFromCodePoint(cx, char32_t(codePoint));
  if (!str) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }

  return AnyRef::fromJSString(str).forCompiledCode();
}

int32_t Instance::stringCharCodeAt(Instance* instance, void* stringArg,
                                   uint32_t index) {
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  Rooted<JSString*> string(cx, stringRef.toJSString());
  if (index >= string->length()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  char16_t c;
  if (!string->getChar(cx, index, &c)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }
  return c;
}

int32_t Instance::stringCodePointAt(Instance* instance, void* stringArg,
                                    uint32_t index) {
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  Rooted<JSString*> string(cx, stringRef.toJSString());
  if (index >= string->length()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  char32_t c;
  if (!string->getCodePoint(cx, index, &c)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }
  return c;
}

int32_t Instance::stringLength(Instance* instance, void* stringArg) {
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  static_assert(JS::MaxStringLength <= INT32_MAX);
  return (int32_t)stringRef.toJSString()->length();
}

void* Instance::stringConcat(Instance* instance, void* firstStringArg,
                             void* secondStringArg) {
  JSContext* cx = instance->cx();

  AnyRef firstStringRef = AnyRef::fromCompiledCode(firstStringArg);
  AnyRef secondStringRef = AnyRef::fromCompiledCode(secondStringArg);
  if (!firstStringRef.isJSString() || !secondStringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return nullptr;
  }

  Rooted<JSString*> firstString(cx, firstStringRef.toJSString());
  Rooted<JSString*> secondString(cx, secondStringRef.toJSString());
  JSString* result = ConcatStrings<CanGC>(cx, firstString, secondString);
  if (!result) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }
  return AnyRef::fromJSString(result).forCompiledCode();
}

void* Instance::stringSubstring(Instance* instance, void* stringArg,
                                int32_t startIndex, int32_t endIndex) {
  JSContext* cx = instance->cx();

  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return nullptr;
  }

  RootedString string(cx, stringRef.toJSString());
  static_assert(JS::MaxStringLength <= INT32_MAX);
  if ((uint32_t)startIndex > string->length() ||
      (uint32_t)endIndex > string->length() || startIndex > endIndex) {
    return AnyRef::fromJSString(cx->names().empty_).forCompiledCode();
  }

  JSString* result =
      SubstringKernel(cx, string, startIndex, endIndex - startIndex);
  if (!result) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }
  return AnyRef::fromJSString(result).forCompiledCode();
}

int32_t Instance::stringEquals(Instance* instance, void* firstStringArg,
                               void* secondStringArg) {
  JSContext* cx = instance->cx();

  AnyRef firstStringRef = AnyRef::fromCompiledCode(firstStringArg);
  AnyRef secondStringRef = AnyRef::fromCompiledCode(secondStringArg);
  if (!firstStringRef.isJSString() || !secondStringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  bool equals;
  if (!EqualStrings(cx, firstStringRef.toJSString(),
                    secondStringRef.toJSString(), &equals)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return -1;
  }
  return equals ? 1 : 0;
}

int32_t Instance::stringCompare(Instance* instance, void* firstStringArg,
                                void* secondStringArg) {
  JSContext* cx = instance->cx();

  AnyRef firstStringRef = AnyRef::fromCompiledCode(firstStringArg);
  AnyRef secondStringRef = AnyRef::fromCompiledCode(secondStringArg);
  if (!firstStringRef.isJSString() || !secondStringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return INT32_MAX;
  }

  int32_t result;
  if (!CompareStrings(cx, firstStringRef.toJSString(),
                      secondStringRef.toJSString(), &result)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return INT32_MAX;
  }

  if (result < 0) {
    return -1;
  }
  if (result > 0) {
    return 1;
  }
  return result;
}

//////////////////////////////////////////////////////////////////////////////
//
// Instance creation and related.

Instance::Instance(JSContext* cx, Handle<WasmInstanceObject*> object,
                   const SharedCode& code, SharedTableVector&& tables,
                   UniqueDebugState maybeDebug)
    : realm_(cx->realm()),
      jsJitArgsRectifier_(
          cx->runtime()->jitRuntime()->getArgumentsRectifier().value),
      jsJitExceptionHandler_(
          cx->runtime()->jitRuntime()->getExceptionTail().value),
      preBarrierCode_(
          cx->runtime()->jitRuntime()->preBarrier(MIRType::WasmAnyRef).value),
      storeBuffer_(&cx->runtime()->gc.storeBuffer()),
      object_(object),
      code_(std::move(code)),
      tables_(std::move(tables)),
      maybeDebug_(std::move(maybeDebug)),
      debugFilter_(nullptr),
      maxInitializedGlobalsIndexPlus1_(0) {
  for (size_t i = 0; i < N_BASELINE_SCRATCH_WORDS; i++) {
    baselineScratchWords_[i] = 0;
  }
}

Instance* Instance::create(JSContext* cx, Handle<WasmInstanceObject*> object,
                           const SharedCode& code, uint32_t instanceDataLength,
                           SharedTableVector&& tables,
                           UniqueDebugState maybeDebug) {
  void* base = js_calloc(alignof(Instance) + offsetof(Instance, data_) +
                         instanceDataLength);
  if (!base) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  void* aligned = (void*)AlignBytes(uintptr_t(base), alignof(Instance));

  auto* instance = new (aligned)
      Instance(cx, object, code, std::move(tables), std::move(maybeDebug));
  instance->allocatedBase_ = base;
  return instance;
}

void Instance::destroy(Instance* instance) {
  instance->~Instance();
  js_free(instance->allocatedBase_);
}

bool Instance::init(JSContext* cx, const JSObjectVector& funcImports,
                    const ValVector& globalImportValues,
                    Handle<WasmMemoryObjectVector> memories,
                    const WasmGlobalObjectVector& globalObjs,
                    const WasmTagObjectVector& tagObjs,
                    const DataSegmentVector& dataSegments,
                    const ModuleElemSegmentVector& elemSegments) {
  MOZ_ASSERT(!!maybeDebug_ == metadata().debugEnabled);

#ifdef DEBUG
  for (auto t : code_->tiers()) {
    MOZ_ASSERT(funcImports.length() == metadata(t).funcImports.length());
  }
#endif
  MOZ_ASSERT(tables_.length() == metadata().tables.length());

  cx_ = cx;
  valueBoxClass_ = AnyRef::valueBoxClass();
  resetInterrupt(cx);
  jumpTable_ = code_->tieringJumpTable();
  debugFilter_ = nullptr;
  addressOfNeedsIncrementalBarrier_ =
      cx->compartment()->zone()->addressOfNeedsIncrementalBarrier();
  addressOfNurseryPosition_ = cx->nursery().addressOfPosition();
#ifdef JS_GC_ZEAL
  addressOfGCZealModeBits_ = cx->runtime()->gc.addressOfZealModeBits();
#endif

  // Initialize type definitions in the instance data.
  const SharedTypeContext& types = metadata().types;
  Zone* zone = realm()->zone();
  for (uint32_t typeIndex = 0; typeIndex < types->length(); typeIndex++) {
    const TypeDef& typeDef = types->type(typeIndex);
    TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);

    // Set default field values.
    new (typeDefData) TypeDefInstanceData();

    // Store the runtime type for this type index
    typeDefData->typeDef = &typeDef;
    typeDefData->superTypeVector = typeDef.superTypeVector();

    if (typeDef.kind() == TypeDefKind::Struct ||
        typeDef.kind() == TypeDefKind::Array) {
      // Compute the parameters that allocation will use.  First, the class
      // and alloc kind for the type definition.
      const JSClass* clasp;
      gc::AllocKind allocKind;

      if (typeDef.kind() == TypeDefKind::Struct) {
        clasp = WasmStructObject::classForTypeDef(&typeDef);
        allocKind = WasmStructObject::allocKindForTypeDef(&typeDef);

        // Move the alloc kind to background if possible
        if (CanChangeToBackgroundAllocKind(allocKind, clasp)) {
          allocKind = ForegroundToBackgroundAllocKind(allocKind);
        }
      } else {
        clasp = &WasmArrayObject::class_;
        allocKind = gc::AllocKind::INVALID;
      }

      // Find the shape using the class and recursion group
      const ObjectFlags objectFlags = {ObjectFlag::NotExtensible};
      typeDefData->shape =
          WasmGCShape::getShape(cx, clasp, cx->realm(), TaggedProto(),
                                &typeDef.recGroup(), objectFlags);
      if (!typeDefData->shape) {
        return false;
      }

      typeDefData->clasp = clasp;
      typeDefData->allocKind = allocKind;

      // Initialize the allocation site for pre-tenuring.
      typeDefData->allocSite.initWasm(zone);

      // If `typeDef` is a struct, cache its size here, so that allocators
      // don't have to chase back through `typeDef` to determine that.
      // Similarly, if `typeDef` is an array, cache its array element size
      // here.
      MOZ_ASSERT(typeDefData->unused == 0);
      if (typeDef.kind() == TypeDefKind::Struct) {
        typeDefData->structTypeSize = typeDef.structType().size_;
        // StructLayout::close ensures this is an integral number of words.
        MOZ_ASSERT((typeDefData->structTypeSize % sizeof(uintptr_t)) == 0);
      } else {
        uint32_t arrayElemSize = typeDef.arrayType().elementType().size();
        typeDefData->arrayElemSize = arrayElemSize;
        MOZ_ASSERT(arrayElemSize == 16 || arrayElemSize == 8 ||
                   arrayElemSize == 4 || arrayElemSize == 2 ||
                   arrayElemSize == 1);
      }
    } else if (typeDef.kind() == TypeDefKind::Func) {
      // Nothing to do; the default values are OK.
    } else {
      MOZ_ASSERT(typeDef.kind() == TypeDefKind::None);
      MOZ_CRASH();
    }
  }

  // Initialize function imports in the instance data
  Tier callerTier = code_->bestTier();
  for (size_t i = 0; i < metadata(callerTier).funcImports.length(); i++) {
    JSObject* f = funcImports[i];

#ifdef ENABLE_WASM_JSPI
    if (JSObject* suspendingObject = MaybeUnwrapSuspendingObject(f)) {
      // Compile suspending function Wasm wrapper.
      const FuncImport& fi = metadata(callerTier).funcImports[i];
      const FuncType& funcType = metadata().getFuncImportType(fi);
      RootedObject wrapped(cx, suspendingObject);
      RootedFunction wrapper(
          cx, WasmSuspendingFunctionCreate(cx, wrapped, funcType));
      if (!wrapper) {
        return false;
      }
      MOZ_ASSERT(IsWasmExportedFunction(wrapper));
      f = wrapper;
    }
#endif

    MOZ_ASSERT(f->isCallable());
    const FuncImport& fi = metadata(callerTier).funcImports[i];
    const FuncType& funcType = metadata().getFuncImportType(fi);
    FuncImportInstanceData& import = funcImportInstanceData(fi);
    import.callable = f;
    if (f->is<JSFunction>()) {
      JSFunction* fun = &f->as<JSFunction>();
      if (!isAsmJS() && IsWasmExportedFunction(fun)) {
        WasmInstanceObject* calleeInstanceObj =
            ExportedFunctionToInstanceObject(fun);
        Instance& calleeInstance = calleeInstanceObj->instance();
        Tier calleeTier = calleeInstance.code().bestTier();
        const CodeRange& codeRange =
            calleeInstanceObj->getExportedFunctionCodeRange(
                &f->as<JSFunction>(), calleeTier);
        import.instance = &calleeInstance;
        import.realm = fun->realm();
        import.code = calleeInstance.codeBase(calleeTier) +
                      codeRange.funcUncheckedCallEntry();
      } else if (void* thunk = MaybeGetBuiltinThunk(fun, funcType)) {
        import.instance = this;
        import.realm = fun->realm();
        import.code = thunk;
      } else {
        import.instance = this;
        import.realm = fun->realm();
        import.code = codeBase(callerTier) + fi.interpExitCodeOffset();
      }
    } else {
      import.instance = this;
      import.realm = f->nonCCWRealm();
      import.code = codeBase(callerTier) + fi.interpExitCodeOffset();
    }
  }

  // Initialize globals in the instance data.
  //
  // This must be performed after we have initialized runtime types as a global
  // initializer may reference them.
  //
  // We increment `maxInitializedGlobalsIndexPlus1_` every iteration of the
  // loop, as we call out to `InitExpr::evaluate` which may call
  // `constantGlobalGet` which uses this value to assert we're never accessing
  // uninitialized globals.
  maxInitializedGlobalsIndexPlus1_ = 0;
  for (size_t i = 0; i < metadata().globals.length();
       i++, maxInitializedGlobalsIndexPlus1_ = i) {
    const GlobalDesc& global = metadata().globals[i];

    // Constants are baked into the code, never stored in the global area.
    if (global.isConstant()) {
      continue;
    }

    uint8_t* globalAddr = data() + global.offset();
    switch (global.kind()) {
      case GlobalKind::Import: {
        size_t imported = global.importIndex();
        if (global.isIndirect()) {
          *(void**)globalAddr =
              (void*)&globalObjs[imported]->val().get().cell();
        } else {
          globalImportValues[imported].writeToHeapLocation(globalAddr);
        }
        break;
      }
      case GlobalKind::Variable: {
        RootedVal val(cx);
        const InitExpr& init = global.initExpr();
        Rooted<WasmInstanceObject*> instanceObj(cx, object());
        if (!init.evaluate(cx, instanceObj, &val)) {
          return false;
        }

        if (global.isIndirect()) {
          // Initialize the cell
          globalObjs[i]->setVal(val);

          // Link to the cell
          *(void**)globalAddr = globalObjs[i]->addressOfCell();
        } else {
          val.get().writeToHeapLocation(globalAddr);
        }
        break;
      }
      case GlobalKind::Constant: {
        MOZ_CRASH("skipped at the top");
      }
    }
  }

  // All globals were initialized
  MOZ_ASSERT(maxInitializedGlobalsIndexPlus1_ == metadata().globals.length());

  // Initialize memories in the instance data
  for (size_t i = 0; i < memories.length(); i++) {
    const MemoryDesc& md = metadata().memories[i];
    MemoryInstanceData& data = memoryInstanceData(i);
    WasmMemoryObject* memory = memories.get()[i];

    data.memory = memory;
    data.base = memory->buffer().dataPointerEither().unwrap();
    size_t limit = memory->boundsCheckLimit();
#if !defined(JS_64BIT)
    // We assume that the limit is a 32-bit quantity
    MOZ_ASSERT(limit <= UINT32_MAX);
#endif
    data.boundsCheckLimit = limit;
    data.isShared = md.isShared();

    // Add observer if our memory base may grow
    if (memory && memory->movingGrowable() &&
        !memory->addMovingGrowObserver(cx, object_)) {
      return false;
    }
  }

  // Cache the default memory's values
  if (memories.length() > 0) {
    MemoryInstanceData& data = memoryInstanceData(0);
    memory0Base_ = data.base;
    memory0BoundsCheckLimit_ = data.boundsCheckLimit;
  } else {
    memory0Base_ = nullptr;
    memory0BoundsCheckLimit_ = 0;
  }

  // Initialize tables in the instance data
  for (size_t i = 0; i < tables_.length(); i++) {
    const TableDesc& td = metadata().tables[i];
    TableInstanceData& table = tableInstanceData(i);
    table.length = tables_[i]->length();
    table.elements = tables_[i]->instanceElements();
    // Non-imported tables, with init_expr, has to be initialized with
    // the evaluated value.
    if (!td.isImported && td.initExpr) {
      Rooted<WasmInstanceObject*> instanceObj(cx, object());
      RootedVal val(cx);
      if (!td.initExpr->evaluate(cx, instanceObj, &val)) {
        return false;
      }
      RootedAnyRef ref(cx, val.get().ref());
      tables_[i]->fillUninitialized(0, tables_[i]->length(), ref, cx);
    }
  }

#ifdef DEBUG
  // All (linked) tables with non-nullable types must be initialized.
  for (size_t i = 0; i < tables_.length(); i++) {
    const TableDesc& td = metadata().tables[i];
    if (!td.elemType.isNullable()) {
      tables_[i]->assertRangeNotNull(0, tables_[i]->length());
    }
  }
#endif  // DEBUG

  // Initialize tags in the instance data
  for (size_t i = 0; i < metadata().tags.length(); i++) {
    MOZ_ASSERT(tagObjs[i] != nullptr);
    tagInstanceData(i).object = tagObjs[i];
  }
  pendingException_ = nullptr;
  pendingExceptionTag_ = nullptr;

  // Add debug filtering table.
  if (metadata().debugEnabled) {
    size_t numFuncs = metadata().debugNumFuncs();
    size_t numWords = std::max<size_t>((numFuncs + 31) / 32, 1);
    debugFilter_ = (uint32_t*)js_calloc(numWords, sizeof(uint32_t));
    if (!debugFilter_) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Add observers if our tables may grow
  for (const SharedTable& table : tables_) {
    if (table->movingGrowable() && !table->addMovingGrowObserver(cx, object_)) {
      return false;
    }
  }

  // Take references to the passive data segments
  if (!passiveDataSegments_.resize(dataSegments.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < dataSegments.length(); i++) {
    if (!dataSegments[i]->active()) {
      passiveDataSegments_[i] = dataSegments[i];
    }
  }

  // Create InstanceElemSegments for any passive element segments, since these
  // are the ones available at runtime.
  if (!passiveElemSegments_.resize(elemSegments.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < elemSegments.length(); i++) {
    const ModuleElemSegment& seg = elemSegments[i];
    if (seg.kind == ModuleElemSegment::Kind::Passive) {
      passiveElemSegments_[i] = InstanceElemSegment();
      InstanceElemSegment& instanceSeg = passiveElemSegments_[i];
      if (!instanceSeg.reserve(seg.numElements())) {
        ReportOutOfMemory(cx);
        return false;
      }

      bool ok = iterElemsAnyrefs(seg, [&](uint32_t _, AnyRef ref) -> bool {
        instanceSeg.infallibleAppend(ref);
        return true;
      });
      if (!ok) {
        return false;
      }
    }
  }

  return true;
}

Instance::~Instance() {
  realm_->wasm.unregisterInstance(*this);

  if (debugFilter_) {
    js_free(debugFilter_);
  }

  // Any pending exceptions should have been consumed.
  MOZ_ASSERT(pendingException_.isNull());
}

void Instance::setInterrupt() {
  interrupt_ = true;
  stackLimit_ = JS::NativeStackLimitMin;
}

bool Instance::isInterrupted() const {
  return interrupt_ || stackLimit_ == JS::NativeStackLimitMin;
}

void Instance::resetInterrupt(JSContext* cx) {
  interrupt_ = false;
#ifdef ENABLE_WASM_JSPI
  if (cx->wasm().suspendableStackLimit != JS::NativeStackLimitMin) {
    stackLimit_ = cx->wasm().suspendableStackLimit;
    return;
  }
#endif
  stackLimit_ = cx->stackLimitForJitCode(JS::StackForUntrustedScript);
}

void Instance::setTemporaryStackLimit(JS::NativeStackLimit limit) {
  if (!isInterrupted()) {
    stackLimit_ = limit;
  }
}

void Instance::resetTemporaryStackLimit(JSContext* cx) {
  if (!isInterrupted()) {
    stackLimit_ = cx->stackLimitForJitCode(JS::StackForUntrustedScript);
  }
}

bool Instance::debugFilter(uint32_t funcIndex) const {
  return (debugFilter_[funcIndex / 32] >> funcIndex % 32) & 1;
}

void Instance::setDebugFilter(uint32_t funcIndex, bool value) {
  if (value) {
    debugFilter_[funcIndex / 32] |= (1 << funcIndex % 32);
  } else {
    debugFilter_[funcIndex / 32] &= ~(1 << funcIndex % 32);
  }
}

bool Instance::memoryAccessInGuardRegion(const uint8_t* addr,
                                         unsigned numBytes) const {
  MOZ_ASSERT(numBytes > 0);

  for (uint32_t memoryIndex = 0; memoryIndex < metadata().memories.length();
       memoryIndex++) {
    uint8_t* base = memoryBase(memoryIndex).unwrap(/* comparison */);
    if (addr < base) {
      continue;
    }

    WasmMemoryObject* mem = memory(memoryIndex);
    size_t lastByteOffset = addr - base + (numBytes - 1);
    if (lastByteOffset >= mem->volatileMemoryLength() &&
        lastByteOffset < mem->buffer().wasmMappedSize()) {
      return true;
    }
  }
  return false;
}

void Instance::tracePrivate(JSTracer* trc) {
  // This method is only called from WasmInstanceObject so the only reason why
  // TraceEdge is called is so that the pointer can be updated during a moving
  // GC.
  MOZ_ASSERT_IF(trc->isMarkingTracer(), gc::IsMarked(trc->runtime(), object_));
  TraceEdge(trc, &object_, "wasm instance object");

  // OK to just do one tier here; though the tiers have different funcImports
  // tables, they share the instance object.
  for (const FuncImport& fi : metadata(code().stableTier()).funcImports) {
    TraceNullableEdge(trc, &funcImportInstanceData(fi).callable, "wasm import");
  }

  for (uint32_t memoryIndex = 0;
       memoryIndex < code().metadata().memories.length(); memoryIndex++) {
    MemoryInstanceData& memoryData = memoryInstanceData(memoryIndex);
    TraceNullableEdge(trc, &memoryData.memory, "wasm memory object");
  }

  for (const SharedTable& table : tables_) {
    table->trace(trc);
  }

  for (const GlobalDesc& global : code().metadata().globals) {
    // Indirect reference globals get traced by the owning WebAssembly.Global.
    if (!global.type().isRefRepr() || global.isConstant() ||
        global.isIndirect()) {
      continue;
    }
    GCPtr<AnyRef>* obj = (GCPtr<AnyRef>*)(data() + global.offset());
    TraceNullableEdge(trc, obj, "wasm reference-typed global");
  }

  for (uint32_t tagIndex = 0; tagIndex < code().metadata().tags.length();
       tagIndex++) {
    TraceNullableEdge(trc, &tagInstanceData(tagIndex).object, "wasm tag");
  }

  const SharedTypeContext& types = metadata().types;
  for (uint32_t typeIndex = 0; typeIndex < types->length(); typeIndex++) {
    TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);
    TraceNullableEdge(trc, &typeDefData->shape, "wasm shape");
  }

  TraceNullableEdge(trc, &pendingException_, "wasm pending exception value");
  TraceNullableEdge(trc, &pendingExceptionTag_, "wasm pending exception tag");

  passiveElemSegments_.trace(trc);

  if (maybeDebug_) {
    maybeDebug_->trace(trc);
  }
}

void js::wasm::TraceInstanceEdge(JSTracer* trc, Instance* instance,
                                 const char* name) {
  if (IsTracerKind(trc, JS::TracerKind::Moving)) {
    // Compacting GC: The Instance does not move so there is nothing to do here.
    // Reading the object from the instance below would be a data race during
    // multi-threaded updates. Compacting GC does not rely on graph traversal
    // to find all edges that need to be updated.
    return;
  }

  // Instance fields are traced by the owning WasmInstanceObject's trace
  // hook. Tracing this ensures they are traced once.
  JSObject* object = instance->objectUnbarriered();
  TraceManuallyBarrieredEdge(trc, &object, name);
}

static uintptr_t* GetFrameScanStartForStackMap(
    const Frame* frame, const StackMap* map,
    uintptr_t* highestByteVisitedInPrevFrame) {
  // |frame| points somewhere in the middle of the area described by |map|.
  // We have to calculate |scanStart|, the lowest address that is described by
  // |map|, by consulting |map->frameOffsetFromTop|.

  const size_t numMappedBytes = map->header.numMappedWords * sizeof(void*);
  const uintptr_t scanStart = uintptr_t(frame) +
                              (map->header.frameOffsetFromTop * sizeof(void*)) -
                              numMappedBytes;
  MOZ_ASSERT(0 == scanStart % sizeof(void*));

  // Do what we can to assert that, for consecutive wasm frames, their stack
  // maps also abut exactly.  This is a useful sanity check on the sizing of
  // stackmaps.
  //
  // In debug builds, the stackmap construction machinery goes to considerable
  // efforts to ensure that the stackmaps for consecutive frames abut exactly.
  // This is so as to ensure there are no areas of stack inadvertently ignored
  // by a stackmap, nor covered by two stackmaps.  Hence any failure of this
  // assertion is serious and should be investigated.
#ifndef JS_CODEGEN_ARM64
  MOZ_ASSERT_IF(
      highestByteVisitedInPrevFrame && *highestByteVisitedInPrevFrame != 0,
      *highestByteVisitedInPrevFrame + 1 == scanStart);
#endif

  if (highestByteVisitedInPrevFrame) {
    *highestByteVisitedInPrevFrame = scanStart + numMappedBytes - 1;
  }

  // If we have some exit stub words, this means the map also covers an area
  // created by a exit stub, and so the highest word of that should be a
  // constant created by (code created by) GenerateTrapExit.
  MOZ_ASSERT_IF(map->header.numExitStubWords > 0,
                ((uintptr_t*)scanStart)[map->header.numExitStubWords - 1 -
                                        TrapExitDummyValueOffsetFromTop] ==
                    TrapExitDummyValue);

  return (uintptr_t*)scanStart;
}

uintptr_t Instance::traceFrame(JSTracer* trc, const wasm::WasmFrameIter& wfi,
                               uint8_t* nextPC,
                               uintptr_t highestByteVisitedInPrevFrame) {
  const StackMap* map = code().lookupStackMap(nextPC);
  if (!map) {
    return 0;
  }
  Frame* frame = wfi.frame();
  uintptr_t* stackWords =
      GetFrameScanStartForStackMap(frame, map, &highestByteVisitedInPrevFrame);

  // Hand refs off to the GC.
  for (uint32_t i = 0; i < map->header.numMappedWords; i++) {
    if (map->get(i) != StackMap::Kind::AnyRef) {
      continue;
    }

    TraceNullableRoot(trc, (AnyRef*)&stackWords[i],
                      "Instance::traceWasmFrame: normal word");
  }

  // Deal with any GC-managed fields in the DebugFrame, if it is
  // present and those fields may be live.
  if (map->header.hasDebugFrameWithLiveRefs) {
    DebugFrame* debugFrame = DebugFrame::from(frame);
    char* debugFrameP = (char*)debugFrame;

    for (size_t i = 0; i < MaxRegisterResults; i++) {
      if (debugFrame->hasSpilledRegisterRefResult(i)) {
        char* resultRefP = debugFrameP + DebugFrame::offsetOfRegisterResult(i);
        TraceNullableRoot(
            trc, (AnyRef*)resultRefP,
            "Instance::traceWasmFrame: DebugFrame::resultResults_");
      }
    }

    if (debugFrame->hasCachedReturnJSValue()) {
      char* cachedReturnJSValueP =
          debugFrameP + DebugFrame::offsetOfCachedReturnJSValue();
      TraceRoot(trc, (js::Value*)cachedReturnJSValueP,
                "Instance::traceWasmFrame: DebugFrame::cachedReturnJSValue_");
    }
  }

  return highestByteVisitedInPrevFrame;
}

void Instance::updateFrameForMovingGC(const wasm::WasmFrameIter& wfi,
                                      uint8_t* nextPC) {
  const StackMap* map = code().lookupStackMap(nextPC);
  if (!map) {
    return;
  }
  Frame* frame = wfi.frame();
  uintptr_t* stackWords = GetFrameScanStartForStackMap(frame, map, nullptr);

  // Update interior array data pointers for any inline-storage arrays that
  // moved.
  for (uint32_t i = 0; i < map->header.numMappedWords; i++) {
    if (map->get(i) != StackMap::Kind::ArrayDataPointer) {
      continue;
    }

    uint8_t** addressOfArrayDataPointer = (uint8_t**)&stackWords[i];
    if (WasmArrayObject::isDataInline(*addressOfArrayDataPointer)) {
      WasmArrayObject* oldArray =
          WasmArrayObject::fromInlineDataPointer(*addressOfArrayDataPointer);
      WasmArrayObject* newArray =
          (WasmArrayObject*)gc::MaybeForwarded(oldArray);
      *addressOfArrayDataPointer =
          WasmArrayObject::addressOfInlineData(newArray);
    }
  }
}

WasmMemoryObject* Instance::memory(uint32_t memoryIndex) const {
  return memoryInstanceData(memoryIndex).memory;
}

SharedMem<uint8_t*> Instance::memoryBase(uint32_t memoryIndex) const {
  MOZ_ASSERT_IF(
      memoryIndex == 0,
      memory0Base_ == memory(memoryIndex)->buffer().dataPointerEither());
  return memory(memoryIndex)->buffer().dataPointerEither();
}

SharedArrayRawBuffer* Instance::sharedMemoryBuffer(uint32_t memoryIndex) const {
  MOZ_ASSERT(memory(memoryIndex)->isShared());
  return memory(memoryIndex)->sharedArrayRawBuffer();
}

WasmInstanceObject* Instance::objectUnbarriered() const {
  return object_.unbarrieredGet();
}

WasmInstanceObject* Instance::object() const { return object_; }

static bool EnsureEntryStubs(const Instance& instance, uint32_t funcIndex,
                             const FuncExport** funcExport,
                             void** interpEntry) {
  Tier tier = instance.code().bestTier();

  size_t funcExportIndex;
  *funcExport =
      &instance.metadata(tier).lookupFuncExport(funcIndex, &funcExportIndex);

  const FuncExport& fe = **funcExport;
  if (fe.hasEagerStubs()) {
    *interpEntry = instance.codeBase(tier) + fe.eagerInterpEntryOffset();
    return true;
  }

  MOZ_ASSERT(!instance.isAsmJS(), "only wasm can lazily export functions");

  // If the best tier is Ion, life is simple: background compilation has
  // already completed and has been committed, so there's no risk of race
  // conditions here.
  //
  // If the best tier is Baseline, there could be a background compilation
  // happening at the same time. The background compilation will lock the
  // first tier lazy stubs first to stop new baseline stubs from being
  // generated, then the second tier stubs to generate them.
  //
  // - either we take the tier1 lazy stub lock before the background
  // compilation gets it, then we generate the lazy stub for tier1. When the
  // background thread gets the tier1 lazy stub lock, it will see it has a
  // lazy stub and will recompile it for tier2.
  // - or we don't take the lock here first. Background compilation won't
  // find a lazy stub for this function, thus won't generate it. So we'll do
  // it ourselves after taking the tier2 lock.
  //
  // Also see doc block for stubs in WasmJS.cpp.

  auto stubs = instance.code(tier).lazyStubs().writeLock();
  *interpEntry = stubs->lookupInterpEntry(fe.funcIndex());
  if (*interpEntry) {
    return true;
  }

  // The best tier might have changed after we've taken the lock.
  Tier prevTier = tier;
  tier = instance.code().bestTier();
  const Metadata& metadata = instance.metadata();
  const CodeTier& codeTier = instance.code(tier);
  if (tier == prevTier) {
    if (!stubs->createOneEntryStub(funcExportIndex, metadata, codeTier)) {
      return false;
    }

    *interpEntry = stubs->lookupInterpEntry(fe.funcIndex());
    MOZ_ASSERT(*interpEntry);
    return true;
  }

  MOZ_RELEASE_ASSERT(prevTier == Tier::Baseline && tier == Tier::Optimized);
  auto stubs2 = instance.code(tier).lazyStubs().writeLock();

  // If it didn't have a stub in the first tier, background compilation
  // shouldn't have made one in the second tier.
  MOZ_ASSERT(!stubs2->hasEntryStub(fe.funcIndex()));

  if (!stubs2->createOneEntryStub(funcExportIndex, metadata, codeTier)) {
    return false;
  }

  *interpEntry = stubs2->lookupInterpEntry(fe.funcIndex());
  MOZ_ASSERT(*interpEntry);
  return true;
}

static bool GetInterpEntryAndEnsureStubs(JSContext* cx, Instance& instance,
                                         uint32_t funcIndex,
                                         const CallArgs& args,
                                         void** interpEntry,
                                         const FuncType** funcType) {
  const FuncExport* funcExport;
  if (!EnsureEntryStubs(instance, funcIndex, &funcExport, interpEntry)) {
    return false;
  }

  *funcType = &instance.metadata().getFuncExportType(*funcExport);

#ifdef DEBUG
  // EnsureEntryStubs() has ensured proper jit-entry stubs have been created and
  // installed in funcIndex's JumpTable entry, so check against the presence of
  // the provisional lazy stub.  See also
  // WasmInstanceObject::getExportedFunction().
  if (!funcExport->hasEagerStubs() && (*funcType)->canHaveJitEntry()) {
    if (!EnsureBuiltinThunksInitialized()) {
      return false;
    }
    JSFunction& callee = args.callee().as<JSFunction>();
    void* provisionalLazyJitEntryStub = ProvisionalLazyJitEntryStub();
    MOZ_ASSERT(provisionalLazyJitEntryStub);
    MOZ_ASSERT(callee.isWasmWithJitEntry());
    MOZ_ASSERT(*callee.wasmJitEntry() != provisionalLazyJitEntryStub);
  }
#endif
  return true;
}

bool wasm::ResultsToJSValue(JSContext* cx, ResultType type,
                            void* registerResultLoc,
                            Maybe<char*> stackResultsLoc,
                            MutableHandleValue rval, CoercionLevel level) {
  if (type.empty()) {
    // No results: set to undefined, and we're done.
    rval.setUndefined();
    return true;
  }

  // If we added support for multiple register results, we'd need to establish a
  // convention for how to store them to memory in registerResultLoc.  For now
  // we can punt.
  static_assert(MaxRegisterResults == 1);

  // Stack results written to stackResultsLoc; register result written
  // to registerResultLoc.

  // First, convert the register return value, and prepare to iterate in
  // push order.  Note that if the register result is a reference type,
  // it may be unrooted, so ToJSValue_anyref must not GC in that case.
  ABIResultIter iter(type);
  DebugOnly<bool> usedRegisterResult = false;
  for (; !iter.done(); iter.next()) {
    if (iter.cur().inRegister()) {
      MOZ_ASSERT(!usedRegisterResult);
      if (!ToJSValue<DebugCodegenVal>(cx, registerResultLoc, iter.cur().type(),
                                      rval, level)) {
        return false;
      }
      usedRegisterResult = true;
    }
  }
  MOZ_ASSERT(usedRegisterResult);

  MOZ_ASSERT((stackResultsLoc.isSome()) == (iter.count() > 1));
  if (!stackResultsLoc) {
    // A single result: we're done.
    return true;
  }

  // Otherwise, collect results in an array, in push order.
  Rooted<ArrayObject*> array(cx, NewDenseEmptyArray(cx));
  if (!array) {
    return false;
  }
  RootedValue tmp(cx);
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    if (result.onStack()) {
      char* loc = stackResultsLoc.value() + result.stackOffset();
      if (!ToJSValue<DebugCodegenVal>(cx, loc, result.type(), &tmp, level)) {
        return false;
      }
      if (!NewbornArrayPush(cx, array, tmp)) {
        return false;
      }
    } else {
      if (!NewbornArrayPush(cx, array, rval)) {
        return false;
      }
    }
  }
  rval.set(ObjectValue(*array));
  return true;
}

class MOZ_RAII ReturnToJSResultCollector {
  class MOZ_RAII StackResultsRooter : public JS::CustomAutoRooter {
    ReturnToJSResultCollector& collector_;

   public:
    StackResultsRooter(JSContext* cx, ReturnToJSResultCollector& collector)
        : JS::CustomAutoRooter(cx), collector_(collector) {}

    void trace(JSTracer* trc) final {
      for (ABIResultIter iter(collector_.type_); !iter.done(); iter.next()) {
        const ABIResult& result = iter.cur();
        if (result.onStack() && result.type().isRefRepr()) {
          char* loc = collector_.stackResultsArea_.get() + result.stackOffset();
          AnyRef* refLoc = reinterpret_cast<AnyRef*>(loc);
          TraceNullableRoot(trc, refLoc, "StackResultsRooter::trace");
        }
      }
    }
  };
  friend class StackResultsRooter;

  ResultType type_;
  UniquePtr<char[], JS::FreePolicy> stackResultsArea_;
  Maybe<StackResultsRooter> rooter_;

 public:
  explicit ReturnToJSResultCollector(const ResultType& type) : type_(type){};
  bool init(JSContext* cx) {
    bool needRooter = false;
    ABIResultIter iter(type_);
    for (; !iter.done(); iter.next()) {
      const ABIResult& result = iter.cur();
      if (result.onStack() && result.type().isRefRepr()) {
        needRooter = true;
      }
    }
    uint32_t areaBytes = iter.stackBytesConsumedSoFar();
    MOZ_ASSERT_IF(needRooter, areaBytes > 0);
    if (areaBytes > 0) {
      // It is necessary to zero storage for ref results, and it doesn't
      // hurt to do so for other POD results.
      stackResultsArea_ = cx->make_zeroed_pod_array<char>(areaBytes);
      if (!stackResultsArea_) {
        return false;
      }
      if (needRooter) {
        rooter_.emplace(cx, *this);
      }
    }
    return true;
  }

  void* stackResultsArea() {
    MOZ_ASSERT(stackResultsArea_);
    return stackResultsArea_.get();
  }

  bool collect(JSContext* cx, void* registerResultLoc, MutableHandleValue rval,
               CoercionLevel level) {
    Maybe<char*> stackResultsLoc =
        stackResultsArea_ ? Some(stackResultsArea_.get()) : Nothing();
    return ResultsToJSValue(cx, type_, registerResultLoc, stackResultsLoc, rval,
                            level);
  }
};

bool Instance::callExport(JSContext* cx, uint32_t funcIndex,
                          const CallArgs& args, CoercionLevel level) {
  if (memory0Base_) {
    // If there has been a moving grow, this Instance should have been notified.
    MOZ_RELEASE_ASSERT(memoryBase(0).unwrap() == memory0Base_);
  }

  void* interpEntry;
  const FuncType* funcType;
  if (!GetInterpEntryAndEnsureStubs(cx, *this, funcIndex, args, &interpEntry,
                                    &funcType)) {
    return false;
  }

  // Lossless coercions can handle unexposable arguments or returns. This is
  // only available in testing code.
  if (level != CoercionLevel::Lossless && funcType->hasUnexposableArgOrRet()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }

  ArgTypeVector argTypes(*funcType);
  ResultType resultType(ResultType::Vector(funcType->results()));
  ReturnToJSResultCollector results(resultType);
  if (!results.init(cx)) {
    return false;
  }

  // The calling convention for an external call into wasm is to pass an
  // array of 16-byte values where each value contains either a coerced int32
  // (in the low word), or a double value (in the low dword) value, with the
  // coercions specified by the wasm signature. The external entry point
  // unpacks this array into the system-ABI-specified registers and stack
  // memory and then calls into the internal entry point. The return value is
  // stored in the first element of the array (which, therefore, must have
  // length >= 1).
  Vector<ExportArg, 8> exportArgs(cx);
  if (!exportArgs.resize(
          std::max<size_t>(1, argTypes.lengthWithStackResults()))) {
    return false;
  }

  Rooted<GCVector<AnyRef, 8, SystemAllocPolicy>> refs(cx);

  DebugCodegen(DebugChannel::Function, "wasm-function[%d] arguments [",
               funcIndex);
  RootedValue v(cx);
  for (size_t i = 0; i < argTypes.lengthWithStackResults(); ++i) {
    void* rawArgLoc = &exportArgs[i];
    if (argTypes.isSyntheticStackResultPointerArg(i)) {
      *reinterpret_cast<void**>(rawArgLoc) = results.stackResultsArea();
      continue;
    }
    size_t naturalIdx = argTypes.naturalIndex(i);
    v = naturalIdx < args.length() ? args[naturalIdx] : UndefinedValue();
    ValType type = funcType->arg(naturalIdx);
    if (!ToWebAssemblyValue<DebugCodegenVal>(cx, v, type, rawArgLoc, true,
                                             level)) {
      return false;
    }
    if (type.isRefRepr()) {
      void* ptr = *reinterpret_cast<void**>(rawArgLoc);
      // Store in rooted array until no more GC is possible.
      RootedAnyRef ref(cx, AnyRef::fromCompiledCode(ptr));
      if (!refs.emplaceBack(ref.get())) {
        return false;
      }
      DebugCodegen(DebugChannel::Function, "/(#%d)", int(refs.length() - 1));
    }
  }

  // Copy over reference values from the rooted array, if any.
  if (refs.length() > 0) {
    DebugCodegen(DebugChannel::Function, "; ");
    size_t nextRef = 0;
    for (size_t i = 0; i < argTypes.lengthWithStackResults(); ++i) {
      if (argTypes.isSyntheticStackResultPointerArg(i)) {
        continue;
      }
      size_t naturalIdx = argTypes.naturalIndex(i);
      ValType type = funcType->arg(naturalIdx);
      if (type.isRefRepr()) {
        AnyRef* rawArgLoc = (AnyRef*)&exportArgs[i];
        *rawArgLoc = refs[nextRef++];
        DebugCodegen(DebugChannel::Function, " ref(#%d) := %p ",
                     int(nextRef - 1), *(void**)rawArgLoc);
      }
    }
    refs.clear();
  }

  DebugCodegen(DebugChannel::Function, "]\n");

  // Ensure pending exception is cleared before and after (below) call.
  MOZ_ASSERT(pendingException_.isNull());

  {
    JitActivation activation(cx);

    // Call the per-exported-function trampoline created by GenerateEntry.
    auto funcPtr = JS_DATA_TO_FUNC_PTR(ExportFuncPtr, interpEntry);
    if (!CALL_GENERATED_2(funcPtr, exportArgs.begin(), this)) {
      return false;
    }
  }

  MOZ_ASSERT(pendingException_.isNull());

  if (isAsmJS() && args.isConstructing()) {
    // By spec, when a JS function is called as a constructor and this
    // function returns a primary type, which is the case for all asm.js
    // exported functions, the returned value is discarded and an empty
    // object is returned instead.
    PlainObject* obj = NewPlainObject(cx);
    if (!obj) {
      return false;
    }
    args.rval().set(ObjectValue(*obj));
    return true;
  }

  // Note that we're not rooting the register result, if any; we depend
  // on ResultsCollector::collect to root the value on our behalf,
  // before causing any GC.
  void* registerResultLoc = &exportArgs[0];
  DebugCodegen(DebugChannel::Function, "wasm-function[%d]; results [",
               funcIndex);
  if (!results.collect(cx, registerResultLoc, args.rval(), level)) {
    return false;
  }
  DebugCodegen(DebugChannel::Function, "]\n");

  return true;
}

void Instance::setPendingException(Handle<WasmExceptionObject*> exn) {
  pendingException_ = AnyRef::fromJSObject(*exn.get());
  pendingExceptionTag_ =
      AnyRef::fromJSObject(exn->as<WasmExceptionObject>().tag());
}

void Instance::constantGlobalGet(uint32_t globalIndex,
                                 MutableHandleVal result) {
  MOZ_RELEASE_ASSERT(globalIndex < maxInitializedGlobalsIndexPlus1_);
  const GlobalDesc& global = metadata().globals[globalIndex];

  // Constant globals are baked into the code and never stored in global data.
  if (global.isConstant()) {
    // We can just re-evaluate the global initializer to get the value.
    result.set(Val(global.constantValue()));
    return;
  }

  // Otherwise, we need to load the initialized value from its cell.
  const void* cell = addressOfGlobalCell(global);
  result.address()->initFromHeapLocation(global.type(), cell);
}

bool Instance::constantRefFunc(uint32_t funcIndex,
                               MutableHandleFuncRef result) {
  void* fnref = Instance::refFunc(this, funcIndex);
  if (fnref == AnyRef::invalid().forCompiledCode()) {
    return false;  // OOM, which has already been reported.
  }
  result.set(FuncRef::fromCompiledCode(fnref));
  return true;
}

WasmStructObject* Instance::constantStructNewDefault(JSContext* cx,
                                                     uint32_t typeIndex) {
  // We assume that constant structs will have a long lifetime and hence
  // allocate them directly in the tenured heap.  Also, we have to dynamically
  // decide whether an OOL storage area is required.  This is slow(er); do not
  // call here from generated code.
  TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);
  const wasm::TypeDef* typeDef = typeDefData->typeDef;
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Struct);
  uint32_t totalBytes = typeDef->structType().size_;

  bool needsOOL = WasmStructObject::requiresOutlineBytes(totalBytes);
  return needsOOL ? WasmStructObject::createStructOOL<true>(cx, typeDefData,
                                                            gc::Heap::Tenured)
                  : WasmStructObject::createStructIL<true>(cx, typeDefData,
                                                           gc::Heap::Tenured);
}

WasmArrayObject* Instance::constantArrayNewDefault(JSContext* cx,
                                                   uint32_t typeIndex,
                                                   uint32_t numElements) {
  TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);
  // We assume that constant arrays will have a long lifetime and hence
  // allocate them directly in the tenured heap.
  return WasmArrayObject::createArray<true>(cx, typeDefData, gc::Heap::Tenured,
                                            numElements);
}

JSAtom* Instance::getFuncDisplayAtom(JSContext* cx, uint32_t funcIndex) const {
  // The "display name" of a function is primarily shown in Error.stack which
  // also includes location, so use getFuncNameBeforeLocation.
  UTF8Bytes name;
  if (!metadata().getFuncNameBeforeLocation(funcIndex, &name)) {
    return nullptr;
  }

  return AtomizeUTF8Chars(cx, name.begin(), name.length());
}

void Instance::ensureProfilingLabels(bool profilingEnabled) const {
  return code_->ensureProfilingLabels(profilingEnabled);
}

void Instance::onMovingGrowMemory(const WasmMemoryObject* memory) {
  MOZ_ASSERT(!isAsmJS());
  MOZ_ASSERT(!memory->isShared());

  for (uint32_t i = 0; i < metadata().memories.length(); i++) {
    MemoryInstanceData& md = memoryInstanceData(i);
    if (memory != md.memory) {
      continue;
    }
    ArrayBufferObject& buffer = md.memory->buffer().as<ArrayBufferObject>();

    md.base = buffer.dataPointer();
    size_t limit = md.memory->boundsCheckLimit();
#if !defined(JS_64BIT)
    // We assume that the limit is a 32-bit quantity
    MOZ_ASSERT(limit <= UINT32_MAX);
#endif
    md.boundsCheckLimit = limit;

    if (i == 0) {
      memory0Base_ = md.base;
      memory0BoundsCheckLimit_ = md.boundsCheckLimit;
    }
  }
}

void Instance::onMovingGrowTable(const Table* table) {
  MOZ_ASSERT(!isAsmJS());

  // `table` has grown and we must update cached data for it.  Importantly,
  // we can have cached those data in more than one location: we'll have
  // cached them once for each time the table was imported into this instance.
  //
  // When an instance is registered as an observer of a table it is only
  // registered once, regardless of how many times the table was imported.
  // Thus when a table is grown, onMovingGrowTable() is only invoked once for
  // the table.
  //
  // Ergo we must go through the entire list of tables in the instance here
  // and check for the table in all the cached-data slots; we can't exit after
  // the first hit.

  for (uint32_t i = 0; i < tables_.length(); i++) {
    if (tables_[i] != table) {
      continue;
    }
    TableInstanceData& table = tableInstanceData(i);
    table.length = tables_[i]->length();
    table.elements = tables_[i]->instanceElements();
  }
}

JSString* Instance::createDisplayURL(JSContext* cx) {
  // In the best case, we simply have a URL, from a streaming compilation of a
  // fetched Response.

  if (metadata().filenameIsURL) {
    const char* filename = metadata().filename.get();
    return NewStringCopyUTF8N(cx, JS::UTF8Chars(filename, strlen(filename)));
  }

  // Otherwise, build wasm module URL from following parts:
  // - "wasm:" as protocol;
  // - URI encoded filename from metadata (if can be encoded), plus ":";
  // - 64-bit hash of the module bytes (as hex dump).

  JSStringBuilder result(cx);
  if (!result.append("wasm:")) {
    return nullptr;
  }

  if (const char* filename = metadata().filename.get()) {
    // EncodeURI returns false due to invalid chars or OOM -- fail only
    // during OOM.
    JSString* filenamePrefix = EncodeURI(cx, filename, strlen(filename));
    if (!filenamePrefix) {
      if (cx->isThrowingOutOfMemory()) {
        return nullptr;
      }

      MOZ_ASSERT(!cx->isThrowingOverRecursed());
      cx->clearPendingException();
      return nullptr;
    }

    if (!result.append(filenamePrefix)) {
      return nullptr;
    }
  }

  if (metadata().debugEnabled) {
    if (!result.append(":")) {
      return nullptr;
    }

    const ModuleHash& hash = metadata().debugHash;
    for (unsigned char byte : hash) {
      unsigned char digit1 = byte / 16, digit2 = byte % 16;
      if (!result.append(
              (char)(digit1 < 10 ? digit1 + '0' : digit1 + 'a' - 10))) {
        return nullptr;
      }
      if (!result.append(
              (char)(digit2 < 10 ? digit2 + '0' : digit2 + 'a' - 10))) {
        return nullptr;
      }
    }
  }

  return result.finishString();
}

WasmBreakpointSite* Instance::getOrCreateBreakpointSite(JSContext* cx,
                                                        uint32_t offset) {
  MOZ_ASSERT(debugEnabled());
  return debug().getOrCreateBreakpointSite(cx, this, offset);
}

void Instance::destroyBreakpointSite(JS::GCContext* gcx, uint32_t offset) {
  MOZ_ASSERT(debugEnabled());
  return debug().destroyBreakpointSite(gcx, this, offset);
}

void Instance::disassembleExport(JSContext* cx, uint32_t funcIndex, Tier tier,
                                 PrintCallback printString) const {
  const MetadataTier& metadataTier = metadata(tier);
  const FuncExport& funcExport = metadataTier.lookupFuncExport(funcIndex);
  const CodeRange& range = metadataTier.codeRange(funcExport);
  const CodeTier& codeTier = code(tier);
  const ModuleSegment& segment = codeTier.segment();

  MOZ_ASSERT(range.begin() < segment.length());
  MOZ_ASSERT(range.end() < segment.length());

  uint8_t* functionCode = segment.base() + range.begin();
  jit::Disassemble(functionCode, range.end() - range.begin(), printString);
}

void Instance::addSizeOfMisc(MallocSizeOf mallocSizeOf,
                             Metadata::SeenSet* seenMetadata,
                             Code::SeenSet* seenCode,
                             Table::SeenSet* seenTables, size_t* code,
                             size_t* data) const {
  *data += mallocSizeOf(this);
  for (const SharedTable& table : tables_) {
    *data += table->sizeOfIncludingThisIfNotSeen(mallocSizeOf, seenTables);
  }

  if (maybeDebug_) {
    maybeDebug_->addSizeOfMisc(mallocSizeOf, seenMetadata, seenCode, code,
                               data);
  }

  code_->addSizeOfMiscIfNotSeen(mallocSizeOf, seenMetadata, seenCode, code,
                                data);
}

//////////////////////////////////////////////////////////////////////////////
//
// Reporting of errors that are traps.

void wasm::ReportTrapError(JSContext* cx, unsigned errorNumber) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber);

  if (cx->isThrowingOutOfMemory()) {
    return;
  }

  // Mark the exception as thrown from a trap to prevent if from being handled
  // by wasm exception handlers.
  RootedValue exn(cx);
  if (!cx->getPendingException(&exn)) {
    return;
  }

  MOZ_ASSERT(exn.isObject() && exn.toObject().is<ErrorObject>());
  exn.toObject().as<ErrorObject>().setFromWasmTrap();
}

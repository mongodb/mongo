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

#include "wasm/WasmPI.h"

#include "builtin/Promise.h"
#include "jit/MIRGenerator.h"
#include "js/CallAndConstruct.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/PromiseObject.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmContext.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmValidate.h"

#include "vm/JSObject-inl.h"
#include "wasm/WasmGcObject-inl.h"
#include "wasm/WasmInstance-inl.h"

#ifdef XP_WIN
#  include "util/WindowsWrapper.h"
#endif

using namespace js;
using namespace js::jit;

#ifdef ENABLE_WASM_JSPI
namespace js::wasm {

SuspenderObjectData::SuspenderObjectData(void* stackMemory)
    : stackMemory_(stackMemory),
      suspendableFP_(nullptr),
      suspendableSP_(static_cast<uint8_t*>(stackMemory) +
                     SuspendableStackPlusRedZoneSize),
      state_(SuspenderState::Initial) {}

void SuspenderObjectData::releaseStackMemory() {
  js_free(stackMemory_);
  stackMemory_ = nullptr;
}

#  ifdef _WIN64
// On WIN64, the Thread Information Block stack limits has to be modified to
// avoid failures on SP checks.
void SuspenderObjectData::updateTIBStackFields() {
  _NT_TIB* tib = reinterpret_cast<_NT_TIB*>(::NtCurrentTeb());
  savedStackBase_ = tib->StackBase;
  savedStackLimit_ = tib->StackLimit;
  uintptr_t stack_limit = (uintptr_t)stackMemory_ + SuspendableRedZoneSize;
  uintptr_t stack_base = stack_limit + SuspendableStackSize;
  tib->StackBase = (void*)stack_base;
  tib->StackLimit = (void*)stack_limit;
}

void SuspenderObjectData::restoreTIBStackFields() {
  _NT_TIB* tib = reinterpret_cast<_NT_TIB*>(::NtCurrentTeb());
  tib->StackBase = savedStackBase_;
  tib->StackLimit = savedStackLimit_;
}
#  endif

// Slots that used in various JSFunctionExtended below.
const size_t SUSPENDER_SLOT = 0;
const size_t WRAPPED_FN_SLOT = 1;
const size_t CONTINUE_ON_SUSPENDABLE_SLOT = 1;

SuspenderContext::SuspenderContext()
    : activeSuspender_(nullptr), suspendedStacks_() {}

SuspenderContext::~SuspenderContext() {
  MOZ_ASSERT(activeSuspender_ == nullptr);
  MOZ_ASSERT(suspendedStacks_.isEmpty());
}

SuspenderObject* SuspenderContext::activeSuspender() {
  return activeSuspender_;
}

void SuspenderContext::setActiveSuspender(SuspenderObject* obj) {
  activeSuspender_.set(obj);
}

void SuspenderContext::trace(JSTracer* trc) {
  if (activeSuspender_) {
    TraceEdge(trc, &activeSuspender_, "suspender");
  }
}

void SuspenderContext::traceRoots(JSTracer* trc) {
  for (const SuspenderObjectData& data : suspendedStacks_) {
    void* startFP = data.suspendableFP();
    void* returnAddress = data.suspendedReturnAddress();
    void* exitFP = data.suspendableExitFP();
    MOZ_ASSERT(startFP != exitFP);

    WasmFrameIter iter(static_cast<FrameWithInstances*>(startFP),
                       returnAddress);
    MOZ_ASSERT(iter.stackSwitched());
    uintptr_t highestByteVisitedInPrevWasmFrame = 0;
    while (true) {
      MOZ_ASSERT(!iter.done());
      uint8_t* nextPC = iter.resumePCinCurrentFrame();
      Instance* instance = iter.instance();
      TraceInstanceEdge(trc, instance, "WasmFrameIter instance");
      highestByteVisitedInPrevWasmFrame = instance->traceFrame(
          trc, iter, nextPC, highestByteVisitedInPrevWasmFrame);
      if (iter.frame() == exitFP) {
        break;
      }
      ++iter;
      if (iter.stackSwitched()) {
        highestByteVisitedInPrevWasmFrame = 0;
      }
    }
  }
}

static_assert(JS_STACK_GROWTH_DIRECTION < 0,
              "JS-PI implemented only for native stacks that grows towards 0");

static void DecrementSuspendableStacksCount(JSContext* cx) {
  for (;;) {
    uint32_t currentCount = cx->wasm().suspendableStacksCount;
    MOZ_ASSERT(currentCount > 0);
    if (cx->wasm().suspendableStacksCount.compareExchange(currentCount,
                                                          currentCount - 1)) {
      break;
    }
    // Failed to decrement suspendableStacksCount, repeat.
  }
}

class SuspenderObject : public NativeObject {
 public:
  static const JSClass class_;

  enum { DataSlot, PromisingPromiseSlot, SuspendingPromiseSlot, SlotCount };

  static SuspenderObject* create(JSContext* cx) {
    for (;;) {
      uint32_t currentCount = cx->wasm().suspendableStacksCount;
      if (currentCount >= SuspendableStacksMaxCount) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_JSPI_SUSPENDER_LIMIT);
        return nullptr;
      }
      if (cx->wasm().suspendableStacksCount.compareExchange(currentCount,
                                                            currentCount + 1)) {
        break;
      }
      // Failed to increment suspendableStacksCount, repeat.
    }

    Rooted<SuspenderObject*> suspender(
        cx, NewBuiltinClassInstance<SuspenderObject>(cx));
    if (!suspender) {
      DecrementSuspendableStacksCount(cx);
      return nullptr;
    }

    void* stackMemory = js_malloc(SuspendableStackPlusRedZoneSize);
    if (!stackMemory) {
      DecrementSuspendableStacksCount(cx);
      ReportOutOfMemory(cx);
      return nullptr;
    }

    SuspenderObjectData* data = js_new<SuspenderObjectData>(stackMemory);
    if (!data) {
      js_free(stackMemory);
      DecrementSuspendableStacksCount(cx);
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_RELEASE_ASSERT(data->state() != SuspenderState::Moribund);

    suspender->initReservedSlot(DataSlot, PrivateValue(data));
    suspender->initReservedSlot(PromisingPromiseSlot, NullValue());
    suspender->initReservedSlot(SuspendingPromiseSlot, NullValue());
    return suspender;
  }

  PromiseObject* promisingPromise() const {
    return &getReservedSlot(PromisingPromiseSlot)
                .toObject()
                .as<PromiseObject>();
  }

  void setPromisingPromise(Handle<PromiseObject*> promise) {
    setReservedSlot(PromisingPromiseSlot, ObjectOrNullValue(promise));
  }

  PromiseObject* suspendingPromise() const {
    return &getReservedSlot(SuspendingPromiseSlot)
                .toObject()
                .as<PromiseObject>();
  }

  void setSuspendingPromise(Handle<PromiseObject*> promise) {
    setReservedSlot(SuspendingPromiseSlot, ObjectOrNullValue(promise));
  }

  JS::NativeStackLimit getStackMemoryLimit() {
    return JS::NativeStackLimit(data()->stackMemory()) + SuspendableRedZoneSize;
  }

  SuspenderState state() { return data()->state(); }

  inline bool hasData() { return !getReservedSlot(DataSlot).isUndefined(); }

  inline SuspenderObjectData* data() {
    return static_cast<SuspenderObjectData*>(
        getReservedSlot(DataSlot).toPrivate());
  }

  void setMoribund(JSContext* cx);
  void setActive(JSContext* cx);
  void setSuspended(JSContext* cx);

  void enter(JSContext* cx);
  void suspend(JSContext* cx);
  void resume(JSContext* cx);
  void leave(JSContext* cx);

 private:
  static const JSClassOps classOps_;

  static void finalize(JS::GCContext* gcx, JSObject* obj);
};

static_assert(SuspenderObjectDataSlot == SuspenderObject::DataSlot);

const JSClass SuspenderObject::class_ = {
    "SuspenderObject",
    JSCLASS_HAS_RESERVED_SLOTS(SlotCount) | JSCLASS_BACKGROUND_FINALIZE,
    &SuspenderObject::classOps_};

const JSClassOps SuspenderObject::classOps_ = {
    nullptr,   // addProperty
    nullptr,   // delProperty
    nullptr,   // enumerate
    nullptr,   // newEnumerate
    nullptr,   // resolve
    nullptr,   // mayResolve
    finalize,  // finalize
    nullptr,   // call
    nullptr,   // construct
    nullptr,   // trace
};

/* static */
void SuspenderObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  SuspenderObject& suspender = obj->as<SuspenderObject>();
  if (!suspender.hasData()) {
    return;
  }
  SuspenderObjectData* data = suspender.data();
  MOZ_RELEASE_ASSERT(data->state() == SuspenderState::Moribund);
  MOZ_RELEASE_ASSERT(!data->stackMemory());
  js_free(data);
}

void SuspenderObject::setMoribund(JSContext* cx) {
  MOZ_ASSERT(state() == SuspenderState::Active);
  ResetInstanceStackLimits(cx);
#  ifdef _WIN64
  data()->restoreTIBStackFields();
#  endif
  SuspenderObjectData* data = this->data();
  data->setState(SuspenderState::Moribund);
  data->releaseStackMemory();
  DecrementSuspendableStacksCount(cx);
  MOZ_ASSERT(
      !cx->wasm().promiseIntegration.suspendedStacks_.ElementProbablyInList(
          data));
}

void SuspenderObject::setActive(JSContext* cx) {
  data()->setState(SuspenderState::Active);
  UpdateInstanceStackLimitsForSuspendableStack(cx, getStackMemoryLimit());
#  ifdef _WIN64
  data()->updateTIBStackFields();
#  endif
}

void SuspenderObject::setSuspended(JSContext* cx) {
  data()->setState(SuspenderState::Suspended);
  ResetInstanceStackLimits(cx);
#  ifdef _WIN64
  data()->restoreTIBStackFields();
#  endif
}

void SuspenderObject::enter(JSContext* cx) {
  MOZ_ASSERT(state() == SuspenderState::Initial);
  cx->wasm().promiseIntegration.setActiveSuspender(this);
  setActive(cx);
}

void SuspenderObject::suspend(JSContext* cx) {
  MOZ_ASSERT(state() == SuspenderState::Active);
  setSuspended(cx);
  cx->wasm().promiseIntegration.suspendedStacks_.pushFront(data());
  cx->wasm().promiseIntegration.setActiveSuspender(nullptr);
}

void SuspenderObject::resume(JSContext* cx) {
  MOZ_ASSERT(state() == SuspenderState::Suspended);
  cx->wasm().promiseIntegration.setActiveSuspender(this);
  setActive(cx);
  cx->wasm().promiseIntegration.suspendedStacks_.remove(data());
}

void SuspenderObject::leave(JSContext* cx) {
  cx->wasm().promiseIntegration.setActiveSuspender(nullptr);
  // We are exiting alternative stack if state is active,
  // otherwise the stack was just suspended.
  switch (state()) {
    case SuspenderState::Active:
      setMoribund(cx);
      break;
    case SuspenderState::Suspended:
      break;
    case SuspenderState::Initial:
    case SuspenderState::Moribund:
      MOZ_CRASH();
  }
}

bool ParseSuspendingPromisingString(JSContext* cx, HandleValue val,
                                    SuspenderArgPosition& result) {
  if (val.isNullOrUndefined()) {
    result = SuspenderArgPosition::None;
    return true;
  }

  RootedString str(cx, ToString(cx, val));
  if (!str) {
    return false;
  }
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  if (StringEqualsLiteral(linear, "first")) {
    result = SuspenderArgPosition::First;
  } else if (StringEqualsLiteral(linear, "last")) {
    result = SuspenderArgPosition::Last;
  } else {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_JSPI_ARG_POSITION);
    return false;
  }
  return true;
}

using CallImportData = Instance::WasmJSPICallImportData;

/*static*/
bool CallImportData::Call(CallImportData* data) {
  Instance* instance = data->instance;
  JSContext* cx = instance->cx();
  return instance->callImport(cx, data->funcImportIndex, data->argc,
                              data->argv);
}

bool CallImportOnMainThread(JSContext* cx, Instance* instance,
                            int32_t funcImportIndex, int32_t argc,
                            uint64_t* argv) {
  CallImportData data = {instance, funcImportIndex, argc, argv};
  Rooted<SuspenderObject*> suspender(
      cx, cx->wasm().promiseIntegration.activeSuspender());
  SuspenderObjectData* stacks = suspender->data();

  cx->wasm().promiseIntegration.setActiveSuspender(nullptr);

  MOZ_ASSERT(suspender->state() == SuspenderState::Active);
  suspender->setSuspended(cx);

  // The platform specific code below inserts offsets as strings into inline
  // assembly. CHECK_OFFSETS verifies the specified literals in macros below.
#  define CHECK_OFFSETS(MAIN_FP_OFFSET, MAIN_SP_OFFSET, SUSPENDABLE_FP_OFFSET, \
                        SUSPENDABLE_SP_OFFSET)                                 \
    static_assert((MAIN_FP_OFFSET) == SuspenderObjectData::offsetOfMainFP() && \
                  (MAIN_SP_OFFSET) == SuspenderObjectData::offsetOfMainSP() && \
                  (SUSPENDABLE_FP_OFFSET) ==                                   \
                      SuspenderObjectData::offsetOfSuspendableFP() &&          \
                  (SUSPENDABLE_SP_OFFSET) ==                                   \
                      SuspenderObjectData::offsetOfSuspendableSP());

  // The following assembly code temporarily switches FP/SP pointers to be on
  // main stack, while maintaining frames linking.  After
  // `CallImportData::Call` execution suspendable stack FP/SP will be restored.
  //
  // Because the assembly sequences contain a call, the trashed-register list
  // must contain all the caller saved registers.  They must also contain "cc"
  // and "memory" since both of those state elements could be modified by the
  // call.  They also need a "volatile" qualifier to ensure that the they don't
  // get optimised out or otherwise messed with by clang/gcc.
  //
  // `Registers::VolatileMask` (in the assembler complex) is useful in that it
  // lists the caller-saved registers.

  uintptr_t res;

  // clang-format off
#if defined(_M_ARM64) || defined(__aarch64__)
#    define CALLER_SAVED_REGS \
      "x0", "x1", "x2", "x3","x4", "x5", "x6", "x7", "x8", "x9", "x10",   \
      "x11", "x12", "x13", "x14", "x15", "x16", "x17", /* "x18", */       \
      "x19", "x20", /* it's unclear who saves these two, so be safe */    \
      /* claim that all the vector regs are caller-saved, for safety */   \
      "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10",  \
      "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19",      \
      "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28",      \
      "v29", "v30", "v31"
#    define INLINED_ASM(MAIN_FP, MAIN_SP, SUSPENDABLE_FP, SUSPENDABLE_SP) \
      CHECK_OFFSETS(MAIN_FP, MAIN_SP, SUSPENDABLE_FP, SUSPENDABLE_SP);    \
      asm volatile(                                                       \
          "\n   mov     x0, %1"                                           \
          "\n   mov     x27, sp "                                         \
          "\n   str     x29, [x0, #" #SUSPENDABLE_FP "] "                 \
          "\n   str     x27, [x0, #" #SUSPENDABLE_SP "] "                 \
                                                                          \
          "\n   ldr     x29, [x0, #" #MAIN_FP "] "                        \
          "\n   ldr     x27, [x0, #" #MAIN_SP "] "                        \
          "\n   mov     sp, x27 "                                         \
                                                                          \
          "\n   stp     x0, x27, [sp, #-16]! "                            \
                                                                          \
          "\n   mov     x0, %3"                                           \
          "\n   blr     %2 "                                              \
                                                                          \
          "\n   ldp     x3, x27, [sp], #16 "                              \
                                                                          \
          "\n   mov     x27, sp "                                         \
          "\n   str     x29, [x3, #" #MAIN_FP "] "                        \
          "\n   str     x27, [x3, #" #MAIN_SP "] "                        \
                                                                          \
          "\n   ldr     x29, [x3, #" #SUSPENDABLE_FP "] "                 \
          "\n   ldr     x27, [x3, #" #SUSPENDABLE_SP "] "                 \
          "\n   mov     sp, x27 "                                         \
          "\n   mov     %0, x0"                                           \
          : "=r"(res)                                                     \
          : "r"(stacks), "r"(CallImportData::Call), "r"(&data)            \
          : "x0", "x3", "x27", CALLER_SAVED_REGS, "cc", "memory")
  INLINED_ASM(24, 32, 40, 48);

#  elif defined(_WIN64) && defined(_M_X64)
#    define INLINED_ASM(MAIN_FP, MAIN_SP, SUSPENDABLE_FP, SUSPENDABLE_SP) \
      CHECK_OFFSETS(MAIN_FP, MAIN_SP, SUSPENDABLE_FP, SUSPENDABLE_SP);    \
      asm("\n   mov     %1, %%rcx"                                        \
          "\n   mov     %%rbp, " #SUSPENDABLE_FP "(%%rcx)"                \
          "\n   mov     %%rsp, " #SUSPENDABLE_SP "(%%rcx)"                \
                                                                          \
          "\n   mov     " #MAIN_FP "(%%rcx), %%rbp"                       \
          "\n   mov     " #MAIN_SP "(%%rcx), %%rsp"                       \
                                                                          \
          "\n   push    %%rcx"                                            \
          "\n   push    %%rdx"                                            \
                                                                          \
          "\n   mov     %3, %%rcx"                                        \
          "\n   call    *%2"                                              \
                                                                          \
          "\n   pop    %%rdx"                                             \
          "\n   pop    %%rcx"                                             \
                                                                          \
          "\n   mov     %%rbp, " #MAIN_FP "(%%rcx)"                       \
          "\n   mov     %%rsp, " #MAIN_SP "(%%rcx)"                       \
                                                                          \
          "\n   mov     " #SUSPENDABLE_FP "(%%rcx), %%rbp"                \
          "\n   mov     " #SUSPENDABLE_SP "(%%rcx), %%rsp"                \
                                                                          \
          "\n   movq     %%rax, %0"                                       \
          : "=r"(res)                                                     \
          : "r"(stacks), "r"(CallImportData::Call), "r"(&data)         \
          : "rcx", "rax")
  INLINED_ASM(24, 32, 40, 48);

#  elif defined(__x86_64__)
#    define INLINED_ASM(MAIN_FP, MAIN_SP, SUSPENDABLE_FP, SUSPENDABLE_SP) \
      CHECK_OFFSETS(MAIN_FP, MAIN_SP, SUSPENDABLE_FP, SUSPENDABLE_SP);    \
      asm("\n   mov     %1, %%rdi"                                        \
          "\n   mov     %%rbp, " #SUSPENDABLE_FP "(%%rdi)"                \
          "\n   mov     %%rsp, " #SUSPENDABLE_SP "(%%rdi)"                \
                                                                          \
          "\n   mov     " #MAIN_FP "(%%rdi), %%rbp"                       \
          "\n   mov     " #MAIN_SP "(%%rdi), %%rsp"                       \
                                                                          \
          "\n   push    %%rdi"                                            \
          "\n   push    %%rdx"                                            \
                                                                          \
          "\n   mov     %3, %%rdi"                                        \
          "\n   call    *%2"                                              \
                                                                          \
          "\n   pop    %%rdx"                                             \
          "\n   pop    %%rdi"                                             \
                                                                          \
          "\n   mov     %%rbp, " #MAIN_FP "(%%rdi)"                       \
          "\n   mov     %%rsp, " #MAIN_SP "(%%rdi)"                       \
                                                                          \
          "\n   mov     " #SUSPENDABLE_FP "(%%rdi), %%rbp"                \
          "\n   mov     " #SUSPENDABLE_SP "(%%rdi), %%rsp"                \
                                                                          \
          "\n   movq     %%rax, %0"                                       \
          : "=r"(res)                                                     \
          : "r"(stacks), "r"(CallImportData::Call), "r"(&data)         \
          : "rdi", "rax")
  INLINED_ASM(24, 32, 40, 48);

#  else
  MOZ_CRASH("Not supported for this platform");
#  endif
  // clang-format on

  bool ok = res;
  suspender->setActive(cx);
  cx->wasm().promiseIntegration.setActiveSuspender(suspender);

#  undef INLINED_ASM
#  undef CHECK_OFFSETS
#  undef CALLER_SAVED_REGS

  return ok;
}

static void CleanupActiveSuspender(JSContext* cx) {
  SuspenderObject* suspender = cx->wasm().promiseIntegration.activeSuspender();
  MOZ_ASSERT(suspender);
  cx->wasm().promiseIntegration.setActiveSuspender(nullptr);
  suspender->setMoribund(cx);
}

// Suspending

// Builds a wasm module with following structure:
// (module
//   (type $params (struct (field ..)*)))
//   (type $results (struct (field ..)*)))
//   (import "" "" (func $suspending.wrappedfn ..))
//   (import "" "" (func $suspending.add-promise-reactions ..))
//   (func $suspending.exported .. )
//   (func $suspending.trampoline ..)
//   (func $suspending.continue-on-suspendable ..)
//   (export "" (func $suspending.exported))
// )
//
// The module provides logic for the state transitions (see the SMDOC):
//  - Invoke Suspending Import via $suspending.exported
//  - Suspending Function Returns a Promise via $suspending.trampoline
//  - Promise Resolved transitions via $suspending.continue-on-suspendable
//
class SuspendingFunctionModuleFactory {
 public:
  enum TypeIdx {
    ParamsTypeIndex,
    ResultsTypeIndex,
  };

  enum FnIdx {
    WrappedFnIndex,
    GetSuspendingResultsFnIndex,
    ExportedFnIndex,
    TrampolineFnIndex,
    ContinueOnSuspendableFnIndex
  };

 private:
  // Builds function that will be imported to wasm module:
  // (func $suspending.exported
  //   (param $suspender externref)? (param ..)* (param $suspender externref)?
  //   (result ..)*
  //   (local $suspender externref)?
  //   (local $results (ref $results))
  //   ;; #if checkSuspender
  //   local.get $suspender
  //   call $builtin.check-suspender
  //   ;; #else
  //   call $builtin.current-suspender
  //   local.tee $suspender
  //   ;; #endif
  //   ref.func $suspending.trampoline
  //   local.get $i*
  //   stuct.new $param-type
  //   stack-switch SwitchToMain ;; <- (suspender,fn,data)
  //   local.get $suspender
  //   call $builtin.get-suspending-promise-result
  //   ref.cast $results-type
  //   local.set $results
  //   (struct.get $results (local.get $results))*
  // )
  bool encodeExportedFunction(ModuleEnvironment& moduleEnv, uint32_t paramsSize,
                              uint32_t resultSize, uint32_t paramsOffset,
                              uint32_t suspenderIndex, bool checkSuspender,
                              RefType resultType, Bytes& bytecode) {
    Encoder encoder(bytecode, *moduleEnv.types);
    ValTypeVector locals;
    if (!checkSuspender && !locals.emplaceBack(RefType::extern_())) {
      return false;
    }
    if (!locals.emplaceBack(resultType)) {
      return false;
    }
    if (!EncodeLocalEntries(encoder, locals)) {
      return false;
    }

    if (checkSuspender) {
      if (!encoder.writeOp(Op::LocalGet) ||
          !encoder.writeVarU32(suspenderIndex)) {
        return false;
      }
      if (!encoder.writeOp(MozOp::CallBuiltinModuleFunc) ||
          !encoder.writeVarU32((uint32_t)BuiltinModuleFuncId::CheckSuspender)) {
        return false;
      }
    } else {
      if (!encoder.writeOp(Op::I32Const) || !encoder.writeVarU32(0)) {
        return false;
      }
      if (!encoder.writeOp(MozOp::CallBuiltinModuleFunc) ||
          !encoder.writeVarU32(
              (uint32_t)BuiltinModuleFuncId::CurrentSuspender)) {
        return false;
      }
      if (!encoder.writeOp(Op::LocalTee) ||
          !encoder.writeVarU32(suspenderIndex)) {
        return false;
      }
    }

    // Results local is located after all params and suspender.
    // Adding 1 to paramSize for checkSuspender and ! cases.
    int resultsIndex = paramsSize + 1;

    if (!encoder.writeOp(Op::RefFunc) ||
        !encoder.writeVarU32(TrampolineFnIndex)) {
      return false;
    }
    for (uint32_t i = 0; i < paramsSize; i++) {
      if (!encoder.writeOp(Op::LocalGet) ||
          !encoder.writeVarU32(i + paramsOffset)) {
        return false;
      }
    }
    if (!encoder.writeOp(GcOp::StructNew) ||
        !encoder.writeVarU32(ParamsTypeIndex)) {
      return false;
    }

    if (!encoder.writeOp(MozOp::StackSwitch) ||
        !encoder.writeVarU32(uint32_t(StackSwitchKind::SwitchToMain))) {
      return false;
    }

    if (!encoder.writeOp(Op::LocalGet) ||
        !encoder.writeVarU32(suspenderIndex)) {
      return false;
    }
    if (!encoder.writeOp(MozOp::CallBuiltinModuleFunc) ||
        !encoder.writeVarU32(
            (uint32_t)BuiltinModuleFuncId::GetSuspendingPromiseResult)) {
      return false;
    }
    if (!encoder.writeOp(GcOp::RefCast) ||
        !encoder.writeVarU32(ResultsTypeIndex) ||
        !encoder.writeOp(Op::LocalSet) || !encoder.writeVarU32(resultsIndex)) {
      return false;
    }
    for (uint32_t i = 0; i < resultSize; i++) {
      if (!encoder.writeOp(Op::LocalGet) ||
          !encoder.writeVarU32(resultsIndex) ||
          !encoder.writeOp(GcOp::StructGet) ||
          !encoder.writeVarU32(ResultsTypeIndex) || !encoder.writeVarU32(i)) {
        return false;
      }
    }
    return encoder.writeOp(Op::End);
  }

  // Builds function that is called on main stack:
  // (func $suspending.trampoline
  //   (param $params (ref $suspender) (ref $param-type))
  //   (result externref)
  //   local.get $suspender ;; for call $process-promise
  //   (struct.get $param-type $i (local.get $param))*
  //   call $suspending.wrappedfn
  //   ref.func $suspending.continue-on-suspendable
  //   call $suspending.add-promise-reactions
  // )
  // The function calls suspending import and returns into the
  // $promising.exported function because that was the top function
  // on the main stack.
  bool encodeTrampolineFunction(ModuleEnvironment& moduleEnv,
                                uint32_t paramsSize, Bytes& bytecode) {
    Encoder encoder(bytecode, *moduleEnv.types);
    if (!EncodeLocalEntries(encoder, ValTypeVector())) {
      return false;
    }
    const uint32_t SuspenderIndex = 0;
    const uint32_t ParamsIndex = 1;

    // For GetSuspendingResultsFnIndex call below.
    if (!encoder.writeOp(Op::LocalGet) ||
        !encoder.writeVarU32(SuspenderIndex)) {
      return false;
    }

    for (uint32_t i = 0; i < paramsSize; i++) {
      if (!encoder.writeOp(Op::LocalGet) || !encoder.writeVarU32(ParamsIndex)) {
        return false;
      }
      if (!encoder.writeOp(GcOp::StructGet) ||
          !encoder.writeVarU32(ParamsTypeIndex) || !encoder.writeVarU32(i)) {
        return false;
      }
    }
    if (!encoder.writeOp(Op::Call) || !encoder.writeVarU32(WrappedFnIndex)) {
      return false;
    }

    if (!encoder.writeOp(Op::RefFunc) ||
        !encoder.writeVarU32(ContinueOnSuspendableFnIndex)) {
      return false;
    }
    if (!encoder.writeOp(Op::Call) ||
        !encoder.writeVarU32(GetSuspendingResultsFnIndex)) {
      return false;
    }

    return encoder.writeOp(Op::End);
  }

  // Builds function that is called on main stack:
  // (func $suspending.continue-on-suspendable
  //   (param $params (ref $suspender))
  //   (result externref)
  //   local.get $suspender
  //   ref.null funcref
  //   ref.null anyref
  //   stack-switch ContinueOnSuspendable
  // )
  bool encodeContinueOnSuspendableFunction(ModuleEnvironment& moduleEnv,
                                           uint32_t resultsSize,
                                           Bytes& bytecode) {
    Encoder encoder(bytecode, *moduleEnv.types);
    if (!EncodeLocalEntries(encoder, ValTypeVector())) {
      return false;
    }

    const uint32_t SuspenderIndex = 0;
    if (!encoder.writeOp(Op::LocalGet) ||
        !encoder.writeVarU32(SuspenderIndex)) {
      return false;
    }
    if (!encoder.writeOp(Op::RefNull) ||
        !encoder.writeValType(ValType(RefType::func()))) {
      return false;
    }
    if (!encoder.writeOp(Op::RefNull) ||
        !encoder.writeValType(ValType(RefType::any()))) {
      return false;
    }

    if (!encoder.writeOp(MozOp::StackSwitch) ||
        !encoder.writeVarU32(
            uint32_t(StackSwitchKind::ContinueOnSuspendable))) {
      return false;
    }

    return encoder.writeOp(Op::End);
  }

 public:
  SharedModule build(JSContext* cx, HandleObject func, ValTypeVector&& params,
                     ValTypeVector&& results,
                     SuspenderArgPosition argPosition) {
    FeatureOptions options;
    options.isBuiltinModule = true;
    options.requireGC = true;
    options.requireTailCalls = true;

    ScriptedCaller scriptedCaller;
    SharedCompileArgs compileArgs =
        CompileArgs::buildAndReport(cx, std::move(scriptedCaller), options);
    if (!compileArgs) {
      return nullptr;
    }

    ModuleEnvironment moduleEnv(compileArgs->features);
    MOZ_ASSERT(IonAvailable(cx));
    CompilerEnvironment compilerEnv(CompileMode::Once, Tier::Optimized,
                                    DebugEnabled::False);
    compilerEnv.computeParameters();

    if (!moduleEnv.init()) {
      return nullptr;
    }

    RefType suspenderType = RefType::extern_();
    RefType promiseType = RefType::extern_();

    size_t paramsSize, paramsOffset, suspenderIndex;
    size_t resultsSize = results.length();
    bool checkSuspender;
    ValTypeVector paramsWithoutSuspender;
    switch (argPosition) {
      case SuspenderArgPosition::First:
        paramsSize = params.length() - 1;
        paramsOffset = 1;
        suspenderIndex = 0;
        if (!paramsWithoutSuspender.append(params.begin() + 1, params.end())) {
          ReportOutOfMemory(cx);
          return nullptr;
        }
        checkSuspender = true;
        break;
      case SuspenderArgPosition::Last:
        paramsSize = params.length() - 1;
        paramsOffset = 0;
        suspenderIndex = paramsSize - 1;
        if (!paramsWithoutSuspender.append(params.begin(), params.end() - 1)) {
          ReportOutOfMemory(cx);
          return nullptr;
        }
        checkSuspender = true;
        break;
      default:
        paramsSize = params.length();
        paramsOffset = 0;
        suspenderIndex = paramsSize;
        if (!paramsWithoutSuspender.append(params.begin(), params.end())) {
          ReportOutOfMemory(cx);
          return nullptr;
        }
        checkSuspender = false;
        break;
    }
    ValTypeVector resultsRef;
    if (!resultsRef.emplaceBack(promiseType)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }

    StructType boxedParamsStruct;
    if (!StructType::createImmutable(paramsWithoutSuspender,
                                     &boxedParamsStruct)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.types->length() == ParamsTypeIndex);
    if (!moduleEnv.types->addType(std::move(boxedParamsStruct))) {
      return nullptr;
    }

    StructType boxedResultType;
    if (!StructType::createImmutable(results, &boxedResultType)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.types->length() == ResultsTypeIndex);
    if (!moduleEnv.types->addType(std::move(boxedResultType))) {
      return nullptr;
    }

    MOZ_ASSERT(moduleEnv.funcs.length() == WrappedFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(paramsWithoutSuspender),
                                  std::move(resultsRef))) {
      return nullptr;
    }

    ValTypeVector paramsGetSuspendingResults, resultsGetSuspendingResults;
    if (!paramsGetSuspendingResults.emplaceBack(suspenderType) ||
        !paramsGetSuspendingResults.emplaceBack(promiseType) ||
        !paramsGetSuspendingResults.emplaceBack(RefType::func())) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.funcs.length() == GetSuspendingResultsFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(paramsGetSuspendingResults),
                                  std::move(resultsGetSuspendingResults))) {
      return nullptr;
    }

    // Imports names are not important, declare functions above as imports.
    moduleEnv.numFuncImports = moduleEnv.funcs.length();

    // We will be looking up and using the exports function by index so
    // the name doesn't matter.
    MOZ_ASSERT(moduleEnv.funcs.length() == ExportedFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(params), std::move(results),
                                  /*declareForRef = */ true,
                                  mozilla::Some(CacheableName()))) {
      return nullptr;
    }

    ValTypeVector paramsTrampoline, resultsTrampoline;
    if (!paramsTrampoline.emplaceBack(suspenderType) ||
        !paramsTrampoline.emplaceBack(RefType::fromTypeDef(
            &(*moduleEnv.types)[ParamsTypeIndex], false))) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.funcs.length() == TrampolineFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(paramsTrampoline),
                                  std::move(resultsTrampoline),
                                  /*declareForRef = */ true)) {
      return nullptr;
    }

    ValTypeVector paramsContinueOnSuspendable, resultsContinueOnSuspendable;
    if (!paramsContinueOnSuspendable.emplaceBack(suspenderType) ||
        !paramsContinueOnSuspendable.emplaceBack(RefType::extern_())) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.funcs.length() == ContinueOnSuspendableFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(paramsContinueOnSuspendable),
                                  std::move(resultsContinueOnSuspendable),
                                  /*declareForRef = */ true)) {
      return nullptr;
    }

    ModuleGenerator mg(*compileArgs, &moduleEnv, &compilerEnv, nullptr, nullptr,
                       nullptr);
    if (!mg.init(nullptr)) {
      return nullptr;
    }
    // Build functions and keep bytecodes around until the end.
    Bytes bytecode;
    if (!encodeExportedFunction(
            moduleEnv, paramsSize, resultsSize, paramsOffset, suspenderIndex,
            checkSuspender,
            RefType::fromTypeDef(&(*moduleEnv.types)[ResultsTypeIndex], false),
            bytecode)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!mg.compileFuncDef(ExportedFnIndex, 0, bytecode.begin(),
                           bytecode.begin() + bytecode.length())) {
      return nullptr;
    }
    Bytes bytecode2;
    if (!encodeTrampolineFunction(moduleEnv, paramsSize, bytecode2)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!mg.compileFuncDef(TrampolineFnIndex, 0, bytecode2.begin(),
                           bytecode2.begin() + bytecode2.length())) {
      return nullptr;
    }
    Bytes bytecode3;
    if (!encodeContinueOnSuspendableFunction(moduleEnv, paramsSize,
                                             bytecode3)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!mg.compileFuncDef(ContinueOnSuspendableFnIndex, 0, bytecode3.begin(),
                           bytecode3.begin() + bytecode3.length())) {
      return nullptr;
    }
    if (!mg.finishFuncDefs()) {
      return nullptr;
    }

    SharedBytes shareableBytes = js_new<ShareableBytes>();
    if (!shareableBytes) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    return mg.finishModule(*shareableBytes);
  }
};

// Reaction on resolved/rejected suspending promise.
static bool WasmPISuspendTaskContinue(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSFunction*> callee(cx, &args.callee().as<JSFunction>());
  RootedValue suspender(cx, callee->getExtendedSlot(SUSPENDER_SLOT));

  // Convert result of the promise into the parameters/arguments for the
  // $suspending.continue-on-suspendable.
  RootedFunction continueOnSuspendable(
      cx, &callee->getExtendedSlot(CONTINUE_ON_SUSPENDABLE_SLOT)
               .toObject()
               .as<JSFunction>());
  RootedValueVector argv(cx);
  if (!argv.emplaceBack(suspender)) {
    ReportOutOfMemory(cx);
    return false;
  }
  MOZ_ASSERT(args.length() > 0);
  if (!argv.emplaceBack(args[0])) {
    ReportOutOfMemory(cx);
    return false;
  }

  JS::Rooted<JS::Value> rval(cx);
  if (Call(cx, UndefinedHandleValue, continueOnSuspendable, argv, &rval)) {
    return true;
  }

  // The stack was unwound during exception -- time to release resources.
  CleanupActiveSuspender(cx);

  if (cx->isThrowingOutOfMemory()) {
    return false;
  }
  Rooted<PromiseObject*> promise(
      cx, suspender.toObject().as<SuspenderObject>().promisingPromise());
  return RejectPromiseWithPendingError(cx, promise);
}

// Collects returned suspending promising, and registers callbacks to
// react on it using WasmPISuspendTaskContinue.
// Seen as $suspending.add-promise-reactions in wasm.
static bool WasmPIAddPromiseReactions(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<SuspenderObject*> suspenderObject(
      cx, &args[0].toObject().as<SuspenderObject>());
  RootedValue rval(cx, args[1]);
  RootedFunction fn(cx, &args[2].toObject().as<JSFunction>());

  MOZ_ASSERT(rval.toObject().is<PromiseObject>(),
             "WasmPIWrapSuspendingImport always returning a promise");
  Rooted<PromiseObject*> promise(cx, &rval.toObject().as<PromiseObject>());
  suspenderObject->setSuspendingPromise(promise);

  // Pass fn here
  RootedFunction then_(
      cx, NewNativeFunction(cx, WasmPISuspendTaskContinue, 1, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  then_->initExtendedSlot(SUSPENDER_SLOT, ObjectValue(*suspenderObject));
  then_->initExtendedSlot(CONTINUE_ON_SUSPENDABLE_SLOT, ObjectValue(*fn));
  return AddPromiseReactions(cx, promise, then_, then_);
}

// Wraps original import to catch all exceptions and convert result to a
// promise.
// Seen as $suspending.wrappedfn in wasm.
static bool WasmPIWrapSuspendingImport(JSContext* cx, unsigned argc,
                                       Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSFunction*> callee(cx, &args.callee().as<JSFunction>());
  RootedFunction originalImportFunc(
      cx,
      &callee->getExtendedSlot(WRAPPED_FN_SLOT).toObject().as<JSFunction>());

  // Catching exceptions here.
  RootedValue rval(cx);
  if (Call(cx, UndefinedHandleValue, originalImportFunc, args, &rval)) {
    // Convert the result to a resolved promise.
    RootedObject promiseConstructor(cx, GetPromiseConstructor(cx));
    RootedObject promiseObj(cx, PromiseResolve(cx, promiseConstructor, rval));
    if (!promiseObj) {
      return false;
    }
    args.rval().setObject(*promiseObj);
    return true;
  }

  if (cx->isThrowingOutOfMemory()) {
    return false;
  }

  // Convert failure to a rejected promise.
  RootedObject promiseObject(cx, NewPromiseObject(cx, nullptr));
  if (!promiseObject) {
    return false;
  }
  args.rval().setObject(*promiseObject);

  Rooted<PromiseObject*> promise(cx, &promiseObject->as<PromiseObject>());
  return RejectPromiseWithPendingError(cx, promise);
}

JSFunction* WasmSuspendingFunctionCreate(JSContext* cx, HandleObject func,
                                         ValTypeVector&& params,
                                         ValTypeVector&& results,
                                         SuspenderArgPosition argPosition) {
  MOZ_ASSERT(IsCallable(ObjectValue(*func)) &&
             !IsCrossCompartmentWrapper(func));

  if (argPosition != SuspenderArgPosition::None) {
    if (params.length() < 1 ||
        params[argPosition == SuspenderArgPosition::Last ? params.length() - 1
                                                         : 0] !=
            RefType::extern_()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_JSPI_EXPECTED_SUSPENDER);
      return nullptr;
    }
  }

  SuspendingFunctionModuleFactory moduleFactory;
  SharedModule module = moduleFactory.build(cx, func, std::move(params),
                                            std::move(results), argPosition);
  if (!module) {
    return nullptr;
  }

  // Instantiate the module.
  Rooted<ImportValues> imports(cx);

  // Add $suspending.wrappedfn to imports.
  RootedFunction funcWrapper(
      cx, NewNativeFunction(cx, WasmPIWrapSuspendingImport, 0, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!funcWrapper) {
    return nullptr;
  }
  funcWrapper->initExtendedSlot(WRAPPED_FN_SLOT, ObjectValue(*func));
  if (!imports.get().funcs.append(funcWrapper)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // Add $suspending.add-promise-reactions to imports.
  RootedFunction addPromiseReactions(
      cx, NewNativeFunction(cx, WasmPIAddPromiseReactions, 3, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!addPromiseReactions) {
    return nullptr;
  }
  if (!imports.get().funcs.append(addPromiseReactions)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  Rooted<WasmInstanceObject*> instance(cx);
  if (!module->instantiate(cx, imports.get(), nullptr, &instance)) {
    // Can also trap on invalid input function.
    return nullptr;
  }

  // Returns the $suspending.exported function.
  RootedFunction wasmFunc(cx);
  if (!WasmInstanceObject::getExportedFunction(
          cx, instance, SuspendingFunctionModuleFactory::ExportedFnIndex,
          &wasmFunc)) {
    return nullptr;
  }
  return wasmFunc;
}

JSFunction* WasmSuspendingFunctionCreate(JSContext* cx, HandleObject func,
                                         const FuncType& type) {
  ValTypeVector params, results;
  if (!params.append(type.args().begin(), type.args().end()) ||
      !results.append(type.results().begin(), type.results().end())) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  return WasmSuspendingFunctionCreate(cx, func, std::move(params),
                                      std::move(results),
                                      SuspenderArgPosition::None);
}

// Promising

// Builds a wasm module with following structure:
// (module
//   (type $params (struct (field ..)*)))
//   (type $results (struct (field ..)*)))
//   (import "" "" (func $promising.wrappedfn ..))
//   (import "" "" (func $promising.create-suspender ..))
//   (func $promising.exported .. )
//   (func $promising.trampoline ..)
//   (export "" (func $promising.exported))
// )
//
// The module provides logic for the Invoke Promising Import state transition
// via $promising.exported and $promising.trampoline (see the SMDOC).
//
class PromisingFunctionModuleFactory {
 public:
  enum TypeIdx {
    ParamsTypeIndex,
    ResultsTypeIndex,
  };

  enum FnIdx {
    WrappedFnIndex,
    CreateSuspenderFnIndex,
    ExportedFnIndex,
    TrampolineFnIndex,
  };

 private:
  // Builds function that will be exported for JS:
  // (func $promising.exported
  //   (param ..)* (result externref)
  //   (local $promise externref)
  //   call $promising.create-suspender ;; -> (suspender,promise)
  //   local.set $promise
  //   ref.func $promising.trampoline
  //   local.get $i*
  //   stuct.new $param-type
  //   stack-switch SwitchToSuspendable ;; <- (suspender,fn,data)
  //   local.get $promise
  // )
  bool encodeExportedFunction(ModuleEnvironment& moduleEnv, uint32_t paramsSize,
                              Bytes& bytecode) {
    Encoder encoder(bytecode, *moduleEnv.types);
    ValTypeVector locals;
    if (!locals.emplaceBack(RefType::extern_())) {
      return false;
    }
    if (!EncodeLocalEntries(encoder, locals)) {
      return false;
    }

    const uint32_t PromiseIndex = paramsSize;
    if (!encoder.writeOp(Op::Call) ||
        !encoder.writeVarU32(CreateSuspenderFnIndex)) {
      return false;
    }
    if (!encoder.writeOp(Op::LocalSet) || !encoder.writeVarU32(PromiseIndex)) {
      return false;
    }

    if (!encoder.writeOp(Op::RefFunc) ||
        !encoder.writeVarU32(TrampolineFnIndex)) {
      return false;
    }
    for (uint32_t i = 0; i < paramsSize; i++) {
      if (!encoder.writeOp(Op::LocalGet) || !encoder.writeVarU32(i)) {
        return false;
      }
    }
    if (!encoder.writeOp(GcOp::StructNew) ||
        !encoder.writeVarU32(ParamsTypeIndex)) {
      return false;
    }
    if (!encoder.writeOp(MozOp::StackSwitch) ||
        !encoder.writeVarU32(uint32_t(StackSwitchKind::SwitchToSuspendable))) {
      return false;
    }

    if (!encoder.writeOp(Op::LocalGet) || !encoder.writeVarU32(PromiseIndex)) {
      return false;
    }
    return encoder.writeOp(Op::End);
  }

  // Builds function that is called on alternative stack:
  // (func $promising.trampoline
  //   (param $suspender externref) (param $params (ref $param-type))
  //   (result externref)
  //   local.get $suspender ;; for call $set-results
  //   (local.get $suspender)?
  //   (struct.get $param-type $i (local.get $param))*
  //   (local.get $suspender)?
  //   call $promising.wrappedfn
  //   struct.new $result-type
  //   call $builtin.set-promising-promise-results
  // )
  bool encodeTrampolineFunction(ModuleEnvironment& moduleEnv,
                                uint32_t paramsSize,
                                SuspenderArgPosition argPosition,
                                Bytes& bytecode) {
    Encoder encoder(bytecode, *moduleEnv.types);
    if (!EncodeLocalEntries(encoder, ValTypeVector())) {
      return false;
    }
    const uint32_t SuspenderIndex = 0;
    const uint32_t ParamsIndex = 1;

    // Reserved for SetResultsFnIndex call at the end
    if (!encoder.writeOp(Op::LocalGet) ||
        !encoder.writeVarU32(SuspenderIndex)) {
      return false;
    }

    if (argPosition == SuspenderArgPosition::First) {
      if (!encoder.writeOp(Op::LocalGet) ||
          !encoder.writeVarU32(SuspenderIndex)) {
        return false;
      }
    }
    for (uint32_t i = 0; i < paramsSize; i++) {
      if (!encoder.writeOp(Op::LocalGet) || !encoder.writeVarU32(ParamsIndex)) {
        return false;
      }
      if (!encoder.writeOp(GcOp::StructGet) ||
          !encoder.writeVarU32(ParamsTypeIndex) || !encoder.writeVarU32(i)) {
        return false;
      }
    }
    if (argPosition == SuspenderArgPosition::Last) {
      if (!encoder.writeOp(Op::LocalGet) ||
          !encoder.writeVarU32(SuspenderIndex)) {
        return false;
      }
    }
    if (!encoder.writeOp(Op::Call) || !encoder.writeVarU32(WrappedFnIndex)) {
      return false;
    }

    if (!encoder.writeOp(GcOp::StructNew) ||
        !encoder.writeVarU32(ResultsTypeIndex)) {
      return false;
    }
    if (!encoder.writeOp(MozOp::CallBuiltinModuleFunc) ||
        !encoder.writeVarU32(
            (uint32_t)BuiltinModuleFuncId::SetPromisingPromiseResults)) {
      return false;
    }

    return encoder.writeOp(Op::End);
  }

 public:
  SharedModule build(JSContext* cx, HandleFunction fn, ValTypeVector&& params,
                     ValTypeVector&& results,
                     SuspenderArgPosition argPosition) {
    const FuncType& fnType = fn->wasmTypeDef()->funcType();
    size_t paramsSize = params.length();

    RefType suspenderType = RefType::extern_();
    RefType promiseType = RefType::extern_();

    FeatureOptions options;
    options.isBuiltinModule = true;
    options.requireGC = true;
    options.requireTailCalls = true;

    ScriptedCaller scriptedCaller;
    SharedCompileArgs compileArgs =
        CompileArgs::buildAndReport(cx, std::move(scriptedCaller), options);
    if (!compileArgs) {
      return nullptr;
    }

    ModuleEnvironment moduleEnv(compileArgs->features);
    MOZ_ASSERT(IonAvailable(cx));
    CompilerEnvironment compilerEnv(CompileMode::Once, Tier::Optimized,
                                    DebugEnabled::False);
    compilerEnv.computeParameters();

    if (!moduleEnv.init()) {
      return nullptr;
    }

    StructType boxedParamsStruct;
    if (!StructType::createImmutable(params, &boxedParamsStruct)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.types->length() == ParamsTypeIndex);
    if (!moduleEnv.types->addType(std::move(boxedParamsStruct))) {
      return nullptr;
    }

    StructType boxedResultType;
    if (!StructType::createImmutable(fnType.results(), &boxedResultType)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.types->length() == ResultsTypeIndex);
    if (!moduleEnv.types->addType(std::move(boxedResultType))) {
      return nullptr;
    }

    ValTypeVector paramsForWrapper, resultsForWrapper;
    if (!paramsForWrapper.append(fnType.args().begin(), fnType.args().end()) ||
        !resultsForWrapper.append(fnType.results().begin(),
                                  fnType.results().end())) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.funcs.length() == WrappedFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(paramsForWrapper),
                                  std::move(resultsForWrapper))) {
      return nullptr;
    }

    ValTypeVector paramsCreateSuspender, resultsCreateSuspender;
    if (!resultsCreateSuspender.emplaceBack(suspenderType) ||
        !resultsCreateSuspender.emplaceBack(promiseType)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.funcs.length() == CreateSuspenderFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(paramsCreateSuspender),
                                  std::move(resultsCreateSuspender))) {
      return nullptr;
    }

    // Imports names are not important, declare functions above as imports.
    moduleEnv.numFuncImports = moduleEnv.funcs.length();

    // We will be looking up and using the exports function by index so
    // the name doesn't matter.
    MOZ_ASSERT(moduleEnv.funcs.length() == ExportedFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(params), std::move(results),
                                  /* declareFoRef = */ true,
                                  mozilla::Some(CacheableName()))) {
      return nullptr;
    }

    ValTypeVector paramsTrampoline, resultsTrampoline;
    if (!paramsTrampoline.emplaceBack(suspenderType) ||
        !paramsTrampoline.emplaceBack(RefType::fromTypeDef(
            &(*moduleEnv.types)[ParamsTypeIndex], false))) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT(moduleEnv.funcs.length() == TrampolineFnIndex);
    if (!moduleEnv.addDefinedFunc(std::move(paramsTrampoline),
                                  std::move(resultsTrampoline),
                                  /* declareFoRef = */ true)) {
      return nullptr;
    }

    ModuleGenerator mg(*compileArgs, &moduleEnv, &compilerEnv, nullptr, nullptr,
                       nullptr);
    if (!mg.init(nullptr)) {
      return nullptr;
    }
    // Build functions and keep bytecodes around until the end.
    Bytes bytecode;
    if (!encodeExportedFunction(moduleEnv, paramsSize, bytecode)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!mg.compileFuncDef(ExportedFnIndex, 0, bytecode.begin(),
                           bytecode.begin() + bytecode.length())) {
      return nullptr;
    }
    Bytes bytecode2;
    if (!encodeTrampolineFunction(moduleEnv, paramsSize, argPosition,
                                  bytecode2)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!mg.compileFuncDef(TrampolineFnIndex, 0, bytecode2.begin(),
                           bytecode2.begin() + bytecode2.length())) {
      return nullptr;
    }
    if (!mg.finishFuncDefs()) {
      return nullptr;
    }

    SharedBytes shareableBytes = js_new<ShareableBytes>();
    if (!shareableBytes) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    return mg.finishModule(*shareableBytes);
  }
};

// Creates a suspender and promise (that will be returned to JS code).
// Seen as $promising.create-suspender to wasm.
static bool WasmPICreateSuspender(JSContext* cx, unsigned argc, Value* vp) {
  Rooted<SuspenderObject*> suspenderObject(cx, SuspenderObject::create(cx));
  RootedObject promiseObject(cx, NewPromiseObject(cx, nullptr));
  if (!promiseObject) {
    return false;
  }

  Rooted<PromiseObject*> promise(cx, &promiseObject->as<PromiseObject>());
  suspenderObject->setPromisingPromise(promise);

  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<ArrayObject*> results(cx, NewDenseEmptyArray(cx));
  if (!NewbornArrayPush(cx, results, ObjectValue(*suspenderObject))) {
    return false;
  }
  if (!NewbornArrayPush(cx, results, ObjectValue(*promise))) {
    return false;
  }
  args.rval().setObject(*results);
  return true;
}

// Wraps call to wasm $promising.exported function to catch an exception and
// return a promise instead.
static bool WasmPIPromisingFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSFunction*> callee(cx, &args.callee().as<JSFunction>());
  RootedFunction fn(
      cx,
      &callee->getExtendedSlot(WRAPPED_FN_SLOT).toObject().as<JSFunction>());

  // Catching exceptions here.
  if (Call(cx, UndefinedHandleValue, fn, args, args.rval())) {
    return true;
  }

  // During an exception the stack was unwound -- time to release resources.
  CleanupActiveSuspender(cx);

  if (cx->isThrowingOutOfMemory()) {
    return false;
  }

  RootedObject promiseObject(cx, NewPromiseObject(cx, nullptr));
  if (!promiseObject) {
    return false;
  }
  args.rval().setObject(*promiseObject);

  Rooted<PromiseObject*> promise(cx, &promiseObject->as<PromiseObject>());
  return RejectPromiseWithPendingError(cx, promise);
}

JSFunction* WasmPromisingFunctionCreate(JSContext* cx, HandleObject func,
                                        ValTypeVector&& params,
                                        ValTypeVector&& results,
                                        SuspenderArgPosition argPosition) {
  RootedFunction wrappedWasmFunc(cx, &func->as<JSFunction>());
  MOZ_ASSERT(wrappedWasmFunc->isWasm());
  const FuncType& wrappedWasmFuncType =
      wrappedWasmFunc->wasmTypeDef()->funcType();

  if (argPosition != SuspenderArgPosition::None) {
    if (results.length() != 1 || results[0] != RefType::extern_()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_JSPI_EXPECTED_PROMISE);
      return nullptr;
    }

    size_t paramsOffset, suspenderIndex;
    switch (argPosition) {
      case SuspenderArgPosition::First:
        paramsOffset = 1;
        suspenderIndex = 0;
        break;
      case SuspenderArgPosition::Last:
        paramsOffset = 0;
        suspenderIndex = params.length();
        break;
      default:
        MOZ_CRASH();
    }

    if (wrappedWasmFuncType.args().length() != params.length() + 1 ||
        wrappedWasmFuncType.args()[suspenderIndex] != RefType::extern_()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_JSPI_EXPECTED_SUSPENDER);
      return nullptr;
    }
    for (size_t i = 0; i < params.length(); i++) {
      if (params[i] != wrappedWasmFuncType.args()[i + paramsOffset]) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_JSPI_SIGNATURE_MISMATCH);
        return nullptr;
      }
    }
  } else {
    MOZ_ASSERT(results.length() == 0 && params.length() == 0);
    if (!results.append(RefType::extern_())) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    if (!params.append(wrappedWasmFuncType.args().begin(),
                       wrappedWasmFuncType.args().end())) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
  }

  PromisingFunctionModuleFactory moduleFactory;
  SharedModule module = moduleFactory.build(
      cx, wrappedWasmFunc, std::move(params), std::move(results), argPosition);
  // Instantiate the module.
  Rooted<ImportValues> imports(cx);

  // Add wrapped function ($promising.wrappedfn) to imports.
  if (!imports.get().funcs.append(func)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // Add $promising.create-suspender to imports.
  RootedFunction createSuspenderFunc(
      cx, NewNativeFunction(cx, WasmPICreateSuspender, 0, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!createSuspenderFunc) {
    return nullptr;
  }
  if (!imports.get().funcs.append(createSuspenderFunc)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  Rooted<WasmInstanceObject*> instance(cx);
  if (!module->instantiate(cx, imports.get(), nullptr, &instance)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }

  // Wrap $promising.exported function for exceptions/traps handling.
  RootedFunction wasmFunc(cx);
  if (!WasmInstanceObject::getExportedFunction(
          cx, instance, PromisingFunctionModuleFactory::ExportedFnIndex,
          &wasmFunc)) {
    return nullptr;
  }

  RootedFunction wasmFuncWrapper(
      cx, NewNativeFunction(cx, WasmPIPromisingFunction, 0, nullptr,
                            gc::AllocKind::FUNCTION_EXTENDED, GenericObject));
  if (!wasmFuncWrapper) {
    return nullptr;
  }
  wasmFuncWrapper->initExtendedSlot(WRAPPED_FN_SLOT, ObjectValue(*wasmFunc));
  return wasmFuncWrapper;
}

// Gets active suspender.
// The reserved parameter is a workaround for limitation in the
// WasmBuiltinModule.yaml generator to always have params.
SuspenderObject* CurrentSuspender(Instance* instance, int32_t reserved) {
  MOZ_ASSERT(SASigCurrentSuspender.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  SuspenderObject* suspender = cx->wasm().promiseIntegration.activeSuspender();
  if (!suspender) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_JSPI_INVALID_STATE);
    return nullptr;
  }
  return suspender;
}

// Checks suspender value.
SuspenderObject* CheckSuspender(Instance* instance, JSObject* maybeSuspender) {
  MOZ_ASSERT(SASigCheckSuspender.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  if (!maybeSuspender || !maybeSuspender->is<SuspenderObject>() ||
      &maybeSuspender->as<SuspenderObject>() !=
          cx->wasm().promiseIntegration.activeSuspender()) {
    // Wrong suspender
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_JSPI_INVALID_SUSPENDER);
    return nullptr;
  }
  SuspenderObject* suspenderObject = &maybeSuspender->as<SuspenderObject>();
  if (suspenderObject->state() != SuspenderState::Active) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_JSPI_INVALID_STATE);
    return nullptr;
  }
  return suspenderObject;
}

// Converts promise results into actual function result, or exception/trap
// if rejected.
JSObject* GetSuspendingPromiseResult(Instance* instance,
                                     SuspenderObject* suspender) {
  MOZ_ASSERT(SASigGetSuspendingPromiseResult.failureMode ==
             FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  Rooted<SuspenderObject*> suspenderObject(cx, suspender);

  Rooted<PromiseObject*> promise(cx, suspenderObject->suspendingPromise());

  if (promise->state() == JS::PromiseState::Rejected) {
    RootedValue reason(cx, promise->reason());
    // Result is also the reason of promise rejection.
    cx->setPendingException(reason, ShouldCaptureStack::Maybe);
    return nullptr;
  }

  // Construct the results object.
  Rooted<WasmStructObject*> results(
      cx, instance->constantStructNewDefault(
              cx, SuspendingFunctionModuleFactory::ResultsTypeIndex));
  const FieldTypeVector& fields = results->typeDef().structType().fields_;

  if (fields.length() > 0) {
    RootedValue jsValue(cx, promise->value());

    // The struct object is constructed based on returns of exported function.
    // It is the only way we can get ValType for Val::fromJSValue call.
    auto bestTier = instance->code().bestTier();
    const wasm::FuncExport& funcExport =
        instance->metadata(bestTier).lookupFuncExport(
            SuspendingFunctionModuleFactory::ExportedFnIndex);
    const wasm::FuncType& sig =
        instance->metadata().getFuncExportType(funcExport);

    if (fields.length() == 1) {
      RootedVal val(cx);
      MOZ_ASSERT(sig.result(0).storageType() == fields[0].type);
      if (!Val::fromJSValue(cx, sig.result(0), jsValue, &val)) {
        return nullptr;
      }
      results->storeVal(val, 0);
    } else {
      // The multi-value result is wrapped into ArrayObject/Iterable.
      Rooted<ArrayObject*> array(cx);
      if (!IterableToArray(cx, jsValue, &array)) {
        return nullptr;
      }
      if (fields.length() != array->length()) {
        UniqueChars expected(JS_smprintf("%zu", fields.length()));
        UniqueChars got(JS_smprintf("%u", array->length()));
        if (!expected || !got) {
          ReportOutOfMemory(cx);
          return nullptr;
        }

        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_WRONG_NUMBER_OF_VALUES,
                                 expected.get(), got.get());
        return nullptr;
      }

      for (size_t i = 0; i < fields.length(); i++) {
        RootedVal val(cx);
        RootedValue v(cx, array->getDenseElement(i));
        MOZ_ASSERT(sig.result(i).storageType() == fields[i].type);
        if (!Val::fromJSValue(cx, sig.result(i), v, &val)) {
          return nullptr;
        }
        results->storeVal(val, i);
      }
    }
  }
  return results;
}

// Resolves the promise using results packed by wasm.
int32_t SetPromisingPromiseResults(Instance* instance,
                                   SuspenderObject* suspender,
                                   WasmStructObject* results) {
  MOZ_ASSERT(SASigSetPromisingPromiseResults.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  Rooted<WasmStructObject*> res(cx, results);
  Rooted<SuspenderObject*> suspenderObject(cx, suspender);
  RootedObject promise(cx, suspenderObject->promisingPromise());

  const StructType& resultType = res->typeDef().structType();
  RootedValue val(cx);
  // Unbox the result value from the struct, if any.
  switch (resultType.fields_.length()) {
    case 0:
      break;
    case 1: {
      if (!res->getField(cx, /*index=*/0, &val)) {
        return false;
      }
    } break;
    default: {
      Rooted<ArrayObject*> array(cx, NewDenseEmptyArray(cx));
      if (!array) {
        return false;
      }
      for (size_t i = 0; i < resultType.fields_.length(); i++) {
        RootedValue item(cx);
        if (!res->getField(cx, i, &item)) {
          return false;
        }
        if (!NewbornArrayPush(cx, array, item)) {
          return false;
        }
      }
      val.setObject(*array);
    } break;
  }
  ResolvePromise(cx, promise, val);
  return 0;
}

void UpdateSuspenderState(Instance* instance, SuspenderObject* suspender,
                          UpdateSuspenderStateAction action) {
  MOZ_ASSERT(SASigUpdateSuspenderState.failureMode == FailureMode::Infallible);

  JSContext* cx = instance->cx();
  switch (action) {
    case UpdateSuspenderStateAction::Enter:
      suspender->enter(cx);
      break;
    case UpdateSuspenderStateAction::Suspend:
      suspender->suspend(cx);
      break;
    case UpdateSuspenderStateAction::Resume:
      suspender->resume(cx);
      break;
    case UpdateSuspenderStateAction::Leave:
      suspender->leave(cx);
      break;
    default:
      MOZ_CRASH();
  }
}

}  // namespace js::wasm
#endif  // ENABLE_WASM_JSPI

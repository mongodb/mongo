/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2017 Mozilla Foundation
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

#include "wasm/WasmBuiltins.h"

#include "mozilla/Atomics.h"
#include "mozilla/ScopeExit.h"

#include "fdlibm.h"
#include "jslibmath.h"
#include "jsmath.h"

#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "jit/JitRuntime.h"
#include "jit/MacroAssembler.h"
#include "jit/ProcessExecutableMemory.h"
#include "jit/Simulator.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "threading/Mutex.h"
#include "util/Memory.h"
#include "util/Poison.h"
#include "vm/BigIntType.h"
#include "vm/ErrorObject.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmDebugFrame.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmPI.h"
#include "wasm/WasmStubs.h"

#include "debugger/DebugAPI-inl.h"
#include "vm/ErrorObject-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/Stack-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace jit;
using namespace wasm;

using mozilla::EnumeratedArray;
using mozilla::HashGeneric;
using mozilla::MakeEnumeratedRange;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

static const unsigned BUILTIN_THUNK_LIFO_SIZE = 64 * 1024;

// ============================================================================
// WebAssembly builtin C++ functions called from wasm code to implement internal
// wasm operations: type descriptions.

// Some abbreviations, for the sake of conciseness.
#define _F64 MIRType::Double
#define _F32 MIRType::Float32
#define _I32 MIRType::Int32
#define _I64 MIRType::Int64
#define _PTR MIRType::Pointer
#define _RoN MIRType::WasmAnyRef
#define _WAD MIRType::WasmArrayData
#define _VOID MIRType::None
#define _END MIRType::None
#define _Infallible FailureMode::Infallible
#define _FailOnNegI32 FailureMode::FailOnNegI32
#define _FailOnMaxI32 FailureMode::FailOnMaxI32
#define _FailOnNullPtr FailureMode::FailOnNullPtr
#define _FailOnInvalidRef FailureMode::FailOnInvalidRef

namespace js {
namespace wasm {

constexpr SymbolicAddressSignature SASigSinNativeD = {
    SymbolicAddress::SinNativeD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigSinFdlibmD = {
    SymbolicAddress::SinFdlibmD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigCosNativeD = {
    SymbolicAddress::CosNativeD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigCosFdlibmD = {
    SymbolicAddress::CosFdlibmD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigTanNativeD = {
    SymbolicAddress::TanNativeD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigTanFdlibmD = {
    SymbolicAddress::TanFdlibmD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigASinD = {
    SymbolicAddress::ASinD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigACosD = {
    SymbolicAddress::ACosD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigATanD = {
    SymbolicAddress::ATanD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigCeilD = {
    SymbolicAddress::CeilD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigCeilF = {
    SymbolicAddress::CeilF, _F32, _Infallible, 1, {_F32, _END}};
constexpr SymbolicAddressSignature SASigFloorD = {
    SymbolicAddress::FloorD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigFloorF = {
    SymbolicAddress::FloorF, _F32, _Infallible, 1, {_F32, _END}};
constexpr SymbolicAddressSignature SASigTruncD = {
    SymbolicAddress::TruncD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigTruncF = {
    SymbolicAddress::TruncF, _F32, _Infallible, 1, {_F32, _END}};
constexpr SymbolicAddressSignature SASigNearbyIntD = {
    SymbolicAddress::NearbyIntD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigNearbyIntF = {
    SymbolicAddress::NearbyIntF, _F32, _Infallible, 1, {_F32, _END}};
constexpr SymbolicAddressSignature SASigExpD = {
    SymbolicAddress::ExpD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigLogD = {
    SymbolicAddress::LogD, _F64, _Infallible, 1, {_F64, _END}};
constexpr SymbolicAddressSignature SASigPowD = {
    SymbolicAddress::PowD, _F64, _Infallible, 2, {_F64, _F64, _END}};
constexpr SymbolicAddressSignature SASigATan2D = {
    SymbolicAddress::ATan2D, _F64, _Infallible, 2, {_F64, _F64, _END}};
constexpr SymbolicAddressSignature SASigArrayMemMove = {
    SymbolicAddress::ArrayMemMove,
    _VOID,
    _Infallible,
    6,
    {_WAD, _I32, _WAD, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigArrayRefsMove = {
    SymbolicAddress::ArrayRefsMove,
    _VOID,
    _Infallible,
    5,
    {_WAD, _I32, _WAD, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigMemoryGrowM32 = {
    SymbolicAddress::MemoryGrowM32,
    _I32,
    _Infallible,
    3,
    {_PTR, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigMemoryGrowM64 = {
    SymbolicAddress::MemoryGrowM64,
    _I64,
    _Infallible,
    3,
    {_PTR, _I64, _I32, _END}};
constexpr SymbolicAddressSignature SASigMemorySizeM32 = {
    SymbolicAddress::MemorySizeM32, _I32, _Infallible, 2, {_PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigMemorySizeM64 = {
    SymbolicAddress::MemorySizeM64, _I64, _Infallible, 2, {_PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigWaitI32M32 = {
    SymbolicAddress::WaitI32M32,
    _I32,
    _FailOnNegI32,
    5,
    {_PTR, _I32, _I32, _I64, _I32, _END}};
constexpr SymbolicAddressSignature SASigWaitI32M64 = {
    SymbolicAddress::WaitI32M64,
    _I32,
    _FailOnNegI32,
    5,
    {_PTR, _I64, _I32, _I64, _I32, _END}};
constexpr SymbolicAddressSignature SASigWaitI64M32 = {
    SymbolicAddress::WaitI64M32,
    _I32,
    _FailOnNegI32,
    5,
    {_PTR, _I32, _I64, _I64, _I32, _END}};
constexpr SymbolicAddressSignature SASigWaitI64M64 = {
    SymbolicAddress::WaitI64M64,
    _I32,
    _FailOnNegI32,
    5,
    {_PTR, _I64, _I64, _I64, _I32, _END}};
constexpr SymbolicAddressSignature SASigWakeM32 = {
    SymbolicAddress::WakeM32,
    _I32,
    _FailOnNegI32,
    4,
    {_PTR, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigWakeM64 = {
    SymbolicAddress::WakeM64,
    _I32,
    _FailOnNegI32,
    4,
    {_PTR, _I64, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigMemCopyM32 = {
    SymbolicAddress::MemCopyM32,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I32, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemCopySharedM32 = {
    SymbolicAddress::MemCopySharedM32,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I32, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemCopyM64 = {
    SymbolicAddress::MemCopyM64,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I64, _I64, _I64, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemCopySharedM64 = {
    SymbolicAddress::MemCopySharedM64,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I64, _I64, _I64, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemCopyAny = {
    SymbolicAddress::MemCopyAny,
    _VOID,
    _FailOnNegI32,
    6,
    {_PTR, _I64, _I64, _I64, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigDataDrop = {
    SymbolicAddress::DataDrop, _VOID, _FailOnNegI32, 2, {_PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigMemFillM32 = {
    SymbolicAddress::MemFillM32,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I32, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemFillSharedM32 = {
    SymbolicAddress::MemFillSharedM32,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I32, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemFillM64 = {
    SymbolicAddress::MemFillM64,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I64, _I32, _I64, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemFillSharedM64 = {
    SymbolicAddress::MemFillSharedM64,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I64, _I32, _I64, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemDiscardM32 = {
    SymbolicAddress::MemDiscardM32,
    _VOID,
    _FailOnNegI32,
    4,
    {_PTR, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemDiscardSharedM32 = {
    SymbolicAddress::MemDiscardSharedM32,
    _VOID,
    _FailOnNegI32,
    4,
    {_PTR, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemDiscardM64 = {
    SymbolicAddress::MemDiscardM64,
    _VOID,
    _FailOnNegI32,
    4,
    {_PTR, _I64, _I64, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemDiscardSharedM64 = {
    SymbolicAddress::MemDiscardSharedM64,
    _VOID,
    _FailOnNegI32,
    4,
    {_PTR, _I64, _I64, _PTR, _END}};
constexpr SymbolicAddressSignature SASigMemInitM32 = {
    SymbolicAddress::MemInitM32,
    _VOID,
    _FailOnNegI32,
    6,
    {_PTR, _I32, _I32, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigMemInitM64 = {
    SymbolicAddress::MemInitM64,
    _VOID,
    _FailOnNegI32,
    6,
    {_PTR, _I64, _I32, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigTableCopy = {
    SymbolicAddress::TableCopy,
    _VOID,
    _FailOnNegI32,
    6,
    {_PTR, _I32, _I32, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigElemDrop = {
    SymbolicAddress::ElemDrop, _VOID, _FailOnNegI32, 2, {_PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigTableFill = {
    SymbolicAddress::TableFill,
    _VOID,
    _FailOnNegI32,
    5,
    {_PTR, _I32, _RoN, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigTableGet = {SymbolicAddress::TableGet,
                                                    _RoN,
                                                    _FailOnInvalidRef,
                                                    3,
                                                    {_PTR, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigTableGrow = {
    SymbolicAddress::TableGrow,
    _I32,
    _Infallible,
    4,
    {_PTR, _RoN, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigTableInit = {
    SymbolicAddress::TableInit,
    _VOID,
    _FailOnNegI32,
    6,
    {_PTR, _I32, _I32, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigTableSet = {
    SymbolicAddress::TableSet,
    _VOID,
    _FailOnNegI32,
    4,
    {_PTR, _I32, _RoN, _I32, _END}};
constexpr SymbolicAddressSignature SASigTableSize = {
    SymbolicAddress::TableSize, _I32, _Infallible, 2, {_PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigRefFunc = {
    SymbolicAddress::RefFunc, _RoN, _FailOnInvalidRef, 2, {_PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigPostBarrierEdge = {
    SymbolicAddress::PostBarrierEdge,
    _VOID,
    _Infallible,
    2,
    {_PTR, _PTR, _END}};
constexpr SymbolicAddressSignature SASigPostBarrierEdgePrecise = {
    SymbolicAddress::PostBarrierEdgePrecise,
    _VOID,
    _Infallible,
    3,
    {_PTR, _PTR, _RoN, _END}};
constexpr SymbolicAddressSignature SASigPostBarrierWholeCell = {
    SymbolicAddress::PostBarrierWholeCell,
    _VOID,
    _Infallible,
    2,
    {_PTR, _PTR, _END}};
constexpr SymbolicAddressSignature SASigExceptionNew = {
    SymbolicAddress::ExceptionNew, _RoN, _FailOnNullPtr, 2, {_PTR, _RoN, _END}};
constexpr SymbolicAddressSignature SASigThrowException = {
    SymbolicAddress::ThrowException,
    _VOID,
    _FailOnNegI32,
    2,
    {_PTR, _RoN, _END}};
constexpr SymbolicAddressSignature SASigStructNewIL_true = {
    SymbolicAddress::StructNewIL_true,
    _RoN,
    _FailOnNullPtr,
    3,
    {_PTR, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigStructNewIL_false = {
    SymbolicAddress::StructNewIL_false,
    _RoN,
    _FailOnNullPtr,
    3,
    {_PTR, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigStructNewOOL_true = {
    SymbolicAddress::StructNewOOL_true,
    _RoN,
    _FailOnNullPtr,
    3,
    {_PTR, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigStructNewOOL_false = {
    SymbolicAddress::StructNewOOL_false,
    _RoN,
    _FailOnNullPtr,
    3,
    {_PTR, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigArrayNew_true = {
    SymbolicAddress::ArrayNew_true,
    _RoN,
    _FailOnNullPtr,
    4,
    {_PTR, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigArrayNew_false = {
    SymbolicAddress::ArrayNew_false,
    _RoN,
    _FailOnNullPtr,
    4,
    {_PTR, _I32, _I32, _PTR, _END}};
constexpr SymbolicAddressSignature SASigArrayNewData = {
    SymbolicAddress::ArrayNewData,
    _RoN,
    _FailOnNullPtr,
    6,
    {_PTR, _I32, _I32, _I32, _PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigArrayNewElem = {
    SymbolicAddress::ArrayNewElem,
    _RoN,
    _FailOnNullPtr,
    6,
    {_PTR, _I32, _I32, _I32, _PTR, _I32, _END}};
constexpr SymbolicAddressSignature SASigArrayInitData = {
    SymbolicAddress::ArrayInitData,
    _VOID,
    _FailOnNegI32,
    6,
    {_PTR, _RoN, _I32, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigArrayInitElem = {
    SymbolicAddress::ArrayInitElem,
    _VOID,
    _FailOnNegI32,
    7,
    {_PTR, _RoN, _I32, _I32, _I32, _I32, _I32, _END}};
constexpr SymbolicAddressSignature SASigArrayCopy = {
    SymbolicAddress::ArrayCopy,
    _VOID,
    _FailOnNegI32,
    7,
    {_PTR, _RoN, _I32, _RoN, _I32, _I32, _I32, _END}};

#define VISIT_BUILTIN_FUNC(op, export, sa_name, ...)    \
  constexpr SymbolicAddressSignature SASig##sa_name = { \
      SymbolicAddress::sa_name,                         \
      DECLARE_BUILTIN_MODULE_FUNC_RESULT_MIRTYPE_##op,  \
      DECLARE_BUILTIN_MODULE_FUNC_FAILMODE_##op,        \
      DECLARE_BUILTIN_MODULE_FUNC_PARAM_MIRTYPES_##op};

FOR_EACH_BUILTIN_MODULE_FUNC(VISIT_BUILTIN_FUNC)
#undef VISIT_BUILTIN_FUNC

#ifdef ENABLE_WASM_JSPI
constexpr SymbolicAddressSignature SASigUpdateSuspenderState = {
    SymbolicAddress::UpdateSuspenderState,
    _VOID,
    _Infallible,
    3,
    {_PTR, _PTR, _I32, _END}};
#endif

}  // namespace wasm
}  // namespace js

#undef _F64
#undef _F32
#undef _I32
#undef _I64
#undef _PTR
#undef _RoN
#undef _VOID
#undef _END
#undef _Infallible
#undef _FailOnNegI32
#undef _FailOnNullPtr

#ifdef DEBUG
ABIType ToABIType(FailureMode mode) {
  switch (mode) {
    case FailureMode::FailOnNegI32:
      return ABIType::Int32;
    case FailureMode::FailOnNullPtr:
    case FailureMode::FailOnInvalidRef:
      return ABIType::General;
    default:
      MOZ_CRASH("unexpected failure mode");
  }
}

ABIType ToABIType(MIRType type) {
  switch (type) {
    case MIRType::None:
    case MIRType::Int32:
      return ABIType::Int32;
    case MIRType::Int64:
      return ABIType::Int64;
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
      return ABIType::General;
    case MIRType::Float32:
      return ABIType::Float32;
    case MIRType::Double:
      return ABIType::Float64;
    default:
      MOZ_CRASH("unexpected type");
  }
}

ABIFunctionType ToABIType(const SymbolicAddressSignature& sig) {
  MOZ_ASSERT_IF(sig.failureMode != FailureMode::Infallible,
                ToABIType(sig.failureMode) == ToABIType(sig.retType));
  int abiType = 0;
  for (int i = 0; i < sig.numArgs; i++) {
    abiType <<= ABITypeArgShift;
    abiType |= uint32_t(ToABIType(sig.argTypes[i]));
  }
  abiType <<= ABITypeArgShift;
  abiType |= uint32_t(ToABIType(sig.retType));
  return ABIFunctionType(abiType);
}
#endif

// ============================================================================
// WebAssembly builtin C++ functions called from wasm code to implement internal
// wasm operations: implementations.

#if defined(JS_CODEGEN_ARM)
extern "C" {

extern MOZ_EXPORT int64_t __aeabi_idivmod(int, int);

extern MOZ_EXPORT int64_t __aeabi_uidivmod(int, int);
}
#endif

// This utility function can only be called for builtins that are called
// directly from wasm code.
static JitActivation* CallingActivation(JSContext* cx) {
  Activation* act = cx->activation();
  MOZ_ASSERT(act->asJit()->hasWasmExitFP());
  return act->asJit();
}

template <typename Fn, typename... Ts>
static bool ForwardToMainStack(Fn fn, JSContext* cx, Ts... args) {
#ifdef ENABLE_WASM_JSPI
  if (IsSuspendableStackActive(cx)) {
    struct InvokeContext {
      bool (*fn)(JSContext*, Ts...);
      JSContext* cx;
      std::tuple<Ts...> args;
      static bool Run(InvokeContext* data) {
        return data->fn(data->cx, std::get<Ts>(data->args)...);
      }
    } data = {fn, cx, std::make_tuple(args...)};
    return CallOnMainStack(
        cx, reinterpret_cast<CallOnMainStackFn>(InvokeContext::Run), &data);
  }
#endif
  return fn(cx, args...);
}

static bool WasmHandleDebugTrap() {
  JSContext* cx = TlsContext.get();  // Cold code
  JitActivation* activation = CallingActivation(cx);
  Frame* fp = activation->wasmExitFP();
  Instance* instance = GetNearestEffectiveInstance(fp);
  const Code& code = instance->code();
  MOZ_ASSERT(code.debugEnabled());

  // The debug trap stub is the innermost frame. It's return address is the
  // actual trap site.
  CallSite site;
  MOZ_ALWAYS_TRUE(code.lookupCallSite(fp->returnAddress(), &site));

  // Advance to the actual trapping frame.
  fp = fp->wasmCaller();
  DebugFrame* debugFrame = DebugFrame::from(fp);

  if (site.kind() == CallSiteKind::EnterFrame) {
    if (!instance->debug().enterFrameTrapsEnabled()) {
      return true;
    }
    debugFrame->setIsDebuggee();
    debugFrame->observe(cx);
    if (!ForwardToMainStack(DebugAPI::onEnterFrame, cx,
                            js::AbstractFramePtr(debugFrame))) {
      if (cx->isPropagatingForcedReturn()) {
        cx->clearPropagatingForcedReturn();
        // Ignoring forced return because changing code execution order is
        // not yet implemented in the wasm baseline.
        // TODO properly handle forced return and resume wasm execution.
        JS_ReportErrorASCII(cx,
                            "Unexpected resumption value from onEnterFrame");
      }
      return false;
    }
    return true;
  }
  if (site.kind() == CallSiteKind::LeaveFrame ||
      site.kind() == CallSiteKind::CollapseFrame) {
    if (site.kind() == CallSiteKind::LeaveFrame &&
        !debugFrame->updateReturnJSValue(cx)) {
      return false;
    }
    if (site.kind() == CallSiteKind::CollapseFrame) {
      debugFrame->discardReturnJSValue();
    }
    bool ok = ForwardToMainStack(DebugAPI::onLeaveFrame, cx,
                                 js::AbstractFramePtr(debugFrame),
                                 (const jsbytecode*)nullptr, true);
    debugFrame->leave(cx);
    return ok;
  }

  DebugState& debug = instance->debug();
  MOZ_ASSERT(debug.hasBreakpointTrapAtOffset(site.lineOrBytecode()));
  if (debug.stepModeEnabled(debugFrame->funcIndex())) {
    if (!ForwardToMainStack(DebugAPI::onSingleStep, cx)) {
      if (cx->isPropagatingForcedReturn()) {
        cx->clearPropagatingForcedReturn();
        // TODO properly handle forced return.
        JS_ReportErrorASCII(cx,
                            "Unexpected resumption value from onSingleStep");
      }
      return false;
    }
  }
  if (debug.hasBreakpointSite(site.lineOrBytecode())) {
    if (!ForwardToMainStack(DebugAPI::onTrap, cx)) {
      if (cx->isPropagatingForcedReturn()) {
        cx->clearPropagatingForcedReturn();
        // TODO properly handle forced return.
        JS_ReportErrorASCII(
            cx, "Unexpected resumption value from breakpoint handler");
      }
      return false;
    }
  }
  return true;
}

// Check if the pending exception, if any, is catchable by wasm.
static WasmExceptionObject* GetOrWrapWasmException(JitActivation* activation,
                                                   JSContext* cx) {
  if (!cx->isExceptionPending()) {
    return nullptr;
  }

  // Traps are generally not catchable as wasm exceptions. The only case in
  // which they are catchable is for Trap::ThrowReported, which the wasm
  // compiler uses to throw exceptions and is the source of exceptions from C++.
  if (activation->isWasmTrapping() &&
      activation->wasmTrapData().trap != Trap::ThrowReported) {
    return nullptr;
  }

  if (cx->isThrowingOverRecursed() || cx->isThrowingOutOfMemory()) {
    return nullptr;
  }

  // Write the exception out here to exn to avoid having to get the pending
  // exception and checking for OOM multiple times.
  RootedValue exn(cx);
  if (cx->getPendingException(&exn)) {
    // Check if a JS exception originated from a wasm trap.
    if (exn.isObject() && exn.toObject().is<ErrorObject>()) {
      ErrorObject& err = exn.toObject().as<ErrorObject>();
      if (err.fromWasmTrap()) {
        return nullptr;
      }
    }

    // Get or create a wasm exception to represent the pending exception
    Rooted<WasmExceptionObject*> wasmExn(cx);
    if (exn.isObject() && exn.toObject().is<WasmExceptionObject>()) {
      // We're already throwing a wasm exception
      wasmExn = &exn.toObject().as<WasmExceptionObject>();

      // If wasm is rethrowing a wrapped JS value, then set the pending
      // exception on cx to be the wrapped value. This will ensure that if we
      // unwind out of wasm the wrapper exception will not escape.
      //
      // We also do this here, and not at the end of wasm::HandleThrow so that
      // any DebugAPI calls see the wrapped JS value, not the wrapper
      // exception.
      if (wasmExn->isWrappedJSValue()) {
        // Re-use exn to avoid needing a new root
        exn = wasmExn->wrappedJSValue();
        cx->setPendingException(exn, nullptr);
      }
    } else {
      // Wrap all thrown JS values in a wasm exception. This is required so
      // that all exceptions have tags, and the 'null' JS value becomes a
      // non-null wasm exception.
      wasmExn = WasmExceptionObject::wrapJSValue(cx, exn);
    }

    if (wasmExn) {
      return wasmExn;
    }
  }

  MOZ_ASSERT(cx->isThrowingOutOfMemory());
  return nullptr;
}

static const wasm::TryNote* FindNonDelegateTryNote(
    const wasm::Code& code, const uint8_t* pc, const CodeBlock** codeBlock) {
  const wasm::TryNote* tryNote = code.lookupTryNote((void*)pc, codeBlock);
  while (tryNote && tryNote->isDelegate()) {
    pc = (*codeBlock)->base() + tryNote->delegateOffset();
    const wasm::TryNote* delegateTryNote =
        code.lookupTryNote((void*)pc, codeBlock);
    MOZ_RELEASE_ASSERT(delegateTryNote == nullptr ||
                       delegateTryNote->tryBodyBegin() <
                           tryNote->tryBodyBegin());
    tryNote = delegateTryNote;
  }
  return tryNote;
}

// Request tier-2 compilation for the calling wasm function.

static void WasmHandleRequestTierUp(Instance* instance) {
  JSContext* cx = instance->cx();

  // Don't turn this into a release assert - TlsContext.get() can be expensive.
  MOZ_ASSERT(cx == TlsContext.get());

  // Neither this routine nor the stub that calls it make any attempt to
  // communicate roots to the GC.  This is OK because we will only be
  // compiling code here, which shouldn't GC.  Nevertheless ..
  JS::AutoAssertNoGC nogc(cx);

  JitActivation* activation = CallingActivation(cx);
  Frame* fp = activation->wasmExitFP();

  // Similarly, don't turn this into a release assert.
  MOZ_ASSERT(instance == GetNearestEffectiveInstance(fp));

  // Figure out the requesting funcIndex.  We could add a field to the
  // Instance and, in the slow path of BaseCompiler::addHotnessCheck, write it
  // in there.  That would avoid having to call LookupCodeBlock here, but (1)
  // LookupCodeBlock is pretty cheap and (2) this would make hotness checks
  // larger.  It doesn't seem like a worthwhile tradeoff.
  void* resumePC = fp->returnAddress();
  const CodeRange* codeRange;
  const CodeBlock* codeBlock = LookupCodeBlock(resumePC, &codeRange);
  MOZ_RELEASE_ASSERT(codeBlock && codeRange);

  uint32_t funcIndex = codeRange->funcIndex();

  // See BaseCompiler::addHotnessCheck for rationale.  If this fails, and
  // `counter` is a very large negative number (close to -2^31), it may be that
  // a hotness check didn't have its step patched in.
  int32_t counter = instance->readHotnessCounter(funcIndex);
  MOZ_RELEASE_ASSERT(counter >= -127 && counter <= -1);

  // Function `funcIndex` is requesting tier-up.  This can go one of three ways:
  // - the request is a duplicate -- ignore
  // - tier-up compilation succeeds -- we hope
  // - tier-up compilation fails (eg, OOMs).
  //   We have no feasible way to recover.
  //
  // Regardless of the outcome, we want to defer duplicate requests as long as
  // possible.  So set the counter to "infinity" right now.
  instance->resetHotnessCounter(funcIndex);

  // Submit the collected profiling information for call_ref to be available
  // for compilation.
  instance->submitCallRefHints(funcIndex);

  if (JS::Prefs::wasm_lazy_tiering_synchronous()) {
    UniqueChars error;
    UniqueCharsVector warnings;
    mozilla::Atomic<bool> cancelled(false);
    bool ok = CompilePartialTier2(*codeBlock->code, funcIndex, &error,
                                  &warnings, &cancelled);
    ReportTier2ResultsOffThread(cancelled, ok, mozilla::Some(funcIndex),
                                codeBlock->code->codeMeta().scriptedCaller(),
                                error, warnings);
    return;
  }

  // Try to Ion-compile it.  Note that `ok == true` signifies either
  // "duplicate request" or "not a duplicate, and compilation succeeded".
  bool ok = codeBlock->code->requestTierUp(funcIndex);

  // If compilation failed, there's no feasible way to recover. We use the
  // 'off thread' logging mechanism to avoid possibly triggering a GC.
  if (!ok) {
    wasm::LogOffThread("Failed to tier-up function=%d in instance=%p.",
                       funcIndex, instance);
  }
}

// Unwind the activation in response to a thrown exception. This function is
// responsible for notifying the debugger of each unwound frame.
//
// This function will look for try-catch handlers and, if not trapping or
// throwing an uncatchable exception, will write the handler info in |*rfe|.
//
// If no try-catch handler is found, return to the caller to continue unwinding
// JS JIT frames.
void wasm::HandleExceptionWasm(JSContext* cx, JitFrameIter& iter,
                               jit::ResumeFromException* rfe) {
  MOZ_ASSERT(iter.isWasm());
  MOZ_ASSERT(CallingActivation(cx) == iter.activation());
  MOZ_ASSERT(cx->activation()->asJit()->hasWasmExitFP());
  MOZ_ASSERT(rfe->kind == ExceptionResumeKind::EntryFrame);

  // WasmFrameIter iterates down wasm frames in the activation starting at
  // JitActivation::wasmExitFP(). Calling WasmFrameIter::startUnwinding pops
  // JitActivation::wasmExitFP() once each time WasmFrameIter is incremented,
  // ultimately leaving no wasm exit FP when the WasmFrameIter is done(). This
  // is necessary to prevent a wasm::DebugFrame from being observed again after
  // we just called onLeaveFrame (which would lead to the frame being re-added
  // to the map of live frames, right as it becomes trash).

#ifdef DEBUG
  auto onExit = mozilla::MakeScopeExit([cx] {
    MOZ_ASSERT(!cx->activation()->asJit()->isWasmTrapping(),
               "unwinding clears the trapping state");
    MOZ_ASSERT(!cx->activation()->asJit()->hasWasmExitFP(),
               "unwinding leaves no wasm exit fp");
  });
#endif

  MOZ_ASSERT(!iter.done());

  // Make the iterator adjust the JitActivation so that each popped frame
  // will not be visible to other FrameIters that are created while we're
  // unwinding (such as by debugging code).
  iter.asWasm().setIsLeavingFrames();

  JitActivation* activation = CallingActivation(cx);
  Rooted<WasmExceptionObject*> wasmExn(cx,
                                       GetOrWrapWasmException(activation, cx));

  for (; !iter.done() && iter.isWasm(); ++iter) {
    // Wasm code can enter same-compartment realms, so reset cx->realm to
    // this frame's realm.
    WasmFrameIter& wasmFrame = iter.asWasm();
    cx->setRealmForJitExceptionHandler(wasmFrame.instance()->realm());

    // Only look for an exception handler if there's a catchable exception.
    if (wasmExn) {
      const wasm::Code& code = wasmFrame.instance()->code();
      const uint8_t* pc = wasmFrame.resumePCinCurrentFrame();
      const wasm::CodeBlock* codeBlock = nullptr;
      const wasm::TryNote* tryNote =
          FindNonDelegateTryNote(code, pc, &codeBlock);

      if (tryNote) {
        // Skip tryNote if pc is at return stub generated by
        // wasmCollapseFrameSlow.
        CallSite site;
        if (code.lookupCallSite((void*)pc, &site) &&
            site.kind() == CallSiteKind::ReturnStub) {
          continue;
        }

        cx->clearPendingException();
        wasmFrame.instance()->setPendingException(wasmExn);

        rfe->kind = ExceptionResumeKind::WasmCatch;
        rfe->framePointer = (uint8_t*)wasmFrame.frame();
        rfe->instance = wasmFrame.instance();

        rfe->stackPointer =
            (uint8_t*)(rfe->framePointer - tryNote->landingPadFramePushed());
        rfe->target = codeBlock->base() + tryNote->landingPadEntryPoint();

        // Make sure to clear trapping state if we got here due to a trap.
        if (activation->isWasmTrapping()) {
          activation->finishWasmTrap();
        }
        activation->setWasmExitFP(nullptr);
        return;
      }
    }

    if (!wasmFrame.debugEnabled()) {
      continue;
    }

    DebugFrame* frame = wasmFrame.debugFrame();
    frame->clearReturnJSValue();

    // Assume ResumeMode::Terminate if no exception is pending --
    // no onExceptionUnwind handlers must be fired.
    if (cx->isExceptionPending()) {
      if (!ForwardToMainStack(DebugAPI::onExceptionUnwind, cx,
                              AbstractFramePtr(frame))) {
        if (cx->isPropagatingForcedReturn()) {
          cx->clearPropagatingForcedReturn();
          // Unexpected trap return -- raising error since throw recovery
          // is not yet implemented in the wasm baseline.
          // TODO properly handle forced return and resume wasm execution.
          JS_ReportErrorASCII(
              cx, "Unexpected resumption value from onExceptionUnwind");
          wasmExn = nullptr;
        }
      }
    }

    bool ok =
        ForwardToMainStack(DebugAPI::onLeaveFrame, cx, AbstractFramePtr(frame),
                           (const jsbytecode*)nullptr, false);
    if (ok) {
      // Unexpected success from the handler onLeaveFrame -- raising error
      // since throw recovery is not yet implemented in the wasm baseline.
      // TODO properly handle success and resume wasm execution.
      JS_ReportErrorASCII(cx, "Unexpected success from onLeaveFrame");
      wasmExn = nullptr;
    }
    frame->leave(cx);
  }

  // Assert that any pending exception escaping to non-wasm code is not a
  // wrapper exception object
#ifdef DEBUG
  if (cx->isExceptionPending()) {
    Rooted<Value> pendingException(cx, cx->getPendingExceptionUnwrapped());
    MOZ_ASSERT_IF(pendingException.isObject() &&
                      pendingException.toObject().is<WasmExceptionObject>(),
                  !pendingException.toObject()
                       .as<WasmExceptionObject>()
                       .isWrappedJSValue());
  }
#endif
}

static void* WasmHandleThrow(jit::ResumeFromException* rfe) {
  jit::HandleException(rfe);
  // Return a pointer to the exception handler trampoline code to jump to from
  // the throw stub.
  JSContext* cx = TlsContext.get();
  return cx->runtime()->jitRuntime()->getExceptionTailReturnValueCheck().value;
}

// Has the same return-value convention as HandleTrap().
static void* CheckInterrupt(JSContext* cx, JitActivation* activation) {
  ResetInterruptState(cx);

  if (!CheckForInterrupt(cx)) {
    return nullptr;
  }

  void* resumePC = activation->wasmTrapData().resumePC;
  activation->finishWasmTrap();
  return resumePC;
}

// The calling convention between this function and its caller in the stub
// generated by GenerateTrapExit() is:
//   - return nullptr if the stub should jump to the throw stub to unwind
//     the activation;
//   - return the (non-null) resumePC that should be jumped if execution should
//     resume after the trap.
static void* WasmHandleTrap() {
  JSContext* cx = TlsContext.get();  // Cold code
  JitActivation* activation = CallingActivation(cx);

  switch (activation->wasmTrapData().trap) {
    case Trap::Unreachable: {
      ReportTrapError(cx, JSMSG_WASM_UNREACHABLE);
      return nullptr;
    }
    case Trap::IntegerOverflow: {
      ReportTrapError(cx, JSMSG_WASM_INTEGER_OVERFLOW);
      return nullptr;
    }
    case Trap::InvalidConversionToInteger: {
      ReportTrapError(cx, JSMSG_WASM_INVALID_CONVERSION);
      return nullptr;
    }
    case Trap::IntegerDivideByZero: {
      ReportTrapError(cx, JSMSG_WASM_INT_DIVIDE_BY_ZERO);
      return nullptr;
    }
    case Trap::IndirectCallToNull: {
      ReportTrapError(cx, JSMSG_WASM_IND_CALL_TO_NULL);
      return nullptr;
    }
    case Trap::IndirectCallBadSig: {
      ReportTrapError(cx, JSMSG_WASM_IND_CALL_BAD_SIG);
      return nullptr;
    }
    case Trap::NullPointerDereference: {
      ReportTrapError(cx, JSMSG_WASM_DEREF_NULL);
      return nullptr;
    }
    case Trap::BadCast: {
      ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
      return nullptr;
    }
    case Trap::OutOfBounds: {
      ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
      return nullptr;
    }
    case Trap::UnalignedAccess: {
      ReportTrapError(cx, JSMSG_WASM_UNALIGNED_ACCESS);
      return nullptr;
    }
    case Trap::CheckInterrupt:
      return CheckInterrupt(cx, activation);
    case Trap::StackOverflow: {
      // Instance::setInterrupt() causes a fake stack overflow. Since
      // Instance::setInterrupt() is called racily, it's possible for a real
      // stack overflow to trap, followed by a racy call to setInterrupt().
      // Thus, we must check for a real stack overflow first before we
      // CheckInterrupt() and possibly resume execution.
      AutoCheckRecursionLimit recursion(cx);
      if (!recursion.check(cx)) {
        return nullptr;
      }
      if (activation->wasmExitInstance()->isInterrupted()) {
        return CheckInterrupt(cx, activation);
      }
      ReportTrapError(cx, JSMSG_OVER_RECURSED);
      return nullptr;
    }
    case Trap::ThrowReported:
      // Error was already reported under another name.
      return nullptr;
    case Trap::Limit:
      break;
  }

  MOZ_CRASH("unexpected trap");
}

static void WasmReportV128JSCall() {
  JSContext* cx = TlsContext.get();  // Cold code
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_VAL_TYPE);
}

static int32_t CoerceInPlace_ToInt32(Value* rawVal) {
  JSContext* cx = TlsContext.get();  // Cold code

  int32_t i32;
  RootedValue val(cx, *rawVal);
  if (!ToInt32(cx, val, &i32)) {
    *rawVal = PoisonedObjectValue(0x42);
    return false;
  }

  *rawVal = Int32Value(i32);
  return true;
}

static int32_t CoerceInPlace_ToBigInt(Value* rawVal) {
  JSContext* cx = TlsContext.get();  // Cold code

  RootedValue val(cx, *rawVal);
  BigInt* bi = ToBigInt(cx, val);
  if (!bi) {
    *rawVal = PoisonedObjectValue(0x43);
    return false;
  }

  *rawVal = BigIntValue(bi);
  return true;
}

static int32_t CoerceInPlace_ToNumber(Value* rawVal) {
  JSContext* cx = TlsContext.get();  // Cold code

  double dbl;
  RootedValue val(cx, *rawVal);
  if (!ToNumber(cx, val, &dbl)) {
    *rawVal = PoisonedObjectValue(0x42);
    return false;
  }

  *rawVal = DoubleValue(dbl);
  return true;
}

static void* BoxValue_Anyref(Value* rawVal) {
  JSContext* cx = TlsContext.get();  // Cold code
  RootedValue val(cx, *rawVal);
  RootedAnyRef result(cx, AnyRef::null());
  if (!AnyRef::fromJSValue(cx, val, &result)) {
    return nullptr;
  }
  return result.get().forCompiledCode();
}

static int32_t CoerceInPlace_JitEntry(int funcIndex, Instance* instance,
                                      Value* argv) {
  JSContext* cx = TlsContext.get();  // Cold code

  const Code& code = instance->code();
  const FuncType& funcType = code.codeMeta().getFuncType(funcIndex);

  for (size_t i = 0; i < funcType.args().length(); i++) {
    HandleValue arg = HandleValue::fromMarkedLocation(&argv[i]);
    switch (funcType.args()[i].kind()) {
      case ValType::I32: {
        int32_t i32;
        if (!ToInt32(cx, arg, &i32)) {
          return false;
        }
        argv[i] = Int32Value(i32);
        break;
      }
      case ValType::I64: {
        // In this case we store a BigInt value as there is no value type
        // corresponding directly to an I64. The conversion to I64 happens
        // in the JIT entry stub.
        BigInt* bigint = ToBigInt(cx, arg);
        if (!bigint) {
          return false;
        }
        argv[i] = BigIntValue(bigint);
        break;
      }
      case ValType::F32:
      case ValType::F64: {
        double dbl;
        if (!ToNumber(cx, arg, &dbl)) {
          return false;
        }
        // No need to convert double-to-float for f32, it's done inline
        // in the wasm stub later.
        argv[i] = DoubleValue(dbl);
        break;
      }
      case ValType::Ref: {
        // Guarded against by temporarilyUnsupportedReftypeForEntry()
        MOZ_RELEASE_ASSERT(funcType.args()[i].refType().isExtern());
        // Perform any fallible boxing that may need to happen so that the JIT
        // code does not need to.
        if (AnyRef::valueNeedsBoxing(arg)) {
          JSObject* boxedValue = AnyRef::boxValue(cx, arg);
          if (!boxedValue) {
            return false;
          }
          argv[i] = ObjectOrNullValue(boxedValue);
        }
        break;
      }
      case ValType::V128: {
        // Guarded against by hasV128ArgOrRet()
        MOZ_CRASH("unexpected input argument in CoerceInPlace_JitEntry");
      }
      default: {
        MOZ_CRASH("unexpected input argument in CoerceInPlace_JitEntry");
      }
    }
  }

  return true;
}

// Allocate a BigInt without GC, corresponds to the similar VMFunction.
static BigInt* AllocateBigIntTenuredNoGC() {
  JSContext* cx = TlsContext.get();  // Cold code (the caller is elaborate)

  return cx->newCell<BigInt, NoGC>(gc::Heap::Tenured);
}

static int64_t DivI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi,
                      uint32_t y_lo) {
  int64_t x = ((uint64_t)x_hi << 32) + x_lo;
  int64_t y = ((uint64_t)y_hi << 32) + y_lo;
  MOZ_ASSERT(x != INT64_MIN || y != -1);
  MOZ_ASSERT(y != 0);
  return x / y;
}

static int64_t UDivI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi,
                       uint32_t y_lo) {
  uint64_t x = ((uint64_t)x_hi << 32) + x_lo;
  uint64_t y = ((uint64_t)y_hi << 32) + y_lo;
  MOZ_ASSERT(y != 0);
  return int64_t(x / y);
}

static int64_t ModI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi,
                      uint32_t y_lo) {
  int64_t x = ((uint64_t)x_hi << 32) + x_lo;
  int64_t y = ((uint64_t)y_hi << 32) + y_lo;
  MOZ_ASSERT(x != INT64_MIN || y != -1);
  MOZ_ASSERT(y != 0);
  return x % y;
}

static int64_t UModI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi,
                       uint32_t y_lo) {
  uint64_t x = ((uint64_t)x_hi << 32) + x_lo;
  uint64_t y = ((uint64_t)y_hi << 32) + y_lo;
  MOZ_ASSERT(y != 0);
  return int64_t(x % y);
}

static int64_t TruncateDoubleToInt64(double input) {
  // Note: INT64_MAX is not representable in double. It is actually
  // INT64_MAX + 1.  Therefore also sending the failure value.
  if (input >= double(INT64_MAX) || input < double(INT64_MIN) ||
      std::isnan(input)) {
    return int64_t(0x8000000000000000);
  }
  return int64_t(input);
}

static uint64_t TruncateDoubleToUint64(double input) {
  // Note: UINT64_MAX is not representable in double. It is actually
  // UINT64_MAX + 1.  Therefore also sending the failure value.
  if (input >= double(UINT64_MAX) || input <= -1.0 || std::isnan(input)) {
    return int64_t(0x8000000000000000);
  }
  return uint64_t(input);
}

static int64_t SaturatingTruncateDoubleToInt64(double input) {
  // Handle in-range values (except INT64_MIN).
  if (fabs(input) < -double(INT64_MIN)) {
    return int64_t(input);
  }
  // Handle NaN.
  if (std::isnan(input)) {
    return 0;
  }
  // Handle positive overflow.
  if (input > 0) {
    return INT64_MAX;
  }
  // Handle negative overflow.
  return INT64_MIN;
}

static uint64_t SaturatingTruncateDoubleToUint64(double input) {
  // Handle positive overflow.
  if (input >= -double(INT64_MIN) * 2.0) {
    return UINT64_MAX;
  }
  // Handle in-range values.
  if (input > -1.0) {
    return uint64_t(input);
  }
  // Handle NaN and negative overflow.
  return 0;
}

static double Int64ToDouble(int32_t x_hi, uint32_t x_lo) {
  int64_t x = int64_t((uint64_t(x_hi) << 32)) + int64_t(x_lo);
  return double(x);
}

static float Int64ToFloat32(int32_t x_hi, uint32_t x_lo) {
  int64_t x = int64_t((uint64_t(x_hi) << 32)) + int64_t(x_lo);
  return float(x);
}

static double Uint64ToDouble(int32_t x_hi, uint32_t x_lo) {
  uint64_t x = (uint64_t(x_hi) << 32) + uint64_t(x_lo);
  return double(x);
}

static float Uint64ToFloat32(int32_t x_hi, uint32_t x_lo) {
  uint64_t x = (uint64_t(x_hi) << 32) + uint64_t(x_lo);
  return float(x);
}

static void WasmArrayMemMove(uint8_t* destArrayData, uint32_t destIndex,
                             const uint8_t* srcArrayData, uint32_t srcIndex,
                             uint32_t elementSize, uint32_t count) {
  AutoUnsafeCallWithABI unsafe;
  memmove(&destArrayData[size_t(elementSize) * destIndex],
          &srcArrayData[size_t(elementSize) * srcIndex],
          size_t(elementSize) * count);
}

static void WasmArrayRefsMove(GCPtr<AnyRef>* destArrayData, uint32_t destIndex,
                              AnyRef* srcArrayData, uint32_t srcIndex,
                              uint32_t count) {
  AutoUnsafeCallWithABI unsafe;
  GCPtr<AnyRef>* dstBegin = destArrayData + destIndex;
  AnyRef* srcBegin = srcArrayData + srcIndex;
  // The std::copy performs GCPtr::set() operation under the hood.
  if (uintptr_t(dstBegin) < uintptr_t(srcBegin)) {
    std::copy(srcBegin, srcBegin + count, dstBegin);
  } else {
    std::copy_backward(srcBegin, srcBegin + count, dstBegin + count);
  }
}

template <class F>
static inline void* FuncCast(F* funcPtr, ABIFunctionType abiType) {
  void* pf = JS_FUNC_TO_DATA_PTR(void*, funcPtr);
#ifdef JS_SIMULATOR
  pf = Simulator::RedirectNativeFunction(pf, abiType);
#endif
  return pf;
}

#ifdef WASM_CODEGEN_DEBUG
void wasm::PrintI32(int32_t val) { fprintf(stderr, "i32(%d) ", val); }

void wasm::PrintPtr(uint8_t* val) { fprintf(stderr, "ptr(%p) ", val); }

void wasm::PrintF32(float val) { fprintf(stderr, "f32(%f) ", val); }

void wasm::PrintF64(double val) { fprintf(stderr, "f64(%lf) ", val); }

void wasm::PrintText(const char* out) { fprintf(stderr, "%s", out); }
#endif

void* wasm::AddressOf(SymbolicAddress imm, ABIFunctionType* abiType) {
  // See NeedsBuiltinThunk for a classification of the different names here.
  switch (imm) {
    case SymbolicAddress::HandleDebugTrap:
      *abiType = Args_General0;
      return FuncCast(WasmHandleDebugTrap, *abiType);
    case SymbolicAddress::HandleRequestTierUp:
      *abiType = Args_General1;
      return FuncCast(WasmHandleRequestTierUp, *abiType);
    case SymbolicAddress::HandleThrow:
      *abiType = Args_General1;
      return FuncCast(WasmHandleThrow, *abiType);
    case SymbolicAddress::HandleTrap:
      *abiType = Args_General0;
      return FuncCast(WasmHandleTrap, *abiType);
    case SymbolicAddress::ReportV128JSCall:
      *abiType = Args_General0;
      return FuncCast(WasmReportV128JSCall, *abiType);
    case SymbolicAddress::CallImport_General:
      *abiType = Args_Int32_GeneralInt32Int32General;
      return FuncCast(Instance::callImport_general, *abiType);
    case SymbolicAddress::CoerceInPlace_ToInt32:
      *abiType = Args_General1;
      return FuncCast(CoerceInPlace_ToInt32, *abiType);
    case SymbolicAddress::CoerceInPlace_ToBigInt:
      *abiType = Args_General1;
      return FuncCast(CoerceInPlace_ToBigInt, *abiType);
    case SymbolicAddress::CoerceInPlace_ToNumber:
      *abiType = Args_General1;
      return FuncCast(CoerceInPlace_ToNumber, *abiType);
    case SymbolicAddress::CoerceInPlace_JitEntry:
      *abiType = Args_General3;
      return FuncCast(CoerceInPlace_JitEntry, *abiType);
    case SymbolicAddress::ToInt32:
      *abiType = Args_Int_Double;
      return FuncCast<int32_t(double)>(JS::ToInt32, *abiType);
    case SymbolicAddress::BoxValue_Anyref:
      *abiType = Args_General1;
      return FuncCast(BoxValue_Anyref, *abiType);
    case SymbolicAddress::AllocateBigInt:
      *abiType = Args_General0;
      return FuncCast(AllocateBigIntTenuredNoGC, *abiType);
    case SymbolicAddress::DivI64:
      *abiType = Args_Int64_Int32Int32Int32Int32;
      return FuncCast(DivI64, *abiType);
    case SymbolicAddress::UDivI64:
      *abiType = Args_Int64_Int32Int32Int32Int32;
      return FuncCast(UDivI64, *abiType);
    case SymbolicAddress::ModI64:
      *abiType = Args_Int64_Int32Int32Int32Int32;
      return FuncCast(ModI64, *abiType);
    case SymbolicAddress::UModI64:
      *abiType = Args_Int64_Int32Int32Int32Int32;
      return FuncCast(UModI64, *abiType);
    case SymbolicAddress::TruncateDoubleToUint64:
      *abiType = Args_Int64_Double;
      return FuncCast(TruncateDoubleToUint64, *abiType);
    case SymbolicAddress::TruncateDoubleToInt64:
      *abiType = Args_Int64_Double;
      return FuncCast(TruncateDoubleToInt64, *abiType);
    case SymbolicAddress::SaturatingTruncateDoubleToUint64:
      *abiType = Args_Int64_Double;
      return FuncCast(SaturatingTruncateDoubleToUint64, *abiType);
    case SymbolicAddress::SaturatingTruncateDoubleToInt64:
      *abiType = Args_Int64_Double;
      return FuncCast(SaturatingTruncateDoubleToInt64, *abiType);
    case SymbolicAddress::Uint64ToDouble:
      *abiType = Args_Double_IntInt;
      return FuncCast(Uint64ToDouble, *abiType);
    case SymbolicAddress::Uint64ToFloat32:
      *abiType = Args_Float32_IntInt;
      return FuncCast(Uint64ToFloat32, *abiType);
    case SymbolicAddress::Int64ToDouble:
      *abiType = Args_Double_IntInt;
      return FuncCast(Int64ToDouble, *abiType);
    case SymbolicAddress::Int64ToFloat32:
      *abiType = Args_Float32_IntInt;
      return FuncCast(Int64ToFloat32, *abiType);
#if defined(JS_CODEGEN_ARM)
    case SymbolicAddress::aeabi_idivmod:
      *abiType = Args_Int64_GeneralGeneral;
      return FuncCast(__aeabi_idivmod, *abiType);
    case SymbolicAddress::aeabi_uidivmod:
      *abiType = Args_Int64_GeneralGeneral;
      return FuncCast(__aeabi_uidivmod, *abiType);
#endif
    case SymbolicAddress::ModD:
      *abiType = Args_Double_DoubleDouble;
      return FuncCast(NumberMod, *abiType);
    case SymbolicAddress::SinNativeD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(sin, *abiType);
    case SymbolicAddress::SinFdlibmD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_sin, *abiType);
    case SymbolicAddress::CosNativeD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(cos, *abiType);
    case SymbolicAddress::CosFdlibmD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_cos, *abiType);
    case SymbolicAddress::TanNativeD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(tan, *abiType);
    case SymbolicAddress::TanFdlibmD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_tan, *abiType);
    case SymbolicAddress::ASinD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_asin, *abiType);
    case SymbolicAddress::ACosD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_acos, *abiType);
    case SymbolicAddress::ATanD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_atan, *abiType);
    case SymbolicAddress::CeilD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_ceil, *abiType);
    case SymbolicAddress::CeilF:
      *abiType = Args_Float32_Float32;
      return FuncCast<float(float)>(fdlibm_ceilf, *abiType);
    case SymbolicAddress::FloorD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_floor, *abiType);
    case SymbolicAddress::FloorF:
      *abiType = Args_Float32_Float32;
      return FuncCast<float(float)>(fdlibm_floorf, *abiType);
    case SymbolicAddress::TruncD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_trunc, *abiType);
    case SymbolicAddress::TruncF:
      *abiType = Args_Float32_Float32;
      return FuncCast<float(float)>(fdlibm_truncf, *abiType);
    case SymbolicAddress::NearbyIntD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_nearbyint, *abiType);
    case SymbolicAddress::NearbyIntF:
      *abiType = Args_Float32_Float32;
      return FuncCast<float(float)>(fdlibm_nearbyintf, *abiType);
    case SymbolicAddress::ExpD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_exp, *abiType);
    case SymbolicAddress::LogD:
      *abiType = Args_Double_Double;
      return FuncCast<double(double)>(fdlibm_log, *abiType);
    case SymbolicAddress::PowD:
      *abiType = Args_Double_DoubleDouble;
      return FuncCast(ecmaPow, *abiType);
    case SymbolicAddress::ATan2D:
      *abiType = Args_Double_DoubleDouble;
      return FuncCast(ecmaAtan2, *abiType);
    case SymbolicAddress::ArrayMemMove:
      *abiType = Args_Void_GeneralInt32GeneralInt32Int32Int32;
      return FuncCast(WasmArrayMemMove, *abiType);
    case SymbolicAddress::ArrayRefsMove:
      *abiType = Args_Void_GeneralInt32GeneralInt32Int32;
      return FuncCast(WasmArrayRefsMove, *abiType);

    case SymbolicAddress::MemoryGrowM32:
      *abiType = Args_Int32_GeneralInt32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemoryGrowM32));
      return FuncCast(Instance::memoryGrow_m32, *abiType);
    case SymbolicAddress::MemoryGrowM64:
      *abiType = Args_Int64_GeneralInt64Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemoryGrowM64));
      return FuncCast(Instance::memoryGrow_m64, *abiType);
    case SymbolicAddress::MemorySizeM32:
      *abiType = Args_Int32_GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemorySizeM32));
      return FuncCast(Instance::memorySize_m32, *abiType);
    case SymbolicAddress::MemorySizeM64:
      *abiType = Args_Int64_GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemorySizeM64));
      return FuncCast(Instance::memorySize_m64, *abiType);
    case SymbolicAddress::WaitI32M32:
      *abiType = Args_Int32_GeneralInt32Int32Int64Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigWaitI32M32));
      return FuncCast(Instance::wait_i32_m32, *abiType);
    case SymbolicAddress::WaitI32M64:
      *abiType = Args_Int32_GeneralInt64Int32Int64Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigWaitI32M64));
      return FuncCast(Instance::wait_i32_m64, *abiType);
    case SymbolicAddress::WaitI64M32:
      *abiType = Args_Int32_GeneralInt32Int64Int64Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigWaitI64M32));
      return FuncCast(Instance::wait_i64_m32, *abiType);
    case SymbolicAddress::WaitI64M64:
      *abiType = Args_Int32_GeneralInt64Int64Int64Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigWaitI64M64));
      return FuncCast(Instance::wait_i64_m64, *abiType);
    case SymbolicAddress::WakeM32:
      *abiType = Args_Int32_GeneralInt32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigWakeM32));
      return FuncCast(Instance::wake_m32, *abiType);
    case SymbolicAddress::WakeM64:
      *abiType = Args_Int32_GeneralInt64Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigWakeM64));
      return FuncCast(Instance::wake_m64, *abiType);
    case SymbolicAddress::MemCopyM32:
      *abiType = Args_Int32_GeneralInt32Int32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemCopyM32));
      return FuncCast(Instance::memCopy_m32, *abiType);
    case SymbolicAddress::MemCopySharedM32:
      *abiType = Args_Int32_GeneralInt32Int32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemCopySharedM32));
      return FuncCast(Instance::memCopyShared_m32, *abiType);
    case SymbolicAddress::MemCopyM64:
      *abiType = Args_Int32_GeneralInt64Int64Int64General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemCopyM64));
      return FuncCast(Instance::memCopy_m64, *abiType);
    case SymbolicAddress::MemCopySharedM64:
      *abiType = Args_Int32_GeneralInt64Int64Int64General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemCopySharedM64));
      return FuncCast(Instance::memCopyShared_m64, *abiType);
    case SymbolicAddress::MemCopyAny:
      *abiType = Args_Int32_GeneralInt64Int64Int64Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemCopyAny));
      return FuncCast(Instance::memCopy_any, *abiType);
    case SymbolicAddress::DataDrop:
      *abiType = Args_Int32_GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigDataDrop));
      return FuncCast(Instance::dataDrop, *abiType);
    case SymbolicAddress::MemFillM32:
      *abiType = Args_Int32_GeneralInt32Int32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemFillM32));
      return FuncCast(Instance::memFill_m32, *abiType);
    case SymbolicAddress::MemFillSharedM32:
      *abiType = Args_Int32_GeneralInt32Int32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemFillSharedM32));
      return FuncCast(Instance::memFillShared_m32, *abiType);
    case SymbolicAddress::MemFillM64:
      *abiType = Args_Int32_GeneralInt64Int32Int64General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemFillM64));
      return FuncCast(Instance::memFill_m64, *abiType);
    case SymbolicAddress::MemFillSharedM64:
      *abiType = Args_Int32_GeneralInt64Int32Int64General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemFillSharedM64));
      return FuncCast(Instance::memFillShared_m64, *abiType);
    case SymbolicAddress::MemDiscardM32:
      *abiType = Args_Int32_GeneralInt32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemDiscardM32));
      return FuncCast(Instance::memDiscard_m32, *abiType);
    case SymbolicAddress::MemDiscardSharedM32:
      *abiType = Args_Int32_GeneralInt32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemDiscardSharedM32));
      return FuncCast(Instance::memDiscardShared_m32, *abiType);
    case SymbolicAddress::MemDiscardM64:
      *abiType = Args_Int32_GeneralInt64Int64General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemDiscardM64));
      return FuncCast(Instance::memDiscard_m64, *abiType);
    case SymbolicAddress::MemDiscardSharedM64:
      *abiType = Args_Int32_GeneralInt64Int64General;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemDiscardSharedM64));
      return FuncCast(Instance::memDiscardShared_m64, *abiType);
    case SymbolicAddress::MemInitM32:
      *abiType = Args_Int32_GeneralInt32Int32Int32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemInitM32));
      return FuncCast(Instance::memInit_m32, *abiType);
    case SymbolicAddress::MemInitM64:
      *abiType = Args_Int32_GeneralInt64Int32Int32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigMemInitM64));
      return FuncCast(Instance::memInit_m64, *abiType);
    case SymbolicAddress::TableCopy:
      *abiType = Args_Int32_GeneralInt32Int32Int32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigTableCopy));
      return FuncCast(Instance::tableCopy, *abiType);
    case SymbolicAddress::ElemDrop:
      *abiType = Args_Int32_GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigElemDrop));
      return FuncCast(Instance::elemDrop, *abiType);
    case SymbolicAddress::TableFill:
      *abiType = Args_Int32_GeneralInt32GeneralInt32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigTableFill));
      return FuncCast(Instance::tableFill, *abiType);
    case SymbolicAddress::TableInit:
      *abiType = Args_Int32_GeneralInt32Int32Int32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigTableInit));
      return FuncCast(Instance::tableInit, *abiType);
    case SymbolicAddress::TableGet:
      *abiType = Args_General_GeneralInt32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigTableGet));
      return FuncCast(Instance::tableGet, *abiType);
    case SymbolicAddress::TableGrow:
      *abiType = Args_Int32_GeneralGeneralInt32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigTableGrow));
      return FuncCast(Instance::tableGrow, *abiType);
    case SymbolicAddress::TableSet:
      *abiType = Args_Int32_GeneralInt32GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigTableSet));
      return FuncCast(Instance::tableSet, *abiType);
    case SymbolicAddress::TableSize:
      *abiType = Args_Int32_GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigTableSize));
      return FuncCast(Instance::tableSize, *abiType);
    case SymbolicAddress::RefFunc:
      *abiType = Args_General_GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigRefFunc));
      return FuncCast(Instance::refFunc, *abiType);
    case SymbolicAddress::PostBarrierEdge:
      *abiType = Args_Int32_GeneralGeneral;
      MOZ_ASSERT(*abiType == ToABIType(SASigPostBarrierEdge));
      return FuncCast(Instance::postBarrierEdge, *abiType);
    case SymbolicAddress::PostBarrierEdgePrecise:
      *abiType = Args_Int32_GeneralGeneralGeneral;
      MOZ_ASSERT(*abiType == ToABIType(SASigPostBarrierEdgePrecise));
      return FuncCast(Instance::postBarrierEdgePrecise, *abiType);
    case SymbolicAddress::PostBarrierWholeCell:
      *abiType = Args_Int32_GeneralGeneral;
      MOZ_ASSERT(*abiType == ToABIType(SASigPostBarrierWholeCell));
      return FuncCast(Instance::postBarrierWholeCell, *abiType);
    case SymbolicAddress::StructNewIL_true:
      *abiType = Args_General_GeneralInt32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigStructNewIL_true));
      return FuncCast(Instance::structNewIL<true>, *abiType);
    case SymbolicAddress::StructNewIL_false:
      *abiType = Args_General_GeneralInt32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigStructNewIL_false));
      return FuncCast(Instance::structNewIL<false>, *abiType);
    case SymbolicAddress::StructNewOOL_true:
      *abiType = Args_General_GeneralInt32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigStructNewOOL_true));
      return FuncCast(Instance::structNewOOL<true>, *abiType);
    case SymbolicAddress::StructNewOOL_false:
      *abiType = Args_General_GeneralInt32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigStructNewOOL_false));
      return FuncCast(Instance::structNewOOL<false>, *abiType);
    case SymbolicAddress::ArrayNew_true:
      *abiType = Args_General_GeneralInt32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigArrayNew_true));
      return FuncCast(Instance::arrayNew<true>, *abiType);
    case SymbolicAddress::ArrayNew_false:
      *abiType = Args_General_GeneralInt32Int32General;
      MOZ_ASSERT(*abiType == ToABIType(SASigArrayNew_false));
      return FuncCast(Instance::arrayNew<false>, *abiType);
    case SymbolicAddress::ArrayNewData:
      *abiType = Args_General_GeneralInt32Int32Int32GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigArrayNewData));
      return FuncCast(Instance::arrayNewData, *abiType);
    case SymbolicAddress::ArrayNewElem:
      *abiType = Args_General_GeneralInt32Int32Int32GeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigArrayNewElem));
      return FuncCast(Instance::arrayNewElem, *abiType);
    case SymbolicAddress::ArrayInitData:
      *abiType = Args_Int32_GeneralGeneralInt32Int32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigArrayInitData));
      return FuncCast(Instance::arrayInitData, *abiType);
    case SymbolicAddress::ArrayInitElem:
      *abiType = Args_Int32_GeneralGeneralInt32Int32Int32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigArrayInitElem));
      return FuncCast(Instance::arrayInitElem, *abiType);
    case SymbolicAddress::ArrayCopy:
      *abiType = Args_Int32_GeneralGeneralInt32GeneralInt32Int32Int32;
      MOZ_ASSERT(*abiType == ToABIType(SASigArrayCopy));
      return FuncCast(Instance::arrayCopy, *abiType);
    case SymbolicAddress::SlotsToAllocKindBytesTable:
      return (void*)gc::slotsToAllocKindBytes;
    case SymbolicAddress::ExceptionNew:
      *abiType = Args_General2;
      MOZ_ASSERT(*abiType == ToABIType(SASigExceptionNew));
      return FuncCast(Instance::exceptionNew, *abiType);
    case SymbolicAddress::ThrowException:
      *abiType = Args_Int32_GeneralGeneral;
      MOZ_ASSERT(*abiType == ToABIType(SASigThrowException));
      return FuncCast(Instance::throwException, *abiType);

#ifdef ENABLE_WASM_JSPI
    case SymbolicAddress::UpdateSuspenderState:
      *abiType = Args_Int32_GeneralGeneralInt32;
      MOZ_ASSERT(*abiType == ToABIType(SASigUpdateSuspenderState));
      return FuncCast(UpdateSuspenderState, *abiType);
#endif

#ifdef WASM_CODEGEN_DEBUG
    case SymbolicAddress::PrintI32:
      *abiType = Args_General1;
      return FuncCast(PrintI32, *abiType);
    case SymbolicAddress::PrintPtr:
      *abiType = Args_General1;
      return FuncCast(PrintPtr, *abiType);
    case SymbolicAddress::PrintF32:
      *abiType = Args_Int_Float32;
      return FuncCast(PrintF32, *abiType);
    case SymbolicAddress::PrintF64:
      *abiType = Args_Int_Double;
      return FuncCast(PrintF64, *abiType);
    case SymbolicAddress::PrintText:
      *abiType = Args_General1;
      return FuncCast(PrintText, *abiType);
#endif
#define VISIT_BUILTIN_FUNC(op, export, sa_name, abitype, entry, ...) \
  case SymbolicAddress::sa_name:                                     \
    *abiType = abitype;                                              \
    return FuncCast(entry, *abiType);
      FOR_EACH_BUILTIN_MODULE_FUNC(VISIT_BUILTIN_FUNC)
#undef VISIT_BUILTIN_FUNC
    case SymbolicAddress::Limit:
      break;
  }

  MOZ_CRASH("Bad SymbolicAddress");
}

bool wasm::IsRoundingFunction(SymbolicAddress callee, jit::RoundingMode* mode) {
  switch (callee) {
    case SymbolicAddress::FloorD:
    case SymbolicAddress::FloorF:
      *mode = jit::RoundingMode::Down;
      return true;
    case SymbolicAddress::CeilD:
    case SymbolicAddress::CeilF:
      *mode = jit::RoundingMode::Up;
      return true;
    case SymbolicAddress::TruncD:
    case SymbolicAddress::TruncF:
      *mode = jit::RoundingMode::TowardsZero;
      return true;
    case SymbolicAddress::NearbyIntD:
    case SymbolicAddress::NearbyIntF:
      *mode = jit::RoundingMode::NearestTiesToEven;
      return true;
    default:
      return false;
  }
}

bool wasm::NeedsBuiltinThunk(SymbolicAddress sym) {
  // Also see "The Wasm Builtin ABIs" in WasmFrame.h.
  switch (sym) {
    // No thunk, because they do their work within the activation
    case SymbolicAddress::HandleThrow:  // GenerateThrowStub
    case SymbolicAddress::HandleTrap:   // GenerateTrapExit
      return false;

    // No thunk, because some work has to be done within the activation before
    // the activation exit: when called, arbitrary wasm registers are live and
    // must be saved, and the stack pointer may not be aligned for any ABI.
    case SymbolicAddress::HandleDebugTrap:      // GenerateDebugStub
    case SymbolicAddress::HandleRequestTierUp:  // GenerateRequestTierUpStub

    // No thunk, because their caller manages the activation exit explicitly
    case SymbolicAddress::CallImport_General:      // GenerateImportInterpExit
    case SymbolicAddress::CoerceInPlace_ToInt32:   // GenerateImportJitExit
    case SymbolicAddress::CoerceInPlace_ToNumber:  // GenerateImportJitExit
    case SymbolicAddress::CoerceInPlace_ToBigInt:  // GenerateImportJitExit
    case SymbolicAddress::BoxValue_Anyref:         // GenerateImportJitExit
      return false;

#ifdef WASM_CODEGEN_DEBUG
    // No thunk, because they call directly into C++ code that does not interact
    // with the rest of the VM at all.
    case SymbolicAddress::PrintI32:  // Debug stub printers
    case SymbolicAddress::PrintPtr:
    case SymbolicAddress::PrintF32:
    case SymbolicAddress::PrintF64:
    case SymbolicAddress::PrintText:
      return false;
#endif

    // No thunk because they're just data
    case SymbolicAddress::SlotsToAllocKindBytesTable:
      return false;

    // Everyone else gets a thunk to handle the exit from the activation
    case SymbolicAddress::ToInt32:
    case SymbolicAddress::DivI64:
    case SymbolicAddress::UDivI64:
    case SymbolicAddress::ModI64:
    case SymbolicAddress::UModI64:
    case SymbolicAddress::TruncateDoubleToUint64:
    case SymbolicAddress::TruncateDoubleToInt64:
    case SymbolicAddress::SaturatingTruncateDoubleToUint64:
    case SymbolicAddress::SaturatingTruncateDoubleToInt64:
    case SymbolicAddress::Uint64ToDouble:
    case SymbolicAddress::Uint64ToFloat32:
    case SymbolicAddress::Int64ToDouble:
    case SymbolicAddress::Int64ToFloat32:
#if defined(JS_CODEGEN_ARM)
    case SymbolicAddress::aeabi_idivmod:
    case SymbolicAddress::aeabi_uidivmod:
#endif
    case SymbolicAddress::AllocateBigInt:
    case SymbolicAddress::ModD:
    case SymbolicAddress::SinNativeD:
    case SymbolicAddress::SinFdlibmD:
    case SymbolicAddress::CosNativeD:
    case SymbolicAddress::CosFdlibmD:
    case SymbolicAddress::TanNativeD:
    case SymbolicAddress::TanFdlibmD:
    case SymbolicAddress::ASinD:
    case SymbolicAddress::ACosD:
    case SymbolicAddress::ATanD:
    case SymbolicAddress::CeilD:
    case SymbolicAddress::CeilF:
    case SymbolicAddress::FloorD:
    case SymbolicAddress::FloorF:
    case SymbolicAddress::TruncD:
    case SymbolicAddress::TruncF:
    case SymbolicAddress::NearbyIntD:
    case SymbolicAddress::NearbyIntF:
    case SymbolicAddress::ExpD:
    case SymbolicAddress::LogD:
    case SymbolicAddress::PowD:
    case SymbolicAddress::ATan2D:
    case SymbolicAddress::ArrayMemMove:
    case SymbolicAddress::ArrayRefsMove:
    case SymbolicAddress::MemoryGrowM32:
    case SymbolicAddress::MemoryGrowM64:
    case SymbolicAddress::MemorySizeM32:
    case SymbolicAddress::MemorySizeM64:
    case SymbolicAddress::WaitI32M32:
    case SymbolicAddress::WaitI32M64:
    case SymbolicAddress::WaitI64M32:
    case SymbolicAddress::WaitI64M64:
    case SymbolicAddress::WakeM32:
    case SymbolicAddress::WakeM64:
    case SymbolicAddress::CoerceInPlace_JitEntry:
    case SymbolicAddress::ReportV128JSCall:
    case SymbolicAddress::MemCopyM32:
    case SymbolicAddress::MemCopySharedM32:
    case SymbolicAddress::MemCopyM64:
    case SymbolicAddress::MemCopySharedM64:
    case SymbolicAddress::MemCopyAny:
    case SymbolicAddress::DataDrop:
    case SymbolicAddress::MemFillM32:
    case SymbolicAddress::MemFillSharedM32:
    case SymbolicAddress::MemFillM64:
    case SymbolicAddress::MemFillSharedM64:
    case SymbolicAddress::MemDiscardM32:
    case SymbolicAddress::MemDiscardSharedM32:
    case SymbolicAddress::MemDiscardM64:
    case SymbolicAddress::MemDiscardSharedM64:
    case SymbolicAddress::MemInitM32:
    case SymbolicAddress::MemInitM64:
    case SymbolicAddress::TableCopy:
    case SymbolicAddress::ElemDrop:
    case SymbolicAddress::TableFill:
    case SymbolicAddress::TableGet:
    case SymbolicAddress::TableGrow:
    case SymbolicAddress::TableInit:
    case SymbolicAddress::TableSet:
    case SymbolicAddress::TableSize:
    case SymbolicAddress::RefFunc:
    case SymbolicAddress::PostBarrierEdge:
    case SymbolicAddress::PostBarrierEdgePrecise:
    case SymbolicAddress::PostBarrierWholeCell:
    case SymbolicAddress::ExceptionNew:
    case SymbolicAddress::ThrowException:
    case SymbolicAddress::StructNewIL_true:
    case SymbolicAddress::StructNewIL_false:
    case SymbolicAddress::StructNewOOL_true:
    case SymbolicAddress::StructNewOOL_false:
    case SymbolicAddress::ArrayNew_true:
    case SymbolicAddress::ArrayNew_false:
    case SymbolicAddress::ArrayNewData:
    case SymbolicAddress::ArrayNewElem:
    case SymbolicAddress::ArrayInitData:
    case SymbolicAddress::ArrayInitElem:
    case SymbolicAddress::ArrayCopy:
#define VISIT_BUILTIN_FUNC(op, export, sa_name, ...) \
  case SymbolicAddress::sa_name:
      FOR_EACH_BUILTIN_MODULE_FUNC(VISIT_BUILTIN_FUNC)
#undef VISIT_BUILTIN_FUNC
#ifdef ENABLE_WASM_JSPI
    case SymbolicAddress::UpdateSuspenderState:
#endif
      return true;

    case SymbolicAddress::Limit:
      break;
  }

  MOZ_CRASH("unexpected symbolic address");
}

// ============================================================================
// [SMDOC] JS Fast Wasm Imports
//
// JS builtins that can be imported by wasm modules and called efficiently
// through thunks. These thunks conform to the internal wasm ABI and thus can be
// patched in for import calls. Calling a JS builtin through a thunk is much
// faster than calling out through the generic import call trampoline which will
// end up in the slowest C++ Instance::callImport path.
//
// Each JS builtin can have several overloads. These must all be enumerated in
// PopulateTypedNatives() so they can be included in the process-wide thunk set.
// Additionally to the traditional overloading based on types, every builtin
// can also have a version implemented by fdlibm or the native math library.
// This is useful for fingerprinting resistance.

#define FOR_EACH_SIN_COS_TAN_NATIVE(_) \
  _(math_sin, MathSin)                 \
  _(math_tan, MathTan)                 \
  _(math_cos, MathCos)

#define FOR_EACH_UNARY_NATIVE(_) \
  _(math_exp, MathExp)           \
  _(math_log, MathLog)           \
  _(math_asin, MathASin)         \
  _(math_atan, MathATan)         \
  _(math_acos, MathACos)         \
  _(math_log10, MathLog10)       \
  _(math_log2, MathLog2)         \
  _(math_log1p, MathLog1P)       \
  _(math_expm1, MathExpM1)       \
  _(math_sinh, MathSinH)         \
  _(math_tanh, MathTanH)         \
  _(math_cosh, MathCosH)         \
  _(math_asinh, MathASinH)       \
  _(math_atanh, MathATanH)       \
  _(math_acosh, MathACosH)       \
  _(math_sign, MathSign)         \
  _(math_trunc, MathTrunc)       \
  _(math_cbrt, MathCbrt)

#define FOR_EACH_BINARY_NATIVE(_) \
  _(ecmaAtan2, MathATan2)         \
  _(ecmaHypot, MathHypot)         \
  _(ecmaPow, MathPow)

#define DEFINE_SIN_COS_TAN_FLOAT_WRAPPER(func, _) \
  static float func##_native_impl_f32(float x) {  \
    return float(func##_native_impl(double(x)));  \
  }                                               \
  static float func##_fdlibm_impl_f32(float x) {  \
    return float(func##_fdlibm_impl(double(x)));  \
  }

#define DEFINE_UNARY_FLOAT_WRAPPER(func, _) \
  static float func##_impl_f32(float x) {   \
    return float(func##_impl(double(x)));   \
  }

#define DEFINE_BINARY_FLOAT_WRAPPER(func, _)  \
  static float func##_f32(float x, float y) { \
    return float(func(double(x), double(y))); \
  }

FOR_EACH_SIN_COS_TAN_NATIVE(DEFINE_SIN_COS_TAN_FLOAT_WRAPPER)
FOR_EACH_UNARY_NATIVE(DEFINE_UNARY_FLOAT_WRAPPER)
FOR_EACH_BINARY_NATIVE(DEFINE_BINARY_FLOAT_WRAPPER)

#undef DEFINE_UNARY_FLOAT_WRAPPER
#undef DEFINE_BINARY_FLOAT_WRAPPER

struct TypedNative {
  InlinableNative native;
  ABIFunctionType abiType;
  enum class FdlibmImpl : uint8_t { No, Yes } fdlibm;

  TypedNative(InlinableNative native, ABIFunctionType abiType,
              FdlibmImpl fdlibm)
      : native(native), abiType(abiType), fdlibm(fdlibm) {}

  using Lookup = TypedNative;
  static HashNumber hash(const Lookup& l) {
    return HashGeneric(uint32_t(l.native), uint32_t(l.abiType),
                       uint32_t(l.fdlibm));
  }
  static bool match(const TypedNative& lhs, const Lookup& rhs) {
    return lhs.native == rhs.native && lhs.abiType == rhs.abiType &&
           lhs.fdlibm == rhs.fdlibm;
  }
};

using TypedNativeToFuncPtrMap =
    HashMap<TypedNative, void*, TypedNative, SystemAllocPolicy>;

static bool PopulateTypedNatives(TypedNativeToFuncPtrMap* typedNatives) {
#define ADD_OVERLOAD(funcName, native, abiType, fdlibm)                   \
  if (!typedNatives->putNew(TypedNative(InlinableNative::native, abiType, \
                                        TypedNative::FdlibmImpl::fdlibm), \
                            FuncCast(funcName, abiType)))                 \
    return false;

#define ADD_SIN_COS_TAN_OVERLOADS(funcName, native)                          \
  ADD_OVERLOAD(funcName##_native_impl, native, Args_Double_Double, No)       \
  ADD_OVERLOAD(funcName##_fdlibm_impl, native, Args_Double_Double, Yes)      \
  ADD_OVERLOAD(funcName##_native_impl_f32, native, Args_Float32_Float32, No) \
  ADD_OVERLOAD(funcName##_fdlibm_impl_f32, native, Args_Float32_Float32, Yes)

#define ADD_UNARY_OVERLOADS(funcName, native)                   \
  ADD_OVERLOAD(funcName##_impl, native, Args_Double_Double, No) \
  ADD_OVERLOAD(funcName##_impl_f32, native, Args_Float32_Float32, No)

#define ADD_BINARY_OVERLOADS(funcName, native)                 \
  ADD_OVERLOAD(funcName, native, Args_Double_DoubleDouble, No) \
  ADD_OVERLOAD(funcName##_f32, native, Args_Float32_Float32Float32, No)

  FOR_EACH_SIN_COS_TAN_NATIVE(ADD_SIN_COS_TAN_OVERLOADS)
  FOR_EACH_UNARY_NATIVE(ADD_UNARY_OVERLOADS)
  FOR_EACH_BINARY_NATIVE(ADD_BINARY_OVERLOADS)

#undef ADD_UNARY_OVERLOADS
#undef ADD_BINARY_OVERLOADS

  return true;
}

#undef FOR_EACH_UNARY_NATIVE
#undef FOR_EACH_BINARY_NATIVE

// ============================================================================
// [SMDOC] Process-wide builtin thunk set
//
// Thunks are inserted between wasm calls and the C++ callee and achieve two
// things:
//  - bridging the few differences between the internal wasm ABI and the
//    external native ABI (viz. float returns on x86 and soft-fp ARM)
//  - executing an exit prologue/epilogue which in turn allows any profiling
//    iterator to see the full stack up to the wasm operation that called out
//
// Thunks are created for two kinds of C++ callees, enumerated above:
//  - SymbolicAddress: for statically compiled calls in the wasm module
//  - Imported JS builtins: optimized calls to imports
//
// All thunks are created up front, lazily, when the first wasm module is
// compiled in the process. Thunks are kept alive until the JS engine shuts down
// in the process. No thunks are created at runtime after initialization. This
// simple scheme allows several simplifications:
//  - no reference counting to keep thunks alive
//  - no problems toggling W^X permissions which, because of multiple executing
//    threads, would require each thunk allocation to be on its own page
// The cost for creating all thunks at once is relatively low since all thunks
// fit within the smallest executable-code allocation quantum (64k).

using TypedNativeToCodeRangeMap =
    HashMap<TypedNative, uint32_t, TypedNative, SystemAllocPolicy>;

using SymbolicAddressToCodeRangeArray =
    EnumeratedArray<SymbolicAddress, uint32_t, size_t(SymbolicAddress::Limit)>;

struct BuiltinThunks {
  uint8_t* codeBase;
  size_t codeSize;
  CodeRangeVector codeRanges;
  TypedNativeToCodeRangeMap typedNativeToCodeRange;
  SymbolicAddressToCodeRangeArray symbolicAddressToCodeRange;
  uint32_t provisionalLazyJitEntryOffset;

  BuiltinThunks() : codeBase(nullptr), codeSize(0) {}

  ~BuiltinThunks() {
    if (codeBase) {
      DeallocateExecutableMemory(codeBase, codeSize);
    }
  }
};

MOZ_RUNINIT Mutex initBuiltinThunks(mutexid::WasmInitBuiltinThunks);
mozilla::Atomic<const BuiltinThunks*> builtinThunks;

bool wasm::EnsureBuiltinThunksInitialized() {
  AutoMarkJitCodeWritableForThread writable;
  return EnsureBuiltinThunksInitialized(writable);
}

bool wasm::EnsureBuiltinThunksInitialized(
    AutoMarkJitCodeWritableForThread& writable) {
  LockGuard<Mutex> guard(initBuiltinThunks);
  if (builtinThunks) {
    return true;
  }

  auto thunks = MakeUnique<BuiltinThunks>();
  if (!thunks) {
    return false;
  }

  LifoAlloc lifo(BUILTIN_THUNK_LIFO_SIZE, js::MallocArena);
  TempAllocator tempAlloc(&lifo);
  WasmMacroAssembler masm(tempAlloc);
  AutoCreatedBy acb(masm, "wasm::EnsureBuiltinThunksInitialized");

  for (auto sym : MakeEnumeratedRange(SymbolicAddress::Limit)) {
    if (!NeedsBuiltinThunk(sym)) {
      thunks->symbolicAddressToCodeRange[sym] = UINT32_MAX;
      continue;
    }

    uint32_t codeRangeIndex = thunks->codeRanges.length();
    thunks->symbolicAddressToCodeRange[sym] = codeRangeIndex;

    ABIFunctionType abiType;
    void* funcPtr = AddressOf(sym, &abiType);

    ExitReason exitReason(sym);

    CallableOffsets offsets;
    if (!GenerateBuiltinThunk(masm, abiType, exitReason, funcPtr, &offsets)) {
      return false;
    }
    if (!thunks->codeRanges.emplaceBack(CodeRange::BuiltinThunk, offsets)) {
      return false;
    }
  }

  TypedNativeToFuncPtrMap typedNatives;
  if (!PopulateTypedNatives(&typedNatives)) {
    return false;
  }

  for (TypedNativeToFuncPtrMap::Range r = typedNatives.all(); !r.empty();
       r.popFront()) {
    TypedNative typedNative = r.front().key();

    uint32_t codeRangeIndex = thunks->codeRanges.length();
    if (!thunks->typedNativeToCodeRange.putNew(typedNative, codeRangeIndex)) {
      return false;
    }

    ABIFunctionType abiType = typedNative.abiType;
    void* funcPtr = r.front().value();

    ExitReason exitReason = ExitReason::Fixed::BuiltinNative;

    CallableOffsets offsets;
    if (!GenerateBuiltinThunk(masm, abiType, exitReason, funcPtr, &offsets)) {
      return false;
    }
    if (!thunks->codeRanges.emplaceBack(CodeRange::BuiltinThunk, offsets)) {
      return false;
    }
  }

  // Provisional lazy JitEntry stub: This is a shared stub that can be installed
  // in the jit-entry jump table.  It uses the JIT ABI and when invoked will
  // retrieve (via TlsContext()) and invoke the context-appropriate
  // invoke-from-interpreter jit stub, thus serving as the initial, unoptimized
  // jit-entry stub for any exported wasm function that has a jit-entry.

#ifdef DEBUG
  // We need to allow this machine code to bake in a C++ code pointer, so we
  // disable the wasm restrictions while generating this stub.
  JitContext jitContext;
  bool oldFlag = jitContext.setIsCompilingWasm(false);
#endif

  Offsets provisionalLazyJitEntryOffsets;
  if (!GenerateProvisionalLazyJitEntryStub(masm,
                                           &provisionalLazyJitEntryOffsets)) {
    return false;
  }
  thunks->provisionalLazyJitEntryOffset = provisionalLazyJitEntryOffsets.begin;

#ifdef DEBUG
  jitContext.setIsCompilingWasm(oldFlag);
#endif

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  size_t allocSize = AlignBytes(masm.bytesNeeded(), ExecutableCodePageSize);

  thunks->codeSize = allocSize;
  thunks->codeBase = (uint8_t*)AllocateExecutableMemory(
      allocSize, ProtectionSetting::Writable, MemCheckKind::MakeUndefined);
  if (!thunks->codeBase) {
    return false;
  }

  masm.executableCopy(thunks->codeBase);
  memset(thunks->codeBase + masm.bytesNeeded(), 0,
         allocSize - masm.bytesNeeded());

  masm.processCodeLabels(thunks->codeBase);
  PatchDebugSymbolicAccesses(thunks->codeBase, masm);

  MOZ_ASSERT(masm.callSites().empty());
  MOZ_ASSERT(masm.callSiteTargets().empty());
  MOZ_ASSERT(masm.trapSites().empty());
  MOZ_ASSERT(masm.tryNotes().empty());
  MOZ_ASSERT(masm.codeRangeUnwindInfos().empty());

  if (!ExecutableAllocator::makeExecutableAndFlushICache(thunks->codeBase,
                                                         thunks->codeSize)) {
    return false;
  }

  builtinThunks = thunks.release();
  return true;
}

void wasm::ReleaseBuiltinThunks() {
  if (builtinThunks) {
    const BuiltinThunks* ptr = builtinThunks;
    js_delete(const_cast<BuiltinThunks*>(ptr));
    builtinThunks = nullptr;
  }
}

void* wasm::SymbolicAddressTarget(SymbolicAddress sym) {
  MOZ_ASSERT(builtinThunks);

  ABIFunctionType abiType;
  void* funcPtr = AddressOf(sym, &abiType);

  if (!NeedsBuiltinThunk(sym)) {
    return funcPtr;
  }

  const BuiltinThunks& thunks = *builtinThunks;
  uint32_t codeRangeIndex = thunks.symbolicAddressToCodeRange[sym];
  return thunks.codeBase + thunks.codeRanges[codeRangeIndex].begin();
}

void* wasm::ProvisionalLazyJitEntryStub() {
  MOZ_ASSERT(builtinThunks);

  const BuiltinThunks& thunks = *builtinThunks;
  return thunks.codeBase + thunks.provisionalLazyJitEntryOffset;
}

static Maybe<ABIFunctionType> ToBuiltinABIFunctionType(
    const FuncType& funcType) {
  const ValTypeVector& args = funcType.args();
  const ValTypeVector& results = funcType.results();

  if (results.length() != 1) {
    return Nothing();
  }

  if ((args.length() + 1) > (sizeof(uint32_t) * 8 / ABITypeArgShift)) {
    return Nothing();
  }

  uint32_t abiType = 0;
  for (size_t i = 0; i < args.length(); i++) {
    switch (args[i].kind()) {
      case ValType::F32:
        abiType <<= ABITypeArgShift;
        abiType |= uint32_t(ABIType::Float32);
        break;
      case ValType::F64:
        abiType <<= ABITypeArgShift;
        abiType |= uint32_t(ABIType::Float64);
        break;
      default:
        return Nothing();
    }
  }

  abiType <<= ABITypeArgShift;
  switch (results[0].kind()) {
    case ValType::F32:
      abiType |= uint32_t(ABIType::Float32);
      break;
    case ValType::F64:
      abiType |= uint32_t(ABIType::Float64);
      break;
    default:
      return Nothing();
  }

  return Some(ABIFunctionType(abiType));
}

void* wasm::MaybeGetBuiltinThunk(JSFunction* f, const FuncType& funcType) {
  MOZ_ASSERT(builtinThunks);

  if (!f->isNativeFun() || !f->hasJitInfo() ||
      f->jitInfo()->type() != JSJitInfo::InlinableNative) {
    return nullptr;
  }

  Maybe<ABIFunctionType> abiType = ToBuiltinABIFunctionType(funcType);
  if (!abiType) {
    return nullptr;
  }

  const BuiltinThunks& thunks = *builtinThunks;

  // If this function must use the fdlibm implementation first try to lookup
  // the fdlibm version. If that version doesn't exist we still fallback to
  // the normal native.
  if (math_use_fdlibm_for_sin_cos_tan() ||
      f->realm()->creationOptions().alwaysUseFdlibm()) {
    TypedNative typedNative(f->jitInfo()->inlinableNative, *abiType,
                            TypedNative::FdlibmImpl::Yes);
    auto p =
        thunks.typedNativeToCodeRange.readonlyThreadsafeLookup(typedNative);
    if (p) {
      return thunks.codeBase + thunks.codeRanges[p->value()].begin();
    }
  }

  TypedNative typedNative(f->jitInfo()->inlinableNative, *abiType,
                          TypedNative::FdlibmImpl::No);
  auto p = thunks.typedNativeToCodeRange.readonlyThreadsafeLookup(typedNative);
  if (!p) {
    return nullptr;
  }

  return thunks.codeBase + thunks.codeRanges[p->value()].begin();
}

bool wasm::LookupBuiltinThunk(void* pc, const CodeRange** codeRange,
                              const uint8_t** codeBase) {
  if (!builtinThunks) {
    return false;
  }

  const BuiltinThunks& thunks = *builtinThunks;
  if (pc < thunks.codeBase || pc >= thunks.codeBase + thunks.codeSize) {
    return false;
  }

  *codeBase = thunks.codeBase;

  CodeRange::OffsetInCode target((uint8_t*)pc - thunks.codeBase);
  *codeRange = LookupInSorted(thunks.codeRanges, target);

  return !!*codeRange;
}

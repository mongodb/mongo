/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#include "fdlibm.h"
#include "jslibmath.h"

#include "jit/AtomicOperations.h"
#include "jit/InlinableNatives.h"
#include "jit/MacroAssembler.h"
#include "threading/Mutex.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmStubs.h"

#include "vm/Debugger-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace jit;
using namespace wasm;

using mozilla::HashGeneric;
using mozilla::IsNaN;
using mozilla::MakeEnumeratedRange;

static const unsigned BUILTIN_THUNK_LIFO_SIZE = 64 * 1024;

// ============================================================================
// WebAssembly builtin C++ functions called from wasm code to implement internal
// wasm operations.

#if defined(JS_CODEGEN_ARM)
extern "C" {

extern MOZ_EXPORT int64_t
__aeabi_idivmod(int, int);

extern MOZ_EXPORT int64_t
__aeabi_uidivmod(int, int);

}
#endif

// This utility function can only be called for builtins that are called
// directly from wasm code.
static JitActivation*
CallingActivation()
{
    Activation* act = TlsContext.get()->activation();
    MOZ_ASSERT(act->asJit()->hasWasmExitFP());
    return act->asJit();
}

static void*
WasmHandleExecutionInterrupt()
{
    JitActivation* activation = CallingActivation();
    MOZ_ASSERT(activation->isWasmInterrupted());

    if (!CheckForInterrupt(activation->cx())) {
        // If CheckForInterrupt failed, it is time to interrupt execution.
        // Returning nullptr to the caller will jump to the throw stub which
        // will call HandleThrow. The JitActivation must stay in the
        // interrupted state until then so that stack unwinding works in
        // HandleThrow.
        return nullptr;
    }

    // If CheckForInterrupt succeeded, then execution can proceed and the
    // interrupt is over.
    void* resumePC = activation->wasmInterruptResumePC();
    activation->finishWasmInterrupt();
    return resumePC;
}

static bool
WasmHandleDebugTrap()
{
    JitActivation* activation = CallingActivation();
    JSContext* cx = activation->cx();
    Frame* fp = activation->wasmExitFP();
    Instance* instance = fp->tls->instance;
    const Code& code = instance->code();
    MOZ_ASSERT(code.metadata().debugEnabled);

    // The debug trap stub is the innermost frame. It's return address is the
    // actual trap site.
    const CallSite* site = code.lookupCallSite(fp->returnAddress);
    MOZ_ASSERT(site);

    // Advance to the actual trapping frame.
    fp = fp->callerFP;
    DebugFrame* debugFrame = DebugFrame::from(fp);

    if (site->kind() == CallSite::EnterFrame) {
        if (!instance->enterFrameTrapsEnabled())
            return true;
        debugFrame->setIsDebuggee();
        debugFrame->observe(cx);
        // TODO call onEnterFrame
        JSTrapStatus status = Debugger::onEnterFrame(cx, debugFrame);
        if (status == JSTRAP_RETURN) {
            // Ignoring forced return (JSTRAP_RETURN) -- changing code execution
            // order is not yet implemented in the wasm baseline.
            // TODO properly handle JSTRAP_RETURN and resume wasm execution.
            JS_ReportErrorASCII(cx, "Unexpected resumption value from onEnterFrame");
            return false;
        }
        return status == JSTRAP_CONTINUE;
    }
    if (site->kind() == CallSite::LeaveFrame) {
        debugFrame->updateReturnJSValue();
        bool ok = Debugger::onLeaveFrame(cx, debugFrame, nullptr, true);
        debugFrame->leave(cx);
        return ok;
    }

    DebugState& debug = instance->debug();
    MOZ_ASSERT(debug.hasBreakpointTrapAtOffset(site->lineOrBytecode()));
    if (debug.stepModeEnabled(debugFrame->funcIndex())) {
        RootedValue result(cx, UndefinedValue());
        JSTrapStatus status = Debugger::onSingleStep(cx, &result);
        if (status == JSTRAP_RETURN) {
            // TODO properly handle JSTRAP_RETURN.
            JS_ReportErrorASCII(cx, "Unexpected resumption value from onSingleStep");
            return false;
        }
        if (status != JSTRAP_CONTINUE)
            return false;
    }
    if (debug.hasBreakpointSite(site->lineOrBytecode())) {
        RootedValue result(cx, UndefinedValue());
        JSTrapStatus status = Debugger::onTrap(cx, &result);
        if (status == JSTRAP_RETURN) {
            // TODO properly handle JSTRAP_RETURN.
            JS_ReportErrorASCII(cx, "Unexpected resumption value from breakpoint handler");
            return false;
        }
        if (status != JSTRAP_CONTINUE)
            return false;
    }
    return true;
}

// Unwind the entire activation in response to a thrown exception. This function
// is responsible for notifying the debugger of each unwound frame. The return
// value is the new stack address which the calling stub will set to the sp
// register before executing a return instruction.

void*
wasm::HandleThrow(JSContext* cx, WasmFrameIter& iter)
{
    // WasmFrameIter iterates down wasm frames in the activation starting at
    // JitActivation::wasmExitFP(). Pass Unwind::True to pop
    // JitActivation::wasmExitFP() once each time WasmFrameIter is incremented,
    // ultimately leaving exit FP null when the WasmFrameIter is done().  This
    // is necessary to prevent a DebugFrame from being observed again after we
    // just called onLeaveFrame (which would lead to the frame being re-added
    // to the map of live frames, right as it becomes trash).

    MOZ_ASSERT(CallingActivation() == iter.activation());
    MOZ_ASSERT(!iter.done());
    iter.setUnwind(WasmFrameIter::Unwind::True);

    // Live wasm code on the stack is kept alive (in TraceJitActivation) by
    // marking the instance of every wasm::Frame found by WasmFrameIter.
    // However, as explained above, we're popping frames while iterating which
    // means that a GC during this loop could collect the code of frames whose
    // code is still on the stack. This is actually mostly fine: as soon as we
    // return to the throw stub, the entire stack will be popped as a whole,
    // returning to the C++ caller. However, we must keep the throw stub alive
    // itself which is owned by the innermost instance.
    RootedWasmInstanceObject keepAlive(cx, iter.instance()->object());

    for (; !iter.done(); ++iter) {
        if (!iter.debugEnabled())
            continue;

        DebugFrame* frame = iter.debugFrame();
        frame->clearReturnJSValue();

        // Assume JSTRAP_ERROR status if no exception is pending --
        // no onExceptionUnwind handlers must be fired.
        if (cx->isExceptionPending()) {
            JSTrapStatus status = Debugger::onExceptionUnwind(cx, frame);
            if (status == JSTRAP_RETURN) {
                // Unexpected trap return -- raising error since throw recovery
                // is not yet implemented in the wasm baseline.
                // TODO properly handle JSTRAP_RETURN and resume wasm execution.
                JS_ReportErrorASCII(cx, "Unexpected resumption value from onExceptionUnwind");
            }
        }

        bool ok = Debugger::onLeaveFrame(cx, frame, nullptr, false);
        if (ok) {
            // Unexpected success from the handler onLeaveFrame -- raising error
            // since throw recovery is not yet implemented in the wasm baseline.
            // TODO properly handle success and resume wasm execution.
            JS_ReportErrorASCII(cx, "Unexpected success from onLeaveFrame");
        }
        frame->leave(cx);
    }

    MOZ_ASSERT(!cx->activation()->asJit()->isWasmInterrupted(), "unwinding clears the interrupt");
    MOZ_ASSERT(!cx->activation()->asJit()->isWasmTrapping(), "unwinding clears the trapping state");

    return iter.unwoundAddressOfReturnAddress();
}

static void*
WasmHandleThrow()
{
    JitActivation* activation = CallingActivation();
    JSContext* cx = activation->cx();
    WasmFrameIter iter(activation);
    return HandleThrow(cx, iter);
}

static void
WasmOldReportTrap(int32_t trapIndex)
{
    JSContext* cx = TlsContext.get();

    MOZ_ASSERT(trapIndex < int32_t(Trap::Limit) && trapIndex >= 0);
    Trap trap = Trap(trapIndex);

    unsigned errorNumber;
    switch (trap) {
      case Trap::Unreachable:
        errorNumber = JSMSG_WASM_UNREACHABLE;
        break;
      case Trap::IntegerOverflow:
        errorNumber = JSMSG_WASM_INTEGER_OVERFLOW;
        break;
      case Trap::InvalidConversionToInteger:
        errorNumber = JSMSG_WASM_INVALID_CONVERSION;
        break;
      case Trap::IntegerDivideByZero:
        errorNumber = JSMSG_WASM_INT_DIVIDE_BY_ZERO;
        break;
      case Trap::IndirectCallToNull:
        errorNumber = JSMSG_WASM_IND_CALL_TO_NULL;
        break;
      case Trap::IndirectCallBadSig:
        errorNumber = JSMSG_WASM_IND_CALL_BAD_SIG;
        break;
      case Trap::ImpreciseSimdConversion:
        errorNumber = JSMSG_SIMD_FAILED_CONVERSION;
        break;
      case Trap::OutOfBounds:
        errorNumber = JSMSG_WASM_OUT_OF_BOUNDS;
        break;
      case Trap::UnalignedAccess:
        errorNumber = JSMSG_WASM_UNALIGNED_ACCESS;
        break;
      case Trap::StackOverflow:
        errorNumber = JSMSG_OVER_RECURSED;
        break;
      case Trap::ThrowReported:
        // Error was already reported under another name.
        return;
      default:
        MOZ_CRASH("unexpected trap");
    }

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber);
}

static void
WasmReportTrap()
{
    Trap trap = TlsContext.get()->runtime()->wasmTrapData().trap;
    WasmOldReportTrap(int32_t(trap));
}

static void
WasmReportOutOfBounds()
{
    JSContext* cx = TlsContext.get();
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_OUT_OF_BOUNDS);
}

static void
WasmReportUnalignedAccess()
{
    JSContext* cx = TlsContext.get();
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_UNALIGNED_ACCESS);
}

static void
WasmReportInt64JSCall()
{
    JSContext* cx = TlsContext.get();
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_I64_TYPE);
}

static int32_t
CoerceInPlace_ToInt32(Value* rawVal)
{
    JSContext* cx = TlsContext.get();

    int32_t i32;
    RootedValue val(cx, *rawVal);
    if (!ToInt32(cx, val, &i32)) {
        *rawVal = PoisonedObjectValue(0x42);
        return false;
    }

    *rawVal = Int32Value(i32);
    return true;
}

static int32_t
CoerceInPlace_ToNumber(Value* rawVal)
{
    JSContext* cx = TlsContext.get();

    double dbl;
    RootedValue val(cx, *rawVal);
    if (!ToNumber(cx, val, &dbl)) {
        *rawVal = PoisonedObjectValue(0x42);
        return false;
    }

    *rawVal = DoubleValue(dbl);
    return true;
}

static int32_t
CoerceInPlace_JitEntry(int funcExportIndex, TlsData* tlsData, Value* argv)
{
    JSContext* cx = CallingActivation()->cx();

    const Code& code = tlsData->instance->code();
    const FuncExport& fe = code.metadata(code.stableTier()).funcExports[funcExportIndex];

    for (size_t i = 0; i < fe.sig().args().length(); i++) {
        HandleValue arg = HandleValue::fromMarkedLocation(&argv[i]);
        switch (fe.sig().args()[i]) {
          case ValType::I32: {
            int32_t i32;
            if (!ToInt32(cx, arg, &i32))
                return false;
            argv[i] = Int32Value(i32);
            break;
          }
          case ValType::F32:
          case ValType::F64: {
            double dbl;
            if (!ToNumber(cx, arg, &dbl))
                return false;
            // No need to convert double-to-float for f32, it's done inline
            // in the wasm stub later.
            argv[i] = DoubleValue(dbl);
            break;
          }
          default: {
            MOZ_CRASH("unexpected input argument in CoerceInPlace_JitEntry");
          }
        }
    }

    return true;
}

static int64_t
DivI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi, uint32_t y_lo)
{
    int64_t x = ((uint64_t)x_hi << 32) + x_lo;
    int64_t y = ((uint64_t)y_hi << 32) + y_lo;
    MOZ_ASSERT(x != INT64_MIN || y != -1);
    MOZ_ASSERT(y != 0);
    return x / y;
}

static int64_t
UDivI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi, uint32_t y_lo)
{
    uint64_t x = ((uint64_t)x_hi << 32) + x_lo;
    uint64_t y = ((uint64_t)y_hi << 32) + y_lo;
    MOZ_ASSERT(y != 0);
    return x / y;
}

static int64_t
ModI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi, uint32_t y_lo)
{
    int64_t x = ((uint64_t)x_hi << 32) + x_lo;
    int64_t y = ((uint64_t)y_hi << 32) + y_lo;
    MOZ_ASSERT(x != INT64_MIN || y != -1);
    MOZ_ASSERT(y != 0);
    return x % y;
}

static int64_t
UModI64(uint32_t x_hi, uint32_t x_lo, uint32_t y_hi, uint32_t y_lo)
{
    uint64_t x = ((uint64_t)x_hi << 32) + x_lo;
    uint64_t y = ((uint64_t)y_hi << 32) + y_lo;
    MOZ_ASSERT(y != 0);
    return x % y;
}

static int64_t
TruncateDoubleToInt64(double input)
{
    // Note: INT64_MAX is not representable in double. It is actually
    // INT64_MAX + 1.  Therefore also sending the failure value.
    if (input >= double(INT64_MAX) || input < double(INT64_MIN) || IsNaN(input))
        return 0x8000000000000000;
    return int64_t(input);
}

static uint64_t
TruncateDoubleToUint64(double input)
{
    // Note: UINT64_MAX is not representable in double. It is actually UINT64_MAX + 1.
    // Therefore also sending the failure value.
    if (input >= double(UINT64_MAX) || input <= -1.0 || IsNaN(input))
        return 0x8000000000000000;
    return uint64_t(input);
}

static int64_t
SaturatingTruncateDoubleToInt64(double input)
{
    // Handle in-range values (except INT64_MIN).
    if (fabs(input) < -double(INT64_MIN))
        return int64_t(input);
    // Handle NaN.
    if (IsNaN(input))
        return 0;
    // Handle positive overflow.
    if (input > 0)
        return INT64_MAX;
    // Handle negative overflow.
    return INT64_MIN;
}

static uint64_t
SaturatingTruncateDoubleToUint64(double input)
{
    // Handle positive overflow.
    if (input >= -double(INT64_MIN) * 2.0)
        return UINT64_MAX;
    // Handle in-range values.
    if (input >= -1.0)
        return uint64_t(input);
    // Handle NaN and negative overflow.
    return 0;
}

static double
Int64ToDouble(int32_t x_hi, uint32_t x_lo)
{
    int64_t x = int64_t((uint64_t(x_hi) << 32)) + int64_t(x_lo);
    return double(x);
}

static float
Int64ToFloat32(int32_t x_hi, uint32_t x_lo)
{
    int64_t x = int64_t((uint64_t(x_hi) << 32)) + int64_t(x_lo);
    return float(x);
}

static double
Uint64ToDouble(int32_t x_hi, uint32_t x_lo)
{
    uint64_t x = (uint64_t(x_hi) << 32) + uint64_t(x_lo);
    return double(x);
}

static float
Uint64ToFloat32(int32_t x_hi, uint32_t x_lo)
{
    uint64_t x = (uint64_t(x_hi) << 32) + uint64_t(x_lo);
    return float(x);
}

template <class F>
static inline void*
FuncCast(F* funcPtr, ABIFunctionType abiType)
{
    void* pf = JS_FUNC_TO_DATA_PTR(void*, funcPtr);
#ifdef JS_SIMULATOR
    pf = Simulator::RedirectNativeFunction(pf, abiType);
#endif
    return pf;
}

static void*
AddressOf(SymbolicAddress imm, ABIFunctionType* abiType)
{
    switch (imm) {
      case SymbolicAddress::HandleExecutionInterrupt:
        *abiType = Args_General0;
        return FuncCast(WasmHandleExecutionInterrupt, *abiType);
      case SymbolicAddress::HandleDebugTrap:
        *abiType = Args_General0;
        return FuncCast(WasmHandleDebugTrap, *abiType);
      case SymbolicAddress::HandleThrow:
        *abiType = Args_General0;
        return FuncCast(WasmHandleThrow, *abiType);
      case SymbolicAddress::ReportTrap:
        *abiType = Args_General0;
        return FuncCast(WasmReportTrap, *abiType);
      case SymbolicAddress::OldReportTrap:
        *abiType = Args_General1;
        return FuncCast(WasmOldReportTrap, *abiType);
      case SymbolicAddress::ReportOutOfBounds:
        *abiType = Args_General0;
        return FuncCast(WasmReportOutOfBounds, *abiType);
      case SymbolicAddress::ReportUnalignedAccess:
        *abiType = Args_General0;
        return FuncCast(WasmReportUnalignedAccess, *abiType);
      case SymbolicAddress::ReportInt64JSCall:
        *abiType = Args_General0;
        return FuncCast(WasmReportInt64JSCall, *abiType);
      case SymbolicAddress::CallImport_Void:
        *abiType = Args_General4;
        return FuncCast(Instance::callImport_void, *abiType);
      case SymbolicAddress::CallImport_I32:
        *abiType = Args_General4;
        return FuncCast(Instance::callImport_i32, *abiType);
      case SymbolicAddress::CallImport_I64:
        *abiType = Args_General4;
        return FuncCast(Instance::callImport_i64, *abiType);
      case SymbolicAddress::CallImport_F64:
        *abiType = Args_General4;
        return FuncCast(Instance::callImport_f64, *abiType);
      case SymbolicAddress::CoerceInPlace_ToInt32:
        *abiType = Args_General1;
        return FuncCast(CoerceInPlace_ToInt32, *abiType);
      case SymbolicAddress::CoerceInPlace_ToNumber:
        *abiType = Args_General1;
        return FuncCast(CoerceInPlace_ToNumber, *abiType);
      case SymbolicAddress::CoerceInPlace_JitEntry:
        *abiType = Args_General3;
        return FuncCast(CoerceInPlace_JitEntry, *abiType);
      case SymbolicAddress::ToInt32:
        *abiType = Args_Int_Double;
        return FuncCast<int32_t (double)>(JS::ToInt32, *abiType);
      case SymbolicAddress::DivI64:
        *abiType = Args_General4;
        return FuncCast(DivI64, *abiType);
      case SymbolicAddress::UDivI64:
        *abiType = Args_General4;
        return FuncCast(UDivI64, *abiType);
      case SymbolicAddress::ModI64:
        *abiType = Args_General4;
        return FuncCast(ModI64, *abiType);
      case SymbolicAddress::UModI64:
        *abiType = Args_General4;
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
        *abiType = Args_General2;
        return FuncCast(__aeabi_idivmod, *abiType);
      case SymbolicAddress::aeabi_uidivmod:
        *abiType = Args_General2;
        return FuncCast(__aeabi_uidivmod, *abiType);
#endif
      case SymbolicAddress::ModD:
        *abiType = Args_Double_DoubleDouble;
        return FuncCast(NumberMod, *abiType);
      case SymbolicAddress::SinD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(sin, *abiType);
      case SymbolicAddress::CosD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(cos, *abiType);
      case SymbolicAddress::TanD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(tan, *abiType);
      case SymbolicAddress::ASinD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::asin, *abiType);
      case SymbolicAddress::ACosD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::acos, *abiType);
      case SymbolicAddress::ATanD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::atan, *abiType);
      case SymbolicAddress::CeilD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::ceil, *abiType);
      case SymbolicAddress::CeilF:
        *abiType = Args_Float32_Float32;
        return FuncCast<float (float)>(fdlibm::ceilf, *abiType);
      case SymbolicAddress::FloorD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::floor, *abiType);
      case SymbolicAddress::FloorF:
        *abiType = Args_Float32_Float32;
        return FuncCast<float (float)>(fdlibm::floorf, *abiType);
      case SymbolicAddress::TruncD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::trunc, *abiType);
      case SymbolicAddress::TruncF:
        *abiType = Args_Float32_Float32;
        return FuncCast<float (float)>(fdlibm::truncf, *abiType);
      case SymbolicAddress::NearbyIntD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::nearbyint, *abiType);
      case SymbolicAddress::NearbyIntF:
        *abiType = Args_Float32_Float32;
        return FuncCast<float (float)>(fdlibm::nearbyintf, *abiType);
      case SymbolicAddress::ExpD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::exp, *abiType);
      case SymbolicAddress::LogD:
        *abiType = Args_Double_Double;
        return FuncCast<double (double)>(fdlibm::log, *abiType);
      case SymbolicAddress::PowD:
        *abiType = Args_Double_DoubleDouble;
        return FuncCast(ecmaPow, *abiType);
      case SymbolicAddress::ATan2D:
        *abiType = Args_Double_DoubleDouble;
        return FuncCast(ecmaAtan2, *abiType);
      case SymbolicAddress::GrowMemory:
        *abiType = Args_General2;
        return FuncCast(Instance::growMemory_i32, *abiType);
      case SymbolicAddress::CurrentMemory:
        *abiType = Args_General1;
        return FuncCast(Instance::currentMemory_i32, *abiType);
      case SymbolicAddress::WaitI32:
        *abiType = Args_Int_GeneralGeneralGeneralInt64;
        return FuncCast(Instance::wait_i32, *abiType);
      case SymbolicAddress::WaitI64:
        *abiType = Args_Int_GeneralGeneralInt64Int64;
        return FuncCast(Instance::wait_i64, *abiType);
      case SymbolicAddress::Wake:
        *abiType = Args_General3;
        return FuncCast(Instance::wake, *abiType);
#if defined(JS_CODEGEN_MIPS32)
      case SymbolicAddress::js_jit_gAtomic64Lock:
        return &js::jit::gAtomic64Lock;
#endif
      case SymbolicAddress::Limit:
        break;
    }

    MOZ_CRASH("Bad SymbolicAddress");
}

bool
wasm::NeedsBuiltinThunk(SymbolicAddress sym)
{
    // Some functions don't want to a thunk, because they already have one or
    // they don't have frame info.
    switch (sym) {
      case SymbolicAddress::HandleExecutionInterrupt: // GenerateInterruptExit
      case SymbolicAddress::HandleDebugTrap:          // GenerateDebugTrapStub
      case SymbolicAddress::HandleThrow:              // GenerateThrowStub
      case SymbolicAddress::ReportTrap:               // GenerateTrapExit
      case SymbolicAddress::OldReportTrap:            // GenerateOldTrapExit
      case SymbolicAddress::ReportOutOfBounds:        // GenerateOutOfBoundsExit
      case SymbolicAddress::ReportUnalignedAccess:    // GenerateUnalignedExit
      case SymbolicAddress::CallImport_Void:          // GenerateImportInterpExit
      case SymbolicAddress::CallImport_I32:
      case SymbolicAddress::CallImport_I64:
      case SymbolicAddress::CallImport_F64:
      case SymbolicAddress::CoerceInPlace_ToInt32:    // GenerateImportJitExit
      case SymbolicAddress::CoerceInPlace_ToNumber:
#if defined(JS_CODEGEN_MIPS32)
      case SymbolicAddress::js_jit_gAtomic64Lock:
#endif
        return false;
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
      case SymbolicAddress::ModD:
      case SymbolicAddress::SinD:
      case SymbolicAddress::CosD:
      case SymbolicAddress::TanD:
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
      case SymbolicAddress::GrowMemory:
      case SymbolicAddress::CurrentMemory:
      case SymbolicAddress::WaitI32:
      case SymbolicAddress::WaitI64:
      case SymbolicAddress::Wake:
      case SymbolicAddress::CoerceInPlace_JitEntry:
      case SymbolicAddress::ReportInt64JSCall:
        return true;
      case SymbolicAddress::Limit:
        break;
    }

    MOZ_CRASH("unexpected symbolic address");
}

// ============================================================================
// JS builtins that can be imported by wasm modules and called efficiently
// through thunks. These thunks conform to the internal wasm ABI and thus can be
// patched in for import calls. Calling a JS builtin through a thunk is much
// faster than calling out through the generic import call trampoline which will
// end up in the slowest C++ Instance::callImport path.
//
// Each JS builtin can have several overloads. These must all be enumerated in
// PopulateTypedNatives() so they can be included in the process-wide thunk set.

#define FOR_EACH_UNARY_NATIVE(_)   \
    _(math_sin, MathSin)           \
    _(math_tan, MathTan)           \
    _(math_cos, MathCos)           \
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

#define FOR_EACH_BINARY_NATIVE(_)  \
    _(ecmaAtan2, MathATan2)        \
    _(ecmaHypot, MathHypot)        \
    _(ecmaPow, MathPow)            \

#define DEFINE_UNARY_FLOAT_WRAPPER(func, _)        \
    static float func##_uncached_f32(float x) {    \
        return float(func##_uncached(double(x)));  \
    }

#define DEFINE_BINARY_FLOAT_WRAPPER(func, _)       \
    static float func##_f32(float x, float y) {    \
        return float(func(double(x), double(y)));  \
    }

FOR_EACH_UNARY_NATIVE(DEFINE_UNARY_FLOAT_WRAPPER)
FOR_EACH_BINARY_NATIVE(DEFINE_BINARY_FLOAT_WRAPPER)

#undef DEFINE_UNARY_FLOAT_WRAPPER
#undef DEFINE_BINARY_FLOAT_WRAPPER

struct TypedNative
{
    InlinableNative native;
    ABIFunctionType abiType;

    TypedNative(InlinableNative native, ABIFunctionType abiType)
      : native(native),
        abiType(abiType)
    {}

    typedef TypedNative Lookup;
    static HashNumber hash(const Lookup& l) {
        return HashGeneric(uint32_t(l.native), uint32_t(l.abiType));
    }
    static bool match(const TypedNative& lhs, const Lookup& rhs) {
        return lhs.native == rhs.native && lhs.abiType == rhs.abiType;
    }
};

using TypedNativeToFuncPtrMap =
    HashMap<TypedNative, void*, TypedNative, SystemAllocPolicy>;

static bool
PopulateTypedNatives(TypedNativeToFuncPtrMap* typedNatives)
{
    if (!typedNatives->init())
        return false;

#define ADD_OVERLOAD(funcName, native, abiType)                                           \
    if (!typedNatives->putNew(TypedNative(InlinableNative::native, abiType),              \
                              FuncCast(funcName, abiType)))                               \
        return false;

#define ADD_UNARY_OVERLOADS(funcName, native)                                             \
    ADD_OVERLOAD(funcName##_uncached, native, Args_Double_Double)                         \
    ADD_OVERLOAD(funcName##_uncached_f32, native, Args_Float32_Float32)

#define ADD_BINARY_OVERLOADS(funcName, native)                                            \
    ADD_OVERLOAD(funcName, native, Args_Double_DoubleDouble)                              \
    ADD_OVERLOAD(funcName##_f32, native, Args_Float32_Float32Float32)

    FOR_EACH_UNARY_NATIVE(ADD_UNARY_OVERLOADS)
    FOR_EACH_BINARY_NATIVE(ADD_BINARY_OVERLOADS)

#undef ADD_UNARY_OVERLOADS
#undef ADD_BINARY_OVERLOADS

    return true;
}

#undef FOR_EACH_UNARY_NATIVE
#undef FOR_EACH_BINARY_NATIVE

// ============================================================================
// Process-wide builtin thunk set
//
// Thunks are inserted between wasm calls and the C++ callee and achieve two
// things:
//  - bridging the few differences between the internal wasm ABI and the external
//    native ABI (viz. float returns on x86 and soft-fp ARM)
//  - executing an exit prologue/epilogue which in turn allows any asynchronous
//    interrupt to see the full stack up to the wasm operation that called out
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
// fit within the smallest executable quanta (64k).

using TypedNativeToCodeRangeMap =
    HashMap<TypedNative, uint32_t, TypedNative, SystemAllocPolicy>;

using SymbolicAddressToCodeRangeArray =
    EnumeratedArray<SymbolicAddress, SymbolicAddress::Limit, uint32_t>;

struct BuiltinThunks
{
    uint8_t* codeBase;
    size_t codeSize;
    CodeRangeVector codeRanges;
    TypedNativeToCodeRangeMap typedNativeToCodeRange;
    SymbolicAddressToCodeRangeArray symbolicAddressToCodeRange;

    BuiltinThunks()
      : codeBase(nullptr), codeSize(0)
    {}

    ~BuiltinThunks() {
        if (codeBase)
            DeallocateExecutableMemory(codeBase, codeSize);
    }
};

Mutex initBuiltinThunks(mutexid::WasmInitBuiltinThunks);
Atomic<const BuiltinThunks*> builtinThunks;

bool
wasm::EnsureBuiltinThunksInitialized()
{
    LockGuard<Mutex> guard(initBuiltinThunks);
    if (builtinThunks)
        return true;

    auto thunks = MakeUnique<BuiltinThunks>();
    if (!thunks)
        return false;

    LifoAlloc lifo(BUILTIN_THUNK_LIFO_SIZE);
    TempAllocator tempAlloc(&lifo);
    MacroAssembler masm(MacroAssembler::WasmToken(), tempAlloc);

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
        if (!GenerateBuiltinThunk(masm, abiType, exitReason, funcPtr, &offsets))
            return false;
        if (!thunks->codeRanges.emplaceBack(CodeRange::BuiltinThunk, offsets))
            return false;
    }

    TypedNativeToFuncPtrMap typedNatives;
    if (!PopulateTypedNatives(&typedNatives))
        return false;

    if (!thunks->typedNativeToCodeRange.init())
        return false;

    for (TypedNativeToFuncPtrMap::Range r = typedNatives.all(); !r.empty(); r.popFront()) {
        TypedNative typedNative = r.front().key();

        uint32_t codeRangeIndex = thunks->codeRanges.length();
        if (!thunks->typedNativeToCodeRange.putNew(typedNative, codeRangeIndex))
            return false;

        ABIFunctionType abiType = typedNative.abiType;
        void* funcPtr = r.front().value();

        ExitReason exitReason = ExitReason::Fixed::BuiltinNative;

        CallableOffsets offsets;
        if (!GenerateBuiltinThunk(masm, abiType, exitReason, funcPtr, &offsets))
            return false;
        if (!thunks->codeRanges.emplaceBack(CodeRange::BuiltinThunk, offsets))
            return false;
    }

    masm.finish();
    if (masm.oom())
        return false;

    size_t allocSize = AlignBytes(masm.bytesNeeded(), ExecutableCodePageSize);

    thunks->codeSize = allocSize;
    thunks->codeBase = (uint8_t*)AllocateExecutableMemory(allocSize, ProtectionSetting::Writable);
    if (!thunks->codeBase)
        return false;

    masm.executableCopy(thunks->codeBase, /* flushICache = */ false);
    memset(thunks->codeBase + masm.bytesNeeded(), 0, allocSize - masm.bytesNeeded());

    masm.processCodeLabels(thunks->codeBase);

    MOZ_ASSERT(masm.callSites().empty());
    MOZ_ASSERT(masm.callSiteTargets().empty());
    MOZ_ASSERT(masm.callFarJumps().empty());
    MOZ_ASSERT(masm.trapSites().empty());
    MOZ_ASSERT(masm.oldTrapSites().empty());
    MOZ_ASSERT(masm.oldTrapFarJumps().empty());
    MOZ_ASSERT(masm.callFarJumps().empty());
    MOZ_ASSERT(masm.memoryAccesses().empty());
    MOZ_ASSERT(masm.symbolicAccesses().empty());

    ExecutableAllocator::cacheFlush(thunks->codeBase, thunks->codeSize);
    if (!ExecutableAllocator::makeExecutable(thunks->codeBase, thunks->codeSize))
        return false;

    builtinThunks = thunks.release();
    return true;
}

void
wasm::ReleaseBuiltinThunks()
{
    if (builtinThunks) {
        const BuiltinThunks* ptr = builtinThunks;
        js_delete(const_cast<BuiltinThunks*>(ptr));
        builtinThunks = nullptr;
    }
}

void*
wasm::SymbolicAddressTarget(SymbolicAddress sym)
{
    MOZ_ASSERT(builtinThunks);

    ABIFunctionType abiType;
    void* funcPtr = AddressOf(sym, &abiType);

    if (!NeedsBuiltinThunk(sym))
        return funcPtr;

    const BuiltinThunks& thunks = *builtinThunks;
    uint32_t codeRangeIndex = thunks.symbolicAddressToCodeRange[sym];
    return thunks.codeBase + thunks.codeRanges[codeRangeIndex].begin();
}

static Maybe<ABIFunctionType>
ToBuiltinABIFunctionType(const Sig& sig)
{
    const ValTypeVector& args = sig.args();
    ExprType ret = sig.ret();

    uint32_t abiType;
    switch (ret) {
      case ExprType::F32: abiType = ArgType_Float32 << RetType_Shift; break;
      case ExprType::F64: abiType = ArgType_Double << RetType_Shift; break;
      default: return Nothing();
    }

    if ((args.length() + 1) > (sizeof(uint32_t) * 8 / ArgType_Shift))
        return Nothing();

    for (size_t i = 0; i < args.length(); i++) {
        switch (args[i]) {
          case ValType::F32: abiType |= (ArgType_Float32 << (ArgType_Shift * (i + 1))); break;
          case ValType::F64: abiType |= (ArgType_Double << (ArgType_Shift * (i + 1))); break;
          default: return Nothing();
        }
    }

    return Some(ABIFunctionType(abiType));
}

void*
wasm::MaybeGetBuiltinThunk(HandleFunction f, const Sig& sig)
{
    MOZ_ASSERT(builtinThunks);

    if (!f->isNative() || !f->hasJitInfo() || f->jitInfo()->type() != JSJitInfo::InlinableNative)
        return nullptr;

    Maybe<ABIFunctionType> abiType = ToBuiltinABIFunctionType(sig);
    if (!abiType)
        return nullptr;

    TypedNative typedNative(f->jitInfo()->inlinableNative, *abiType);

    const BuiltinThunks& thunks = *builtinThunks;
    auto p = thunks.typedNativeToCodeRange.readonlyThreadsafeLookup(typedNative);
    if (!p)
        return nullptr;

    return thunks.codeBase + thunks.codeRanges[p->value()].begin();
}

bool
wasm::LookupBuiltinThunk(void* pc, const CodeRange** codeRange, uint8_t** codeBase)
{
    if (!builtinThunks)
        return false;

    const BuiltinThunks& thunks = *builtinThunks;
    if (pc < thunks.codeBase || pc >= thunks.codeBase + thunks.codeSize)
        return false;

    *codeBase = thunks.codeBase;

    CodeRange::OffsetInCode target((uint8_t*)pc - thunks.codeBase);
    *codeRange = LookupInSorted(thunks.codeRanges, target);

    return !!*codeRange;
}

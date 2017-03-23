/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2014 Mozilla Foundation
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

#include "asmjs/AsmJSModule.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/Compression.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TaggedAnonymousMemory.h"
#include "mozilla/Vector.h"

#include "jslibmath.h"
#include "jsmath.h"
#include "jsprf.h"

#include "builtin/AtomicsObject.h"
#include "frontend/Parser.h"
#include "jit/IonCode.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/MemoryMetrics.h"
#include "vm/Time.h"

#include "jsobjinlines.h"

#include "frontend/ParseNode-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;
using namespace js::frontend;
using mozilla::BinarySearch;
using mozilla::Compression::LZ4;
using mozilla::MakeEnumeratedRange;
using mozilla::MallocSizeOf;
using mozilla::PodCopy;
using mozilla::PodEqual;
using mozilla::PodZero;
using mozilla::Swap;
using JS::GenericNaN;

static uint8_t*
AllocateExecutableMemory(ExclusiveContext* cx, size_t bytes)
{
    // On most platforms, this will allocate RWX memory. On iOS, or when
    // --non-writable-jitcode is used, this will allocate RW memory. In this
    // case, DynamicallyLinkModule will reprotect the code as RX.
    unsigned permissions =
        ExecutableAllocator::initialProtectionFlags(ExecutableAllocator::Writable);
    void* p = AllocateExecutableMemory(nullptr, bytes, permissions, "asm-js-code", AsmJSPageSize);
    if (!p)
        ReportOutOfMemory(cx);
    return (uint8_t*)p;
}

AsmJSModule::AsmJSModule(ScriptSource* scriptSource, uint32_t srcStart, uint32_t srcBodyStart,
                         bool strict, bool canUseSignalHandlers)
  : srcStart_(srcStart),
    srcBodyStart_(srcBodyStart),
    scriptSource_(scriptSource),
    globalArgumentName_(nullptr),
    importArgumentName_(nullptr),
    bufferArgumentName_(nullptr),
    code_(nullptr),
    interruptExit_(nullptr),
    prevLinked_(nullptr),
    nextLinked_(nullptr),
    dynamicallyLinked_(false),
    loadedFromCache_(false),
    profilingEnabled_(false),
    interrupted_(false)
{
    mozilla::PodZero(&pod);
    pod.globalBytes_ = sInitialGlobalDataBytes;
    pod.minHeapLength_ = RoundUpToNextValidAsmJSHeapLength(0);
    pod.maxHeapLength_ = 0x80000000;
    pod.strict_ = strict;
    pod.canUseSignalHandlers_ = canUseSignalHandlers;

    // AsmJSCheckedImmediateRange should be defined to be at most the minimum
    // heap length so that offsets can be folded into bounds checks.
    MOZ_ASSERT(pod.minHeapLength_ - AsmJSCheckedImmediateRange <= pod.minHeapLength_);

    scriptSource_->incref();
}

AsmJSModule::~AsmJSModule()
{
    MOZ_ASSERT(!interrupted_);

    scriptSource_->decref();

    if (code_) {
        for (unsigned i = 0; i < numExits(); i++) {
            AsmJSModule::ExitDatum& exitDatum = exit(i).datum(*this);
            if (!exitDatum.baselineScript)
                continue;

            jit::DependentAsmJSModuleExit exit(this, i);
            exitDatum.baselineScript->removeDependentAsmJSModule(exit);
        }

        DeallocateExecutableMemory(code_, pod.totalBytes_, AsmJSPageSize);
    }

    if (prevLinked_)
        *prevLinked_ = nextLinked_;
    if (nextLinked_)
        nextLinked_->prevLinked_ = prevLinked_;
}

void
AsmJSModule::trace(JSTracer* trc)
{
    for (Global& global : globals_)
        global.trace(trc);
    for (Exit& exit : exits_) {
        if (exit.datum(*this).fun)
            TraceEdge(trc, &exit.datum(*this).fun, "asm.js imported function");
    }
    for (ExportedFunction& exp : exports_)
        exp.trace(trc);
    for (Name& name : names_)
        TraceManuallyBarrieredEdge(trc, &name.name(), "asm.js module function name");
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    for (ProfiledFunction& profiledFunction : profiledFunctions_)
        profiledFunction.trace(trc);
#endif
    if (globalArgumentName_)
        TraceManuallyBarrieredEdge(trc, &globalArgumentName_, "asm.js global argument name");
    if (importArgumentName_)
        TraceManuallyBarrieredEdge(trc, &importArgumentName_, "asm.js import argument name");
    if (bufferArgumentName_)
        TraceManuallyBarrieredEdge(trc, &bufferArgumentName_, "asm.js buffer argument name");
    if (maybeHeap_)
        TraceEdge(trc, &maybeHeap_, "asm.js heap");
}

void
AsmJSModule::addSizeOfMisc(MallocSizeOf mallocSizeOf, size_t* asmJSModuleCode,
                           size_t* asmJSModuleData)
{
    *asmJSModuleCode += pod.totalBytes_;
    *asmJSModuleData += mallocSizeOf(this) +
                        globals_.sizeOfExcludingThis(mallocSizeOf) +
                        exits_.sizeOfExcludingThis(mallocSizeOf) +
                        exports_.sizeOfExcludingThis(mallocSizeOf) +
                        callSites_.sizeOfExcludingThis(mallocSizeOf) +
                        codeRanges_.sizeOfExcludingThis(mallocSizeOf) +
                        names_.sizeOfExcludingThis(mallocSizeOf) +
                        heapAccesses_.sizeOfExcludingThis(mallocSizeOf) +
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
                        profiledFunctions_.sizeOfExcludingThis(mallocSizeOf) +
#endif
                        staticLinkData_.sizeOfExcludingThis(mallocSizeOf);
}

struct CallSiteRetAddrOffset
{
    const CallSiteVector& callSites;
    explicit CallSiteRetAddrOffset(const CallSiteVector& callSites) : callSites(callSites) {}
    uint32_t operator[](size_t index) const {
        return callSites[index].returnAddressOffset();
    }
};

const CallSite*
AsmJSModule::lookupCallSite(void* returnAddress) const
{
    MOZ_ASSERT(isFinished());

    uint32_t target = ((uint8_t*)returnAddress) - code_;
    size_t lowerBound = 0;
    size_t upperBound = callSites_.length();

    size_t match;
    if (!BinarySearch(CallSiteRetAddrOffset(callSites_), lowerBound, upperBound, target, &match))
        return nullptr;

    return &callSites_[match];
}

namespace js {

// Create an ordering on CodeRange and pc offsets suitable for BinarySearch.
// Stick these in the same namespace as AsmJSModule so that argument-dependent
// lookup will find it.
bool
operator==(size_t pcOffset, const AsmJSModule::CodeRange& rhs)
{
    return pcOffset >= rhs.begin() && pcOffset < rhs.end();
}
bool
operator<=(const AsmJSModule::CodeRange& lhs, const AsmJSModule::CodeRange& rhs)
{
    return lhs.begin() <= rhs.begin();
}
bool
operator<(size_t pcOffset, const AsmJSModule::CodeRange& rhs)
{
    return pcOffset < rhs.begin();
}

} // namespace js

const AsmJSModule::CodeRange*
AsmJSModule::lookupCodeRange(void* pc) const
{
    MOZ_ASSERT(isFinished());

    uint32_t target = ((uint8_t*)pc) - code_;
    size_t lowerBound = 0;
    size_t upperBound = codeRanges_.length();

    size_t match;
    if (!BinarySearch(codeRanges_, lowerBound, upperBound, target, &match))
        return nullptr;

    return &codeRanges_[match];
}

struct HeapAccessOffset
{
    const HeapAccessVector& accesses;
    explicit HeapAccessOffset(const HeapAccessVector& accesses) : accesses(accesses) {}
    uintptr_t operator[](size_t index) const {
        return accesses[index].insnOffset();
    }
};

const HeapAccess*
AsmJSModule::lookupHeapAccess(void* pc) const
{
    MOZ_ASSERT(isFinished());
    MOZ_ASSERT(containsFunctionPC(pc));

    uint32_t target = ((uint8_t*)pc) - code_;
    size_t lowerBound = 0;
    size_t upperBound = heapAccesses_.length();

    size_t match;
    if (!BinarySearch(HeapAccessOffset(heapAccesses_), lowerBound, upperBound, target, &match))
        return nullptr;

    return &heapAccesses_[match];
}

bool
AsmJSModule::finish(ExclusiveContext* cx, TokenStream& tokenStream, MacroAssembler& masm)
{
    MOZ_ASSERT(!isFinished());

    uint32_t endBeforeCurly = tokenStream.currentToken().pos.end;
    TokenPos pos;
    if (!tokenStream.peekTokenPos(&pos, TokenStream::Operand))
        return false;
    uint32_t endAfterCurly = pos.end;
    MOZ_ASSERT(endBeforeCurly >= srcBodyStart_);
    MOZ_ASSERT(endAfterCurly >= srcBodyStart_);
    pod.srcLength_ = endBeforeCurly - srcStart_;
    pod.srcLengthWithRightBrace_ = endAfterCurly - srcStart_;

    // Start global data on a new page so JIT code may be given independent
    // protection flags.
    pod.codeBytes_ = AlignBytes(masm.bytesNeeded(), AsmJSPageSize);
    MOZ_ASSERT(pod.functionBytes_ <= pod.codeBytes_);

    // The entire region is allocated via mmap/VirtualAlloc which requires
    // units of pages.
    pod.totalBytes_ = AlignBytes(pod.codeBytes_ + pod.globalBytes_, AsmJSPageSize);

    MOZ_ASSERT(!code_);
    code_ = AllocateExecutableMemory(cx, pod.totalBytes_);
    if (!code_)
        return false;

    // Delay flushing until dynamic linking. The flush-inhibited range is set within
    // masm.executableCopy.
    AutoFlushICache afc("CheckModule", /* inhibit = */ true);

    // Copy the code from the MacroAssembler into its final resting place in the
    // AsmJSModule.
    MOZ_ASSERT(uintptr_t(code_) % AsmJSPageSize == 0);
    masm.executableCopy(code_);

    // c.f. JitCode::copyFrom
    MOZ_ASSERT(masm.jumpRelocationTableBytes() == 0);
    MOZ_ASSERT(masm.dataRelocationTableBytes() == 0);
    MOZ_ASSERT(masm.preBarrierTableBytes() == 0);
    MOZ_ASSERT(!masm.hasSelfReference());

    // Heap-access metadata used for link-time patching and fault-handling.
    heapAccesses_ = masm.extractHeapAccesses();

    // Call-site metadata used for stack unwinding.
    const CallSiteAndTargetVector& callSites = masm.callSites();
    if (!callSites_.appendAll(callSites))
        return false;

    // Absolute link metadata: absolute addresses that refer to some fixed
    // address in the address space.
    AbsoluteLinkArray& absoluteLinks = staticLinkData_.absoluteLinks;
    for (size_t i = 0; i < masm.numAsmJSAbsoluteLinks(); i++) {
        AsmJSAbsoluteLink src = masm.asmJSAbsoluteLink(i);
        if (!absoluteLinks[src.target].append(src.patchAt.offset()))
            return false;
    }

    // Relative link metadata: absolute addresses that refer to another point within
    // the asm.js module.

    // CodeLabels are used for switch cases and loads from floating-point /
    // SIMD values in the constant pool.
    for (size_t i = 0; i < masm.numCodeLabels(); i++) {
        CodeLabel cl = masm.codeLabel(i);
        RelativeLink link(RelativeLink::CodeLabel);
        link.patchAtOffset = masm.labelToPatchOffset(*cl.patchAt());
        link.targetOffset = cl.target()->offset();
        if (!staticLinkData_.relativeLinks.append(link))
            return false;
    }

#if defined(JS_CODEGEN_X86)
    // Global data accesses in x86 need to be patched with the absolute
    // address of the global. Globals are allocated sequentially after the
    // code section so we can just use an RelativeLink.
    for (size_t i = 0; i < masm.numAsmJSGlobalAccesses(); i++) {
        AsmJSGlobalAccess a = masm.asmJSGlobalAccess(i);
        RelativeLink link(RelativeLink::RawPointer);
        link.patchAtOffset = masm.labelToPatchOffset(a.patchAt);
        link.targetOffset = offsetOfGlobalData() + a.globalDataOffset;
        if (!staticLinkData_.relativeLinks.append(link))
            return false;
    }
#endif

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    // On MIPS we need to update all the long jumps because they contain an
    // absolute adress. The values are correctly patched for the current address
    // space, but not after serialization or profiling-mode toggling.
    for (size_t i = 0; i < masm.numLongJumps(); i++) {
        size_t off = masm.longJump(i);
        RelativeLink link(RelativeLink::InstructionImmediate);
        link.patchAtOffset = off;
        link.targetOffset = Assembler::ExtractInstructionImmediate(code_ + off) - uintptr_t(code_);
        if (!staticLinkData_.relativeLinks.append(link))
            return false;
    }
#endif

#if defined(JS_CODEGEN_X64)
    // Global data accesses on x64 use rip-relative addressing and thus do
    // not need patching after deserialization.
    for (size_t i = 0; i < masm.numAsmJSGlobalAccesses(); i++) {
        AsmJSGlobalAccess a = masm.asmJSGlobalAccess(i);
        masm.patchAsmJSGlobalAccess(a.patchAt, code_, globalData(), a.globalDataOffset);
    }
#endif

    return true;
}

void
AsmJSModule::setAutoFlushICacheRange()
{
    MOZ_ASSERT(isFinished());
    AutoFlushICache::setRange(uintptr_t(code_), pod.codeBytes_);
}

static void
AsmJSReportOverRecursed()
{
    JSContext* cx = JSRuntime::innermostAsmJSActivation()->cx();
    ReportOverRecursed(cx);
}

static void
OnDetached()
{
    // See hasDetachedHeap comment in LinkAsmJS.
    JSContext* cx = JSRuntime::innermostAsmJSActivation()->cx();
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_OUT_OF_MEMORY);
}

static void
OnOutOfBounds()
{
    JSContext* cx = JSRuntime::innermostAsmJSActivation()->cx();
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_BAD_INDEX);
}

static void
OnImpreciseConversion()
{
    JSContext* cx = JSRuntime::innermostAsmJSActivation()->cx();
    JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_SIMD_FAILED_CONVERSION);
}

static bool
AsmJSHandleExecutionInterrupt()
{
    AsmJSActivation* act = JSRuntime::innermostAsmJSActivation();
    act->module().setInterrupted(true);
    bool ret = CheckForInterrupt(act->cx());
    act->module().setInterrupted(false);
    return ret;
}

static int32_t
CoerceInPlace_ToInt32(MutableHandleValue val)
{
    JSContext* cx = JSRuntime::innermostAsmJSActivation()->cx();

    int32_t i32;
    if (!ToInt32(cx, val, &i32))
        return false;
    val.set(Int32Value(i32));

    return true;
}

static int32_t
CoerceInPlace_ToNumber(MutableHandleValue val)
{
    JSContext* cx = JSRuntime::innermostAsmJSActivation()->cx();

    double dbl;
    if (!ToNumber(cx, val, &dbl))
        return false;
    val.set(DoubleValue(dbl));

    return true;
}

static bool
TryEnablingJit(JSContext* cx, AsmJSModule& module, HandleFunction fun, uint32_t exitIndex,
               int32_t argc, Value* argv)
{
    if (!fun->hasScript())
        return true;

    // Test if the function is JIT compiled.
    JSScript* script = fun->nonLazyScript();
    if (!script->hasBaselineScript()) {
        MOZ_ASSERT(!script->hasIonScript());
        return true;
    }

    // Don't enable jit entry when we have a pending ion builder.
    // Take the interpreter path which will link it and enable
    // the fast path on the next call.
    if (script->baselineScript()->hasPendingIonBuilder())
        return true;

    // Currently we can't rectify arguments. Therefore disabling if argc is too low.
    if (fun->nargs() > size_t(argc))
        return true;

    // Ensure the argument types are included in the argument TypeSets stored in
    // the TypeScript. This is necessary for Ion, because the FFI exit will
    // use the skip-arg-checks entry point.
    //
    // Note that the TypeScript is never discarded while the script has a
    // BaselineScript, so if those checks hold now they must hold at least until
    // the BaselineScript is discarded and when that happens the FFI exit is
    // patched back.
    if (!TypeScript::ThisTypes(script)->hasType(TypeSet::UndefinedType()))
        return true;
    for (uint32_t i = 0; i < fun->nargs(); i++) {
        StackTypeSet* typeset = TypeScript::ArgTypes(script, i);
        TypeSet::Type type = TypeSet::DoubleType();
        if (!argv[i].isDouble())
            type = TypeSet::PrimitiveType(argv[i].extractNonDoubleType());
        if (!typeset->hasType(type))
            return true;
    }

    // The exit may have become optimized while executing the FFI.
    AsmJSModule::Exit& exit = module.exit(exitIndex);
    if (exit.isOptimized(module))
        return true;

    BaselineScript* baselineScript = script->baselineScript();
    if (!baselineScript->addDependentAsmJSModule(cx, DependentAsmJSModuleExit(&module, exitIndex)))
        return false;

    exit.optimize(module, baselineScript);
    return true;
}

static bool
InvokeFromAsmJS(AsmJSActivation* activation, int32_t exitIndex, int32_t argc, Value* argv,
                MutableHandleValue rval)
{
    JSContext* cx = activation->cx();
    AsmJSModule& module = activation->module();

    RootedFunction fun(cx, module.exit(exitIndex).datum(module).fun);
    RootedValue fval(cx, ObjectValue(*fun));
    if (!Invoke(cx, UndefinedValue(), fval, argc, argv, rval))
        return false;

    return TryEnablingJit(cx, module, fun, exitIndex, argc, argv);
}

// Use an int32_t return type instead of bool since bool does not have a
// specified width and the caller is assuming a word-sized return.
static int32_t
InvokeFromAsmJS_Ignore(int32_t exitIndex, int32_t argc, Value* argv)
{
    AsmJSActivation* activation = JSRuntime::innermostAsmJSActivation();
    JSContext* cx = activation->cx();

    RootedValue rval(cx);
    return InvokeFromAsmJS(activation, exitIndex, argc, argv, &rval);
}

// Use an int32_t return type instead of bool since bool does not have a
// specified width and the caller is assuming a word-sized return.
static int32_t
InvokeFromAsmJS_ToInt32(int32_t exitIndex, int32_t argc, Value* argv)
{
    AsmJSActivation* activation = JSRuntime::innermostAsmJSActivation();
    JSContext* cx = activation->cx();

    RootedValue rval(cx);
    if (!InvokeFromAsmJS(activation, exitIndex, argc, argv, &rval))
        return false;

    int32_t i32;
    if (!ToInt32(cx, rval, &i32))
        return false;

    argv[0] = Int32Value(i32);
    return true;
}

// Use an int32_t return type instead of bool since bool does not have a
// specified width and the caller is assuming a word-sized return.
static int32_t
InvokeFromAsmJS_ToNumber(int32_t exitIndex, int32_t argc, Value* argv)
{
    AsmJSActivation* activation = JSRuntime::innermostAsmJSActivation();
    JSContext* cx = activation->cx();

    RootedValue rval(cx);
    if (!InvokeFromAsmJS(activation, exitIndex, argc, argv, &rval))
        return false;

    double dbl;
    if (!ToNumber(cx, rval, &dbl))
        return false;

    argv[0] = DoubleValue(dbl);
    return true;
}

#if defined(JS_CODEGEN_ARM)
extern "C" {

extern MOZ_EXPORT int64_t
__aeabi_idivmod(int, int);

extern MOZ_EXPORT int64_t
__aeabi_uidivmod(int, int);

}
#endif

template <class F>
static inline void*
FuncCast(F* pf)
{
    return JS_FUNC_TO_DATA_PTR(void*, pf);
}

static void*
RedirectCall(void* fun, ABIFunctionType type)
{
#ifdef JS_SIMULATOR
    fun = Simulator::RedirectNativeFunction(fun, type);
#endif
    return fun;
}

static void*
AddressOf(SymbolicAddress imm, ExclusiveContext* cx)
{
    switch (imm) {
      case SymbolicAddress::Runtime:
        return cx->runtimeAddressForJit();
      case SymbolicAddress::RuntimeInterruptUint32:
        return cx->runtimeAddressOfInterruptUint32();
      case SymbolicAddress::StackLimit:
        return cx->stackLimitAddressForJitCode(StackForUntrustedScript);
      case SymbolicAddress::ReportOverRecursed:
        return RedirectCall(FuncCast(AsmJSReportOverRecursed), Args_General0);
      case SymbolicAddress::OnDetached:
        return RedirectCall(FuncCast(OnDetached), Args_General0);
      case SymbolicAddress::OnOutOfBounds:
        return RedirectCall(FuncCast(OnOutOfBounds), Args_General0);
      case SymbolicAddress::OnImpreciseConversion:
        return RedirectCall(FuncCast(OnImpreciseConversion), Args_General0);
      case SymbolicAddress::HandleExecutionInterrupt:
        return RedirectCall(FuncCast(AsmJSHandleExecutionInterrupt), Args_General0);
      case SymbolicAddress::InvokeFromAsmJS_Ignore:
        return RedirectCall(FuncCast(InvokeFromAsmJS_Ignore), Args_General3);
      case SymbolicAddress::InvokeFromAsmJS_ToInt32:
        return RedirectCall(FuncCast(InvokeFromAsmJS_ToInt32), Args_General3);
      case SymbolicAddress::InvokeFromAsmJS_ToNumber:
        return RedirectCall(FuncCast(InvokeFromAsmJS_ToNumber), Args_General3);
      case SymbolicAddress::CoerceInPlace_ToInt32:
        return RedirectCall(FuncCast(CoerceInPlace_ToInt32), Args_General1);
      case SymbolicAddress::CoerceInPlace_ToNumber:
        return RedirectCall(FuncCast(CoerceInPlace_ToNumber), Args_General1);
      case SymbolicAddress::ToInt32:
        return RedirectCall(FuncCast<int32_t (double)>(JS::ToInt32), Args_Int_Double);
#if defined(JS_CODEGEN_ARM)
      case SymbolicAddress::aeabi_idivmod:
        return RedirectCall(FuncCast(__aeabi_idivmod), Args_General2);
      case SymbolicAddress::aeabi_uidivmod:
        return RedirectCall(FuncCast(__aeabi_uidivmod), Args_General2);
      case SymbolicAddress::AtomicCmpXchg:
        return RedirectCall(FuncCast<int32_t (int32_t, int32_t, int32_t, int32_t)>(js::atomics_cmpxchg_asm_callout), Args_General4);
      case SymbolicAddress::AtomicXchg:
        return RedirectCall(FuncCast<int32_t (int32_t, int32_t, int32_t)>(js::atomics_xchg_asm_callout), Args_General3);
      case SymbolicAddress::AtomicFetchAdd:
        return RedirectCall(FuncCast<int32_t (int32_t, int32_t, int32_t)>(js::atomics_add_asm_callout), Args_General3);
      case SymbolicAddress::AtomicFetchSub:
        return RedirectCall(FuncCast<int32_t (int32_t, int32_t, int32_t)>(js::atomics_sub_asm_callout), Args_General3);
      case SymbolicAddress::AtomicFetchAnd:
        return RedirectCall(FuncCast<int32_t (int32_t, int32_t, int32_t)>(js::atomics_and_asm_callout), Args_General3);
      case SymbolicAddress::AtomicFetchOr:
        return RedirectCall(FuncCast<int32_t (int32_t, int32_t, int32_t)>(js::atomics_or_asm_callout), Args_General3);
      case SymbolicAddress::AtomicFetchXor:
        return RedirectCall(FuncCast<int32_t (int32_t, int32_t, int32_t)>(js::atomics_xor_asm_callout), Args_General3);
#endif
      case SymbolicAddress::ModD:
        return RedirectCall(FuncCast(NumberMod), Args_Double_DoubleDouble);
      case SymbolicAddress::SinD:
#ifdef _WIN64
        // Workaround a VS 2013 sin issue, see math_sin_uncached.
        return RedirectCall(FuncCast<double (double)>(js::math_sin_uncached), Args_Double_Double);
#else
        return RedirectCall(FuncCast<double (double)>(sin), Args_Double_Double);
#endif
      case SymbolicAddress::CosD:
        return RedirectCall(FuncCast<double (double)>(cos), Args_Double_Double);
      case SymbolicAddress::TanD:
        return RedirectCall(FuncCast<double (double)>(tan), Args_Double_Double);
      case SymbolicAddress::ASinD:
        return RedirectCall(FuncCast<double (double)>(asin), Args_Double_Double);
      case SymbolicAddress::ACosD:
        return RedirectCall(FuncCast<double (double)>(acos), Args_Double_Double);
      case SymbolicAddress::ATanD:
        return RedirectCall(FuncCast<double (double)>(atan), Args_Double_Double);
      case SymbolicAddress::CeilD:
        return RedirectCall(FuncCast<double (double)>(ceil), Args_Double_Double);
      case SymbolicAddress::CeilF:
        return RedirectCall(FuncCast<float (float)>(ceilf), Args_Float32_Float32);
      case SymbolicAddress::FloorD:
        return RedirectCall(FuncCast<double (double)>(floor), Args_Double_Double);
      case SymbolicAddress::FloorF:
        return RedirectCall(FuncCast<float (float)>(floorf), Args_Float32_Float32);
      case SymbolicAddress::ExpD:
        return RedirectCall(FuncCast<double (double)>(exp), Args_Double_Double);
      case SymbolicAddress::LogD:
        return RedirectCall(FuncCast<double (double)>(log), Args_Double_Double);
      case SymbolicAddress::PowD:
        return RedirectCall(FuncCast(ecmaPow), Args_Double_DoubleDouble);
      case SymbolicAddress::ATan2D:
        return RedirectCall(FuncCast(ecmaAtan2), Args_Double_DoubleDouble);
      case SymbolicAddress::Limit:
        break;
    }

    MOZ_CRASH("Bad SymbolicAddress");
}

void
AsmJSModule::staticallyLink(ExclusiveContext* cx)
{
    MOZ_ASSERT(isFinished());

    // Process staticLinkData_

    MOZ_ASSERT(staticLinkData_.pod.interruptExitOffset != 0);
    interruptExit_ = code_ + staticLinkData_.pod.interruptExitOffset;

    MOZ_ASSERT(staticLinkData_.pod.outOfBoundsExitOffset != 0);
    outOfBoundsExit_ = code_ + staticLinkData_.pod.outOfBoundsExitOffset;

    for (size_t i = 0; i < staticLinkData_.relativeLinks.length(); i++) {
        RelativeLink link = staticLinkData_.relativeLinks[i];
        uint8_t* patchAt = code_ + link.patchAtOffset;
        uint8_t* target = code_ + link.targetOffset;

        // In the case of long-jumps on MIPS and possibly future cases, a
        // RelativeLink is used to patch a pointer to the function entry. If
        // profiling is enabled (by cloning a module with profiling enabled),
        // the target should be the profiling entry.
        if (profilingEnabled_) {
            const CodeRange* codeRange = lookupCodeRange(target);
            if (codeRange && codeRange->isFunction() && link.targetOffset == codeRange->entry())
                target = code_ + codeRange->profilingEntry();
        }

        if (link.isRawPointerPatch())
            *(uint8_t**)(patchAt) = target;
        else
            Assembler::PatchInstructionImmediate(patchAt, PatchedImmPtr(target));
    }

    for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
        const OffsetVector& offsets = staticLinkData_.absoluteLinks[imm];
        for (size_t i = 0; i < offsets.length(); i++) {
            uint8_t* patchAt = code_ + offsets[i];
            void* target = AddressOf(imm, cx);

            // Builtin calls are another case where, when profiling is enabled,
            // we must point to the profiling entry.
            Builtin builtin;
            if (profilingEnabled_ && ImmediateIsBuiltin(imm, &builtin)) {
                const CodeRange* codeRange = lookupCodeRange(patchAt);
                if (codeRange->isFunction())
                    target = code_ + staticLinkData_.pod.builtinThunkOffsets[builtin];
            }

            Assembler::PatchDataWithValueCheck(CodeLocationLabel(patchAt),
                                               PatchedImmPtr(target),
                                               PatchedImmPtr((void*)-1));
        }
    }

    // Initialize global data segment

    *(double*)(globalData() + NaN64GlobalDataOffset) = GenericNaN();
    *(float*)(globalData() + NaN32GlobalDataOffset) = GenericNaN();

    for (size_t tableIndex = 0; tableIndex < staticLinkData_.funcPtrTables.length(); tableIndex++) {
        FuncPtrTable& funcPtrTable = staticLinkData_.funcPtrTables[tableIndex];
        const OffsetVector& offsets = funcPtrTable.elemOffsets();
        auto array = reinterpret_cast<void**>(globalData() + funcPtrTable.globalDataOffset());
        for (size_t elemIndex = 0; elemIndex < offsets.length(); elemIndex++) {
            uint8_t* target = code_ + offsets[elemIndex];
            if (profilingEnabled_)
                target = code_ + lookupCodeRange(target)->profilingEntry();
            array[elemIndex] = target;
        }
    }

    for (AsmJSModule::Exit& exit : exits_)
        exit.initDatum(*this);
}

void
AsmJSModule::initHeap(Handle<ArrayBufferObjectMaybeShared*> heap, JSContext* cx)
{
    MOZ_ASSERT_IF(heap->is<ArrayBufferObject>(), heap->as<ArrayBufferObject>().isAsmJS());
    MOZ_ASSERT(IsValidAsmJSHeapLength(heap->byteLength()));
    MOZ_ASSERT(dynamicallyLinked_);
    MOZ_ASSERT(!maybeHeap_);

    maybeHeap_ = heap;
    // heapDatum() may point to shared memory but that memory is only
    // accessed from maybeHeap(), which wraps it, and from
    // hasDetachedHeap(), which checks it for null.
    heapDatum() = heap->dataPointerEither().unwrap(/*safe - explained above*/);

#if defined(JS_CODEGEN_X86)
    uint8_t* heapOffset = heap->dataPointerEither().unwrap(/*safe - used for value*/);
    uint32_t heapLength = heap->byteLength();
    for (unsigned i = 0; i < heapAccesses_.length(); i++) {
        const HeapAccess& access = heapAccesses_[i];
        // An access is out-of-bounds iff
        //      ptr + offset + data-type-byte-size > heapLength
        // i.e. ptr > heapLength - data-type-byte-size - offset.
        // data-type-byte-size and offset are already included in the addend
        // so we just have to add the heap length here.
        if (access.hasLengthCheck())
            X86Encoding::AddInt32(access.patchLengthAt(code_), heapLength);
        void* addr = access.patchHeapPtrImmAt(code_);
        uint32_t disp = reinterpret_cast<uint32_t>(X86Encoding::GetPointer(addr));
        MOZ_ASSERT(disp <= INT32_MAX);
        X86Encoding::SetPointer(addr, (void*)(heapOffset + disp));
    }
#elif defined(JS_CODEGEN_X64)
    // Even with signal handling being used for most bounds checks, there may be
    // atomic operations that depend on explicit checks.
    //
    // If we have any explicit bounds checks, we need to patch the heap length
    // checks at the right places. All accesses that have been recorded are the
    // only ones that need bound checks (see also
    // CodeGeneratorX64::visitAsmJS{Load,Store,CompareExchange,Exchange,AtomicBinop}Heap)
    uint32_t heapLength = heap->byteLength();
    for (size_t i = 0; i < heapAccesses_.length(); i++) {
        const HeapAccess& access = heapAccesses_[i];
        // See comment above for x86 codegen.
        if (access.hasLengthCheck())
            X86Encoding::AddInt32(access.patchLengthAt(code_), heapLength);
    }
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    uint32_t heapLength = heap->byteLength();
    for (unsigned i = 0; i < heapAccesses_.length(); i++) {
        jit::Assembler::UpdateBoundsCheck(heapLength,
                                          (jit::Instruction*)(heapAccesses_[i].insnOffset() + code_));
    }
#endif
}

void
AsmJSModule::restoreHeapToInitialState(ArrayBufferObjectMaybeShared* maybePrevBuffer)
{
#if defined(JS_CODEGEN_X86)
    if (maybePrevBuffer) {
        // Subtract out the base-pointer added by AsmJSModule::initHeap.
        uint8_t* ptrBase = maybePrevBuffer->dataPointerEither().unwrap(/*safe - used for value*/);
        uint32_t heapLength = maybePrevBuffer->byteLength();
        for (unsigned i = 0; i < heapAccesses_.length(); i++) {
            const HeapAccess& access = heapAccesses_[i];
            // Subtract the heap length back out, leaving the raw displacement in place.
            if (access.hasLengthCheck())
                X86Encoding::AddInt32(access.patchLengthAt(code_), -heapLength);
            void* addr = access.patchHeapPtrImmAt(code_);
            uint8_t* ptr = reinterpret_cast<uint8_t*>(X86Encoding::GetPointer(addr));
            MOZ_ASSERT(ptr >= ptrBase);
            X86Encoding::SetPointer(addr, (void*)(ptr - ptrBase));
        }
    }
#elif defined(JS_CODEGEN_X64)
    if (maybePrevBuffer) {
        uint32_t heapLength = maybePrevBuffer->byteLength();
        for (unsigned i = 0; i < heapAccesses_.length(); i++) {
            const HeapAccess& access = heapAccesses_[i];
            // See comment above for x86 codegen.
            if (access.hasLengthCheck())
                X86Encoding::AddInt32(access.patchLengthAt(code_), -heapLength);
        }
    }
#endif

    maybeHeap_ = nullptr;
    heapDatum() = nullptr;
}

void
AsmJSModule::restoreToInitialState(ArrayBufferObjectMaybeShared* maybePrevBuffer,
                                   uint8_t* prevCode,
                                   ExclusiveContext* cx)
{
#ifdef DEBUG
    // Put the absolute links back to -1 so PatchDataWithValueCheck assertions
    // in staticallyLink are valid.
    for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
        void* callee = AddressOf(imm, cx);

        // If we are in profiling mode, calls to builtins will have been patched
        // by setProfilingEnabled to be calls to thunks.
        Builtin builtin;
        void* profilingCallee = profilingEnabled_ && ImmediateIsBuiltin(imm, &builtin)
                                ? prevCode + staticLinkData_.pod.builtinThunkOffsets[builtin]
                                : nullptr;

        const AsmJSModule::OffsetVector& offsets = staticLinkData_.absoluteLinks[imm];
        for (size_t i = 0; i < offsets.length(); i++) {
            uint8_t* caller = code_ + offsets[i];
            void* originalValue = profilingCallee && !lookupCodeRange(caller)->isThunk()
                                  ? profilingCallee
                                  : callee;
            Assembler::PatchDataWithValueCheck(CodeLocationLabel(caller),
                                               PatchedImmPtr((void*)-1),
                                               PatchedImmPtr(originalValue));
        }
    }
#endif

    restoreHeapToInitialState(maybePrevBuffer);
}

namespace {

class MOZ_STACK_CLASS AutoMutateCode
{
    AutoWritableJitCode awjc_;
    AutoFlushICache afc_;

   public:
    AutoMutateCode(JSContext* cx, AsmJSModule& module, const char* name)
      : awjc_(cx->runtime(), module.codeBase(), module.codeBytes()),
        afc_(name)
    {
        module.setAutoFlushICacheRange();
    }
};

} // namespace

bool
AsmJSModule::detachHeap(JSContext* cx)
{
    MOZ_ASSERT(isDynamicallyLinked());
    MOZ_ASSERT(maybeHeap_);

    // Content JS should not be able to run (and detach heap) from within an
    // interrupt callback, but in case it does, fail. Otherwise, the heap can
    // change at an arbitrary instruction and break the assumption below.
    if (interrupted_) {
        JS_ReportError(cx, "attempt to detach from inside interrupt handler");
        return false;
    }

    // Even if this->active(), to reach here, the activation must have called
    // out via an FFI stub. FFI stubs check if heapDatum() is null on reentry
    // and throw an exception if so.
    MOZ_ASSERT_IF(active(), activation()->exitReason().kind() == ExitReason::Jit ||
                            activation()->exitReason().kind() == ExitReason::Slow);

    AutoMutateCode amc(cx, *this, "AsmJSModule::detachHeap");
    restoreHeapToInitialState(maybeHeap_);

    MOZ_ASSERT(hasDetachedHeap());
    return true;
}

bool
js::OnDetachAsmJSArrayBuffer(JSContext* cx, Handle<ArrayBufferObject*> buffer)
{
    for (AsmJSModule* m = cx->runtime()->linkedAsmJSModules; m; m = m->nextLinked()) {
        if (buffer == m->maybeHeapBufferObject() && !m->detachHeap(cx))
            return false;
    }
    return true;
}

static void
AsmJSModuleObject_finalize(FreeOp* fop, JSObject* obj)
{
    fop->delete_(&obj->as<AsmJSModuleObject>().module());
}

static void
AsmJSModuleObject_trace(JSTracer* trc, JSObject* obj)
{
    obj->as<AsmJSModuleObject>().module().trace(trc);
}

const Class AsmJSModuleObject::class_ = {
    "AsmJSModuleObject",
    JSCLASS_IS_ANONYMOUS | JSCLASS_DELAY_METADATA_CALLBACK |
    JSCLASS_HAS_RESERVED_SLOTS(AsmJSModuleObject::RESERVED_SLOTS),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    AsmJSModuleObject_finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    AsmJSModuleObject_trace
};

AsmJSModuleObject*
AsmJSModuleObject::create(ExclusiveContext* cx, ScopedJSDeletePtr<AsmJSModule>* module)
{
    AutoSetNewObjectMetadata metadata(cx);
    JSObject* obj = NewObjectWithGivenProto(cx, &AsmJSModuleObject::class_, nullptr);
    if (!obj)
        return nullptr;
    AsmJSModuleObject* nobj = &obj->as<AsmJSModuleObject>();

    nobj->setReservedSlot(MODULE_SLOT, PrivateValue(module->forget()));

    return nobj;
}

AsmJSModule&
AsmJSModuleObject::module() const
{
    MOZ_ASSERT(is<AsmJSModuleObject>());
    return *(AsmJSModule*)getReservedSlot(MODULE_SLOT).toPrivate();
}

static inline uint8_t*
WriteBytes(uint8_t* dst, const void* src, size_t nbytes)
{
    memcpy(dst, src, nbytes);
    return dst + nbytes;
}

static inline const uint8_t*
ReadBytes(const uint8_t* src, void* dst, size_t nbytes)
{
    memcpy(dst, src, nbytes);
    return src + nbytes;
}

template <class T>
static inline uint8_t*
WriteScalar(uint8_t* dst, T t)
{
    memcpy(dst, &t, sizeof(t));
    return dst + sizeof(t);
}

template <class T>
static inline const uint8_t*
ReadScalar(const uint8_t* src, T* dst)
{
    memcpy(dst, src, sizeof(*dst));
    return src + sizeof(*dst);
}

static size_t
SerializedNameSize(PropertyName* name)
{
    size_t s = sizeof(uint32_t);
    if (name)
        s += name->length() * (name->hasLatin1Chars() ? sizeof(Latin1Char) : sizeof(char16_t));
    return s;
}

size_t
AsmJSModule::Name::serializedSize() const
{
    return SerializedNameSize(name_);
}

static uint8_t*
SerializeName(uint8_t* cursor, PropertyName* name)
{
    MOZ_ASSERT_IF(name, !name->empty());
    if (name) {
        static_assert(JSString::MAX_LENGTH <= INT32_MAX, "String length must fit in 31 bits");
        uint32_t length = name->length();
        uint32_t lengthAndEncoding = (length << 1) | uint32_t(name->hasLatin1Chars());
        cursor = WriteScalar<uint32_t>(cursor, lengthAndEncoding);
        JS::AutoCheckCannotGC nogc;
        if (name->hasLatin1Chars())
            cursor = WriteBytes(cursor, name->latin1Chars(nogc), length * sizeof(Latin1Char));
        else
            cursor = WriteBytes(cursor, name->twoByteChars(nogc), length * sizeof(char16_t));
    } else {
        cursor = WriteScalar<uint32_t>(cursor, 0);
    }
    return cursor;
}

uint8_t*
AsmJSModule::Name::serialize(uint8_t* cursor) const
{
    return SerializeName(cursor, name_);
}

template <typename CharT>
static const uint8_t*
DeserializeChars(ExclusiveContext* cx, const uint8_t* cursor, size_t length, PropertyName** name)
{
    Vector<CharT> tmp(cx);
    CharT* src;
    if ((size_t(cursor) & (sizeof(CharT) - 1)) != 0) {
        // Align 'src' for AtomizeChars.
        if (!tmp.resize(length))
            return nullptr;
        memcpy(tmp.begin(), cursor, length * sizeof(CharT));
        src = tmp.begin();
    } else {
        src = (CharT*)cursor;
    }

    JSAtom* atom = AtomizeChars(cx, src, length);
    if (!atom)
        return nullptr;

    *name = atom->asPropertyName();
    return cursor + length * sizeof(CharT);
}

static const uint8_t*
DeserializeName(ExclusiveContext* cx, const uint8_t* cursor, PropertyName** name)
{
    uint32_t lengthAndEncoding;
    cursor = ReadScalar<uint32_t>(cursor, &lengthAndEncoding);

    uint32_t length = lengthAndEncoding >> 1;
    if (length == 0) {
        *name = nullptr;
        return cursor;
    }

    bool latin1 = lengthAndEncoding & 0x1;
    return latin1
           ? DeserializeChars<Latin1Char>(cx, cursor, length, name)
           : DeserializeChars<char16_t>(cx, cursor, length, name);
}

const uint8_t*
AsmJSModule::Name::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    return DeserializeName(cx, cursor, &name_);
}

bool
AsmJSModule::Name::clone(ExclusiveContext* cx, Name* out) const
{
    out->name_ = name_;
    return true;
}

template <class T, size_t N>
size_t
SerializedVectorSize(const mozilla::Vector<T, N, SystemAllocPolicy>& vec)
{
    size_t size = sizeof(uint32_t);
    for (size_t i = 0; i < vec.length(); i++)
        size += vec[i].serializedSize();
    return size;
}

template <class T, size_t N>
uint8_t*
SerializeVector(uint8_t* cursor, const mozilla::Vector<T, N, SystemAllocPolicy>& vec)
{
    cursor = WriteScalar<uint32_t>(cursor, vec.length());
    for (size_t i = 0; i < vec.length(); i++)
        cursor = vec[i].serialize(cursor);
    return cursor;
}

template <class T, size_t N>
const uint8_t*
DeserializeVector(ExclusiveContext* cx, const uint8_t* cursor,
                  mozilla::Vector<T, N, SystemAllocPolicy>* vec)
{
    uint32_t length;
    cursor = ReadScalar<uint32_t>(cursor, &length);
    if (!vec->resize(length))
        return nullptr;
    for (size_t i = 0; i < vec->length(); i++) {
        if (!(cursor = (*vec)[i].deserialize(cx, cursor)))
            return nullptr;
    }
    return cursor;
}

template <class T, size_t N>
bool
CloneVector(ExclusiveContext* cx, const mozilla::Vector<T, N, SystemAllocPolicy>& in,
            mozilla::Vector<T, N, SystemAllocPolicy>* out)
{
    if (!out->resize(in.length()))
        return false;
    for (size_t i = 0; i < in.length(); i++) {
        if (!in[i].clone(cx, &(*out)[i]))
            return false;
    }
    return true;
}

template <class T, size_t N, class AllocPolicy>
size_t
SerializedPodVectorSize(const mozilla::Vector<T, N, AllocPolicy>& vec)
{
    return sizeof(uint32_t) +
           vec.length() * sizeof(T);
}

template <class T, size_t N, class AllocPolicy>
uint8_t*
SerializePodVector(uint8_t* cursor, const mozilla::Vector<T, N, AllocPolicy>& vec)
{
    cursor = WriteScalar<uint32_t>(cursor, vec.length());
    cursor = WriteBytes(cursor, vec.begin(), vec.length() * sizeof(T));
    return cursor;
}

template <class T, size_t N, class AllocPolicy>
const uint8_t*
DeserializePodVector(ExclusiveContext* cx, const uint8_t* cursor,
                     mozilla::Vector<T, N, AllocPolicy>* vec)
{
    uint32_t length;
    cursor = ReadScalar<uint32_t>(cursor, &length);
    if (!vec->resize(length))
        return nullptr;
    cursor = ReadBytes(cursor, vec->begin(), length * sizeof(T));
    return cursor;
}

template <class T, size_t N>
bool
ClonePodVector(ExclusiveContext* cx, const mozilla::Vector<T, N, SystemAllocPolicy>& in,
               mozilla::Vector<T, N, SystemAllocPolicy>* out)
{
    if (!out->resize(in.length()))
        return false;
    PodCopy(out->begin(), in.begin(), in.length());
    return true;
}

size_t
SerializedSigSize(const MallocSig& sig)
{
    return sizeof(ExprType) +
           SerializedPodVectorSize(sig.args());
}

uint8_t*
SerializeSig(uint8_t* cursor, const MallocSig& sig)
{
    cursor = WriteScalar<ExprType>(cursor, sig.ret());
    cursor = SerializePodVector(cursor, sig.args());
    return cursor;
}

const uint8_t*
DeserializeSig(ExclusiveContext* cx, const uint8_t* cursor, MallocSig* sig)
{
    ExprType ret;
    cursor = ReadScalar<ExprType>(cursor, &ret);

    MallocSig::ArgVector args;
    cursor = DeserializePodVector(cx, cursor, &args);
    if (!cursor)
        return nullptr;

    sig->init(Move(args), ret);
    return cursor;
}

bool
CloneSig(ExclusiveContext* cx, const MallocSig& sig, MallocSig* out)
{
    MallocSig::ArgVector args;
    if (!ClonePodVector(cx, sig.args(), &args))
        return false;

    out->init(Move(args), sig.ret());
    return true;
}

uint8_t*
AsmJSModule::Global::serialize(uint8_t* cursor) const
{
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    cursor = SerializeName(cursor, name_);
    return cursor;
}

size_t
AsmJSModule::Global::serializedSize() const
{
    return sizeof(pod) +
           SerializedNameSize(name_);
}

const uint8_t*
AsmJSModule::Global::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = ReadBytes(cursor, &pod, sizeof(pod))) &&
    (cursor = DeserializeName(cx, cursor, &name_));
    return cursor;
}

bool
AsmJSModule::Global::clone(ExclusiveContext* cx, Global* out) const
{
    *out = *this;
    return true;
}

uint8_t*
AsmJSModule::Exit::serialize(uint8_t* cursor) const
{
    cursor = SerializeSig(cursor, sig_);
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    return cursor;
}

size_t
AsmJSModule::Exit::serializedSize() const
{
    return SerializedSigSize(sig_) +
           sizeof(pod);
}

const uint8_t*
AsmJSModule::Exit::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = DeserializeSig(cx, cursor, &sig_)) &&
    (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
    return cursor;
}

bool
AsmJSModule::Exit::clone(ExclusiveContext* cx, Exit* out) const
{
    out->pod = pod;
    return CloneSig(cx, sig_, &out->sig_);
}

uint8_t*
AsmJSModule::ExportedFunction::serialize(uint8_t* cursor) const
{
    cursor = SerializeName(cursor, name_);
    cursor = SerializeName(cursor, maybeFieldName_);
    cursor = SerializeSig(cursor, sig_);
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    return cursor;
}

size_t
AsmJSModule::ExportedFunction::serializedSize() const
{
    return SerializedNameSize(name_) +
           SerializedNameSize(maybeFieldName_) +
           SerializedSigSize(sig_) +
           sizeof(pod);
}

const uint8_t*
AsmJSModule::ExportedFunction::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = DeserializeName(cx, cursor, &name_)) &&
    (cursor = DeserializeName(cx, cursor, &maybeFieldName_)) &&
    (cursor = DeserializeSig(cx, cursor, &sig_)) &&
    (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
    return cursor;
}

bool
AsmJSModule::ExportedFunction::clone(ExclusiveContext* cx, ExportedFunction* out) const
{
    out->name_ = name_;
    out->maybeFieldName_ = maybeFieldName_;
    out->pod = pod;
    return CloneSig(cx, sig_, &out->sig_);
}

AsmJSModule::CodeRange::CodeRange(uint32_t lineNumber, AsmJSFunctionOffsets offsets)
  : nameIndex_(UINT32_MAX),
    lineNumber_(lineNumber)
{
    PodZero(&u);  // zero padding for Valgrind
    u.kind_ = Function;

    MOZ_ASSERT(offsets.nonProfilingEntry - offsets.begin <= UINT8_MAX);
    begin_ = offsets.begin;
    u.func.beginToEntry_ = offsets.nonProfilingEntry - begin_;

    MOZ_ASSERT(offsets.nonProfilingEntry < offsets.profilingReturn);
    MOZ_ASSERT(offsets.profilingReturn - offsets.profilingJump <= UINT8_MAX);
    MOZ_ASSERT(offsets.profilingReturn - offsets.profilingEpilogue <= UINT8_MAX);
    profilingReturn_ = offsets.profilingReturn;
    u.func.profilingJumpToProfilingReturn_ = profilingReturn_ - offsets.profilingJump;
    u.func.profilingEpilogueToProfilingReturn_ = profilingReturn_ - offsets.profilingEpilogue;

    MOZ_ASSERT(offsets.nonProfilingEntry < offsets.end);
    end_ = offsets.end;
}

AsmJSModule::CodeRange::CodeRange(Kind kind, AsmJSOffsets offsets)
  : nameIndex_(0),
    lineNumber_(0),
    begin_(offsets.begin),
    profilingReturn_(0),
    end_(offsets.end)
{
    PodZero(&u);  // zero padding for Valgrind
    u.kind_ = kind;

    MOZ_ASSERT(begin_ <= end_);
    MOZ_ASSERT(u.kind_ == Entry || u.kind_ == Inline);
}

AsmJSModule::CodeRange::CodeRange(Kind kind, AsmJSProfilingOffsets offsets)
  : nameIndex_(0),
    lineNumber_(0),
    begin_(offsets.begin),
    profilingReturn_(offsets.profilingReturn),
    end_(offsets.end)
{
    PodZero(&u);  // zero padding for Valgrind
    u.kind_ = kind;

    MOZ_ASSERT(begin_ < profilingReturn_);
    MOZ_ASSERT(profilingReturn_ < end_);
    MOZ_ASSERT(u.kind_ == JitFFI || u.kind_ == SlowFFI || u.kind_ == Interrupt);
}

AsmJSModule::CodeRange::CodeRange(Builtin builtin, AsmJSProfilingOffsets offsets)
  : nameIndex_(0),
    lineNumber_(0),
    begin_(offsets.begin),
    profilingReturn_(offsets.profilingReturn),
    end_(offsets.end)
{
    PodZero(&u);  // zero padding for Valgrind
    u.kind_ = Thunk;
    u.thunk.target_ = uint16_t(builtin);

    MOZ_ASSERT(begin_ < profilingReturn_);
    MOZ_ASSERT(profilingReturn_ < end_);
}

#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
size_t
AsmJSModule::ProfiledFunction::serializedSize() const
{
    return SerializedNameSize(name) +
           sizeof(pod);
}

uint8_t*
AsmJSModule::ProfiledFunction::serialize(uint8_t* cursor) const
{
    cursor = SerializeName(cursor, name);
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    return cursor;
}

const uint8_t*
AsmJSModule::ProfiledFunction::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = DeserializeName(cx, cursor, &name)) &&
    (cursor = ReadBytes(cursor, &pod, sizeof(pod)));
    return cursor;
}
#endif

size_t
AsmJSModule::AbsoluteLinkArray::serializedSize() const
{
    size_t size = 0;
    for (const OffsetVector& offsets : *this)
        size += SerializedPodVectorSize(offsets);
    return size;
}

uint8_t*
AsmJSModule::AbsoluteLinkArray::serialize(uint8_t* cursor) const
{
    for (const OffsetVector& offsets : *this)
        cursor = SerializePodVector(cursor, offsets);
    return cursor;
}

const uint8_t*
AsmJSModule::AbsoluteLinkArray::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    for (OffsetVector& offsets : *this) {
        cursor = DeserializePodVector(cx, cursor, &offsets);
        if (!cursor)
            return nullptr;
    }
    return cursor;
}

bool
AsmJSModule::AbsoluteLinkArray::clone(ExclusiveContext* cx, AbsoluteLinkArray* out) const
{
    for (auto imm : MakeEnumeratedRange(SymbolicAddress::Limit)) {
        if (!ClonePodVector(cx, (*this)[imm], &(*out)[imm]))
            return false;
    }
    return true;
}

size_t
AsmJSModule::AbsoluteLinkArray::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    size_t size = 0;
    for (const OffsetVector& offsets : *this)
        size += offsets.sizeOfExcludingThis(mallocSizeOf);
    return size;
}

size_t
AsmJSModule::FuncPtrTable::serializedSize() const
{
    return sizeof(pod) +
           SerializedPodVectorSize(elemOffsets_);
}

uint8_t*
AsmJSModule::FuncPtrTable::serialize(uint8_t* cursor) const
{
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    cursor = SerializePodVector(cursor, elemOffsets_);
    return cursor;
}

const uint8_t*
AsmJSModule::FuncPtrTable::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = ReadBytes(cursor, &pod, sizeof(pod))) &&
    (cursor = DeserializePodVector(cx, cursor, &elemOffsets_));
    return cursor;
}

bool
AsmJSModule::FuncPtrTable::clone(ExclusiveContext* cx, FuncPtrTable* out) const
{
    out->pod = pod;
    return ClonePodVector(cx, elemOffsets_, &out->elemOffsets_);
}

size_t
AsmJSModule::FuncPtrTable::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return elemOffsets_.sizeOfExcludingThis(mallocSizeOf);
}

size_t
AsmJSModule::StaticLinkData::serializedSize() const
{
    return sizeof(pod) +
           SerializedPodVectorSize(relativeLinks) +
           absoluteLinks.serializedSize() +
           SerializedVectorSize(funcPtrTables);
}

uint8_t*
AsmJSModule::StaticLinkData::serialize(uint8_t* cursor) const
{
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    cursor = SerializePodVector(cursor, relativeLinks);
    cursor = absoluteLinks.serialize(cursor);
    cursor = SerializeVector(cursor, funcPtrTables);
    return cursor;
}

const uint8_t*
AsmJSModule::StaticLinkData::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    (cursor = ReadBytes(cursor, &pod, sizeof(pod))) &&
    (cursor = DeserializePodVector(cx, cursor, &relativeLinks)) &&
    (cursor = absoluteLinks.deserialize(cx, cursor)) &&
    (cursor = DeserializeVector(cx, cursor, &funcPtrTables));
    return cursor;
}

bool
AsmJSModule::StaticLinkData::clone(ExclusiveContext* cx, StaticLinkData* out) const
{
    out->pod = pod;
    return ClonePodVector(cx, relativeLinks, &out->relativeLinks) &&
           absoluteLinks.clone(cx, &out->absoluteLinks) &&
           CloneVector(cx, funcPtrTables, &out->funcPtrTables);
}

size_t
AsmJSModule::StaticLinkData::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    size_t size = relativeLinks.sizeOfExcludingThis(mallocSizeOf) +
                  absoluteLinks.sizeOfExcludingThis(mallocSizeOf) +
                  funcPtrTables.sizeOfExcludingThis(mallocSizeOf);

    for (const FuncPtrTable& table : funcPtrTables)
        size += table.sizeOfExcludingThis(mallocSizeOf);

    return size;
}

size_t
AsmJSModule::serializedSize() const
{
    return sizeof(pod) +
           pod.codeBytes_ +
           SerializedNameSize(globalArgumentName_) +
           SerializedNameSize(importArgumentName_) +
           SerializedNameSize(bufferArgumentName_) +
           SerializedVectorSize(globals_) +
           SerializedVectorSize(exits_) +
           SerializedVectorSize(exports_) +
           SerializedPodVectorSize(callSites_) +
           SerializedPodVectorSize(codeRanges_) +
           SerializedVectorSize(names_) +
           SerializedPodVectorSize(heapAccesses_) +
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
           SerializedVectorSize(profiledFunctions_) +
#endif
           staticLinkData_.serializedSize();
}

uint8_t*
AsmJSModule::serialize(uint8_t* cursor) const
{
    MOZ_ASSERT(!dynamicallyLinked_);
    MOZ_ASSERT(!loadedFromCache_);
    MOZ_ASSERT(!profilingEnabled_);
    MOZ_ASSERT(!interrupted_);

    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    cursor = WriteBytes(cursor, code_, pod.codeBytes_);
    cursor = SerializeName(cursor, globalArgumentName_);
    cursor = SerializeName(cursor, importArgumentName_);
    cursor = SerializeName(cursor, bufferArgumentName_);
    cursor = SerializeVector(cursor, globals_);
    cursor = SerializeVector(cursor, exits_);
    cursor = SerializeVector(cursor, exports_);
    cursor = SerializePodVector(cursor, callSites_);
    cursor = SerializePodVector(cursor, codeRanges_);
    cursor = SerializeVector(cursor, names_);
    cursor = SerializePodVector(cursor, heapAccesses_);
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    cursor = SerializeVector(cursor, profiledFunctions_);
#endif
    cursor = staticLinkData_.serialize(cursor);
    return cursor;
}

const uint8_t*
AsmJSModule::deserialize(ExclusiveContext* cx, const uint8_t* cursor)
{
    // To avoid GC-during-deserialization corner cases, prevent atoms from
    // being collected.
    AutoKeepAtoms aka(cx->perThreadData);

    (cursor = ReadBytes(cursor, &pod, sizeof(pod))) &&
    (code_ = AllocateExecutableMemory(cx, pod.totalBytes_)) &&
    (cursor = ReadBytes(cursor, code_, pod.codeBytes_)) &&
    (cursor = DeserializeName(cx, cursor, &globalArgumentName_)) &&
    (cursor = DeserializeName(cx, cursor, &importArgumentName_)) &&
    (cursor = DeserializeName(cx, cursor, &bufferArgumentName_)) &&
    (cursor = DeserializeVector(cx, cursor, &globals_)) &&
    (cursor = DeserializeVector(cx, cursor, &exits_)) &&
    (cursor = DeserializeVector(cx, cursor, &exports_)) &&
    (cursor = DeserializePodVector(cx, cursor, &callSites_)) &&
    (cursor = DeserializePodVector(cx, cursor, &codeRanges_)) &&
    (cursor = DeserializeVector(cx, cursor, &names_)) &&
    (cursor = DeserializePodVector(cx, cursor, &heapAccesses_)) &&
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    (cursor = DeserializeVector(cx, cursor, &profiledFunctions_)) &&
#endif
    (cursor = staticLinkData_.deserialize(cx, cursor));

    loadedFromCache_ = true;

    return cursor;
}

bool
AsmJSModule::clone(JSContext* cx, ScopedJSDeletePtr<AsmJSModule>* moduleOut) const
{
    *moduleOut = cx->new_<AsmJSModule>(scriptSource_, srcStart_, srcBodyStart_, pod.strict_,
                                       pod.canUseSignalHandlers_);
    if (!*moduleOut)
        return false;

    AsmJSModule& out = **moduleOut;

    // Mirror the order of serialize/deserialize in cloning:

    out.pod = pod;

    out.code_ = AllocateExecutableMemory(cx, pod.totalBytes_);
    if (!out.code_)
        return false;

    memcpy(out.code_, code_, pod.codeBytes_);

    out.globalArgumentName_ = globalArgumentName_;
    out.importArgumentName_ = importArgumentName_;
    out.bufferArgumentName_ = bufferArgumentName_;

    if (!CloneVector(cx, globals_, &out.globals_) ||
        !CloneVector(cx, exits_, &out.exits_) ||
        !CloneVector(cx, exports_, &out.exports_) ||
        !ClonePodVector(cx, callSites_, &out.callSites_) ||
        !ClonePodVector(cx, codeRanges_, &out.codeRanges_) ||
        !CloneVector(cx, names_, &out.names_) ||
        !ClonePodVector(cx, heapAccesses_, &out.heapAccesses_) ||
        !staticLinkData_.clone(cx, &out.staticLinkData_))
    {
        return false;
    }

    out.loadedFromCache_ = loadedFromCache_;
    out.profilingEnabled_ = profilingEnabled_;

    if (profilingEnabled_) {
        if (!out.profilingLabels_.resize(profilingLabels_.length()))
            return false;
        for (size_t i = 0; i < profilingLabels_.length(); i++) {
            out.profilingLabels_[i] = DuplicateString(cx, profilingLabels_[i].get());
            if (!out.profilingLabels_[i])
                return false;
        }
    }


    // Delay flushing until dynamic linking.
    AutoFlushICache afc("AsmJSModule::clone", /* inhibit = */ true);
    out.setAutoFlushICacheRange();

    out.restoreToInitialState(maybeHeap_, code_, cx);
    out.staticallyLink(cx);
    return true;
}

bool
AsmJSModule::changeHeap(Handle<ArrayBufferObject*> newHeap, JSContext* cx)
{
    MOZ_ASSERT(hasArrayView());

    // Content JS should not be able to run (and change heap) from within an
    // interrupt callback, but in case it does, fail to change heap. Otherwise,
    // the heap can change at every single instruction which would prevent
    // future optimizations like heap-base hoisting.
    if (interrupted_)
        return false;

    AutoMutateCode amc(cx, *this, "AsmJSModule::changeHeap");
    restoreHeapToInitialState(maybeHeap_);
    initHeap(newHeap, cx);
    return true;
}

size_t
AsmJSModule::heapLength() const
{
    MOZ_ASSERT(isDynamicallyLinked());
    return maybeHeap_ ? maybeHeap_->byteLength() : 0;
}

void
AsmJSModule::setProfilingEnabled(bool enabled, JSContext* cx)
{
    MOZ_ASSERT(isDynamicallyLinked());

    if (profilingEnabled_ == enabled)
        return;

    // When enabled, generate profiling labels for every name in names_ that is
    // the name of some Function CodeRange. This involves malloc() so do it now
    // since, once we start sampling, we'll be in a signal-handing context where
    // we cannot malloc.
    if (enabled) {
        profilingLabels_.resize(names_.length());
        const char* filename = scriptSource_->filename();
        JS::AutoCheckCannotGC nogc;
        for (size_t i = 0; i < codeRanges_.length(); i++) {
            CodeRange& cr = codeRanges_[i];
            if (!cr.isFunction())
                continue;
            unsigned lineno = cr.functionLineNumber();
            PropertyName* name = names_[cr.functionNameIndex()].name();
            profilingLabels_[cr.functionNameIndex()].reset(
                name->hasLatin1Chars()
                ? JS_smprintf("%s (%s:%u)", name->latin1Chars(nogc), filename, lineno)
                : JS_smprintf("%hs (%s:%u)", name->twoByteChars(nogc), filename, lineno));
        }
    } else {
        profilingLabels_.clear();
    }

    AutoMutateCode amc(cx, *this, "AsmJSModule::setProfilingEnabled");

    // Patch all internal (asm.js->asm.js) callsites to call the profiling
    // prologues:
    for (size_t i = 0; i < callSites_.length(); i++) {
        CallSite& cs = callSites_[i];
        if (cs.kind() != CallSite::Relative)
            continue;

        uint8_t* callerRetAddr = code_ + cs.returnAddressOffset();
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        void* callee = X86Encoding::GetRel32Target(callerRetAddr);
#elif defined(JS_CODEGEN_ARM)
        uint8_t* caller = callerRetAddr - 4;
        Instruction* callerInsn = reinterpret_cast<Instruction*>(caller);
        BOffImm calleeOffset;
        callerInsn->as<InstBLImm>()->extractImm(&calleeOffset);
        void* callee = calleeOffset.getDest(callerInsn);
#elif defined(JS_CODEGEN_ARM64)
        MOZ_CRASH();
        void* callee = nullptr;
        (void)callerRetAddr;
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        uint8_t* instr = callerRetAddr - Assembler::PatchWrite_NearCallSize();
        void* callee = (void*)Assembler::ExtractInstructionImmediate(instr);
#elif defined(JS_CODEGEN_NONE)
        MOZ_CRASH();
        void* callee = nullptr;
#else
# error "Missing architecture"
#endif

        const CodeRange* codeRange = lookupCodeRange(callee);
        if (codeRange->kind() != CodeRange::Function)
            continue;

        uint8_t* profilingEntry = code_ + codeRange->profilingEntry();
        uint8_t* entry = code_ + codeRange->entry();
        MOZ_ASSERT_IF(profilingEnabled_, callee == profilingEntry);
        MOZ_ASSERT_IF(!profilingEnabled_, callee == entry);
        uint8_t* newCallee = enabled ? profilingEntry : entry;

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        X86Encoding::SetRel32(callerRetAddr, newCallee);
#elif defined(JS_CODEGEN_ARM)
        new (caller) InstBLImm(BOffImm(newCallee - caller), Assembler::Always);
#elif defined(JS_CODEGEN_ARM64)
        (void)newCallee;
        MOZ_CRASH();
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        Assembler::PatchInstructionImmediate(instr, PatchedImmPtr(newCallee));
#elif defined(JS_CODEGEN_NONE)
        MOZ_CRASH();
#else
# error "Missing architecture"
#endif
    }

    // Update all the addresses in the function-pointer tables to point to the
    // profiling prologues:
    for (FuncPtrTable& funcPtrTable : staticLinkData_.funcPtrTables) {
        auto array = reinterpret_cast<void**>(globalData() + funcPtrTable.globalDataOffset());
        for (size_t i = 0; i < funcPtrTable.elemOffsets().length(); i++) {
            void* callee = array[i];
            const CodeRange* codeRange = lookupCodeRange(callee);
            void* profilingEntry = code_ + codeRange->profilingEntry();
            void* entry = code_ + codeRange->entry();
            MOZ_ASSERT_IF(profilingEnabled_, callee == profilingEntry);
            MOZ_ASSERT_IF(!profilingEnabled_, callee == entry);
            if (enabled)
                array[i] = profilingEntry;
            else
                array[i] = entry;
        }
    }

    // Replace all the nops in all the epilogues of asm.js functions with jumps
    // to the profiling epilogues.
    for (size_t i = 0; i < codeRanges_.length(); i++) {
        CodeRange& cr = codeRanges_[i];
        if (!cr.isFunction())
            continue;
        uint8_t* jump = code_ + cr.profilingJump();
        uint8_t* profilingEpilogue = code_ + cr.profilingEpilogue();
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        // An unconditional jump with a 1 byte offset immediate has the opcode
        // 0x90. The offset is relative to the address of the instruction after
        // the jump. 0x66 0x90 is the canonical two-byte nop.
        ptrdiff_t jumpImmediate = profilingEpilogue - jump - 2;
        MOZ_ASSERT(jumpImmediate > 0 && jumpImmediate <= 127);
        if (enabled) {
            MOZ_ASSERT(jump[0] == 0x66);
            MOZ_ASSERT(jump[1] == 0x90);
            jump[0] = 0xeb;
            jump[1] = jumpImmediate;
        } else {
            MOZ_ASSERT(jump[0] == 0xeb);
            MOZ_ASSERT(jump[1] == jumpImmediate);
            jump[0] = 0x66;
            jump[1] = 0x90;
        }
#elif defined(JS_CODEGEN_ARM)
        if (enabled) {
            MOZ_ASSERT(reinterpret_cast<Instruction*>(jump)->is<InstNOP>());
            new (jump) InstBImm(BOffImm(profilingEpilogue - jump), Assembler::Always);
        } else {
            MOZ_ASSERT(reinterpret_cast<Instruction*>(jump)->is<InstBImm>());
            new (jump) InstNOP();
        }
#elif defined(JS_CODEGEN_ARM64)
        (void)jump;
        (void)profilingEpilogue;
        MOZ_CRASH();
#elif defined(JS_CODEGEN_MIPS32)
        Instruction* instr = (Instruction*)jump;
        if (enabled) {
            Assembler::WriteLuiOriInstructions(instr, instr->next(),
                                               ScratchRegister, (uint32_t)profilingEpilogue);
            instr[2] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr);
        } else {
            instr[0].makeNop();
            instr[1].makeNop();
            instr[2].makeNop();
        }
#elif defined(JS_CODEGEN_MIPS64)
        Instruction* instr = (Instruction*)jump;
        if (enabled) {
            Assembler::WriteLoad64Instructions(instr, ScratchRegister, (uint64_t)profilingEpilogue);
            instr[4] = InstReg(op_special, ScratchRegister, zero, zero, ff_jr);
        } else {
            instr[0].makeNop();
            instr[1].makeNop();
            instr[2].makeNop();
            instr[3].makeNop();
            instr[4].makeNop();
        }
#elif defined(JS_CODEGEN_NONE)
        MOZ_CRASH();
#else
# error "Missing architecture"
#endif
    }

    // Replace all calls to builtins with calls to profiling thunks that push a
    // frame pointer. Since exit unwinding always starts at the caller of fp,
    // this avoids losing the innermost asm.js function.
    for (auto builtin : MakeEnumeratedRange(Builtin::Limit)) {
        auto imm = BuiltinToImmediate(builtin);
        const OffsetVector& offsets = staticLinkData_.absoluteLinks[imm];
        void* from = AddressOf(imm, nullptr);
        void* to = code_ + staticLinkData_.pod.builtinThunkOffsets[builtin];
        if (!enabled)
            Swap(from, to);
        for (size_t j = 0; j < offsets.length(); j++) {
            uint8_t* caller = code_ + offsets[j];
            const AsmJSModule::CodeRange* codeRange = lookupCodeRange(caller);
            if (codeRange->isThunk())
                continue;
            MOZ_ASSERT(codeRange->isFunction());
            Assembler::PatchDataWithValueCheck(CodeLocationLabel(caller),
                                               PatchedImmPtr(to),
                                               PatchedImmPtr(from));
        }
    }

    profilingEnabled_ = enabled;
}

static bool
GetCPUID(uint32_t* cpuId)
{
    enum Arch {
        X86 = 0x1,
        X64 = 0x2,
        ARM = 0x3,
        MIPS = 0x4,
        MIPS64 = 0x5,
        ARCH_BITS = 3
    };

#if defined(JS_CODEGEN_X86)
    MOZ_ASSERT(uint32_t(CPUInfo::GetSSEVersion()) <= (UINT32_MAX >> ARCH_BITS));
    *cpuId = X86 | (uint32_t(CPUInfo::GetSSEVersion()) << ARCH_BITS);
    return true;
#elif defined(JS_CODEGEN_X64)
    MOZ_ASSERT(uint32_t(CPUInfo::GetSSEVersion()) <= (UINT32_MAX >> ARCH_BITS));
    *cpuId = X64 | (uint32_t(CPUInfo::GetSSEVersion()) << ARCH_BITS);
    return true;
#elif defined(JS_CODEGEN_ARM)
    MOZ_ASSERT(GetARMFlags() <= (UINT32_MAX >> ARCH_BITS));
    *cpuId = ARM | (GetARMFlags() << ARCH_BITS);
    return true;
#elif defined(JS_CODEGEN_MIPS32)
    MOZ_ASSERT(GetMIPSFlags() <= (UINT32_MAX >> ARCH_BITS));
    *cpuId = MIPS | (GetMIPSFlags() << ARCH_BITS);
    return true;
#elif defined(JS_CODEGEN_MIPS64)
    MOZ_ASSERT(GetMIPSFlags() <= (UINT32_MAX >> ARCH_BITS));
    *cpuId = MIPS64 | (GetMIPSFlags() << ARCH_BITS);
    return true;
#else
    return false;
#endif
}

class MachineId
{
    uint32_t cpuId_;
    JS::BuildIdCharVector buildId_;

  public:
    bool extractCurrentState(ExclusiveContext* cx) {
        if (!cx->asmJSCacheOps().buildId)
            return false;
        if (!cx->asmJSCacheOps().buildId(&buildId_))
            return false;
        if (!GetCPUID(&cpuId_))
            return false;
        return true;
    }

    size_t serializedSize() const {
        return sizeof(uint32_t) +
               SerializedPodVectorSize(buildId_);
    }

    uint8_t* serialize(uint8_t* cursor) const {
        cursor = WriteScalar<uint32_t>(cursor, cpuId_);
        cursor = SerializePodVector(cursor, buildId_);
        return cursor;
    }

    const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor) {
        (cursor = ReadScalar<uint32_t>(cursor, &cpuId_)) &&
        (cursor = DeserializePodVector(cx, cursor, &buildId_));
        return cursor;
    }

    bool operator==(const MachineId& rhs) const {
        return cpuId_ == rhs.cpuId_ &&
               buildId_.length() == rhs.buildId_.length() &&
               PodEqual(buildId_.begin(), rhs.buildId_.begin(), buildId_.length());
    }
    bool operator!=(const MachineId& rhs) const {
        return !(*this == rhs);
    }
};

struct PropertyNameWrapper
{
    PropertyName* name;

    PropertyNameWrapper()
      : name(nullptr)
    {}
    explicit PropertyNameWrapper(PropertyName* name)
      : name(name)
    {}
    size_t serializedSize() const {
        return SerializedNameSize(name);
    }
    uint8_t* serialize(uint8_t* cursor) const {
        return SerializeName(cursor, name);
    }
    const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor) {
        return DeserializeName(cx, cursor, &name);
    }
};

class ModuleChars
{
  protected:
    uint32_t isFunCtor_;
    Vector<PropertyNameWrapper, 0, SystemAllocPolicy> funCtorArgs_;

  public:
    static uint32_t beginOffset(AsmJSParser& parser) {
        return parser.pc->maybeFunction->pn_pos.begin;
    }

    static uint32_t endOffset(AsmJSParser& parser) {
        TokenPos pos(0, 0);  // initialize to silence GCC warning
        MOZ_ALWAYS_TRUE(parser.tokenStream.peekTokenPos(&pos, TokenStream::Operand));
        return pos.end;
    }
};

class ModuleCharsForStore : ModuleChars
{
    uint32_t uncompressedSize_;
    uint32_t compressedSize_;
    Vector<char, 0, SystemAllocPolicy> compressedBuffer_;

  public:
    bool init(AsmJSParser& parser) {
        MOZ_ASSERT(beginOffset(parser) < endOffset(parser));

        uncompressedSize_ = (endOffset(parser) - beginOffset(parser)) * sizeof(char16_t);
        size_t maxCompressedSize = LZ4::maxCompressedSize(uncompressedSize_);
        if (maxCompressedSize < uncompressedSize_)
            return false;

        if (!compressedBuffer_.resize(maxCompressedSize))
            return false;

        const char16_t* chars = parser.tokenStream.rawCharPtrAt(beginOffset(parser));
        const char* source = reinterpret_cast<const char*>(chars);
        size_t compressedSize = LZ4::compress(source, uncompressedSize_, compressedBuffer_.begin());
        if (!compressedSize || compressedSize > UINT32_MAX)
            return false;

        compressedSize_ = compressedSize;

        // For a function statement or named function expression:
        //   function f(x,y,z) { abc }
        // the range [beginOffset, endOffset) captures the source:
        //   f(x,y,z) { abc }
        // An unnamed function expression captures the same thing, sans 'f'.
        // Since asm.js modules do not contain any free variables, equality of
        // [beginOffset, endOffset) is sufficient to guarantee identical code
        // generation, modulo MachineId.
        //
        // For functions created with 'new Function', function arguments are
        // not present in the source so we must manually explicitly serialize
        // and match the formals as a Vector of PropertyName.
        isFunCtor_ = parser.pc->isFunctionConstructorBody();
        if (isFunCtor_) {
            unsigned numArgs;
            ParseNode* arg = FunctionArgsList(parser.pc->maybeFunction, &numArgs);
            for (unsigned i = 0; i < numArgs; i++, arg = arg->pn_next) {
                if (!funCtorArgs_.append(arg->name()))
                    return false;
            }
        }

        return true;
    }

    size_t serializedSize() const {
        return sizeof(uint32_t) +
               sizeof(uint32_t) +
               compressedSize_ +
               sizeof(uint32_t) +
               (isFunCtor_ ? SerializedVectorSize(funCtorArgs_) : 0);
    }

    uint8_t* serialize(uint8_t* cursor) const {
        cursor = WriteScalar<uint32_t>(cursor, uncompressedSize_);
        cursor = WriteScalar<uint32_t>(cursor, compressedSize_);
        cursor = WriteBytes(cursor, compressedBuffer_.begin(), compressedSize_);
        cursor = WriteScalar<uint32_t>(cursor, isFunCtor_);
        if (isFunCtor_)
            cursor = SerializeVector(cursor, funCtorArgs_);
        return cursor;
    }
};

class ModuleCharsForLookup : ModuleChars
{
    Vector<char16_t, 0, SystemAllocPolicy> chars_;

  public:
    const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor) {
        uint32_t uncompressedSize;
        cursor = ReadScalar<uint32_t>(cursor, &uncompressedSize);

        uint32_t compressedSize;
        cursor = ReadScalar<uint32_t>(cursor, &compressedSize);

        if (!chars_.resize(uncompressedSize / sizeof(char16_t)))
            return nullptr;

        const char* source = reinterpret_cast<const char*>(cursor);
        char* dest = reinterpret_cast<char*>(chars_.begin());
        if (!LZ4::decompress(source, dest, uncompressedSize))
            return nullptr;

        cursor += compressedSize;

        cursor = ReadScalar<uint32_t>(cursor, &isFunCtor_);
        if (isFunCtor_)
            cursor = DeserializeVector(cx, cursor, &funCtorArgs_);

        return cursor;
    }

    bool match(AsmJSParser& parser) const {
        const char16_t* parseBegin = parser.tokenStream.rawCharPtrAt(beginOffset(parser));
        const char16_t* parseLimit = parser.tokenStream.rawLimit();
        MOZ_ASSERT(parseLimit >= parseBegin);
        if (uint32_t(parseLimit - parseBegin) < chars_.length())
            return false;
        if (!PodEqual(chars_.begin(), parseBegin, chars_.length()))
            return false;
        if (isFunCtor_ != parser.pc->isFunctionConstructorBody())
            return false;
        if (isFunCtor_) {
            // For function statements, the closing } is included as the last
            // character of the matched source. For Function constructor,
            // parsing terminates with EOF which we must explicitly check. This
            // prevents
            //   new Function('"use asm"; function f() {} return f')
            // from incorrectly matching
            //   new Function('"use asm"; function f() {} return ff')
            if (parseBegin + chars_.length() != parseLimit)
                return false;
            unsigned numArgs;
            ParseNode* arg = FunctionArgsList(parser.pc->maybeFunction, &numArgs);
            if (funCtorArgs_.length() != numArgs)
                return false;
            for (unsigned i = 0; i < funCtorArgs_.length(); i++, arg = arg->pn_next) {
                if (funCtorArgs_[i].name != arg->name())
                    return false;
            }
        }
        return true;
    }
};

struct ScopedCacheEntryOpenedForWrite
{
    ExclusiveContext* cx;
    const size_t serializedSize;
    uint8_t* memory;
    intptr_t handle;

    ScopedCacheEntryOpenedForWrite(ExclusiveContext* cx, size_t serializedSize)
      : cx(cx), serializedSize(serializedSize), memory(nullptr), handle(-1)
    {}

    ~ScopedCacheEntryOpenedForWrite() {
        if (memory)
            cx->asmJSCacheOps().closeEntryForWrite(serializedSize, memory, handle);
    }
};

JS::AsmJSCacheResult
js::StoreAsmJSModuleInCache(AsmJSParser& parser,
                            const AsmJSModule& module,
                            ExclusiveContext* cx)
{
    MachineId machineId;
    if (!machineId.extractCurrentState(cx))
        return JS::AsmJSCache_InternalError;

    ModuleCharsForStore moduleChars;
    if (!moduleChars.init(parser))
        return JS::AsmJSCache_InternalError;

    size_t serializedSize = machineId.serializedSize() +
                            moduleChars.serializedSize() +
                            module.serializedSize();

    JS::OpenAsmJSCacheEntryForWriteOp open = cx->asmJSCacheOps().openEntryForWrite;
    if (!open)
        return JS::AsmJSCache_Disabled_Internal;

    const char16_t* begin = parser.tokenStream.rawCharPtrAt(ModuleChars::beginOffset(parser));
    const char16_t* end = parser.tokenStream.rawCharPtrAt(ModuleChars::endOffset(parser));
    bool installed = parser.options().installedFile;

    ScopedCacheEntryOpenedForWrite entry(cx, serializedSize);
    JS::AsmJSCacheResult openResult =
        open(cx->global(), installed, begin, end, serializedSize, &entry.memory, &entry.handle);
    if (openResult != JS::AsmJSCache_Success)
        return openResult;

    uint8_t* cursor = entry.memory;
    cursor = machineId.serialize(cursor);
    cursor = moduleChars.serialize(cursor);
    cursor = module.serialize(cursor);

    MOZ_ASSERT(cursor == entry.memory + serializedSize);
    return JS::AsmJSCache_Success;
}

struct ScopedCacheEntryOpenedForRead
{
    ExclusiveContext* cx;
    size_t serializedSize;
    const uint8_t* memory;
    intptr_t handle;

    explicit ScopedCacheEntryOpenedForRead(ExclusiveContext* cx)
      : cx(cx), serializedSize(0), memory(nullptr), handle(0)
    {}

    ~ScopedCacheEntryOpenedForRead() {
        if (memory)
            cx->asmJSCacheOps().closeEntryForRead(serializedSize, memory, handle);
    }
};

bool
js::LookupAsmJSModuleInCache(ExclusiveContext* cx,
                             AsmJSParser& parser,
                             ScopedJSDeletePtr<AsmJSModule>* moduleOut,
                             ScopedJSFreePtr<char>* compilationTimeReport)
{
    int64_t usecBefore = PRMJ_Now();

    MachineId machineId;
    if (!machineId.extractCurrentState(cx))
        return true;

    JS::OpenAsmJSCacheEntryForReadOp open = cx->asmJSCacheOps().openEntryForRead;
    if (!open)
        return true;

    const char16_t* begin = parser.tokenStream.rawCharPtrAt(ModuleChars::beginOffset(parser));
    const char16_t* limit = parser.tokenStream.rawLimit();

    ScopedCacheEntryOpenedForRead entry(cx);
    if (!open(cx->global(), begin, limit, &entry.serializedSize, &entry.memory, &entry.handle))
        return true;

    const uint8_t* cursor = entry.memory;

    MachineId cachedMachineId;
    cursor = cachedMachineId.deserialize(cx, cursor);
    if (!cursor)
        return false;
    if (machineId != cachedMachineId)
        return true;

    ModuleCharsForLookup moduleChars;
    cursor = moduleChars.deserialize(cx, cursor);
    if (!moduleChars.match(parser))
        return true;

    uint32_t srcStart = parser.pc->maybeFunction->pn_body->pn_pos.begin;
    uint32_t srcBodyStart = parser.tokenStream.currentToken().pos.end;
    bool strict = parser.pc->sc->strict() && !parser.pc->sc->hasExplicitUseStrict();

    // canUseSignalHandlers will be clobbered when deserializing and checked below
    ScopedJSDeletePtr<AsmJSModule> module(
        cx->new_<AsmJSModule>(parser.ss, srcStart, srcBodyStart, strict,
                              /* canUseSignalHandlers = */ false));
    if (!module)
        return false;

    cursor = module->deserialize(cx, cursor);
    if (!cursor)
        return false;

    bool atEnd = cursor == entry.memory + entry.serializedSize;
    MOZ_ASSERT(atEnd, "Corrupt cache file");
    if (!atEnd)
        return true;

    if (module->canUseSignalHandlers() != cx->canUseSignalHandlers())
        return true;

    if (!parser.tokenStream.advance(module->srcEndBeforeCurly()))
        return false;

    {
        // Delay flushing until dynamic linking.
        AutoFlushICache afc("LookupAsmJSModuleInCache", /* inhibit = */ true);
        module->setAutoFlushICacheRange();

        module->staticallyLink(cx);
    }

    int64_t usecAfter = PRMJ_Now();
    int ms = (usecAfter - usecBefore) / PRMJ_USEC_PER_MSEC;
    *compilationTimeReport = JS_smprintf("loaded from cache in %dms", ms);
    *moduleOut = module.forget();
    return true;
}

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

#ifndef asmjs_AsmJSModule_h
#define asmjs_AsmJSModule_h

#include "mozilla/EnumeratedArray.h"
#include "mozilla/Maybe.h"
#include "mozilla/Move.h"
#include "mozilla/PodOperations.h"

#include "jsscript.h"

#include "asmjs/AsmJSFrameIterator.h"
#include "asmjs/AsmJSValidate.h"
#include "asmjs/Wasm.h"
#include "builtin/SIMD.h"
#include "gc/Tracer.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "vm/TypedArrayObject.h"

namespace js {

namespace frontend { class TokenStream; }
namespace jit { struct BaselineScript; class MacroAssembler; }

// The asm.js spec recognizes this set of builtin Math functions.
enum AsmJSMathBuiltinFunction
{
    AsmJSMathBuiltin_sin, AsmJSMathBuiltin_cos, AsmJSMathBuiltin_tan,
    AsmJSMathBuiltin_asin, AsmJSMathBuiltin_acos, AsmJSMathBuiltin_atan,
    AsmJSMathBuiltin_ceil, AsmJSMathBuiltin_floor, AsmJSMathBuiltin_exp,
    AsmJSMathBuiltin_log, AsmJSMathBuiltin_pow, AsmJSMathBuiltin_sqrt,
    AsmJSMathBuiltin_abs, AsmJSMathBuiltin_atan2, AsmJSMathBuiltin_imul,
    AsmJSMathBuiltin_fround, AsmJSMathBuiltin_min, AsmJSMathBuiltin_max,
    AsmJSMathBuiltin_clz32
};

// The asm.js spec will recognize this set of builtin Atomics functions.
enum AsmJSAtomicsBuiltinFunction
{
    AsmJSAtomicsBuiltin_compareExchange,
    AsmJSAtomicsBuiltin_exchange,
    AsmJSAtomicsBuiltin_load,
    AsmJSAtomicsBuiltin_store,
    AsmJSAtomicsBuiltin_fence,
    AsmJSAtomicsBuiltin_add,
    AsmJSAtomicsBuiltin_sub,
    AsmJSAtomicsBuiltin_and,
    AsmJSAtomicsBuiltin_or,
    AsmJSAtomicsBuiltin_xor,
    AsmJSAtomicsBuiltin_isLockFree
};

// Set of known global object SIMD's attributes, i.e. types
enum AsmJSSimdType
{
    AsmJSSimdType_int32x4,
    AsmJSSimdType_float32x4
};

// Set of known operations, for a given SIMD type (int32x4, float32x4,...)
enum AsmJSSimdOperation
{
#define ASMJSSIMDOPERATION(op) AsmJSSimdOperation_##op,
    FORALL_SIMD_OP(ASMJSSIMDOPERATION)
#undef ASMJSSIMDOPERATION
};

// An asm.js module represents the collection of functions nested inside a
// single outer "use asm" function. For example, this asm.js module:
//   function() { "use asm"; function f() {} function g() {} return f }
// contains the functions 'f' and 'g'.
//
// An asm.js module contains both the jit-code produced by compiling all the
// functions in the module as well all the data required to perform the
// link-time validation step in the asm.js spec.
//
// NB: this means that AsmJSModule must be GC-safe.
class AsmJSModule
{
  public:
    class Global
    {
      public:
        enum Which { Variable, FFI, ArrayView, ArrayViewCtor, MathBuiltinFunction,
                     AtomicsBuiltinFunction, Constant, SimdCtor, SimdOperation, ByteLength };
        enum VarInitKind { InitConstant, InitImport };
        enum ConstantKind { GlobalConstant, MathConstant };

      private:
        struct Pod {
            Which which_;
            union {
                struct {
                    uint32_t globalDataOffset_;
                    VarInitKind initKind_;
                    union {
                        wasm::ValType importType_;
                        wasm::Val val_;
                    } u;
                } var;
                uint32_t ffiIndex_;
                Scalar::Type viewType_;
                AsmJSMathBuiltinFunction mathBuiltinFunc_;
                AsmJSAtomicsBuiltinFunction atomicsBuiltinFunc_;
                AsmJSSimdType simdCtorType_;
                struct {
                    AsmJSSimdType type_;
                    AsmJSSimdOperation which_;
                } simdOp;
                struct {
                    ConstantKind kind_;
                    double value_;
                } constant;
            } u;
        } pod;
        PropertyName* name_;

        friend class AsmJSModule;

        Global(Which which, PropertyName* name) {
            mozilla::PodZero(&pod);  // zero padding for Valgrind
            pod.which_ = which;
            name_ = name;
            MOZ_ASSERT_IF(name_, name_->isTenured());
        }

        void trace(JSTracer* trc) {
            if (name_)
                TraceManuallyBarrieredEdge(trc, &name_, "asm.js global name");
        }

      public:
        Global() {}
        Which which() const {
            return pod.which_;
        }
        uint32_t varGlobalDataOffset() const {
            MOZ_ASSERT(pod.which_ == Variable);
            return pod.u.var.globalDataOffset_;
        }
        VarInitKind varInitKind() const {
            MOZ_ASSERT(pod.which_ == Variable);
            return pod.u.var.initKind_;
        }
        wasm::Val varInitVal() const {
            MOZ_ASSERT(pod.which_ == Variable);
            MOZ_ASSERT(pod.u.var.initKind_ == InitConstant);
            return pod.u.var.u.val_;
        }
        wasm::ValType varInitImportType() const {
            MOZ_ASSERT(pod.which_ == Variable);
            MOZ_ASSERT(pod.u.var.initKind_ == InitImport);
            return pod.u.var.u.importType_;
        }
        PropertyName* varImportField() const {
            MOZ_ASSERT(pod.which_ == Variable);
            MOZ_ASSERT(pod.u.var.initKind_ == InitImport);
            return name_;
        }
        PropertyName* ffiField() const {
            MOZ_ASSERT(pod.which_ == FFI);
            return name_;
        }
        uint32_t ffiIndex() const {
            MOZ_ASSERT(pod.which_ == FFI);
            return pod.u.ffiIndex_;
        }
        // When a view is created from an imported constructor:
        //   var I32 = stdlib.Int32Array;
        //   var i32 = new I32(buffer);
        // the second import has nothing to validate and thus has a null field.
        PropertyName* maybeViewName() const {
            MOZ_ASSERT(pod.which_ == ArrayView || pod.which_ == ArrayViewCtor);
            return name_;
        }
        Scalar::Type viewType() const {
            MOZ_ASSERT(pod.which_ == ArrayView || pod.which_ == ArrayViewCtor);
            return pod.u.viewType_;
        }
        PropertyName* mathName() const {
            MOZ_ASSERT(pod.which_ == MathBuiltinFunction);
            return name_;
        }
        PropertyName* atomicsName() const {
            MOZ_ASSERT(pod.which_ == AtomicsBuiltinFunction);
            return name_;
        }
        AsmJSMathBuiltinFunction mathBuiltinFunction() const {
            MOZ_ASSERT(pod.which_ == MathBuiltinFunction);
            return pod.u.mathBuiltinFunc_;
        }
        AsmJSAtomicsBuiltinFunction atomicsBuiltinFunction() const {
            MOZ_ASSERT(pod.which_ == AtomicsBuiltinFunction);
            return pod.u.atomicsBuiltinFunc_;
        }
        AsmJSSimdType simdCtorType() const {
            MOZ_ASSERT(pod.which_ == SimdCtor);
            return pod.u.simdCtorType_;
        }
        PropertyName* simdCtorName() const {
            MOZ_ASSERT(pod.which_ == SimdCtor);
            return name_;
        }
        PropertyName* simdOperationName() const {
            MOZ_ASSERT(pod.which_ == SimdOperation);
            return name_;
        }
        AsmJSSimdOperation simdOperation() const {
            MOZ_ASSERT(pod.which_ == SimdOperation);
            return pod.u.simdOp.which_;
        }
        AsmJSSimdType simdOperationType() const {
            MOZ_ASSERT(pod.which_ == SimdOperation);
            return pod.u.simdOp.type_;
        }
        PropertyName* constantName() const {
            MOZ_ASSERT(pod.which_ == Constant);
            return name_;
        }
        ConstantKind constantKind() const {
            MOZ_ASSERT(pod.which_ == Constant);
            return pod.u.constant.kind_;
        }
        double constantValue() const {
            MOZ_ASSERT(pod.which_ == Constant);
            return pod.u.constant.value_;
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, Global* out) const;
    };

    // An Exit holds bookkeeping information about an exit; the ExitDatum
    // struct overlays the actual runtime data stored in the global data
    // section.

    struct ExitDatum
    {
        uint8_t* exit;
        jit::BaselineScript* baselineScript;
        HeapPtrFunction fun;
    };

    class Exit
    {
        wasm::MallocSig sig_;
        struct Pod {
            unsigned ffiIndex_;
            unsigned globalDataOffset_;
            unsigned interpCodeOffset_;
            unsigned jitCodeOffset_;
        } pod;

      public:
        Exit() {}
        Exit(Exit&& rhs) : sig_(Move(rhs.sig_)), pod(rhs.pod) {}
        Exit(wasm::MallocSig&& sig, unsigned ffiIndex, unsigned globalDataOffset)
          : sig_(Move(sig))
        {
            pod.ffiIndex_ = ffiIndex;
            pod.globalDataOffset_ = globalDataOffset;
            pod.interpCodeOffset_ = 0;
            pod.jitCodeOffset_ = 0;
        }
        const wasm::MallocSig& sig() const {
            return sig_;
        }
        unsigned ffiIndex() const {
            return pod.ffiIndex_;
        }
        unsigned globalDataOffset() const {
            return pod.globalDataOffset_;
        }
        void initInterpOffset(unsigned off) {
            MOZ_ASSERT(!pod.interpCodeOffset_);
            pod.interpCodeOffset_ = off;
        }
        void initJitOffset(unsigned off) {
            MOZ_ASSERT(!pod.jitCodeOffset_);
            pod.jitCodeOffset_ = off;
        }
        ExitDatum& datum(const AsmJSModule& module) const {
            return *reinterpret_cast<ExitDatum*>(module.globalData() + pod.globalDataOffset_);
        }
        void initDatum(const AsmJSModule& module) const {
            MOZ_ASSERT(pod.interpCodeOffset_);
            ExitDatum& d = datum(module);
            d.exit = module.codeBase() + pod.interpCodeOffset_;
            d.baselineScript = nullptr;
            d.fun = nullptr;
        }
        bool isOptimized(const AsmJSModule& module) const {
            return datum(module).exit == module.codeBase() + pod.jitCodeOffset_;
        }
        void optimize(const AsmJSModule& module, jit::BaselineScript* baselineScript) const {
            ExitDatum& d = datum(module);
            d.exit = module.codeBase() + pod.jitCodeOffset_;
            d.baselineScript = baselineScript;
        }
        void deoptimize(const AsmJSModule& module) const {
            ExitDatum& d = datum(module);
            d.exit = module.codeBase() + pod.interpCodeOffset_;
            d.baselineScript = nullptr;
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, Exit* out) const;
    };

    struct EntryArg {
        uint64_t lo;
        uint64_t hi;
    };

    typedef int32_t (*CodePtr)(EntryArg* args, uint8_t* global);

    class ExportedFunction
    {
        PropertyName* name_;
        PropertyName* maybeFieldName_;
        wasm::MallocSig sig_;
        struct Pod {
            bool isChangeHeap_;
            uint32_t funcIndex_;
            uint32_t codeOffset_;
            uint32_t startOffsetInModule_;  // Store module-start-relative offsets
            uint32_t endOffsetInModule_;    // so preserved by serialization.
        } pod;

        friend class AsmJSModule;

        ExportedFunction(PropertyName* name, uint32_t funcIndex,
                         uint32_t startOffsetInModule, uint32_t endOffsetInModule,
                         PropertyName* maybeFieldName,
                         wasm::MallocSig&& sig)
         : name_(name),
           maybeFieldName_(maybeFieldName),
           sig_(Move(sig))
        {
            MOZ_ASSERT(name_->isTenured());
            MOZ_ASSERT_IF(maybeFieldName_, maybeFieldName_->isTenured());
            mozilla::PodZero(&pod);  // zero padding for Valgrind
            pod.funcIndex_ = funcIndex;
            pod.isChangeHeap_ = false;
            pod.codeOffset_ = UINT32_MAX;
            pod.startOffsetInModule_ = startOffsetInModule;
            pod.endOffsetInModule_ = endOffsetInModule;
        }

        ExportedFunction(PropertyName* name,
                         uint32_t startOffsetInModule, uint32_t endOffsetInModule,
                         PropertyName* maybeFieldName)
          : name_(name),
            maybeFieldName_(maybeFieldName)
        {
            MOZ_ASSERT(name_->isTenured());
            MOZ_ASSERT_IF(maybeFieldName_, maybeFieldName_->isTenured());
            mozilla::PodZero(&pod);  // zero padding for Valgrind
            pod.isChangeHeap_ = true;
            pod.startOffsetInModule_ = startOffsetInModule;
            pod.endOffsetInModule_ = endOffsetInModule;
        }

        void trace(JSTracer* trc) {
            TraceManuallyBarrieredEdge(trc, &name_, "asm.js export name");
            if (maybeFieldName_)
                TraceManuallyBarrieredEdge(trc, &maybeFieldName_, "asm.js export field");
        }

      public:
        ExportedFunction() {}
        ExportedFunction(ExportedFunction&& rhs)
          : name_(rhs.name_),
            maybeFieldName_(rhs.maybeFieldName_),
            sig_(mozilla::Move(rhs.sig_))
        {
            mozilla::PodZero(&pod);  // zero padding for Valgrind
            pod = rhs.pod;
        }

        PropertyName* name() const {
            return name_;
        }
        PropertyName* maybeFieldName() const {
            return maybeFieldName_;
        }
        uint32_t startOffsetInModule() const {
            return pod.startOffsetInModule_;
        }
        uint32_t endOffsetInModule() const {
            return pod.endOffsetInModule_;
        }
        bool isChangeHeap() const {
            return pod.isChangeHeap_;
        }
        uint32_t funcIndex() const {
            MOZ_ASSERT(!isChangeHeap());
            return pod.funcIndex_;
        }
        void initCodeOffset(unsigned off) {
            MOZ_ASSERT(!isChangeHeap());
            MOZ_ASSERT(pod.codeOffset_ == UINT32_MAX);
            pod.codeOffset_ = off;
        }
        const wasm::MallocSig& sig() const {
            MOZ_ASSERT(!isChangeHeap());
            return sig_;
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, ExportedFunction* out) const;
    };

    class CodeRange
    {
      protected:
        uint32_t nameIndex_;

      private:
        uint32_t lineNumber_;
        uint32_t begin_;
        uint32_t profilingReturn_;
        uint32_t end_;
        union {
            struct {
                uint8_t kind_;
                uint8_t beginToEntry_;
                uint8_t profilingJumpToProfilingReturn_;
                uint8_t profilingEpilogueToProfilingReturn_;
            } func;
            struct {
                uint8_t kind_;
                uint16_t target_;
            } thunk;
            uint8_t kind_;
        } u;

        void assertValid();

      public:
        enum Kind { Function, Entry, JitFFI, SlowFFI, Interrupt, Thunk, Inline };

        CodeRange() {}
        CodeRange(Kind kind, AsmJSOffsets offsets);
        CodeRange(Kind kind, AsmJSProfilingOffsets offsets);
        CodeRange(wasm::Builtin builtin, AsmJSProfilingOffsets offsets);
        CodeRange(uint32_t lineNumber, AsmJSFunctionOffsets offsets);

        Kind kind() const { return Kind(u.kind_); }
        bool isFunction() const { return kind() == Function; }
        bool isEntry() const { return kind() == Entry; }
        bool isFFI() const { return kind() == JitFFI || kind() == SlowFFI; }
        bool isInterrupt() const { return kind() == Interrupt; }
        bool isThunk() const { return kind() == Thunk; }

        uint32_t begin() const {
            return begin_;
        }
        uint32_t profilingEntry() const {
            return begin();
        }
        uint32_t entry() const {
            MOZ_ASSERT(isFunction());
            return begin_ + u.func.beginToEntry_;
        }
        uint32_t end() const {
            return end_;
        }
        uint32_t profilingJump() const {
            MOZ_ASSERT(isFunction());
            return profilingReturn_ - u.func.profilingJumpToProfilingReturn_;
        }
        uint32_t profilingEpilogue() const {
            MOZ_ASSERT(isFunction());
            return profilingReturn_ - u.func.profilingEpilogueToProfilingReturn_;
        }
        uint32_t profilingReturn() const {
            MOZ_ASSERT(isFunction() || isFFI() || isInterrupt() || isThunk());
            return profilingReturn_;
        }
        void initNameIndex(uint32_t nameIndex) {
            MOZ_ASSERT(nameIndex_ == UINT32_MAX);
            nameIndex_ = nameIndex;
        }
        uint32_t functionNameIndex() const {
            MOZ_ASSERT(isFunction());
            MOZ_ASSERT(nameIndex_ != UINT32_MAX);
            return nameIndex_;
        }
        PropertyName* functionName(const AsmJSModule& module) const {
            return module.names_[functionNameIndex()].name();
        }
        const char* functionProfilingLabel(const AsmJSModule& module) const {
            MOZ_ASSERT(isFunction());
            return module.profilingLabels_[nameIndex_].get();
        }
        uint32_t functionLineNumber() const {
            MOZ_ASSERT(isFunction());
            return lineNumber_;
        }
        void functionOffsetBy(uint32_t offset) {
            MOZ_ASSERT(isFunction());
            begin_ += offset;
            profilingReturn_ += offset;
            end_ += offset;
        }
        wasm::Builtin thunkTarget() const {
            MOZ_ASSERT(isThunk());
            return wasm::Builtin(u.thunk.target_);
        }
    };

    class Name
    {
        PropertyName* name_;
      public:
        Name() : name_(nullptr) {}
        MOZ_IMPLICIT Name(PropertyName* name) : name_(name) {}
        PropertyName* name() const { return name_; }
        PropertyName*& name() { return name_; }
        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, Name* out) const;
    };

    typedef mozilla::UniquePtr<char[], JS::FreePolicy> ProfilingLabel;

#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    // Function information to add to the VTune JIT profiler following linking.
    struct ProfiledFunction
    {
        PropertyName* name;
        struct Pod {
            unsigned startCodeOffset;
            unsigned endCodeOffset;
            unsigned lineno;
            unsigned columnIndex;
        } pod;

        explicit ProfiledFunction()
          : name(nullptr)
        { }

        ProfiledFunction(PropertyName* name, unsigned start, unsigned end,
                         unsigned line = 0, unsigned column = 0)
          : name(name)
        {
            MOZ_ASSERT(name->isTenured());

            pod.startCodeOffset = start;
            pod.endCodeOffset = end;
            pod.lineno = line;
            pod.columnIndex = column;
        }

        void trace(JSTracer* trc) {
            if (name)
                TraceManuallyBarrieredEdge(trc, &name, "asm.js profiled function name");
        }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
    };
#endif

    struct RelativeLink
    {
        enum Kind
        {
            RawPointer,
            CodeLabel,
            InstructionImmediate
        };

        RelativeLink()
        { }

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
        // On MIPS, CodeLabels are instruction immediates so RelativeLinks only
        // patch instruction immediates.
        explicit RelativeLink(Kind kind) {
            MOZ_ASSERT(kind == CodeLabel || kind == InstructionImmediate);
        }
        bool isRawPointerPatch() {
            return false;
        }
#else
        // On the rest, CodeLabels are raw pointers so RelativeLinks only patch
        // raw pointers.
        explicit RelativeLink(Kind kind) {
            MOZ_ASSERT(kind == CodeLabel || kind == RawPointer);
        }
        bool isRawPointerPatch() {
            return true;
        }
#endif

        uint32_t patchAtOffset;
        uint32_t targetOffset;
    };

    typedef Vector<RelativeLink, 0, SystemAllocPolicy> RelativeLinkVector;

    typedef mozilla::EnumeratedArray<wasm::Builtin,
                                     wasm::Builtin::Limit,
                                     uint32_t> BuiltinThunkOffsetArray;

    typedef Vector<uint32_t, 0, SystemAllocPolicy> OffsetVector;
    typedef mozilla::EnumeratedArray<wasm::SymbolicAddress,
                                     wasm::SymbolicAddress::Limit,
                                     OffsetVector> OffsetVectorArray;

    struct AbsoluteLinkArray : public OffsetVectorArray
    {
        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, AbsoluteLinkArray* out) const;

        size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
    };

    class FuncPtrTable
    {
        struct Pod {
            uint32_t globalDataOffset_;
        } pod;
        OffsetVector elemOffsets_;

      public:
        FuncPtrTable() {}
        FuncPtrTable(FuncPtrTable&& rhs) : pod(rhs.pod), elemOffsets_(Move(rhs.elemOffsets_)) {}
        explicit FuncPtrTable(uint32_t globalDataOffset) { pod.globalDataOffset_ = globalDataOffset; }
        void define(OffsetVector&& elemOffsets) { elemOffsets_ = Move(elemOffsets); }
        uint32_t globalDataOffset() const { return pod.globalDataOffset_; }
        const OffsetVector& elemOffsets() const { return elemOffsets_; }

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, FuncPtrTable* out) const;

        size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
    };

    typedef Vector<FuncPtrTable, 0, SystemAllocPolicy> FuncPtrTableVector;

    // Static-link data is used to patch a module either after it has been
    // compiled or deserialized with various absolute addresses (of code or
    // data in the process) or relative addresses (of code or data in the same
    // AsmJSModule).
    struct StaticLinkData
    {
        StaticLinkData() { mozilla::PodZero(&pod); }

        struct Pod {
            uint32_t interruptExitOffset;
            uint32_t outOfBoundsExitOffset;
            BuiltinThunkOffsetArray builtinThunkOffsets;
        } pod;

        RelativeLinkVector relativeLinks;
        AbsoluteLinkArray absoluteLinks;
        FuncPtrTableVector funcPtrTables;

        size_t serializedSize() const;
        uint8_t* serialize(uint8_t* cursor) const;
        const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);
        bool clone(ExclusiveContext* cx, StaticLinkData* out) const;

        size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
    };

  private:
    struct Pod {
        uint32_t                          functionBytes_;
        uint32_t                          codeBytes_;
        uint32_t                          globalBytes_;
        uint32_t                          totalBytes_;
        uint32_t                          minHeapLength_;
        uint32_t                          maxHeapLength_;
        uint32_t                          heapLengthMask_;
        uint32_t                          numFFIs_;
        uint32_t                          srcLength_;
        uint32_t                          srcLengthWithRightBrace_;
        bool                              strict_;
        bool                              hasArrayView_;
        bool                              isSharedView_;
        bool                              hasFixedMinHeapLength_;
        bool                              canUseSignalHandlers_;
    } pod;

    // These two fields need to be kept out pod as they depend on the position
    // of the module within the ScriptSource and thus aren't invariant with
    // respect to caching.
    const uint32_t                        srcStart_;
    const uint32_t                        srcBodyStart_;

    Vector<Global,                 0, SystemAllocPolicy> globals_;
    Vector<Exit,                   0, SystemAllocPolicy> exits_;
    Vector<ExportedFunction,       0, SystemAllocPolicy> exports_;
    Vector<wasm::CallSite,         0, SystemAllocPolicy> callSites_;
    Vector<CodeRange,              0, SystemAllocPolicy> codeRanges_;
    Vector<Name,                   0, SystemAllocPolicy> names_;
    Vector<ProfilingLabel,         0, SystemAllocPolicy> profilingLabels_;
    Vector<wasm::HeapAccess,       0, SystemAllocPolicy> heapAccesses_;
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    Vector<ProfiledFunction,       0, SystemAllocPolicy> profiledFunctions_;
#endif

    ScriptSource *                        scriptSource_;
    PropertyName *                        globalArgumentName_;
    PropertyName *                        importArgumentName_;
    PropertyName *                        bufferArgumentName_;
    uint8_t *                             code_;
    uint8_t *                             interruptExit_;
    uint8_t *                             outOfBoundsExit_;
    StaticLinkData                        staticLinkData_;
    RelocatablePtrArrayBufferObjectMaybeShared maybeHeap_;
    AsmJSModule **                        prevLinked_;
    AsmJSModule *                         nextLinked_;
    bool                                  dynamicallyLinked_;
    bool                                  loadedFromCache_;
    bool                                  profilingEnabled_;
    bool                                  interrupted_;

    void restoreHeapToInitialState(ArrayBufferObjectMaybeShared* maybePrevBuffer);
    void restoreToInitialState(ArrayBufferObjectMaybeShared* maybePrevBuffer, uint8_t* prevCode,
                               ExclusiveContext* cx);

  public:
    explicit AsmJSModule(ScriptSource* scriptSource, uint32_t srcStart, uint32_t srcBodyStart,
                         bool strict, bool canUseSignalHandlers);
    void trace(JSTracer* trc);
    ~AsmJSModule();

    // An AsmJSModule transitions from !finished to finished to dynamically linked.
    bool isFinished() const { return !!code_; }
    bool isDynamicallyLinked() const { return dynamicallyLinked_; }

    /*************************************************************************/
    // These functions may be used as soon as the module is constructed:

    ScriptSource* scriptSource() const {
        MOZ_ASSERT(scriptSource_);
        return scriptSource_;
    }
    bool strict() const {
        return pod.strict_;
    }
    bool canUseSignalHandlers() const {
        return pod.canUseSignalHandlers_;
    }
    bool usesSignalHandlersForInterrupt() const {
        return pod.canUseSignalHandlers_;
    }
    bool usesSignalHandlersForOOB() const {
#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)
        return pod.canUseSignalHandlers_;
#else
        return false;
#endif
    }
    bool loadedFromCache() const {
        return loadedFromCache_;
    }

    // srcStart() refers to the offset in the ScriptSource to the beginning of
    // the asm.js module function. If the function has been created with the
    // Function constructor, this will be the first character in the function
    // source. Otherwise, it will be the opening parenthesis of the arguments
    // list.
    uint32_t srcStart() const {
        return srcStart_;
    }

    // srcBodyStart() refers to the offset in the ScriptSource to the end
    // of the 'use asm' string-literal token.
    uint32_t srcBodyStart() const {
        return srcBodyStart_;
    }

    // While these functions may be accessed at any time, their values will
    // change as the module is compiled.
    uint32_t minHeapLength() const {
        return pod.minHeapLength_;
    }
    uint32_t maxHeapLength() const {
        return pod.maxHeapLength_;
    }
    uint32_t heapLengthMask() const {
        MOZ_ASSERT(pod.hasFixedMinHeapLength_);
        return pod.heapLengthMask_;
    }

    // about:memory reporting
    void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* asmJSModuleCode,
                       size_t* asmJSModuleData);

    /*************************************************************************/
    // These functions build the global scope of the module while parsing the
    // module prologue (before the function bodies):

    void initGlobalArgumentName(PropertyName* n) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT_IF(n, n->isTenured());
        globalArgumentName_ = n;
    }
    void initImportArgumentName(PropertyName* n) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT_IF(n, n->isTenured());
        importArgumentName_ = n;
    }
    void initBufferArgumentName(PropertyName* n) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT_IF(n, n->isTenured());
        bufferArgumentName_ = n;
    }
    PropertyName* globalArgumentName() const {
        return globalArgumentName_;
    }
    PropertyName* importArgumentName() const {
        return importArgumentName_;
    }
    PropertyName* bufferArgumentName() const {
        return bufferArgumentName_;
    }

    /*************************************************************************/
    // These functions may only be called before finish():

  private:
    bool allocateGlobalBytes(uint32_t bytes, uint32_t align, uint32_t* globalDataOffset) {
        MOZ_ASSERT(!isFinished());
        uint32_t pad = ComputeByteAlignment(pod.globalBytes_, align);
        if (UINT32_MAX - pod.globalBytes_ < pad + bytes)
            return false;
        pod.globalBytes_ += pad;
        *globalDataOffset = pod.globalBytes_;
        pod.globalBytes_ += bytes;
        return true;
    }
    bool addGlobalVar(wasm::ValType type, uint32_t* globalDataOffset) {
        MOZ_ASSERT(!isFinished());
        unsigned width = 0;
        switch (type) {
          case wasm::ValType::I32:   case wasm::ValType::F32:   width = 4;  break;
          case wasm::ValType::I64:   case wasm::ValType::F64:   width = 8;  break;
          case wasm::ValType::I32x4: case wasm::ValType::F32x4: width = 16; break;
        }
        return allocateGlobalBytes(width, width, globalDataOffset);
    }
  public:
    bool addGlobalVarInit(const wasm::Val& v, uint32_t* globalDataOffset) {
        MOZ_ASSERT(!isFinished());
        if (!addGlobalVar(v.type(), globalDataOffset))
            return false;
        Global g(Global::Variable, nullptr);
        g.pod.u.var.initKind_ = Global::InitConstant;
        g.pod.u.var.u.val_ = v;
        g.pod.u.var.globalDataOffset_ = *globalDataOffset;
        return globals_.append(g);
    }
    bool addGlobalVarImport(PropertyName* name, wasm::ValType importType, uint32_t* globalDataOffset) {
        MOZ_ASSERT(!isFinished());
        if (!addGlobalVar(importType, globalDataOffset))
            return false;
        Global g(Global::Variable, name);
        g.pod.u.var.initKind_ = Global::InitImport;
        g.pod.u.var.u.importType_ = importType;
        g.pod.u.var.globalDataOffset_ = *globalDataOffset;
        return globals_.append(g);
    }
    bool addFFI(PropertyName* field, uint32_t* ffiIndex) {
        MOZ_ASSERT(!isFinished());
        if (pod.numFFIs_ == UINT32_MAX)
            return false;
        Global g(Global::FFI, field);
        g.pod.u.ffiIndex_ = *ffiIndex = pod.numFFIs_++;
        return globals_.append(g);
    }
    bool addArrayView(Scalar::Type vt, PropertyName* maybeField) {
        MOZ_ASSERT(!isFinished());
        pod.hasArrayView_ = true;
        pod.isSharedView_ = false;
        Global g(Global::ArrayView, maybeField);
        g.pod.u.viewType_ = vt;
        return globals_.append(g);
    }
    bool addArrayViewCtor(Scalar::Type vt, PropertyName* field) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT(field);
        pod.isSharedView_ = false;
        Global g(Global::ArrayViewCtor, field);
        g.pod.u.viewType_ = vt;
        return globals_.append(g);
    }
    bool addByteLength() {
        MOZ_ASSERT(!isFinished());
        Global g(Global::ByteLength, nullptr);
        return globals_.append(g);
    }
    bool addMathBuiltinFunction(AsmJSMathBuiltinFunction func, PropertyName* field) {
        MOZ_ASSERT(!isFinished());
        Global g(Global::MathBuiltinFunction, field);
        g.pod.u.mathBuiltinFunc_ = func;
        return globals_.append(g);
    }
    bool addMathBuiltinConstant(double value, PropertyName* field) {
        MOZ_ASSERT(!isFinished());
        Global g(Global::Constant, field);
        g.pod.u.constant.value_ = value;
        g.pod.u.constant.kind_ = Global::MathConstant;
        return globals_.append(g);
    }
    bool addAtomicsBuiltinFunction(AsmJSAtomicsBuiltinFunction func, PropertyName* field) {
        MOZ_ASSERT(!isFinished());
        Global g(Global::AtomicsBuiltinFunction, field);
        g.pod.u.atomicsBuiltinFunc_ = func;
        return globals_.append(g);
    }
    bool addSimdCtor(AsmJSSimdType type, PropertyName* field) {
        MOZ_ASSERT(!isFinished());
        Global g(Global::SimdCtor, field);
        g.pod.u.simdCtorType_ = type;
        return globals_.append(g);
    }
    bool addSimdOperation(AsmJSSimdType type, AsmJSSimdOperation op, PropertyName* field) {
        MOZ_ASSERT(!isFinished());
        Global g(Global::SimdOperation, field);
        g.pod.u.simdOp.type_ = type;
        g.pod.u.simdOp.which_ = op;
        return globals_.append(g);
    }
    bool addGlobalConstant(double value, PropertyName* name) {
        MOZ_ASSERT(!isFinished());
        Global g(Global::Constant, name);
        g.pod.u.constant.value_ = value;
        g.pod.u.constant.kind_ = Global::GlobalConstant;
        return globals_.append(g);
    }
    unsigned numGlobals() const {
        return globals_.length();
    }
    Global& global(unsigned i) {
        return globals_[i];
    }
    void setViewsAreShared() {
        if (pod.hasArrayView_)
            pod.isSharedView_ = true;
    }

    /*************************************************************************/
    // These functions are called while parsing/compiling function bodies:

    bool hasArrayView() const {
        return pod.hasArrayView_;
    }
    bool isSharedView() const {
        MOZ_ASSERT(pod.hasArrayView_);
        return pod.isSharedView_;
    }
    void addChangeHeap(uint32_t mask, uint32_t min, uint32_t max) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT(!pod.hasFixedMinHeapLength_);
        MOZ_ASSERT(IsValidAsmJSHeapLength(mask + 1));
        MOZ_ASSERT(min >= RoundUpToNextValidAsmJSHeapLength(0));
        MOZ_ASSERT(max <= pod.maxHeapLength_);
        MOZ_ASSERT(min <= max);
        pod.heapLengthMask_ = mask;
        pod.minHeapLength_ = min;
        pod.maxHeapLength_ = max;
        pod.hasFixedMinHeapLength_ = true;
    }
    bool tryRequireHeapLengthToBeAtLeast(uint32_t len) {
        MOZ_ASSERT(!isFinished());
        if (pod.hasFixedMinHeapLength_ && len > pod.minHeapLength_)
            return false;
        if (len > pod.maxHeapLength_)
            return false;
        len = RoundUpToNextValidAsmJSHeapLength(len);
        if (len > pod.minHeapLength_)
            pod.minHeapLength_ = len;
        return true;
    }
    bool addCodeRange(CodeRange::Kind kind, AsmJSOffsets offsets) {
        return codeRanges_.append(CodeRange(kind, offsets));
    }
    bool addCodeRange(CodeRange::Kind kind, AsmJSProfilingOffsets offsets) {
        return codeRanges_.append(CodeRange(kind, offsets));
    }
    bool addFunctionCodeRange(PropertyName* name, CodeRange codeRange) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT(name->isTenured());
        if (names_.length() >= UINT32_MAX)
            return false;
        codeRange.initNameIndex(names_.length());
        return names_.append(name) && codeRanges_.append(codeRange);
    }
    bool addBuiltinThunkCodeRange(wasm::Builtin builtin, AsmJSProfilingOffsets offsets) {
        MOZ_ASSERT(staticLinkData_.pod.builtinThunkOffsets[builtin] == 0);
        staticLinkData_.pod.builtinThunkOffsets[builtin] = offsets.begin;
        return codeRanges_.append(CodeRange(builtin, offsets));
    }
    bool addExit(wasm::MallocSig&& sig, unsigned ffiIndex, unsigned* exitIndex) {
        MOZ_ASSERT(!isFinished());
        static_assert(sizeof(ExitDatum) % sizeof(void*) == 0, "word aligned");
        uint32_t globalDataOffset;
        if (!allocateGlobalBytes(sizeof(ExitDatum), sizeof(void*), &globalDataOffset))
            return false;
        *exitIndex = unsigned(exits_.length());
        return exits_.append(Exit(Move(sig), ffiIndex, globalDataOffset));
    }
    unsigned numExits() const {
        return exits_.length();
    }
    Exit& exit(unsigned i) {
        return exits_[i];
    }
    const Exit& exit(unsigned i) const {
        return exits_[i];
    }
    bool declareFuncPtrTable(unsigned numElems, uint32_t* funcPtrTableIndex) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT(IsPowerOfTwo(numElems));
        uint32_t globalDataOffset;
        if (!allocateGlobalBytes(numElems * sizeof(void*), sizeof(void*), &globalDataOffset))
            return false;
        *funcPtrTableIndex = staticLinkData_.funcPtrTables.length();
        return staticLinkData_.funcPtrTables.append(FuncPtrTable(globalDataOffset));
    }
    FuncPtrTable& funcPtrTable(uint32_t funcPtrTableIndex) {
        return staticLinkData_.funcPtrTables[funcPtrTableIndex];
    }
#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
    bool addProfiledFunction(ProfiledFunction func) {
        MOZ_ASSERT(!isFinished());
        return profiledFunctions_.append(func);
    }
    unsigned numProfiledFunctions() const {
        return profiledFunctions_.length();
    }
    ProfiledFunction& profiledFunction(unsigned i) {
        return profiledFunctions_[i];
    }
#endif

    bool addExportedFunction(PropertyName* name,
                             uint32_t funcIndex,
                             uint32_t funcSrcBegin,
                             uint32_t funcSrcEnd,
                             PropertyName* maybeFieldName,
                             wasm::MallocSig&& sig)
    {
        // NB: funcSrcBegin/funcSrcEnd are given relative to the ScriptSource
        // (the entire file) and ExportedFunctions store offsets relative to
        // the beginning of the module (so that they are caching-invariant).
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT(srcStart_ < funcSrcBegin);
        MOZ_ASSERT(funcSrcBegin < funcSrcEnd);
        ExportedFunction func(name, funcIndex, funcSrcBegin - srcStart_, funcSrcEnd - srcStart_,
                              maybeFieldName, mozilla::Move(sig));
        return exports_.length() < UINT32_MAX && exports_.append(mozilla::Move(func));
    }
    bool addExportedChangeHeap(PropertyName* name,
                               uint32_t funcSrcBegin,
                               uint32_t funcSrcEnd,
                               PropertyName* maybeFieldName)
    {
        // See addExportedFunction.
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT(srcStart_ < funcSrcBegin);
        MOZ_ASSERT(funcSrcBegin < funcSrcEnd);
        ExportedFunction func(name, funcSrcBegin - srcStart_, funcSrcEnd - srcStart_,
                              maybeFieldName);
        return exports_.length() < UINT32_MAX && exports_.append(mozilla::Move(func));
    }
    unsigned numExportedFunctions() const {
        return exports_.length();
    }
    const ExportedFunction& exportedFunction(unsigned i) const {
        return exports_[i];
    }
    ExportedFunction& exportedFunction(unsigned i) {
        return exports_[i];
    }
    void setAsyncInterruptOffset(uint32_t o) {
        staticLinkData_.pod.interruptExitOffset = o;
    }
    void setOnOutOfBoundsExitOffset(uint32_t o) {
        staticLinkData_.pod.outOfBoundsExitOffset = o;
    }

    /*************************************************************************/

    // finish() is called once the entire module has been parsed (via
    // tokenStream) and all function and entry/exit trampolines have been
    // generated (via masm). After this function, the module must still be
    // statically and dynamically linked before code can be run.
    bool finish(ExclusiveContext* cx, frontend::TokenStream& ts, jit::MacroAssembler& masm);

    /*************************************************************************/
    // These accessor functions can be used after finish():

    uint8_t* codeBase() const {
        MOZ_ASSERT(isFinished());
        MOZ_ASSERT(uintptr_t(code_) % AsmJSPageSize == 0);
        return code_;
    }
    uint32_t codeBytes() const {
        MOZ_ASSERT(isFinished());
        return pod.codeBytes_;
    }
    bool containsCodePC(void* pc) const {
        MOZ_ASSERT(isFinished());
        return pc >= code_ && pc < (code_ + codeBytes());
    }

    // The range [0, functionBytes) is a subrange of [0, codeBytes) that
    // contains only function body code, not the stub code. This distinction is
    // used by the async interrupt handler to only interrupt when the pc is in
    // function code which, in turn, simplifies reasoning about how stubs
    // enter/exit.
    void setFunctionBytes(uint32_t functionBytes) {
        MOZ_ASSERT(!isFinished());
        MOZ_ASSERT(!pod.functionBytes_);
        pod.functionBytes_ = functionBytes;
    }
    uint32_t functionBytes() const {
        MOZ_ASSERT(isFinished());
        return pod.functionBytes_;
    }
    bool containsFunctionPC(void* pc) const {
        MOZ_ASSERT(isFinished());
        return pc >= code_ && pc < (code_ + functionBytes());
    }

    uint32_t globalBytes() const {
        MOZ_ASSERT(isFinished());
        return pod.globalBytes_;
    }

    unsigned numFFIs() const {
        MOZ_ASSERT(isFinished());
        return pod.numFFIs_;
    }
    uint32_t srcEndBeforeCurly() const {
        MOZ_ASSERT(isFinished());
        return srcStart_ + pod.srcLength_;
    }
    uint32_t srcEndAfterCurly() const {
        MOZ_ASSERT(isFinished());
        return srcStart_ + pod.srcLengthWithRightBrace_;
    }

    // Lookup a callsite by the return pc (from the callee to the caller).
    // Return null if no callsite was found.
    const wasm::CallSite* lookupCallSite(void* returnAddress) const;

    // Lookup the name the code range containing the given pc. Return null if no
    // code range was found.
    const CodeRange* lookupCodeRange(void* pc) const;

    // Lookup a heap access site by the pc which performs the access. Return
    // null if no heap access was found.
    const wasm::HeapAccess* lookupHeapAccess(void* pc) const;

    // The global data section is placed after the executable code (i.e., at
    // offset codeBytes_) in the module's linear allocation. The global data
    // starts with some fixed allocations followed by interleaved global,
    // function-pointer table and exit allocations.
    uint32_t offsetOfGlobalData() const {
        MOZ_ASSERT(isFinished());
        return pod.codeBytes_;
    }
    uint8_t* globalData() const {
        MOZ_ASSERT(isFinished());
        return codeBase() + offsetOfGlobalData();
    }
    static void assertGlobalDataOffsets() {
        static_assert(wasm::ActivationGlobalDataOffset == 0,
                      "an AsmJSActivation* data goes first");
        static_assert(wasm::HeapGlobalDataOffset == wasm::ActivationGlobalDataOffset + sizeof(void*),
                      "then a pointer to the heap*");
        static_assert(wasm::NaN64GlobalDataOffset == wasm::HeapGlobalDataOffset + sizeof(uint8_t*),
                      "then a 64-bit NaN");
        static_assert(wasm::NaN32GlobalDataOffset == wasm::NaN64GlobalDataOffset + sizeof(double),
                      "then a 32-bit NaN");
        static_assert(sInitialGlobalDataBytes == wasm::NaN32GlobalDataOffset + sizeof(float),
                      "then all the normal global data (globals, exits, func-ptr-tables)");
    }
    static const uint32_t sInitialGlobalDataBytes = wasm::NaN32GlobalDataOffset + sizeof(float);

    AsmJSActivation*& activation() const {
        MOZ_ASSERT(isFinished());
        return *(AsmJSActivation**)(globalData() + wasm::ActivationGlobalDataOffset);
    }
    bool active() const {
        return activation() != nullptr;
    }
  private:
    // The pointer may reference shared memory, use with care.
    // Generally you want to use maybeHeap(), not heapDatum().
    uint8_t*& heapDatum() const {
        MOZ_ASSERT(isFinished());
        return *(uint8_t**)(globalData() + wasm::HeapGlobalDataOffset);
    }
  public:

    /*************************************************************************/
    // These functions are called after finish() but before staticallyLink():

    bool addRelativeLink(RelativeLink link) {
        MOZ_ASSERT(isFinished());
        return staticLinkData_.relativeLinks.append(link);
    }

    // A module is serialized after it is finished but before it is statically
    // linked. (Technically, it could be serialized after static linking, but it
    // would still need to be statically linked on deserialization.)
    size_t serializedSize() const;
    uint8_t* serialize(uint8_t* cursor) const;
    const uint8_t* deserialize(ExclusiveContext* cx, const uint8_t* cursor);

    // Additionally, this function is called to flush the i-cache after
    // deserialization and cloning (but still before static linking, to prevent
    // a bunch of expensive micro-flushes).
    void setAutoFlushICacheRange();

    /*************************************************************************/

    // After a module is finished compiling or deserializing, it is "statically
    // linked" which specializes the code to its current address (this allows
    // code to be relocated between serialization and deserialization).
    void staticallyLink(ExclusiveContext* cx);

    // After a module is statically linked, it is "dynamically linked" which
    // specializes it to a particular set of arguments. In particular, this
    // binds the code to a particular heap (via initHeap) and set of global
    // variables. A given asm.js module cannot be dynamically linked more than
    // once so, if JS tries, the module is cloned. When linked, an asm.js module
    // is kept in a list so that it can be updated if the linked buffer is
    // detached.
    void setIsDynamicallyLinked(JSRuntime* rt) {
        MOZ_ASSERT(isFinished());
        MOZ_ASSERT(!isDynamicallyLinked());
        dynamicallyLinked_ = true;
        nextLinked_ = rt->linkedAsmJSModules;
        prevLinked_ = &rt->linkedAsmJSModules;
        if (nextLinked_)
            nextLinked_->prevLinked_ = &nextLinked_;
        rt->linkedAsmJSModules = this;
        MOZ_ASSERT(isDynamicallyLinked());
    }

    void initHeap(Handle<ArrayBufferObjectMaybeShared*> heap, JSContext* cx);
    bool changeHeap(Handle<ArrayBufferObject*> newHeap, JSContext* cx);
    bool detachHeap(JSContext* cx);

    bool clone(JSContext* cx, ScopedJSDeletePtr<AsmJSModule>* moduleOut) const;

    /*************************************************************************/
    // Functions that can be called after dynamic linking succeeds:

    AsmJSModule* nextLinked() const {
        MOZ_ASSERT(isDynamicallyLinked());
        return nextLinked_;
    }
    bool hasDetachedHeap() const {
        MOZ_ASSERT(isDynamicallyLinked());
        return hasArrayView() && !heapDatum();
    }
    CodePtr entryTrampoline(const ExportedFunction& func) const {
        MOZ_ASSERT(isDynamicallyLinked());
        MOZ_ASSERT(!func.isChangeHeap());
        return JS_DATA_TO_FUNC_PTR(CodePtr, code_ + func.pod.codeOffset_);
    }
    uint8_t* interruptExit() const {
        MOZ_ASSERT(isDynamicallyLinked());
        return interruptExit_;
    }
    uint8_t* outOfBoundsExit() const {
        MOZ_ASSERT(isDynamicallyLinked());
        return outOfBoundsExit_;
    }
    SharedMem<uint8_t*> maybeHeap() const {
        MOZ_ASSERT(isDynamicallyLinked());
        return hasArrayView() && isSharedView() ? SharedMem<uint8_t*>::shared(heapDatum())
            : SharedMem<uint8_t*>::unshared(heapDatum());
    }
    ArrayBufferObjectMaybeShared* maybeHeapBufferObject() const {
        MOZ_ASSERT(isDynamicallyLinked());
        return maybeHeap_;
    }
    size_t heapLength() const;
    bool profilingEnabled() const {
        MOZ_ASSERT(isDynamicallyLinked());
        return profilingEnabled_;
    }
    void setProfilingEnabled(bool enabled, JSContext* cx);
    void setInterrupted(bool interrupted) {
        MOZ_ASSERT(isDynamicallyLinked());
        interrupted_ = interrupted;
    }
};

// Store the just-parsed module in the cache using AsmJSCacheOps.
extern JS::AsmJSCacheResult
StoreAsmJSModuleInCache(AsmJSParser& parser,
                        const AsmJSModule& module,
                        ExclusiveContext* cx);

// Attempt to load the asm.js module that is about to be parsed from the cache
// using AsmJSCacheOps. On cache hit, *module will be non-null. Note: the
// return value indicates whether or not an error was encountered, not whether
// there was a cache hit.
extern bool
LookupAsmJSModuleInCache(ExclusiveContext* cx,
                         AsmJSParser& parser,
                         ScopedJSDeletePtr<AsmJSModule>* module,
                         ScopedJSFreePtr<char>* compilationTimeReport);

// This function must be called for every detached ArrayBuffer.
extern bool
OnDetachAsmJSArrayBuffer(JSContext* cx, Handle<ArrayBufferObject*> buffer);

// An AsmJSModuleObject is an internal implementation object (i.e., not exposed
// directly to user script) which manages the lifetime of an AsmJSModule. A
// JSObject is necessary since we want LinkAsmJS/CallAsmJS JSFunctions to be
// able to point to their module via their extended slots.
class AsmJSModuleObject : public NativeObject
{
    static const unsigned MODULE_SLOT = 0;

  public:
    static const unsigned RESERVED_SLOTS = 1;

    // On success, return an AsmJSModuleClass JSObject that has taken ownership
    // (and release()ed) the given module.
    static AsmJSModuleObject* create(ExclusiveContext* cx, ScopedJSDeletePtr<AsmJSModule>* module);

    AsmJSModule& module() const;

    void addSizeOfMisc(mozilla::MallocSizeOf mallocSizeOf, size_t* asmJSModuleCode,
                       size_t* asmJSModuleData) {
        module().addSizeOfMisc(mallocSizeOf, asmJSModuleCode, asmJSModuleData);
    }

    static const Class class_;
};

} // namespace js

#endif /* asmjs_AsmJSModule_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
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

#include "wasm/WasmIonCompile.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#include "jit/ABIArgGenerator.h"
#include "jit/CodeGenerator.h"
#include "jit/CompileInfo.h"
#include "jit/Ion.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/ShuffleAnalysis.h"
#include "js/GCAPI.h"       // JS::AutoSuppressGCAnalysis
#include "js/ScalarType.h"  // js::Scalar::Type
#include "util/DifferentialTesting.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmBuiltinModule.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::IsPowerOfTwo;
using mozilla::Nothing;

namespace {

using UniqueCompileInfo = UniquePtr<CompileInfo>;
using UniqueCompileInfoVector = Vector<UniqueCompileInfo, 1, SystemAllocPolicy>;

using BlockVector = Vector<MBasicBlock*, 8, SystemAllocPolicy>;
using DefVector = Vector<MDefinition*, 8, SystemAllocPolicy>;
using ControlInstructionVector =
    Vector<MControlInstruction*, 8, SystemAllocPolicy>;

// [SMDOC] WebAssembly Exception Handling in Ion
// =======================================================
//
// ## Throwing instructions
//
// Wasm exceptions can be thrown by either a throw instruction (local throw),
// or by a wasm call.
//
// ## The "catching try control"
//
// We know we are in try-code if there is a surrounding ControlItem with
// LabelKind::Try. The innermost such control is called the
// "catching try control".
//
// ## Throws without a catching try control
//
// Such throws are implemented with an instance call that triggers the exception
// unwinding runtime. The exception unwinding runtime will not return to the
// function.
//
// ## "landing pad" and "pre-pad" blocks
//
// When an exception is thrown, the unwinder will search for the nearest
// enclosing try block and redirect control flow to it. The code that executes
// before any catch blocks is called the 'landing pad'. The 'landing pad' is
// responsible to:
//   1. Consume the pending exception state from
//      Instance::pendingException(Tag)
//   2. Branch to the correct catch block, or else rethrow
//
// There is one landing pad for each try block. The immediate predecessors of
// the landing pad are called 'pre-pad' blocks. There is one pre-pad block per
// throwing instruction.
//
// ## Creating pre-pad blocks
//
// There are two possible sorts of pre-pad blocks, depending on whether we
// are branching after a local throw instruction, or after a wasm call:
//
// - If we encounter a local throw, we create the exception and tag objects,
//   store them to Instance::pendingException(Tag), and then jump to the
//   landing pad.
//
// - If we encounter a wasm call, we construct a MWasmCallCatchable which is a
//   control instruction with either a branch to a fallthrough block or
//   to a pre-pad block.
//
//   The pre-pad block for a wasm call is empty except for a jump to the
//   landing pad. It only exists to avoid critical edges which when split would
//   violate the invariants of MWasmCallCatchable. The pending exception state
//   is taken care of by the unwinder.
//
// Each pre-pad ends with a pending jump to the landing pad. The pending jumps
// to the landing pad are tracked in `tryPadPatches`. These are called
// "pad patches".
//
// ## Creating the landing pad
//
// When we exit try-code, we check if tryPadPatches has captured any control
// instructions (pad patches). If not, we don't compile any catches and we mark
// the rest as dead code.
//
// If there are pre-pad blocks, we join them to create a landing pad (or just
// "pad"). The pad's last two slots are the caught exception, and the
// exception's tag object.
//
// There are three different forms of try-catch/catch_all Wasm instructions,
// which result in different form of landing pad.
//
// 1. A catchless try, so a Wasm instruction of the form "try ... end".
//    - In this case, we end the pad by rethrowing the caught exception.
//
// 2. A single catch_all after a try.
//    - If the first catch after a try is a catch_all, then there won't be
//      any more catches, but we need the exception and its tag object, in
//      case the code in a catch_all contains "rethrow" instructions.
//      - The Wasm instruction "rethrow", gets the exception and tag object to
//        rethrow from the last two slots of the landing pad which, due to
//        validation, is the l'th surrounding ControlItem.
//      - We immediately GoTo to a new block after the pad and pop both the
//        exception and tag object, as we don't need them anymore in this case.
//
// 3. Otherwise, there is one or more catch code blocks following.
//    - In this case, we construct the landing pad by creating a sequence
//      of compare and branch blocks that compare the pending exception tag
//      object to the tag object of the current tagged catch block. This is
//      done incrementally as we visit each tagged catch block in the bytecode
//      stream. At every step, we update the ControlItem's block to point to
//      the next block to be created in the landing pad sequence. The final
//      block will either be a rethrow, if there is no catch_all, or else a
//      jump to a catch_all block.

struct TryControl {
  // Branches to bind to the try's landing pad.
  ControlInstructionVector landingPadPatches;
  // For `try_table`, the list of tagged catches and labels to branch to.
  TryTableCatchVector catches;
  // The pending exception for the try's landing pad.
  MDefinition* pendingException;
  // The pending exception's tag for the try's landing pad.
  MDefinition* pendingExceptionTag;
  // Whether this try is in the body and should catch any thrown exception.
  bool inBody;

  TryControl()
      : pendingException(nullptr),
        pendingExceptionTag(nullptr),
        inBody(false) {}

  // Reset the try control for when it is cached in FunctionCompiler.
  void reset() {
    landingPadPatches.clearAndFree();
    catches.clearAndFree();
    inBody = false;
  }
};
using UniqueTryControl = UniquePtr<TryControl>;
using VectorUniqueTryControl = Vector<UniqueTryControl, 2, SystemAllocPolicy>;

struct ControlFlowPatch {
  MControlInstruction* ins;
  uint32_t index;
  ControlFlowPatch(MControlInstruction* ins, uint32_t index)
      : ins(ins), index(index) {}
};

using ControlFlowPatchVector = Vector<ControlFlowPatch, 0, SystemAllocPolicy>;

struct PendingBlockTarget {
  ControlFlowPatchVector patches;
  BranchHint hint = BranchHint::Invalid;
};

using PendingBlockTargetVector =
    Vector<PendingBlockTarget, 0, SystemAllocPolicy>;

// Inlined functions accumulate all returns to be bound to a caller function
// after compilation is finished.
struct PendingInlineReturn {
  PendingInlineReturn(MGoto* jump, DefVector&& results)
      : jump(jump), results(std::move(results)) {}

  MGoto* jump;
  DefVector results;
};

using PendingInlineReturnVector =
    Vector<PendingInlineReturn, 1, SystemAllocPolicy>;

// CallCompileState describes a call that is being compiled.
struct CallCompileState {
  // A generator object that is passed each argument as it is compiled.
  WasmABIArgGenerator abi;

  // Accumulates the register arguments while compiling arguments.
  MWasmCallBase::Args regArgs;

  // Reserved argument for passing Instance* to builtin instance method calls.
  ABIArg instanceArg;

  // The stack area in which the callee will write stack return values, or
  // nullptr if no stack results.
  MWasmStackResultArea* stackResultArea = nullptr;

  // Indicates that the call is a return/tail call.
  bool returnCall = false;

  // The landing pad patches for the nearest enclosing try-catch. This is
  // non-null iff the call is catchable.
  ControlInstructionVector* tryLandingPadPatches = nullptr;

  // The index of the try note for a catchable call.
  uint32_t tryNoteIndex = UINT32_MAX;

  // The block to take for fallthrough execution for a catchable call.
  MBasicBlock* fallthroughBlock = nullptr;

  // The block to take for exceptional execution for a catchable call.
  MBasicBlock* prePadBlock = nullptr;

  bool isCatchable() const { return tryLandingPadPatches != nullptr; }
};

struct Control {
  MBasicBlock* block;
  UniqueTryControl tryControl;

  Control() : block(nullptr), tryControl(nullptr) {}
  Control(Control&&) = default;
  Control(const Control&) = delete;
};

struct IonCompilePolicy {
  // We store SSA definitions in the value stack.
  using Value = MDefinition*;
  using ValueVector = DefVector;

  // We store loop headers and then/else blocks in the control flow stack.
  // In the case of try-catch control blocks, we collect additional information
  // regarding the possible paths from throws and calls to a landing pad, as
  // well as information on the landing pad's handlers (its catches).
  using ControlItem = Control;
};

using IonOpIter = OpIter<IonCompilePolicy>;

// Statistics for inlining (at all depths) into the root function.
struct InliningStats {
  size_t inlinedDirectBytecodeSize = 0;   // sum of sizes of inlinees
  size_t inlinedDirectFunctions = 0;      // number of inlinees
  size_t inlinedCallRefBytecodeSize = 0;  // sum of sizes of inlinees
  size_t inlinedCallRefFunctions = 0;     // number of inlinees
  bool largeFunctionBackoff = false;      // did large function backoff happen?
};

// Encapsulates the generation of MIR for a wasm function and any functions
// that become inlined into it.
class RootCompiler {
  const CompilerEnvironment& compilerEnv_;
  const CodeMetadata& codeMeta_;
  const CodeTailMetadata* codeTailMeta_;

  const ValTypeVector& locals_;
  const FuncCompileInput& func_;
  Decoder& decoder_;
  FeatureUsage observedFeatures_;

  CompileInfo compileInfo_;
  const JitCompileOptions options_;
  TempAllocator& alloc_;
  MIRGraph mirGraph_;
  MIRGenerator mirGen_;

  // The current loop depth we're generating inside of. This includes all
  // callee functions when we're generating an inlined function, and so it
  // lives here on the root compiler.
  uint32_t loopDepth_;

  // The current stack of bytecode offsets of the caller functions of the
  // function currently being inlined.
  BytecodeOffsetVector inlinedCallerOffsets_;
  InlinedCallerOffsetIndex inlinedCallerOffsetsIndex_;

  // Compilation statistics for this function.
  CompileStats funcStats_;

  // Accumulated inlining statistics for this function.
  InliningStats inliningStats_;
  // The remaining inlining budget, in terms of bytecode bytes. This may go
  // negative and so is signed.
  int64_t localInliningBudget_;

  // All jit::CompileInfo objects created during this compilation. This must
  // be kept alive for as long as the MIR graph is alive.
  UniqueCompileInfoVector compileInfos_;

  // Cache of TryControl to minimize heap allocations.
  VectorUniqueTryControl tryControlCache_;

  // Reference to masm.tryNotes()
  wasm::TryNoteVector& tryNotes_;

  // Reference to masm.inliningContext()
  wasm::InliningContext& inliningContext_;

 public:
  RootCompiler(const CompilerEnvironment& compilerEnv,
               const CodeMetadata& codeMeta,
               const CodeTailMetadata* codeTailMeta, TempAllocator& alloc,
               const ValTypeVector& locals, const FuncCompileInput& func,
               Decoder& decoder, wasm::TryNoteVector& tryNotes,
               wasm::InliningContext& inliningContext)
      : compilerEnv_(compilerEnv),
        codeMeta_(codeMeta),
        codeTailMeta_(codeTailMeta),
        locals_(locals),
        func_(func),
        decoder_(decoder),
        observedFeatures_(FeatureUsage::None),
        compileInfo_(locals.length()),
        alloc_(alloc),
        mirGraph_(&alloc),
        mirGen_(nullptr, options_, &alloc_, &mirGraph_, &compileInfo_,
                IonOptimizations.get(OptimizationLevel::Wasm), &codeMeta),
        loopDepth_(0),
        localInliningBudget_(0),
        tryNotes_(tryNotes),
        inliningContext_(inliningContext) {}

  const CompilerEnvironment& compilerEnv() const { return compilerEnv_; }
  const CodeMetadata& codeMeta() const { return codeMeta_; }
  const CodeTailMetadata* codeTailMeta() const { return codeTailMeta_; }
  const FuncCompileInput& func() const { return func_; }
  TempAllocator& alloc() { return alloc_; }
  MIRGraph& mirGraph() { return mirGraph_; }
  MIRGenerator& mirGen() { return mirGen_; }
  int64_t inliningBudget() const { return localInliningBudget_; }
  FeatureUsage observedFeatures() const { return observedFeatures_; }
  const CompileStats& funcStats() const { return funcStats_; }
  void noteLargeFunctionBackoffWasApplied() {
    inliningStats_.largeFunctionBackoff = true;
  }

  uint32_t loopDepth() const { return loopDepth_; }
  void startLoop() { loopDepth_++; }
  void closeLoop() { loopDepth_--; }

  [[nodiscard]] bool generate();

  InlinedCallerOffsetIndex inlinedCallerOffsetsIndex() const {
    return inlinedCallerOffsetsIndex_;
  }

  // Add a compile info for an inlined function. This keeps the inlined
  // function's compile info alive for the outermost function's
  // compilation.
  [[nodiscard]] CompileInfo* startInlineCall(
      uint32_t callerFuncIndex, BytecodeOffset callerOffset,
      uint32_t calleeFuncIndex, uint32_t numLocals, size_t inlineeBytecodeSize,
      InliningHeuristics::CallKind callKind);
  void finishInlineCall();

  // Add a try note and return the index.
  [[nodiscard]] bool addTryNote(uint32_t* tryNoteIndex) {
    if (!tryNotes_.append(wasm::TryNote())) {
      return false;
    }
    *tryNoteIndex = tryNotes_.length() - 1;
    return true;
  }

  // Try to get a free TryControl from the cache, or allocate a new one.
  [[nodiscard]] UniqueTryControl newTryControl() {
    if (tryControlCache_.empty()) {
      return UniqueTryControl(js_new<TryControl>());
    }
    UniqueTryControl tryControl = std::move(tryControlCache_.back());
    tryControlCache_.popBack();
    return tryControl;
  }

  // Release the TryControl to the cache.
  void freeTryControl(UniqueTryControl&& tryControl) {
    // Ensure that it's in a consistent state
    tryControl->reset();
    // Ignore any OOM, as we'll fail later
    (void)tryControlCache_.append(std::move(tryControl));
  }
};

// Encapsulates the generation of MIR for a single function in a wasm module.
class FunctionCompiler {
  // The root function compiler we are being compiled within.
  RootCompiler& rootCompiler_;

  // The caller function compiler, if any, that we are being inlined into.
  // Note that `inliningDepth_` is zero for the first inlinee, one for the
  // second inlinee, etc.
  const FunctionCompiler* callerCompiler_;
  const uint32_t inliningDepth_;

  // Information about this function's bytecode and parsing state
  IonOpIter iter_;
  uint32_t functionBodyOffset_;
  const FuncCompileInput& func_;
  const ValTypeVector& locals_;
  size_t lastReadCallSite_;
  size_t numCallRefs_;
  size_t numAllocSites_;

  // CompileInfo for compiling the MIR for this function. Allocated inside of
  // RootCompiler::compileInfos, and kept alive for the duration of the
  // total compilation.
  const jit::CompileInfo& info_;

  MBasicBlock* curBlock_;
  uint32_t maxStackArgBytes_;

  // When generating a forward branch we haven't created the basic block that
  // the branch needs to target. We handle this by accumulating all the branch
  // instructions that want to target a block we have not yet created into
  // `pendingBlocks_` and then patching them in `bindBranches`.
  //
  // For performance reasons we only grow `pendingBlocks_` as needed, never
  // shrink it. So the length of the vector has no relation to the current
  // nesting depth of wasm blocks. We use `pendingBlockDepth_` to track the
  // current wasm block depth. We assert that all entries beyond the current
  // block depth are empty.
  uint32_t pendingBlockDepth_;
  PendingBlockTargetVector pendingBlocks_;
  // Control flow patches for exceptions that are caught without a landing
  // pad they can directly jump to. This happens when either:
  //  (1) `delegate` targets the function body label.
  //  (2) A `try` ends without any cases, and there is no enclosing `try`.
  //  (3) There is no `try` in this function, but a caller function (when
  //      inlining) has a `try`.
  //
  // These exceptions will be rethrown using `emitBodyRethrowPad`.
  ControlInstructionVector bodyRethrowPadPatches_;
  // A vector of the returns in this function for use when we're being inlined
  // into another function.
  PendingInlineReturnVector pendingInlineReturns_;
  // A block that all uncaught exceptions in this function will jump to. The
  // inline caller will link this to the nearest enclosing catch handler.
  MBasicBlock* pendingInlineCatchBlock_;

  // Instance pointer argument to the current function.
  MWasmParameter* instancePointer_;
  MWasmParameter* stackResultPointer_;

 public:
  // Construct a FunctionCompiler for the root function of a compilation
  FunctionCompiler(RootCompiler& rootCompiler, Decoder& decoder,
                   const FuncCompileInput& func, const ValTypeVector& locals,
                   const CompileInfo& compileInfo)
      : rootCompiler_(rootCompiler),
        callerCompiler_(nullptr),
        inliningDepth_(0),
        iter_(rootCompiler.codeMeta(), decoder, locals),
        functionBodyOffset_(decoder.beginOffset()),
        func_(func),
        locals_(locals),
        lastReadCallSite_(0),
        numCallRefs_(0),
        numAllocSites_(0),
        info_(compileInfo),
        curBlock_(nullptr),
        maxStackArgBytes_(0),
        pendingBlockDepth_(0),
        pendingInlineCatchBlock_(nullptr),
        instancePointer_(nullptr),
        stackResultPointer_(nullptr) {}

  // Construct a FunctionCompiler for an inlined callee of a compilation
  FunctionCompiler(const FunctionCompiler* callerCompiler, Decoder& decoder,
                   const FuncCompileInput& func, const ValTypeVector& locals,
                   const CompileInfo& compileInfo)
      : rootCompiler_(callerCompiler->rootCompiler_),
        callerCompiler_(callerCompiler),
        inliningDepth_(callerCompiler_->inliningDepth() + 1),
        iter_(rootCompiler_.codeMeta(), decoder, locals),
        functionBodyOffset_(decoder.beginOffset()),
        func_(func),
        locals_(locals),
        lastReadCallSite_(0),
        numCallRefs_(0),
        numAllocSites_(0),
        info_(compileInfo),
        curBlock_(nullptr),
        maxStackArgBytes_(0),
        pendingBlockDepth_(0),
        pendingInlineCatchBlock_(nullptr),
        instancePointer_(callerCompiler_->instancePointer_),
        stackResultPointer_(nullptr) {}

  RootCompiler& rootCompiler() { return rootCompiler_; }
  const CodeMetadata& codeMeta() const { return rootCompiler_.codeMeta(); }
  const CodeTailMetadata* codeTailMeta() const {
    return rootCompiler_.codeTailMeta();
  }

  IonOpIter& iter() { return iter_; }
  uint32_t relativeBytecodeOffset() {
    return readBytecodeOffset() - functionBodyOffset_;
  }
  TempAllocator& alloc() const { return rootCompiler_.alloc(); }
  // FIXME(1401675): Replace with BlockType.
  uint32_t funcIndex() const { return func_.index; }
  const FuncType& funcType() const {
    return codeMeta().getFuncType(func_.index);
  }

  bool isInlined() const { return callerCompiler_ != nullptr; }
  uint32_t inliningDepth() const { return inliningDepth_; }

  MBasicBlock* getCurBlock() const { return curBlock_; }
  BytecodeOffset bytecodeOffset() const { return iter_.bytecodeOffset(); }
  TrapSiteDesc trapSiteDesc() {
    return TrapSiteDesc(wasm::BytecodeOffset(bytecodeOffset()),
                        rootCompiler_.inlinedCallerOffsetsIndex());
  }
  TrapSiteDesc trapSiteDescWithCallSiteLineNumber() {
    return TrapSiteDesc(wasm::BytecodeOffset(readCallSiteLineOrBytecode()),
                        rootCompiler_.inlinedCallerOffsetsIndex());
  }
  FeatureUsage featureUsage() const { return iter_.featureUsage(); }

  [[nodiscard]] bool initRoot() {
    // We are not being inlined into something
    MOZ_ASSERT(!callerCompiler_);

    // Prepare the entry block for MIR generation:

    const FuncType& ft = funcType();
    const ArgTypeVector args(ft);

    if (!mirGen().ensureBallast()) {
      return false;
    }
    if (!newBlock(/* prev */ nullptr, &curBlock_)) {
      return false;
    }

    for (WasmABIArgIter i(args); !i.done(); i++) {
      MaybeRefType argRefType;
      if (!args.isSyntheticStackResultPointerArg(i.index())) {
        ValType argType = ft.arg(i.index());
        argRefType = argType.isRefType() ? MaybeRefType(argType.refType())
                                         : MaybeRefType();
      }

      MWasmParameter* ins =
          MWasmParameter::New(alloc(), *i, i.mirType(), argRefType);
      curBlock_->add(ins);
      if (args.isSyntheticStackResultPointerArg(i.index())) {
        MOZ_ASSERT(stackResultPointer_ == nullptr);
        stackResultPointer_ = ins;
      } else {
        curBlock_->initSlot(info().localSlot(args.naturalIndex(i.index())),
                            ins);
      }
      if (!mirGen().ensureBallast()) {
        return false;
      }
    }

    // Set up a parameter that receives the hidden instance pointer argument.
    instancePointer_ =
        MWasmParameter::New(alloc(), ABIArg(InstanceReg), MIRType::Pointer);
    curBlock_->add(instancePointer_);
    if (!mirGen().ensureBallast()) {
      return false;
    }

    for (size_t i = args.lengthWithoutStackResults(); i < locals_.length();
         i++) {
      ValType slotValType = locals_[i];
#ifndef ENABLE_WASM_SIMD
      if (slotValType == ValType::V128) {
        return iter().fail("Ion has no SIMD support yet");
      }
#endif
      MDefinition* zero = constantZeroOfValType(slotValType);
      curBlock_->initSlot(info().localSlot(i), zero);
      if (!mirGen().ensureBallast()) {
        return false;
      }
    }

    return true;
  }

  [[nodiscard]] bool initInline(const DefVector& argValues) {
    // "This is an inlined-callee FunctionCompiler"
    MOZ_ASSERT(callerCompiler_);

    // Prepare the entry block for MIR generation:
    if (!mirGen().ensureBallast()) {
      return false;
    }
    if (!newBlock(nullptr, &curBlock_)) {
      return false;
    }

    MBasicBlock* pred = callerCompiler_->curBlock_;
    pred->end(MGoto::New(alloc(), curBlock_));
    if (!curBlock_->addPredecessorWithoutPhis(pred)) {
      return false;
    }

    // Set up args slots to point to passed argument values
    const FuncType& type = funcType();
    for (uint32_t argIndex = 0; argIndex < type.args().length(); argIndex++) {
      curBlock_->initSlot(info().localSlot(argIndex), argValues[argIndex]);
    }

    // Set up a parameter that receives the hidden instance pointer argument.
    instancePointer_ = callerCompiler_->instancePointer_;

    // Initialize all local slots to zero value
    for (size_t i = type.args().length(); i < locals_.length(); i++) {
      ValType slotValType = locals_[i];
#ifndef ENABLE_WASM_SIMD
      if (slotValType == ValType::V128) {
        return iter().fail("Ion has no SIMD support yet");
      }
#endif
      MDefinition* zero = constantZeroOfValType(slotValType);
      curBlock_->initSlot(info().localSlot(i), zero);
      if (!mirGen().ensureBallast()) {
        return false;
      }
    }

    return true;
  }

  void finish() {
    mirGen().accumulateWasmMaxStackArgBytes(maxStackArgBytes_);

    MOZ_ASSERT(pendingBlockDepth_ == 0);
#ifdef DEBUG
    for (PendingBlockTarget& targets : pendingBlocks_) {
      MOZ_ASSERT(targets.patches.empty());
    }
#endif
    MOZ_ASSERT(inDeadCode());
    MOZ_ASSERT(done());
    MOZ_ASSERT(func_.callSiteLineNums.length() == lastReadCallSite_);
    MOZ_ASSERT_IF(
        compilerEnv().mode() == CompileMode::LazyTiering,
        codeTailMeta()->getFuncDefCallRefs(funcIndex()).length == numCallRefs_);
    MOZ_ASSERT_IF(codeTailMeta(),
                  codeTailMeta()->getFuncDefAllocSites(funcIndex()).length ==
                      numAllocSites_);
    MOZ_ASSERT_IF(!isInlined(),
                  pendingInlineReturns_.empty() && !pendingInlineCatchBlock_);
    MOZ_ASSERT(bodyRethrowPadPatches_.empty());
  }

  /************************* Read-only interface (after local scope setup) */

  MIRGenerator& mirGen() const { return rootCompiler_.mirGen(); }
  MIRGraph& mirGraph() const { return rootCompiler_.mirGraph(); }
  const CompileInfo& info() const { return info_; }
  const CompilerEnvironment& compilerEnv() const {
    return rootCompiler_.compilerEnv();
  }

  MDefinition* getLocalDef(unsigned slot) {
    if (inDeadCode()) {
      return nullptr;
    }
    return curBlock_->getSlot(info().localSlot(slot));
  }

  const ValTypeVector& locals() const { return locals_; }

  /*********************************************************** Constants ***/

  MDefinition* constantF32(float f) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* cst = MWasmFloatConstant::NewFloat32(alloc(), f);
    curBlock_->add(cst);
    return cst;
  }
  // Hide all other overloads, to guarantee no implicit argument conversion.
  template <typename T>
  MDefinition* constantF32(T) = delete;

  MDefinition* constantF64(double d) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* cst = MWasmFloatConstant::NewDouble(alloc(), d);
    curBlock_->add(cst);
    return cst;
  }
  template <typename T>
  MDefinition* constantF64(T) = delete;

  MDefinition* constantI32(int32_t i) {
    if (inDeadCode()) {
      return nullptr;
    }
    MConstant* constant =
        MConstant::New(alloc(), Int32Value(i), MIRType::Int32);
    curBlock_->add(constant);
    return constant;
  }
  template <typename T>
  MDefinition* constantI32(T) = delete;

  MDefinition* constantI64(int64_t i) {
    if (inDeadCode()) {
      return nullptr;
    }
    MConstant* constant = MConstant::NewInt64(alloc(), i);
    curBlock_->add(constant);
    return constant;
  }
  template <typename T>
  MDefinition* constantI64(T) = delete;

  // Produce an MConstant of the machine's target int type (Int32 or Int64).
  MDefinition* constantTargetWord(intptr_t n) {
    return targetIs64Bit() ? constantI64(int64_t(n)) : constantI32(int32_t(n));
  }
  template <typename T>
  MDefinition* constantTargetWord(T) = delete;

#ifdef ENABLE_WASM_SIMD
  MDefinition* constantV128(V128 v) {
    if (inDeadCode()) {
      return nullptr;
    }
    MWasmFloatConstant* constant = MWasmFloatConstant::NewSimd128(
        alloc(), SimdConstant::CreateSimd128((int8_t*)v.bytes));
    curBlock_->add(constant);
    return constant;
  }
  template <typename T>
  MDefinition* constantV128(T) = delete;
#endif

  MDefinition* constantNullRef(MaybeRefType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    // MConstant has a lot of baggage so we don't use that here.
    MWasmNullConstant* constant = MWasmNullConstant::New(alloc(), type);
    curBlock_->add(constant);
    return constant;
  }

  // Produce a zero constant for the specified ValType.
  MDefinition* constantZeroOfValType(ValType valType) {
    switch (valType.kind()) {
      case ValType::I32:
        return constantI32(0);
      case ValType::I64:
        return constantI64(int64_t(0));
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
        return constantV128(V128(0));
#endif
      case ValType::F32:
        return constantF32(0.0f);
      case ValType::F64:
        return constantF64(0.0);
      case ValType::Ref:
        return constantNullRef(MaybeRefType(valType.refType()));
      default:
        MOZ_CRASH();
    }
  }

  /***************************** Code generation (after local scope setup) */

  void fence() {
    if (inDeadCode()) {
      return;
    }
    MWasmFence* ins = MWasmFence::New(alloc());
    curBlock_->add(ins);
  }

  template <class T>
  MDefinition* unary(MDefinition* op) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), op);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* unary(MDefinition* op, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), op, type);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* binary(MDefinition* lhs, MDefinition* rhs) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), lhs, rhs);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* binary(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* binary(MDefinition* lhs, MDefinition* rhs, MIRType type,
                      MWasmBinaryBitwise::SubOpcode subOpc) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), lhs, rhs, type, subOpc);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* ursh(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MUrsh::NewWasm(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* add(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MAdd::NewWasm(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  bool mustPreserveNaN(MIRType type) {
    return IsFloatingPointType(type) && !codeMeta().isAsmJS();
  }

  MDefinition* sub(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }

    // wasm can't fold x - 0.0 because of NaN with custom payloads.
    MSub* ins = MSub::NewWasm(alloc(), lhs, rhs, type, mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* nearbyInt(MDefinition* input, RoundingMode roundingMode) {
    if (inDeadCode()) {
      return nullptr;
    }

    auto* ins = MNearbyInt::New(alloc(), input, input->type(), roundingMode);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* minMax(MDefinition* lhs, MDefinition* rhs, MIRType type,
                      bool isMax) {
    if (inDeadCode()) {
      return nullptr;
    }

    if (mustPreserveNaN(type)) {
      // Convert signaling NaN to quiet NaNs.
      MDefinition* zero = constantZeroOfValType(ValType::fromMIRType(type));
      lhs = sub(lhs, zero, type);
      rhs = sub(rhs, zero, type);
    }

    MMinMax* ins = MMinMax::NewWasm(alloc(), lhs, rhs, type, isMax);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* mul(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   MMul::Mode mode) {
    if (inDeadCode()) {
      return nullptr;
    }

    // wasm can't fold x * 1.0 because of NaN with custom payloads.
    auto* ins =
        MMul::NewWasm(alloc(), lhs, rhs, type, mode, mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* div(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   bool unsignd) {
    if (inDeadCode()) {
      return nullptr;
    }
    bool trapOnError = !codeMeta().isAsmJS();
    if (!unsignd && type == MIRType::Int32) {
      // Enforce the signedness of the operation by coercing the operands
      // to signed.  Otherwise, operands that "look" unsigned to Ion but
      // are not unsigned to Baldr (eg, unsigned right shifts) may lead to
      // the operation being executed unsigned.  Applies to mod() as well.
      //
      // Do this for Int32 only since Int64 is not subject to the same
      // issues.
      //
      // Note the offsets passed to MWasmBuiltinTruncateToInt32 are wrong here,
      // but it doesn't matter: they're not codegen'd to calls since inputs
      // already are int32.
      auto* lhs2 = createTruncateToInt32(lhs);
      curBlock_->add(lhs2);
      lhs = lhs2;
      auto* rhs2 = createTruncateToInt32(rhs);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    // For x86 and arm we implement i64 div via c++ builtin.
    // A call to c++ builtin requires instance pointer.
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_ARM)
    if (type == MIRType::Int64) {
      auto* ins = MWasmBuiltinDivI64::New(alloc(), lhs, rhs, instancePointer_,
                                          unsignd, trapOnError, trapSiteDesc());
      curBlock_->add(ins);
      return ins;
    }
#endif

    auto* ins = MDiv::New(alloc(), lhs, rhs, type, unsignd, trapOnError,
                          trapSiteDesc(), mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MInstruction* createTruncateToInt32(MDefinition* op) {
    if (op->type() == MIRType::Double || op->type() == MIRType::Float32) {
      return MWasmBuiltinTruncateToInt32::New(alloc(), op, instancePointer_);
    }

    return MTruncateToInt32::New(alloc(), op);
  }

  MDefinition* mod(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   bool unsignd) {
    if (inDeadCode()) {
      return nullptr;
    }
    bool trapOnError = !codeMeta().isAsmJS();
    if (!unsignd && type == MIRType::Int32) {
      // See block comment in div().
      auto* lhs2 = createTruncateToInt32(lhs);
      curBlock_->add(lhs2);
      lhs = lhs2;
      auto* rhs2 = createTruncateToInt32(rhs);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    // For x86 and arm we implement i64 mod via c++ builtin.
    // A call to c++ builtin requires instance pointer.
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_ARM)
    if (type == MIRType::Int64) {
      auto* ins = MWasmBuiltinModI64::New(alloc(), lhs, rhs, instancePointer_,
                                          unsignd, trapOnError, trapSiteDesc());
      curBlock_->add(ins);
      return ins;
    }
#endif

    // Should be handled separately because we call BuiltinThunk for this case
    // and so, need to add the dependency from instancePointer.
    if (type == MIRType::Double) {
      auto* ins = MWasmBuiltinModD::New(alloc(), lhs, rhs, instancePointer_,
                                        type, bytecodeOffset());
      curBlock_->add(ins);
      return ins;
    }

    auto* ins = MMod::New(alloc(), lhs, rhs, type, unsignd, trapOnError,
                          trapSiteDesc());
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* bitnot(MDefinition* op, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MBitNot::New(alloc(), op, type);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* select(MDefinition* trueExpr, MDefinition* falseExpr,
                      MDefinition* condExpr) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MWasmSelect::New(alloc(), trueExpr, falseExpr, condExpr);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* extendI32(MDefinition* op, bool isUnsigned) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MExtendInt32ToInt64::New(alloc(), op, isUnsigned);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* signExtend(MDefinition* op, uint32_t srcSize,
                          uint32_t targetSize) {
    if (inDeadCode()) {
      return nullptr;
    }
    MInstruction* ins;
    switch (targetSize) {
      case 4: {
        MSignExtendInt32::Mode mode;
        switch (srcSize) {
          case 1:
            mode = MSignExtendInt32::Byte;
            break;
          case 2:
            mode = MSignExtendInt32::Half;
            break;
          default:
            MOZ_CRASH("Bad sign extension");
        }
        ins = MSignExtendInt32::New(alloc(), op, mode);
        break;
      }
      case 8: {
        MSignExtendInt64::Mode mode;
        switch (srcSize) {
          case 1:
            mode = MSignExtendInt64::Byte;
            break;
          case 2:
            mode = MSignExtendInt64::Half;
            break;
          case 4:
            mode = MSignExtendInt64::Word;
            break;
          default:
            MOZ_CRASH("Bad sign extension");
        }
        ins = MSignExtendInt64::New(alloc(), op, mode);
        break;
      }
      default: {
        MOZ_CRASH("Bad sign extension");
      }
    }
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* convertI64ToFloatingPoint(MDefinition* op, MIRType type,
                                         bool isUnsigned) {
    if (inDeadCode()) {
      return nullptr;
    }
#if defined(JS_CODEGEN_ARM)
    auto* ins = MBuiltinInt64ToFloatingPoint::New(
        alloc(), op, instancePointer_, type, bytecodeOffset(), isUnsigned);
#else
    auto* ins = MInt64ToFloatingPoint::New(alloc(), op, type, bytecodeOffset(),
                                           isUnsigned);
#endif
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* rotate(MDefinition* input, MDefinition* count, MIRType type,
                      bool left) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MRotate::New(alloc(), input, count, type, left);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* truncate(MDefinition* op, TruncFlags flags) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = T::New(alloc(), op, flags, trapSiteDesc());
    curBlock_->add(ins);
    return ins;
  }

#if defined(JS_CODEGEN_ARM)
  MDefinition* truncateWithInstance(MDefinition* op, TruncFlags flags) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MWasmBuiltinTruncateToInt64::New(alloc(), op, instancePointer_,
                                                 flags, trapSiteDesc());
    curBlock_->add(ins);
    return ins;
  }
#endif

  MDefinition* compare(MDefinition* lhs, MDefinition* rhs, JSOp op,
                       MCompare::CompareType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MCompare::NewWasm(alloc(), lhs, rhs, op, type);
    curBlock_->add(ins);
    return ins;
  }

  void assign(unsigned slot, MDefinition* def) {
    if (inDeadCode()) {
      return;
    }
    curBlock_->setSlot(info().localSlot(slot), def);
  }

  MDefinition* compareIsNull(MDefinition* ref, JSOp compareOp) {
    MDefinition* nullVal = constantNullRef(MaybeRefType());
    if (!nullVal) {
      return nullptr;
    }
    return compare(ref, nullVal, compareOp, MCompare::Compare_WasmAnyRef);
  }

  [[nodiscard]] MDefinition* refAsNonNull(MDefinition* ref) {
    MOZ_ASSERT(!inDeadCode());
    auto* ins = MWasmRefAsNonNull::New(alloc(), ref, trapSiteDesc());
    if (!ins) {
      return nullptr;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] bool brOnNull(uint32_t relativeDepth, const DefVector& values,
                              const ResultType& type, MDefinition* condition) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    MDefinition* check = compareIsNull(condition, JSOp::Eq);
    if (!check) {
      return false;
    }
    MTest* test = MTest::New(alloc(), check, nullptr, fallthroughBlock);
    if (!test ||
        !addControlFlowPatch(test, relativeDepth, MTest::TrueBranchIndex)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = fallthroughBlock;
    return true;
  }

  [[nodiscard]] bool brOnNonNull(uint32_t relativeDepth,
                                 const DefVector& values,
                                 const ResultType& type,
                                 MDefinition* condition) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    MDefinition* check = compareIsNull(condition, JSOp::Ne);
    if (!check) {
      return false;
    }
    MTest* test = MTest::New(alloc(), check, nullptr, fallthroughBlock);
    if (!test ||
        !addControlFlowPatch(test, relativeDepth, MTest::TrueBranchIndex)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = fallthroughBlock;
    return true;
  }

  MDefinition* refI31(MDefinition* input) {
    auto* ins = MWasmNewI31Ref::New(alloc(), input);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* i31Get(MDefinition* input, FieldWideningOp wideningOp) {
    auto* ins = MWasmI31RefGet::New(alloc(), input, wideningOp);
    curBlock_->add(ins);
    return ins;
  }

#ifdef ENABLE_WASM_SIMD
  // About Wasm SIMD as supported by Ion:
  //
  // The expectation is that Ion will only ever support SIMD on x86 and x64,
  // since ARMv7 will cease to be a tier-1 platform soon, and MIPS64 will never
  // implement SIMD.
  //
  // The division of the operations into MIR nodes reflects that expectation,
  // and is a good fit for x86/x64.  Should the expectation change we'll
  // possibly want to re-architect the SIMD support to be a little more general.
  //
  // Most SIMD operations map directly to a single MIR node that ultimately ends
  // up being expanded in the macroassembler.
  //
  // Some SIMD operations that do have a complete macroassembler expansion are
  // open-coded into multiple MIR nodes here; in some cases that's just
  // convenience, in other cases it may also allow them to benefit from Ion
  // optimizations.  The reason for the expansions will be documented by a
  // comment.

  // (v128,v128) -> v128 effect-free binary operations
  MDefinition* binarySimd128(MDefinition* lhs, MDefinition* rhs,
                             bool commutative, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128 &&
               rhs->type() == MIRType::Simd128);

    auto* ins = MWasmBinarySimd128::New(alloc(), lhs, rhs, commutative, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128,i32) -> v128 effect-free shift operations
  MDefinition* shiftSimd128(MDefinition* lhs, MDefinition* rhs, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128 &&
               rhs->type() == MIRType::Int32);

    int32_t maskBits;
    if (MacroAssembler::MustMaskShiftCountSimd128(op, &maskBits)) {
      MDefinition* mask = constantI32(maskBits);
      auto* rhs2 = MBitAnd::New(alloc(), rhs, mask, MIRType::Int32);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    auto* ins = MWasmShiftSimd128::New(alloc(), lhs, rhs, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128,scalar,imm) -> v128
  MDefinition* replaceLaneSimd128(MDefinition* lhs, MDefinition* rhs,
                                  uint32_t laneIndex, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128);

    auto* ins = MWasmReplaceLaneSimd128::New(alloc(), lhs, rhs, laneIndex, op);
    curBlock_->add(ins);
    return ins;
  }

  // (scalar) -> v128 effect-free unary operations
  MDefinition* scalarToSimd128(MDefinition* src, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    auto* ins = MWasmScalarToSimd128::New(alloc(), src, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128) -> v128 effect-free unary operations
  MDefinition* unarySimd128(MDefinition* src, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(src->type() == MIRType::Simd128);
    auto* ins = MWasmUnarySimd128::New(alloc(), src, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, imm) -> scalar effect-free unary operations
  MDefinition* reduceSimd128(MDefinition* src, SimdOp op, ValType outType,
                             uint32_t imm = 0) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(src->type() == MIRType::Simd128);
    auto* ins =
        MWasmReduceSimd128::New(alloc(), src, op, outType.toMIRType(), imm);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, v128, v128) -> v128 effect-free operations
  MDefinition* ternarySimd128(MDefinition* v0, MDefinition* v1, MDefinition* v2,
                              SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(v0->type() == MIRType::Simd128 &&
               v1->type() == MIRType::Simd128 &&
               v2->type() == MIRType::Simd128);

    auto* ins = MWasmTernarySimd128::New(alloc(), v0, v1, v2, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, v128, imm_v128) -> v128 effect-free operations
  MDefinition* shuffleSimd128(MDefinition* v1, MDefinition* v2, V128 control) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(v1->type() == MIRType::Simd128);
    MOZ_ASSERT(v2->type() == MIRType::Simd128);
    auto* ins = BuildWasmShuffleSimd128(
        alloc(), reinterpret_cast<int8_t*>(control.bytes), v1, v2);
    curBlock_->add(ins);
    return ins;
  }

  // Also see below for SIMD memory references

#endif  // ENABLE_WASM_SIMD

  /************************************************ Linear memory accesses */

  // For detailed information about memory accesses, see "Linear memory
  // addresses and bounds checking" in WasmMemory.cpp.

 private:
  // If the platform does not have a HeapReg, load the memory base from
  // instance.
  MDefinition* maybeLoadMemoryBase(uint32_t memoryIndex) {
#ifdef WASM_HAS_HEAPREG
    if (memoryIndex == 0) {
      return nullptr;
    }
#endif
    return memoryBase(memoryIndex);
  }

 public:
  // A value holding the memory base, whether that's HeapReg or some other
  // register.
  MDefinition* memoryBase(uint32_t memoryIndex) {
    AliasSet aliases = !codeMeta().memories[memoryIndex].canMovingGrow()
                           ? AliasSet::None()
                           : AliasSet::Load(AliasSet::WasmHeapMeta);
#ifdef WASM_HAS_HEAPREG
    if (memoryIndex == 0) {
      MWasmHeapReg* base = MWasmHeapReg::New(alloc(), aliases);
      curBlock_->add(base);
      return base;
    }
#endif
    uint32_t offset =
        memoryIndex == 0
            ? Instance::offsetOfMemory0Base()
            : (Instance::offsetInData(
                  codeMeta().offsetOfMemoryInstanceData(memoryIndex) +
                  offsetof(MemoryInstanceData, base)));
    MWasmLoadInstance* base = MWasmLoadInstance::New(
        alloc(), instancePointer_, offset, MIRType::Pointer, aliases);
    curBlock_->add(base);
    return base;
  }

 private:
  // If the bounds checking strategy requires it, load the bounds check limit
  // from the instance.
  MWasmLoadInstance* maybeLoadBoundsCheckLimit(uint32_t memoryIndex,
                                               MIRType type) {
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Int64);
    if (codeMeta().hugeMemoryEnabled(memoryIndex)) {
      return nullptr;
    }
    uint32_t offset =
        memoryIndex == 0
            ? Instance::offsetOfMemory0BoundsCheckLimit()
            : (Instance::offsetInData(
                  codeMeta().offsetOfMemoryInstanceData(memoryIndex) +
                  offsetof(MemoryInstanceData, boundsCheckLimit)));
    AliasSet aliases = !codeMeta().memories[memoryIndex].canMovingGrow()
                           ? AliasSet::None()
                           : AliasSet::Load(AliasSet::WasmHeapMeta);
    auto* load = MWasmLoadInstance::New(alloc(), instancePointer_, offset, type,
                                        aliases);
    curBlock_->add(load);
    return load;
  }

  MDefinition* maybeCanonicalizeNaN(Scalar::Type accessType,
                                    MDefinition* value) {
    MOZ_ASSERT(codeMeta().isAsmJS());

    // Canonicalize floating point values for differential testing.
    if (Scalar::isFloatingType(accessType) &&
        js::SupportDifferentialTesting()) {
      auto* canonicalize = MCanonicalizeNaN::New(alloc(), value);
      curBlock_->add(canonicalize);
      return canonicalize;
    }
    return value;
  }

  // Return true if the access requires an alignment check.  If so, sets
  // *mustAdd to true if the offset must be added to the pointer before
  // checking.
  bool needAlignmentCheck(MemoryAccessDesc* access, MDefinition* base,
                          bool* mustAdd) {
    MOZ_ASSERT(!*mustAdd);

    // asm.js accesses are always aligned and need no checks.
    if (codeMeta().isAsmJS() || !access->isAtomic()) {
      return false;
    }

    // If the EA is known and aligned it will need no checks.
    if (base->isConstant()) {
      // We only care about the low bits, so overflow is OK, as is chopping off
      // the high bits of an i64 pointer.
      uint32_t ptr = 0;
      if (isMem64(access->memoryIndex())) {
        ptr = uint32_t(base->toConstant()->toInt64());
      } else {
        ptr = base->toConstant()->toInt32();
      }
      if (((ptr + access->offset64()) & (access->byteSize() - 1)) == 0) {
        return false;
      }
    }

    // If the offset is aligned then the EA is just the pointer, for
    // the purposes of this check.
    *mustAdd = (access->offset64() & (access->byteSize() - 1)) != 0;
    return true;
  }

  // Fold a constant base into the offset and make the base 0, provided the
  // offset stays below the guard limit.  The reason for folding the base into
  // the offset rather than vice versa is that a small offset can be ignored
  // by both explicit bounds checking and bounds check elimination.
  void foldConstantPointer(MemoryAccessDesc* access, MDefinition** base) {
    uint64_t offsetGuardLimit = GetMaxOffsetGuardLimit(
        codeMeta().hugeMemoryEnabled(access->memoryIndex()));

    if ((*base)->isConstant()) {
      uint64_t basePtr = 0;
      if (isMem64(access->memoryIndex())) {
        basePtr = uint64_t((*base)->toConstant()->toInt64());
      } else {
        basePtr = uint64_t(int64_t((*base)->toConstant()->toInt32()));
      }

      uint64_t offset = access->offset64();
      if (offset < offsetGuardLimit && basePtr < offsetGuardLimit - offset) {
        offset += basePtr;
        access->setOffset32(uint32_t(offset));
        *base = isMem64(access->memoryIndex()) ? constantI64(int64_t(0))
                                               : constantI32(0);
      }
    }
  }

  // If the offset must be added because it is large or because the true EA must
  // be checked, compute the effective address, trapping on overflow.
  void maybeComputeEffectiveAddress(MemoryAccessDesc* access,
                                    MDefinition** base, bool mustAddOffset) {
    uint64_t offsetGuardLimit = GetMaxOffsetGuardLimit(
        codeMeta().hugeMemoryEnabled(access->memoryIndex()));

    if (access->offset64() >= offsetGuardLimit ||
        access->offset64() > UINT32_MAX || mustAddOffset ||
        !JitOptions.wasmFoldOffsets) {
      *base = computeEffectiveAddress(*base, access);
    }
  }

  MWasmLoadInstance* needBoundsCheck(uint32_t memoryIndex) {
#ifdef JS_64BIT
    // For 32-bit base pointers:
    //
    // If the bounds check uses the full 64 bits of the bounds check limit, then
    // the base pointer must be zero-extended to 64 bits before checking and
    // wrapped back to 32-bits after Spectre masking.  (And it's important that
    // the value we end up with has flowed through the Spectre mask.)
    //
    // If the memory's max size is known to be smaller than 64K pages exactly,
    // we can use a 32-bit check and avoid extension and wrapping.
    static_assert(0x100000000 % PageSize == 0);
    bool mem32LimitIs64Bits =
        isMem32(memoryIndex) &&
        !codeMeta().memories[memoryIndex].boundsCheckLimitIs32Bits() &&
        MaxMemoryPages(codeMeta().memories[memoryIndex].addressType()) >=
            Pages(0x100000000 / PageSize);
#else
    // On 32-bit platforms we have no more than 2GB memory and the limit for a
    // 32-bit base pointer is never a 64-bit value.
    bool mem32LimitIs64Bits = false;
#endif
    return maybeLoadBoundsCheckLimit(memoryIndex,
                                     mem32LimitIs64Bits || isMem64(memoryIndex)
                                         ? MIRType::Int64
                                         : MIRType::Int32);
  }

  void performBoundsCheck(uint32_t memoryIndex, MDefinition** base,
                          MWasmLoadInstance* boundsCheckLimit) {
    // At the outset, actualBase could be the result of pretty much any integer
    // operation, or it could be the load of an integer constant.  If its type
    // is i32, we may assume the value has a canonical representation for the
    // platform, see doc block in MacroAssembler.h.
    MDefinition* actualBase = *base;

    // Extend an i32 index value to perform a 64-bit bounds check if the memory
    // can be 4GB or larger.
    bool extendAndWrapIndex =
        isMem32(memoryIndex) && boundsCheckLimit->type() == MIRType::Int64;
    if (extendAndWrapIndex) {
      auto* extended = MWasmExtendU32Index::New(alloc(), actualBase);
      curBlock_->add(extended);
      actualBase = extended;
    }

    auto target = memoryIndex == 0 ? MWasmBoundsCheck::Memory0
                                   : MWasmBoundsCheck::Unknown;
    auto* ins = MWasmBoundsCheck::New(alloc(), actualBase, boundsCheckLimit,
                                      trapSiteDesc(), target);
    curBlock_->add(ins);
    actualBase = ins;

    // If we're masking, then we update *base to create a dependency chain
    // through the masked index.  But we will first need to wrap the index
    // value if it was extended above.
    if (JitOptions.spectreIndexMasking) {
      if (extendAndWrapIndex) {
        auto* wrapped = MWasmWrapU32Index::New(alloc(), actualBase);
        curBlock_->add(wrapped);
        actualBase = wrapped;
      }
      *base = actualBase;
    }
  }

  // Perform all necessary checking before a wasm heap access, based on the
  // attributes of the access and base pointer.
  //
  // For 64-bit indices on platforms that are limited to indices that fit into
  // 32 bits (all 32-bit platforms and mips64), this returns a bounds-checked
  // `base` that has type Int32.  Lowering code depends on this and will assert
  // that the base has this type.  See the end of this function.

  void checkOffsetAndAlignmentAndBounds(MemoryAccessDesc* access,
                                        MDefinition** base) {
    MOZ_ASSERT(!inDeadCode());
    MOZ_ASSERT(!codeMeta().isAsmJS());

    // Attempt to fold a constant base pointer into the offset so as to simplify
    // the addressing expression. This may update *base.
    foldConstantPointer(access, base);

    // Determine whether an alignment check is needed and whether the offset
    // must be checked too.
    bool mustAddOffsetForAlignmentCheck = false;
    bool alignmentCheck =
        needAlignmentCheck(access, *base, &mustAddOffsetForAlignmentCheck);

    // If bounds checking or alignment checking requires it, compute the
    // effective address: add the offset into the pointer and trap on overflow.
    // This may update *base.
    maybeComputeEffectiveAddress(access, base, mustAddOffsetForAlignmentCheck);

    // Emit the alignment check if necessary; it traps if it fails.
    if (alignmentCheck) {
      curBlock_->add(MWasmAlignmentCheck::New(
          alloc(), *base, access->byteSize(), trapSiteDesc()));
    }

    // Emit the bounds check if necessary; it traps if it fails.  This may
    // update *base.
    MWasmLoadInstance* boundsCheckLimit =
        needBoundsCheck(access->memoryIndex());
    if (boundsCheckLimit) {
      performBoundsCheck(access->memoryIndex(), base, boundsCheckLimit);
    }

#ifndef JS_64BIT
    if (isMem64(access->memoryIndex())) {
      // We must have had an explicit bounds check (or one was elided if it was
      // proved redundant), and on 32-bit systems the index will for sure fit in
      // 32 bits: the max memory is 2GB.  So chop the index down to 32-bit to
      // simplify the back-end.
      MOZ_ASSERT((*base)->type() == MIRType::Int64);
      MOZ_ASSERT(!codeMeta().hugeMemoryEnabled(access->memoryIndex()));
      auto* chopped = MWasmWrapU32Index::New(alloc(), *base);
      MOZ_ASSERT(chopped->type() == MIRType::Int32);
      curBlock_->add(chopped);
      *base = chopped;
    }
#endif
  }

  bool isSmallerAccessForI64(ValType result, const MemoryAccessDesc* access) {
    if (result == ValType::I64 && access->byteSize() <= 4) {
      // These smaller accesses should all be zero-extending.
      MOZ_ASSERT(!isSignedIntType(access->type()));
      return true;
    }
    return false;
  }

 public:
  bool isMem32(uint32_t memoryIndex) {
    return codeMeta().memories[memoryIndex].addressType() == AddressType::I32;
  }
  bool isMem64(uint32_t memoryIndex) {
    return codeMeta().memories[memoryIndex].addressType() == AddressType::I64;
  }
  bool hugeMemoryEnabled(uint32_t memoryIndex) {
    return codeMeta().hugeMemoryEnabled(memoryIndex);
  }

  // Add the offset into the pointer to yield the EA; trap on overflow. Clears
  // the offset on the memory access as a result.
  MDefinition* computeEffectiveAddress(MDefinition* base,
                                       MemoryAccessDesc* access) {
    if (inDeadCode()) {
      return nullptr;
    }
    uint64_t offset = access->offset64();
    if (offset == 0) {
      return base;
    }
    auto* ins = MWasmAddOffset::New(alloc(), base, offset, trapSiteDesc());
    curBlock_->add(ins);
    access->clearOffset();
    return ins;
  }

  MDefinition* load(MDefinition* base, MemoryAccessDesc* access,
                    ValType result) {
    if (inDeadCode()) {
      return nullptr;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* load = nullptr;
    if (codeMeta().isAsmJS()) {
      MOZ_ASSERT(access->offset64() == 0);
      MWasmLoadInstance* boundsCheckLimit =
          maybeLoadBoundsCheckLimit(access->memoryIndex(), MIRType::Int32);
      load = MAsmJSLoadHeap::New(alloc(), memoryBase, base, boundsCheckLimit,
                                 access->type());
    } else {
      checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
      MOZ_ASSERT(base->type() == MIRType::Int32);
#endif
      load = MWasmLoad::New(alloc(), memoryBase, base, *access,
                            result.toMIRType());
    }
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  void store(MDefinition* base, MemoryAccessDesc* access, MDefinition* v) {
    if (inDeadCode()) {
      return;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* store = nullptr;
    if (codeMeta().isAsmJS()) {
      MOZ_ASSERT(access->offset64() == 0);
      MWasmLoadInstance* boundsCheckLimit =
          maybeLoadBoundsCheckLimit(access->memoryIndex(), MIRType::Int32);
      v = maybeCanonicalizeNaN(access->type(), v);
      store = MAsmJSStoreHeap::New(alloc(), memoryBase, base, boundsCheckLimit,
                                   access->type(), v);
    } else {
      checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
      MOZ_ASSERT(base->type() == MIRType::Int32);
#endif
      store = MWasmStore::New(alloc(), memoryBase, base, *access, v);
    }
    if (!store) {
      return;
    }
    curBlock_->add(store);
  }

  MDefinition* atomicCompareExchangeHeap(MDefinition* base,
                                         MemoryAccessDesc* access,
                                         ValType result, MDefinition* oldv,
                                         MDefinition* newv) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtOldv =
          MWrapInt64ToInt32::New(alloc(), oldv, /*bottomHalf=*/true);
      curBlock_->add(cvtOldv);
      oldv = cvtOldv;

      auto* cvtNewv =
          MWrapInt64ToInt32::New(alloc(), newv, /*bottomHalf=*/true);
      curBlock_->add(cvtNewv);
      newv = cvtNewv;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* cas = MWasmCompareExchangeHeap::New(
        alloc(), bytecodeOffset(), memoryBase, base, *access, oldv, newv,
        instancePointer_);
    if (!cas) {
      return nullptr;
    }
    curBlock_->add(cas);

    if (isSmallerAccessForI64(result, access)) {
      cas = MExtendInt32ToInt64::New(alloc(), cas, true);
      curBlock_->add(cas);
    }

    return cas;
  }

  MDefinition* atomicExchangeHeap(MDefinition* base, MemoryAccessDesc* access,
                                  ValType result, MDefinition* value) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtValue =
          MWrapInt64ToInt32::New(alloc(), value, /*bottomHalf=*/true);
      curBlock_->add(cvtValue);
      value = cvtValue;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* xchg =
        MWasmAtomicExchangeHeap::New(alloc(), bytecodeOffset(), memoryBase,
                                     base, *access, value, instancePointer_);
    if (!xchg) {
      return nullptr;
    }
    curBlock_->add(xchg);

    if (isSmallerAccessForI64(result, access)) {
      xchg = MExtendInt32ToInt64::New(alloc(), xchg, true);
      curBlock_->add(xchg);
    }

    return xchg;
  }

  MDefinition* atomicBinopHeap(AtomicOp op, MDefinition* base,
                               MemoryAccessDesc* access, ValType result,
                               MDefinition* value) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtValue =
          MWrapInt64ToInt32::New(alloc(), value, /*bottomHalf=*/true);
      curBlock_->add(cvtValue);
      value = cvtValue;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* binop =
        MWasmAtomicBinopHeap::New(alloc(), bytecodeOffset(), op, memoryBase,
                                  base, *access, value, instancePointer_);
    if (!binop) {
      return nullptr;
    }
    curBlock_->add(binop);

    if (isSmallerAccessForI64(result, access)) {
      binop = MExtendInt32ToInt64::New(alloc(), binop, true);
      curBlock_->add(binop);
    }

    return binop;
  }

#ifdef ENABLE_WASM_SIMD
  MDefinition* loadSplatSimd128(Scalar::Type viewType,
                                const LinearMemoryAddress<MDefinition*>& addr,
                                wasm::SimdOp splatOp) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                            trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));

    // Generate better code (on x86)
    // If AVX2 is enabled, more broadcast operators are available.
    if (viewType == Scalar::Float64
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        || (js::jit::CPUInfo::IsAVX2Present() &&
            (viewType == Scalar::Uint8 || viewType == Scalar::Uint16 ||
             viewType == Scalar::Float32))
#  endif
    ) {
      access.setSplatSimd128Load();
      return load(addr.base, &access, ValType::V128);
    }

    ValType resultType = ValType::I32;
    if (viewType == Scalar::Float32) {
      resultType = ValType::F32;
      splatOp = wasm::SimdOp::F32x4Splat;
    }
    auto* scalar = load(addr.base, &access, resultType);
    if (!inDeadCode() && !scalar) {
      return nullptr;
    }
    return scalarToSimd128(scalar, splatOp);
  }

  MDefinition* loadExtendSimd128(const LinearMemoryAddress<MDefinition*>& addr,
                                 wasm::SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    // Generate better code (on x86) by loading as a double with an
    // operation that sign extends directly.
    MemoryAccessDesc access(addr.memoryIndex, Scalar::Float64, addr.align,
                            addr.offset, trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    access.setWidenSimd128Load(op);
    return load(addr.base, &access, ValType::V128);
  }

  MDefinition* loadZeroSimd128(Scalar::Type viewType, size_t numBytes,
                               const LinearMemoryAddress<MDefinition*>& addr) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                            trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    access.setZeroExtendSimd128Load();
    return load(addr.base, &access, ValType::V128);
  }

  MDefinition* loadLaneSimd128(uint32_t laneSize,
                               const LinearMemoryAddress<MDefinition*>& addr,
                               uint32_t laneIndex, MDefinition* src) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(addr.memoryIndex, Scalar::Simd128, addr.align,
                            addr.offset, trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    MDefinition* memoryBase = maybeLoadMemoryBase(access.memoryIndex());
    MDefinition* base = addr.base;
    MOZ_ASSERT(!codeMeta().isAsmJS());
    checkOffsetAndAlignmentAndBounds(&access, &base);
#  ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#  endif
    MInstruction* load = MWasmLoadLaneSimd128::New(
        alloc(), memoryBase, base, access, laneSize, laneIndex, src);
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  void storeLaneSimd128(uint32_t laneSize,
                        const LinearMemoryAddress<MDefinition*>& addr,
                        uint32_t laneIndex, MDefinition* src) {
    if (inDeadCode()) {
      return;
    }
    MemoryAccessDesc access(addr.memoryIndex, Scalar::Simd128, addr.align,
                            addr.offset, trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    MDefinition* memoryBase = maybeLoadMemoryBase(access.memoryIndex());
    MDefinition* base = addr.base;
    MOZ_ASSERT(!codeMeta().isAsmJS());
    checkOffsetAndAlignmentAndBounds(&access, &base);
#  ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#  endif
    MInstruction* store = MWasmStoreLaneSimd128::New(
        alloc(), memoryBase, base, access, laneSize, laneIndex, src);
    if (!store) {
      return;
    }
    curBlock_->add(store);
  }
#endif  // ENABLE_WASM_SIMD

  /************************************************ Global variable accesses */

  MDefinition* loadGlobalVar(const GlobalDesc& global) {
    if (inDeadCode()) {
      return nullptr;
    }

    MInstruction* load;
    if (global.isIndirect()) {
      // Pull a pointer to the value out of Instance::globalArea, then
      // load from that pointer.  Note that the pointer is immutable
      // even though the value it points at may change, hence the use of
      // |true| for the first node's |isConst| value, irrespective of
      // the |isConst| formal parameter to this method.  The latter
      // applies to the denoted value as a whole.
      auto* cellPtr = MWasmLoadInstanceDataField::New(
          alloc(), MIRType::Pointer, global.offset(),
          /*isConst=*/true, instancePointer_);
      curBlock_->add(cellPtr);
      load = MWasmLoadGlobalCell::New(alloc(), global.type().toMIRType(),
                                      cellPtr, global.type());
    } else {
      // Pull the value directly out of Instance::globalArea.
      load = MWasmLoadInstanceDataField::New(
          alloc(), global.type().toMIRType(), global.offset(),
          !global.isMutable(), instancePointer_,
          global.type().toMaybeRefType());
    }
    curBlock_->add(load);
    return load;
  }

  [[nodiscard]] bool storeGlobalVar(uint32_t lineOrBytecode,
                                    const GlobalDesc& global, MDefinition* v) {
    if (inDeadCode()) {
      return true;
    }

    if (global.isIndirect()) {
      // Pull a pointer to the value out of Instance::globalArea, then
      // store through that pointer.
      auto* valueAddr = MWasmLoadInstanceDataField::New(
          alloc(), MIRType::Pointer, global.offset(),
          /*isConstant=*/true, instancePointer_);
      curBlock_->add(valueAddr);

      // Handle a store to a ref-typed field specially
      if (global.type().toMIRType() == MIRType::WasmAnyRef) {
        // Load the previous value for the post-write barrier
        MOZ_ASSERT(v->type() == MIRType::WasmAnyRef);
        auto* prevValue = MWasmLoadGlobalCell::New(alloc(), MIRType::WasmAnyRef,
                                                   valueAddr, global.type());
        curBlock_->add(prevValue);

        // Store the new value
        auto* store =
            MWasmStoreRef::New(alloc(), instancePointer_, valueAddr,
                               /*valueOffset=*/0, v, AliasSet::WasmGlobalCell,
                               WasmPreBarrierKind::Normal);
        curBlock_->add(store);

        // Call the post-write barrier
        return postBarrierEdgePrecise(lineOrBytecode, valueAddr, prevValue);
      }

      auto* store = MWasmStoreGlobalCell::New(alloc(), v, valueAddr);
      curBlock_->add(store);
      return true;
    }
    // Or else store the value directly in Instance::globalArea.

    // Handle a store to a ref-typed field specially
    if (global.type().toMIRType() == MIRType::WasmAnyRef) {
      // Compute the address of the ref-typed global
      auto* valueAddr = MWasmDerivedPointer::New(
          alloc(), instancePointer_,
          wasm::Instance::offsetInData(global.offset()));
      curBlock_->add(valueAddr);

      // Load the previous value for the post-write barrier
      MOZ_ASSERT(v->type() == MIRType::WasmAnyRef);
      auto* prevValue = MWasmLoadGlobalCell::New(alloc(), MIRType::WasmAnyRef,
                                                 valueAddr, global.type());
      curBlock_->add(prevValue);

      // Store the new value
      auto* store =
          MWasmStoreRef::New(alloc(), instancePointer_, valueAddr,
                             /*valueOffset=*/0, v, AliasSet::WasmInstanceData,
                             WasmPreBarrierKind::Normal);
      curBlock_->add(store);

      // Call the post-write barrier
      return postBarrierEdgePrecise(lineOrBytecode, valueAddr, prevValue);
    }

    auto* store = MWasmStoreInstanceDataField::New(alloc(), global.offset(), v,
                                                   instancePointer_);
    curBlock_->add(store);
    return true;
  }

  // Load the slot on the instance where the result of `ref.func` is cached.
  // This may be null if a function reference for this function has not been
  // asked for yet.
  MDefinition* loadCachedRefFunc(uint32_t funcIndex) {
    uint32_t exportedFuncIndex = codeMeta().findFuncExportIndex(funcIndex);
    MWasmLoadInstanceDataField* refFunc = MWasmLoadInstanceDataField::New(
        alloc(), MIRType::WasmAnyRef,
        codeMeta().offsetOfFuncExportInstanceData(exportedFuncIndex) +
            offsetof(FuncExportInstanceData, func),
        true, instancePointer_);
    curBlock_->add(refFunc);
    return refFunc;
  }

  MDefinition* loadTableField(uint32_t tableIndex, unsigned fieldOffset,
                              MIRType type) {
    uint32_t instanceDataOffset = wasm::Instance::offsetInData(
        codeMeta().offsetOfTableInstanceData(tableIndex) + fieldOffset);
    auto* load =
        MWasmLoadInstance::New(alloc(), instancePointer_, instanceDataOffset,
                               type, AliasSet::Load(AliasSet::WasmTableMeta));
    curBlock_->add(load);
    return load;
  }

  MDefinition* loadTableLength(uint32_t tableIndex) {
    return loadTableField(tableIndex, offsetof(TableInstanceData, length),
                          MIRType::Int32);
  }

  MDefinition* loadTableElements(uint32_t tableIndex) {
    return loadTableField(tableIndex, offsetof(TableInstanceData, elements),
                          MIRType::Pointer);
  }

  MDefinition* tableAddressToI32(AddressType addressType,
                                 MDefinition* address) {
    switch (addressType) {
      case AddressType::I32:
        return address;
      case AddressType::I64:
        auto* clamp = MWasmClampTable64Address::New(alloc(), address);
        if (!clamp) {
          return nullptr;
        }
        curBlock_->add(clamp);
        return clamp;
    }
    MOZ_CRASH("unknown address type");
  }

  MDefinition* tableGetAnyRef(uint32_t tableIndex, MDefinition* address) {
    const TableDesc& table = codeMeta().tables[tableIndex];

    // Load the table length and perform a bounds check with spectre index
    // masking
    auto* length = loadTableLength(tableIndex);
    auto* check = MWasmBoundsCheck::New(
        alloc(), address, length, trapSiteDesc(), MWasmBoundsCheck::Unknown);
    curBlock_->add(check);
    if (JitOptions.spectreIndexMasking) {
      address = check;
    }

    // Load the table elements and load the element
    auto* elements = loadTableElements(tableIndex);
    auto* element =
        MWasmLoadTableElement::New(alloc(), elements, address, table.elemType);
    curBlock_->add(element);
    return element;
  }

  [[nodiscard]] bool tableSetAnyRef(uint32_t tableIndex, MDefinition* address,
                                    MDefinition* value,
                                    uint32_t lineOrBytecode) {
    const TableDesc& table = codeMeta().tables[tableIndex];

    // Load the table length and perform a bounds check with spectre index
    // masking
    auto* length = loadTableLength(tableIndex);
    auto* check = MWasmBoundsCheck::New(
        alloc(), address, length, trapSiteDesc(), MWasmBoundsCheck::Unknown);
    curBlock_->add(check);
    if (JitOptions.spectreIndexMasking) {
      address = check;
    }

    // Load the table elements
    auto* elements = loadTableElements(tableIndex);

    // Load the previous value
    auto* prevValue =
        MWasmLoadTableElement::New(alloc(), elements, address, table.elemType);
    curBlock_->add(prevValue);

    // Compute the value's location for the post barrier
    auto* loc =
        MWasmDerivedIndexPointer::New(alloc(), elements, address, ScalePointer);
    curBlock_->add(loc);

    // Store the new value
    auto* store = MWasmStoreRef::New(
        alloc(), instancePointer_, loc, /*valueOffset=*/0, value,
        AliasSet::WasmTableElement, WasmPreBarrierKind::Normal);
    curBlock_->add(store);

    // Perform the post barrier
    return postBarrierEdgePrecise(lineOrBytecode, loc, prevValue);
  }

  void addInterruptCheck() {
    if (inDeadCode()) {
      return;
    }
    curBlock_->add(
        MWasmInterruptCheck::New(alloc(), instancePointer_, trapSiteDesc()));
  }

  // Perform a post-write barrier to update the generational store buffer. This
  // version stores the entire containing object (e.g. a struct) rather than a
  // single edge.
  [[nodiscard]] bool postBarrierWholeCell(uint32_t lineOrBytecode,
                                          MDefinition* object,
                                          MDefinition* newValue) {
    auto* barrier = MWasmPostWriteBarrierWholeCell::New(
        alloc(), instancePointer_, object, newValue);
    if (!barrier) {
      return false;
    }
    curBlock_->add(barrier);
    return true;
  }

  // Perform a post-write barrier to update the generational store buffer. This
  // version tracks a single tenured -> nursery edge, and will remove a previous
  // store buffer entry if it is no longer needed.
  [[nodiscard]] bool postBarrierEdgePrecise(uint32_t lineOrBytecode,
                                            MDefinition* valueAddr,
                                            MDefinition* value) {
    return emitInstanceCall2(lineOrBytecode, SASigPostBarrierEdgePrecise,
                             valueAddr, value);
  }

  // Perform a post-write barrier to update the generational store buffer. This
  // version does not remove a previous store buffer entry if it is no longer
  // needed.
  [[nodiscard]] bool postBarrierEdgeAtIndex(uint32_t lineOrBytecode,
                                            MDefinition* object,
                                            MDefinition* valueBase,
                                            MDefinition* index, uint32_t scale,
                                            MDefinition* newValue) {
    auto* barrier = MWasmPostWriteBarrierEdgeAtIndex::New(
        alloc(), instancePointer_, object, valueBase, index, scale, newValue);
    if (!barrier) {
      return false;
    }
    curBlock_->add(barrier);
    return true;
  }

  /***************************************************************** Calls */

  // The IonMonkey backend maintains a single stack offset (from the stack
  // pointer to the base of the frame) by adding the total amount of spill
  // space required plus the maximum stack required for argument passing.
  // Since we do not use IonMonkey's MPrepareCall/MPassArg/MCall, we must
  // manually accumulate, for the entire function, the maximum required stack
  // space for argument passing. (This is passed to the CodeGenerator via
  // MIRGenerator::maxWasmStackArgBytes.) This is just be the maximum of the
  // stack space required for each individual call (as determined by the call
  // ABI).

  [[nodiscard]]
  bool passInstanceCallArg(MIRType instanceType, CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }

    // Should only pass an instance once.  And it must be a non-GC pointer.
    MOZ_ASSERT(callState->instanceArg == ABIArg());
    MOZ_ASSERT(instanceType == MIRType::Pointer);
    callState->instanceArg = callState->abi.next(MIRType::Pointer);
    return true;
  }

  // Do not call this directly.  Call one of the passCallArg() variants instead.
  [[nodiscard]]
  bool passCallArgWorker(MDefinition* argDef, MIRType type,
                         CallCompileState* callState) {
    MOZ_ASSERT(argDef->type() == type);

    ABIArg arg = callState->abi.next(type);
    switch (arg.kind()) {
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR: {
        auto mirLow =
            MWrapInt64ToInt32::New(alloc(), argDef, /* bottomHalf = */ true);
        curBlock_->add(mirLow);
        auto mirHigh =
            MWrapInt64ToInt32::New(alloc(), argDef, /* bottomHalf = */ false);
        curBlock_->add(mirHigh);
        return callState->regArgs.append(
                   MWasmCallBase::Arg(AnyRegister(arg.gpr64().low), mirLow)) &&
               callState->regArgs.append(
                   MWasmCallBase::Arg(AnyRegister(arg.gpr64().high), mirHigh));
      }
#endif
      case ABIArg::GPR:
      case ABIArg::FPU:
        return callState->regArgs.append(MWasmCallBase::Arg(arg.reg(), argDef));
      case ABIArg::Stack: {
        auto* mir =
            MWasmStackArg::New(alloc(), arg.offsetFromArgBase(), argDef);
        curBlock_->add(mir);
        return true;
      }
      case ABIArg::Uninitialized:
        MOZ_ASSERT_UNREACHABLE("Uninitialized ABIArg kind");
    }
    MOZ_CRASH("Unknown ABIArg kind.");
  }

  template <typename VecT>
  [[nodiscard]]
  bool passCallArgs(const DefVector& argDefs, const VecT& types,
                    CallCompileState* callState) {
    MOZ_ASSERT(argDefs.length() == types.length());
    for (uint32_t i = 0; i < argDefs.length(); i++) {
      MDefinition* def = argDefs[i];
      ValType type = types[i];
      if (!passCallArg(def, type, callState)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]]
  bool passCallArg(MDefinition* argDef, MIRType type,
                   CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }
    return passCallArgWorker(argDef, type, callState);
  }

  [[nodiscard]]
  bool passCallArg(MDefinition* argDef, ValType type,
                   CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }
    return passCallArgWorker(argDef, type.toMIRType(), callState);
  }

  // If the call returns results on the stack, prepare a stack area to receive
  // them, and pass the address of the stack area to the callee as an additional
  // argument.
  [[nodiscard]]
  bool passStackResultAreaCallArg(const ResultType& resultType,
                                  CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }
    ABIResultIter iter(resultType);
    while (!iter.done() && iter.cur().inRegister()) {
      iter.next();
    }
    if (iter.done()) {
      // No stack results.
      return true;
    }

    auto* stackResultArea = MWasmStackResultArea::New(alloc());
    if (!stackResultArea) {
      return false;
    }
    if (!stackResultArea->init(alloc(), iter.remaining())) {
      return false;
    }
    for (uint32_t base = iter.index(); !iter.done(); iter.next()) {
      MWasmStackResultArea::StackResult loc(iter.cur().stackOffset(),
                                            iter.cur().type().toMIRType());
      stackResultArea->initResult(iter.index() - base, loc);
    }
    curBlock_->add(stackResultArea);
    MDefinition* def = callState->returnCall ? (MDefinition*)stackResultPointer_
                                             : (MDefinition*)stackResultArea;
    if (!passCallArg(def, MIRType::StackResults, callState)) {
      return false;
    }
    callState->stackResultArea = stackResultArea;
    return true;
  }

  [[nodiscard]]
  bool finishCallArgs(CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }

    if (!callState->regArgs.append(
            MWasmCallBase::Arg(AnyRegister(InstanceReg), instancePointer_))) {
      return false;
    }

    uint32_t stackBytes = callState->abi.stackBytesConsumedSoFar();

    maxStackArgBytes_ = std::max(maxStackArgBytes_, stackBytes);
    return true;
  }

  [[nodiscard]]
  bool emitCallArgs(const FuncType& funcType, const DefVector& args,
                    CallCompileState* callState) {
    for (size_t i = 0, n = funcType.args().length(); i < n; ++i) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      if (!passCallArg(args[i], funcType.args()[i], callState)) {
        return false;
      }
    }

    ResultType resultType = ResultType::Vector(funcType.results());
    if (!passStackResultAreaCallArg(resultType, callState)) {
      return false;
    }

    return finishCallArgs(callState);
  }

  [[nodiscard]]
  bool collectUnaryCallResult(MIRType type, MDefinition** result) {
    MInstruction* def;
    switch (type) {
      case MIRType::Int32:
        def = MWasmRegisterResult::New(alloc(), MIRType::Int32, ReturnReg);
        break;
      case MIRType::Int64:
        def = MWasmRegister64Result::New(alloc(), ReturnReg64);
        break;
      case MIRType::Float32:
        def = MWasmFloatRegisterResult::New(alloc(), type, ReturnFloat32Reg);
        break;
      case MIRType::Double:
        def = MWasmFloatRegisterResult::New(alloc(), type, ReturnDoubleReg);
        break;
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
        def = MWasmFloatRegisterResult::New(alloc(), type, ReturnSimd128Reg);
        break;
#endif
      case MIRType::WasmAnyRef:
        def = MWasmRegisterResult::New(alloc(), MIRType::WasmAnyRef, ReturnReg);
        break;
      case MIRType::None:
        MOZ_ASSERT(result == nullptr, "Not expecting any results created");
        return true;
      default:
        MOZ_CRASH("unexpected MIRType result for builtin call");
    }

    if (!def) {
      return false;
    }

    curBlock_->add(def);
    *result = def;

    return true;
  }

  [[nodiscard]]
  bool collectCallResults(const ResultType& type,
                          MWasmStackResultArea* stackResultArea,
                          DefVector* results) {
    if (!results->reserve(type.length())) {
      return false;
    }

    // The result iterator goes in the order in which results would be popped
    // off; we want the order in which they would be pushed.
    ABIResultIter iter(type);
    uint32_t stackResultCount = 0;
    while (!iter.done()) {
      if (iter.cur().onStack()) {
        stackResultCount++;
      }
      iter.next();
    }

    for (iter.switchToPrev(); !iter.done(); iter.prev()) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      const ABIResult& result = iter.cur();
      MInstruction* def;
      if (result.inRegister()) {
        switch (result.type().kind()) {
          case wasm::ValType::I32:
            def =
                MWasmRegisterResult::New(alloc(), MIRType::Int32, result.gpr());
            break;
          case wasm::ValType::I64:
            def = MWasmRegister64Result::New(alloc(), result.gpr64());
            break;
          case wasm::ValType::F32:
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Float32,
                                                result.fpr());
            break;
          case wasm::ValType::F64:
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Double,
                                                result.fpr());
            break;
          case wasm::ValType::Ref:
            def = MWasmRegisterResult::New(alloc(), MIRType::WasmAnyRef,
                                           result.gpr(),
                                           result.type().toMaybeRefType());
            break;
          case wasm::ValType::V128:
#ifdef ENABLE_WASM_SIMD
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Simd128,
                                                result.fpr());
#else
            return this->iter().fail("Ion has no SIMD support yet");
#endif
        }
      } else {
        MOZ_ASSERT(stackResultArea);
        MOZ_ASSERT(stackResultCount);
        uint32_t idx = --stackResultCount;
        def = MWasmStackResult::New(alloc(), stackResultArea, idx);
      }

      if (!def) {
        return false;
      }
      curBlock_->add(def);
      results->infallibleAppend(def);
    }

    MOZ_ASSERT(results->length() == type.length());

    return true;
  }

  [[nodiscard]]
  bool call(CallCompileState* callState, const CallSiteDesc& desc,
            const CalleeDesc& callee, const ArgTypeVector& argTypes,
            MDefinition* addressOrRef = nullptr) {
    if (!beginCatchableCall(callState)) {
      return false;
    }

    MInstruction* ins;
    if (callState->isCatchable()) {
      ins = MWasmCallCatchable::New(
          alloc(), desc, callee, callState->regArgs,
          StackArgAreaSizeUnaligned(argTypes), callState->tryNoteIndex,
          callState->fallthroughBlock, callState->prePadBlock, addressOrRef);
    } else {
      ins = MWasmCallUncatchable::New(alloc(), desc, callee, callState->regArgs,
                                      StackArgAreaSizeUnaligned(argTypes),
                                      addressOrRef);
    }
    if (!ins) {
      return false;
    }
    curBlock_->add(ins);

    return finishCatchableCall(callState);
  }

  [[nodiscard]]
  CallRefHint auditInlineableCallees(InliningHeuristics::CallKind kind,
                                     CallRefHint hints) {
    // Takes candidates for inlining as provided in `hints`, and returns a
    // subset (or all) of them for which inlining is approved.  To indicate
    // that they are all disallowed, return an empty CallRefHint.

    MOZ_ASSERT_IF(kind == InliningHeuristics::CallKind::Direct,
                  hints.length() == 1);

    // We only support inlining when lazy tiering. This is currently a
    // requirement because we need the full module bytecode and function
    // definition ranges, which are not available in other modes.
    if (compilerEnv().mode() != CompileMode::LazyTiering) {
      return CallRefHint();
    }

    // We don't support asm.js and inlining. asm.js also doesn't support
    // baseline, which is required for lazy tiering, so we should never get
    // here. The biggest complication for asm.js is getting correct stack
    // traces with inlining.
    MOZ_ASSERT(!codeMeta().isAsmJS());

    // If we were given no candidates, give up now.
    if (hints.empty()) {
      return CallRefHint();
    }

    // We can't inline if we've exceeded our per-root-function inlining
    // budget.
    //
    // This logic will cause `availableBudget` to be driven slightly negative
    // if a budget overshoot happens, so we will have performed slightly more
    // inlining than allowed by the initial setting of `availableBudget`.  The
    // size of this overshoot is however very limited -- it can't exceed the
    // size of three function bodies that are inlined (3 because that's what
    // CallRefHint can hold).  And the max size of an inlineable function body
    // is limited by InliningHeuristics::isSmallEnoughToInline.
    if (rootCompiler_.inliningBudget() < 0) {
      return CallRefHint();
    }

    // Check each candidate in turn, and add all acceptable ones to `filtered`.
    // It is important that `filtered` retains the same ordering as `hints`.
    CallRefHint filtered;
    for (uint32_t i = 0; i < hints.length(); i++) {
      uint32_t funcIndex = hints.get(i);

      // We can't inline an imported function.
      if (codeMeta().funcIsImport(funcIndex)) {
        continue;
      }

      // We do not support inlining a callee which uses tail calls
      FeatureUsage funcFeatureUsage =
          codeTailMeta()->funcDefFeatureUsage(funcIndex);
      if (funcFeatureUsage & FeatureUsage::ReturnCall) {
        continue;
      }

      // Ask the heuristics system if we're allowed to inline a function of
      // this size and kind at the current inlining depth.
      uint32_t inlineeBodySize = codeTailMeta()->funcDefRange(funcIndex).size();
      uint32_t rootFunctionBodySize = rootCompiler_.func().bytecodeSize();
      bool largeFunctionBackoff;
      bool smallEnough = InliningHeuristics::isSmallEnoughToInline(
          kind, inliningDepth(), inlineeBodySize, rootFunctionBodySize,
          &largeFunctionBackoff);
      if (largeFunctionBackoff) {
        rootCompiler_.noteLargeFunctionBackoffWasApplied();
      }
      if (!smallEnough) {
        continue;
      }

      filtered.append(funcIndex);
    }

    // Whatever ends up in `filtered` is approved for inlining.
    return filtered;
  }

  [[nodiscard]]
  bool finishInlinedCallDirect(FunctionCompiler& calleeCompiler,
                               DefVector* results) {
    const PendingInlineReturnVector& calleeReturns =
        calleeCompiler.pendingInlineReturns_;
    MBasicBlock* calleeCatchBlock = calleeCompiler.pendingInlineCatchBlock_;
    const FuncType& calleeFuncType = calleeCompiler.funcType();
    MBasicBlock* lastBlockBeforeCall = curBlock_;

    // Add the observed features from the inlined function to this function
    iter_.addFeatureUsage(calleeCompiler.featureUsage());

    // Create a block, if needed, to handle exceptions from the callee function
    if (calleeCatchBlock) {
      ControlInstructionVector* tryLandingPadPatches;
      bool inTryCode = inTryBlock(&tryLandingPadPatches);

      // The callee compiler should never create a catch block unless we have
      // a landing pad for it
      MOZ_RELEASE_ASSERT(inTryCode);

      // Create a block in our function to jump to the nearest try block. We
      // cannot just use the callee's catch block for this, as the slots on it
      // are set up for all the locals from that function. We need to create a
      // new block in our function with the slots for this function, that then
      // does the jump to the landing pad. Ion should be able to optimize this
      // away using jump threading.
      MBasicBlock* callerCatchBlock = nullptr;
      if (!newBlock(nullptr, &callerCatchBlock)) {
        return false;
      }

      // Our catch block inherits all of the locals state from immediately
      // before the inlined call
      callerCatchBlock->inheritSlots(lastBlockBeforeCall);

      // The callee catch block jumps to our catch block
      calleeCatchBlock->end(MGoto::New(alloc(), callerCatchBlock));

      // Our catch block has the callee rethrow block as a predecessor, but
      // ignores all phi's, because we use our own locals state.
      if (!callerCatchBlock->addPredecessorWithoutPhis(calleeCatchBlock)) {
        return false;
      }

      // Our catch block ends with a patch to jump to the enclosing try block.
      MBasicBlock* prevBlock = curBlock_;
      curBlock_ = callerCatchBlock;
      if (!endWithPadPatch(tryLandingPadPatches)) {
        return false;
      }
      curBlock_ = prevBlock;
    }

    // If there were no returns, then we are now in dead code
    if (calleeReturns.empty()) {
      curBlock_ = nullptr;
      return true;
    }

    // Create a block to join all of the returns from the inlined function
    MBasicBlock* joinAfterCall = nullptr;
    if (!newBlock(nullptr, &joinAfterCall)) {
      return false;
    }

    // The join block inherits all of the locals state from immediately before
    // the inlined call
    joinAfterCall->inheritSlots(lastBlockBeforeCall);

    // The join block has a phi node for every result of the inlined function
    // type. Each phi node has an operand for each of the returns of the
    // inlined function.
    for (uint32_t i = 0; i < calleeFuncType.results().length(); i++) {
      MPhi* phi = MPhi::New(alloc(), calleeFuncType.results()[i].toMIRType());
      if (!phi || !phi->reserveLength(calleeReturns.length())) {
        return false;
      }
      joinAfterCall->addPhi(phi);
      if (!results->append(phi)) {
        return false;
      }
    }

    // Bind every return from the inlined function to go to the join block, and
    // add the results for the return to the phi nodes.
    for (size_t i = 0; i < calleeReturns.length(); i++) {
      const PendingInlineReturn& calleeReturn = calleeReturns[i];

      // Setup the predecessor and successor relationship
      MBasicBlock* pred = calleeReturn.jump->block();
      if (!joinAfterCall->addPredecessorWithoutPhis(pred)) {
        return false;
      }
      calleeReturn.jump->replaceSuccessor(MGoto::TargetIndex, joinAfterCall);

      // For each result in this return, add it to the corresponding phi node
      for (uint32_t resultIndex = 0;
           resultIndex < calleeFuncType.results().length(); resultIndex++) {
        MDefinition* result = (*results)[resultIndex];
        ((MPhi*)(result))->addInput(calleeReturn.results[resultIndex]);
      }
    }

    // Continue MIR generation starting in the join block
    curBlock_ = joinAfterCall;

    return true;
  }

  [[nodiscard]]
  bool callDirect(const FuncType& funcType, uint32_t funcIndex,
                  uint32_t lineOrBytecode, const DefVector& args,
                  DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState;
    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Func);
    ResultType resultType = ResultType::Vector(funcType.results());
    auto callee = CalleeDesc::function(funcIndex);
    ArgTypeVector argTypes(funcType);

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes) &&
           collectCallResults(resultType, callState.stackResultArea, results);
  }

  [[nodiscard]]
  bool returnCallDirect(const FuncType& funcType, uint32_t funcIndex,
                        uint32_t lineOrBytecode, const DefVector& args,
                        DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    // We do not support tail calls in inlined functions.
    MOZ_RELEASE_ASSERT(!isInlined());

    CallCompileState callState;
    callState.returnCall = true;
    CallSiteDesc desc(lineOrBytecode, CallSiteKind::ReturnFunc);
    auto callee = CalleeDesc::function(funcIndex);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    auto ins =
        MWasmReturnCall::New(alloc(), desc, callee, callState.regArgs,
                             StackArgAreaSizeUnaligned(argTypes), nullptr);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]]
  bool returnCallImport(unsigned globalDataOffset, uint32_t lineOrBytecode,
                        const FuncType& funcType, const DefVector& args,
                        DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    // We do not support tail calls in inlined functions.
    MOZ_RELEASE_ASSERT(!isInlined());

    CallCompileState callState;
    callState.returnCall = true;
    CallSiteDesc desc(lineOrBytecode, CallSiteKind::Import);
    auto callee = CalleeDesc::import(globalDataOffset);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    auto* ins =
        MWasmReturnCall::New(alloc(), desc, callee, callState.regArgs,
                             StackArgAreaSizeUnaligned(argTypes), nullptr);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]]
  bool returnCallIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                          MDefinition* address, uint32_t lineOrBytecode,
                          const DefVector& args, DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    // We do not support tail calls in inlined functions.
    MOZ_RELEASE_ASSERT(!isInlined());

    const FuncType& funcType = (*codeMeta().types)[funcTypeIndex].funcType();
    CallIndirectId callIndirectId =
        CallIndirectId::forFuncType(codeMeta(), funcTypeIndex);

    CallCompileState callState;
    callState.returnCall = true;
    CalleeDesc callee;
    MOZ_ASSERT(callIndirectId.kind() != CallIndirectIdKind::AsmJS);
    const TableDesc& table = codeMeta().tables[tableIndex];
    callee =
        CalleeDesc::wasmTable(codeMeta(), table, tableIndex, callIndirectId);

    CallSiteDesc desc(lineOrBytecode, CallSiteKind::Indirect);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    MDefinition* address32 = tableAddressToI32(table.addressType(), address);
    if (!address32) {
      return false;
    }

    auto* ins =
        MWasmReturnCall::New(alloc(), desc, callee, callState.regArgs,
                             StackArgAreaSizeUnaligned(argTypes), address32);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]]
  bool callIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                    MDefinition* address, uint32_t lineOrBytecode,
                    const DefVector& args, DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState;
    const FuncType& funcType = (*codeMeta().types)[funcTypeIndex].funcType();
    CallIndirectId callIndirectId =
        CallIndirectId::forFuncType(codeMeta(), funcTypeIndex);

    CalleeDesc callee;
    if (codeMeta().isAsmJS()) {
      MOZ_ASSERT(tableIndex == 0);
      MOZ_ASSERT(callIndirectId.kind() == CallIndirectIdKind::AsmJS);
      uint32_t tableIndex = codeMeta().asmJSSigToTableIndex[funcTypeIndex];
      const TableDesc& table = codeMeta().tables[tableIndex];
      // ensured by asm.js validation
      MOZ_ASSERT(table.initialLength() <= UINT32_MAX);
      MOZ_ASSERT(IsPowerOfTwo(table.initialLength()));

      MDefinition* mask = constantI32(int32_t(table.initialLength() - 1));
      MBitAnd* maskedAddress =
          MBitAnd::New(alloc(), address, mask, MIRType::Int32);
      curBlock_->add(maskedAddress);

      address = maskedAddress;
      callee = CalleeDesc::asmJSTable(codeMeta(), tableIndex);
    } else {
      MOZ_ASSERT(callIndirectId.kind() != CallIndirectIdKind::AsmJS);
      const TableDesc& table = codeMeta().tables[tableIndex];
      callee =
          CalleeDesc::wasmTable(codeMeta(), table, tableIndex, callIndirectId);
      address = tableAddressToI32(table.addressType(), address);
      if (!address) {
        return false;
      }
    }

    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Indirect);
    ArgTypeVector argTypes(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes, address) &&
           collectCallResults(resultType, callState.stackResultArea, results);
  }

  [[nodiscard]]
  bool callImport(unsigned instanceDataOffset, uint32_t lineOrBytecode,
                  const FuncType& funcType, const DefVector& args,
                  DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState;
    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Import);
    auto callee = CalleeDesc::import(instanceDataOffset);
    ArgTypeVector argTypes(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes) &&
           collectCallResults(resultType, callState.stackResultArea, results);
  }

  [[nodiscard]]
  bool builtinCall(CallCompileState* callState,
                   const SymbolicAddressSignature& builtin,
                   uint32_t lineOrBytecode, MDefinition** result) {
    if (inDeadCode()) {
      *result = nullptr;
      return true;
    }

    MOZ_ASSERT(builtin.failureMode == FailureMode::Infallible);

    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Symbolic);
    auto callee = CalleeDesc::builtin(builtin.identity);

    auto* ins =
        MWasmCallUncatchable::New(alloc(), desc, callee, callState->regArgs,
                                  StackArgAreaSizeUnaligned(builtin));
    if (!ins) {
      return false;
    }
    curBlock_->add(ins);

    return collectUnaryCallResult(builtin.retType, result);
  }

  [[nodiscard]]
  bool builtinCall1(const SymbolicAddressSignature& builtin,
                    uint32_t lineOrBytecode, MDefinition* arg,
                    MDefinition** result) {
    CallCompileState callState;
    return passCallArg(arg, builtin.argTypes[0], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, lineOrBytecode, result);
  }

  [[nodiscard]]
  bool builtinCall2(const SymbolicAddressSignature& builtin,
                    uint32_t lineOrBytecode, MDefinition* arg1,
                    MDefinition* arg2, MDefinition** result) {
    CallCompileState callState;
    return passCallArg(arg1, builtin.argTypes[0], &callState) &&
           passCallArg(arg2, builtin.argTypes[1], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, lineOrBytecode, result);
  }

  [[nodiscard]]
  bool builtinCall5(const SymbolicAddressSignature& builtin,
                    uint32_t lineOrBytecode, MDefinition* arg1,
                    MDefinition* arg2, MDefinition* arg3, MDefinition* arg4,
                    MDefinition* arg5, MDefinition** result) {
    CallCompileState callState;
    return passCallArg(arg1, builtin.argTypes[0], &callState) &&
           passCallArg(arg2, builtin.argTypes[1], &callState) &&
           passCallArg(arg3, builtin.argTypes[2], &callState) &&
           passCallArg(arg4, builtin.argTypes[3], &callState) &&
           passCallArg(arg5, builtin.argTypes[4], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, lineOrBytecode, result);
  }

  [[nodiscard]]
  bool builtinCall6(const SymbolicAddressSignature& builtin,
                    uint32_t lineOrBytecode, MDefinition* arg1,
                    MDefinition* arg2, MDefinition* arg3, MDefinition* arg4,
                    MDefinition* arg5, MDefinition* arg6,
                    MDefinition** result) {
    CallCompileState callState;
    return passCallArg(arg1, builtin.argTypes[0], &callState) &&
           passCallArg(arg2, builtin.argTypes[1], &callState) &&
           passCallArg(arg3, builtin.argTypes[2], &callState) &&
           passCallArg(arg4, builtin.argTypes[3], &callState) &&
           passCallArg(arg5, builtin.argTypes[4], &callState) &&
           passCallArg(arg6, builtin.argTypes[5], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, lineOrBytecode, result);
  }

  [[nodiscard]]
  bool instanceCall(CallCompileState* callState,
                    const SymbolicAddressSignature& builtin,
                    uint32_t lineOrBytecode, MDefinition** result = nullptr) {
    MOZ_ASSERT_IF(!result, builtin.retType == MIRType::None);
    if (inDeadCode()) {
      if (result) {
        *result = nullptr;
      }
      return true;
    }

    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Symbolic);
    if (builtin.failureMode != FailureMode::Infallible &&
        !beginCatchableCall(callState)) {
      return false;
    }

    MInstruction* ins;
    if (callState->isCatchable()) {
      ins = MWasmCallCatchable::NewBuiltinInstanceMethodCall(
          alloc(), desc, builtin.identity, builtin.failureMode,
          callState->instanceArg, callState->regArgs,
          StackArgAreaSizeUnaligned(builtin), callState->tryNoteIndex,
          callState->fallthroughBlock, callState->prePadBlock);
    } else {
      ins = MWasmCallUncatchable::NewBuiltinInstanceMethodCall(
          alloc(), desc, builtin.identity, builtin.failureMode,
          callState->instanceArg, callState->regArgs,
          StackArgAreaSizeUnaligned(builtin));
    }
    if (!ins) {
      return false;
    }
    curBlock_->add(ins);

    if (!finishCatchableCall(callState)) {
      return false;
    }

    if (!result) {
      return true;
    }
    return collectUnaryCallResult(builtin.retType, result);
  }

  /*********************************************** Instance call helpers ***/

  // Do not call this function directly -- it offers no protection against
  // mis-counting of arguments.  Instead call one of
  // ::emitInstanceCall{0,1,2,3,4,5,6}.
  //
  // Emits a call to the Instance function indicated by `callee`.  This is
  // assumed to take an Instance pointer as its first argument.  The remaining
  // args are taken from `args`, which is assumed to hold `numArgs` entries.
  // If `result` is non-null, the MDefinition* holding the return value is
  // written to `*result`.
  [[nodiscard]]
  bool emitInstanceCallN(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition** args, size_t numArgs,
                         MDefinition** result = nullptr) {
    // Check that the first formal parameter is plausibly an Instance pointer.
    MOZ_ASSERT(callee.numArgs > 0);
    MOZ_ASSERT(callee.argTypes[0] == MIRType::Pointer);
    // Check we agree on the number of args.
    MOZ_ASSERT(numArgs + 1 /* the instance pointer */ == callee.numArgs);
    // Check we agree on whether a value is returned.
    MOZ_ASSERT((result == nullptr) == (callee.retType == MIRType::None));

    // If we are in dead code, it can happen that some of the `args` entries
    // are nullptr, which will look like an OOM to the logic below.  So exit
    // at this point.  `passInstanceCallArg`, `passCallArg`, `finishCall` and
    // `instanceCall` all do nothing in dead code, so it's valid
    // to exit here.
    if (inDeadCode()) {
      if (result) {
        *result = nullptr;
      }
      return true;
    }

    // Check all args for signs of OOMness before attempting to allocating any
    // more memory.
    for (size_t i = 0; i < numArgs; i++) {
      if (!args[i]) {
        if (result) {
          *result = nullptr;
        }
        return false;
      }
    }

    // Finally, construct the call.
    CallCompileState callState;
    if (!passInstanceCallArg(callee.argTypes[0], &callState)) {
      return false;
    }
    for (size_t i = 0; i < numArgs; i++) {
      if (!passCallArg(args[i], callee.argTypes[i + 1], &callState)) {
        return false;
      }
    }
    if (!finishCallArgs(&callState)) {
      return false;
    }
    return instanceCall(&callState, callee, lineOrBytecode, result);
  }

  [[nodiscard]]
  bool emitInstanceCall0(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition** result = nullptr) {
    MDefinition** args = nullptr;
    return emitInstanceCallN(lineOrBytecode, callee, args, 0, result);
  }
  [[nodiscard]]
  bool emitInstanceCall1(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition** result = nullptr) {
    MDefinition* args[1] = {arg1};
    return emitInstanceCallN(lineOrBytecode, callee, args, 1, result);
  }
  [[nodiscard]]
  bool emitInstanceCall2(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition** result = nullptr) {
    MDefinition* args[2] = {arg1, arg2};
    return emitInstanceCallN(lineOrBytecode, callee, args, 2, result);
  }
  [[nodiscard]]
  bool emitInstanceCall3(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition** result = nullptr) {
    MDefinition* args[3] = {arg1, arg2, arg3};
    return emitInstanceCallN(lineOrBytecode, callee, args, 3, result);
  }
  [[nodiscard]]
  bool emitInstanceCall4(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition* arg4,
                         MDefinition** result = nullptr) {
    MDefinition* args[4] = {arg1, arg2, arg3, arg4};
    return emitInstanceCallN(lineOrBytecode, callee, args, 4, result);
  }
  [[nodiscard]]
  bool emitInstanceCall5(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition* arg4,
                         MDefinition* arg5, MDefinition** result = nullptr) {
    MDefinition* args[5] = {arg1, arg2, arg3, arg4, arg5};
    return emitInstanceCallN(lineOrBytecode, callee, args, 5, result);
  }
  [[nodiscard]]
  bool emitInstanceCall6(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition* arg4,
                         MDefinition* arg5, MDefinition* arg6,
                         MDefinition** result = nullptr) {
    MDefinition* args[6] = {arg1, arg2, arg3, arg4, arg5, arg6};
    return emitInstanceCallN(lineOrBytecode, callee, args, 6, result);
  }

  [[nodiscard]] MDefinition* stackSwitch(MDefinition* suspender,
                                         MDefinition* fn, MDefinition* data,
                                         StackSwitchKind kind) {
    MOZ_ASSERT(!inDeadCode());

    MInstruction* ins;
    switch (kind) {
      case StackSwitchKind::SwitchToMain:
        ins = MWasmStackSwitchToMain::New(alloc(), suspender, fn, data);
        break;
      case StackSwitchKind::SwitchToSuspendable:
        ins = MWasmStackSwitchToSuspendable::New(alloc(), suspender, fn, data);
        break;
      case StackSwitchKind::ContinueOnSuspendable:
        ins = MWasmStackContinueOnSuspendable::New(alloc(), suspender, data);
        break;
    }
    if (!ins) {
      return nullptr;
    }

    curBlock_->add(ins);

    return ins;
  }

  [[nodiscard]]
  bool callRef(const FuncType& funcType, MDefinition* ref,
               uint32_t lineOrBytecode, const DefVector& args,
               DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState;
    CalleeDesc callee = CalleeDesc::wasmFuncRef();
    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::FuncRef);
    ArgTypeVector argTypes(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes, ref) &&
           collectCallResults(resultType, callState.stackResultArea, results);
  }

  [[nodiscard]]
  bool returnCallRef(const FuncType& funcType, MDefinition* ref,
                     uint32_t lineOrBytecode, const DefVector& args,
                     DefVector* results) {
    MOZ_ASSERT(!inDeadCode());
    MOZ_ASSERT(!isInlined());

    CallCompileState callState;
    callState.returnCall = true;
    CalleeDesc callee = CalleeDesc::wasmFuncRef();
    CallSiteDesc desc(lineOrBytecode, CallSiteKind::FuncRef);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    auto* ins = MWasmReturnCall::New(alloc(), desc, callee, callState.regArgs,
                                     StackArgAreaSizeUnaligned(argTypes), ref);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]] MDefinition* stringCast(MDefinition* string) {
    auto* ins = MWasmTrapIfAnyRefIsNotJSString::New(
        alloc(), string, wasm::Trap::BadCast, trapSiteDesc());
    if (!ins) {
      return ins;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] MDefinition* stringTest(MDefinition* string) {
    auto* ins = MWasmAnyRefIsJSString::New(alloc(), string);
    if (!ins) {
      return nullptr;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] MDefinition* stringLength(MDefinition* string) {
    auto* ins = MWasmAnyRefJSStringLength::New(
        alloc(), string, wasm::Trap::BadCast, trapSiteDesc());
    if (!ins) {
      return nullptr;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] bool dispatchInlineBuiltinModuleFunc(
      const BuiltinModuleFunc& builtinModuleFunc, const DefVector& params) {
    BuiltinInlineOp inlineOp = builtinModuleFunc.inlineOp();
    MOZ_ASSERT(inlineOp != BuiltinInlineOp::None);
    switch (inlineOp) {
      case BuiltinInlineOp::StringCast: {
        MOZ_ASSERT(params.length() == 1);
        MDefinition* string = params[0];
        MDefinition* cast = stringCast(string);
        if (!cast) {
          return false;
        }
        iter().setResult(string);
        return true;
      }
      case BuiltinInlineOp::StringTest: {
        MOZ_ASSERT(params.length() == 1);
        MDefinition* string = params[0];
        MDefinition* test = stringTest(string);
        if (!test) {
          return false;
        }
        iter().setResult(test);
        return true;
      }
      case BuiltinInlineOp::StringLength: {
        MOZ_ASSERT(params.length() == 1);
        MDefinition* string = params[0];
        MDefinition* length = stringLength(string);
        if (!length) {
          return false;
        }
        iter().setResult(length);
        return true;
      }
      case BuiltinInlineOp::None:
      case BuiltinInlineOp::Limit:
        break;
    }
    MOZ_CRASH();
  }

  [[nodiscard]] bool callBuiltinModuleFunc(
      const BuiltinModuleFunc& builtinModuleFunc, const DefVector& params) {
    MOZ_ASSERT(!inDeadCode());

    BuiltinInlineOp inlineOp = builtinModuleFunc.inlineOp();
    if (inlineOp != BuiltinInlineOp::None) {
      return dispatchInlineBuiltinModuleFunc(builtinModuleFunc, params);
    }

    // It's almost possible to use FunctionCompiler::emitInstanceCallN here.
    // Unfortunately not currently possible though, since ::emitInstanceCallN
    // expects an array of arguments along with a size, and that's not what is
    // available here.  It would be possible if we were prepared to copy
    // `builtinModuleFunc->params` into a fixed-sized (16 element?) array, add
    // `memoryBase`, and make the call.
    const SymbolicAddressSignature& callee = *builtinModuleFunc.sig();

    CallCompileState callState;
    if (!passInstanceCallArg(callee.argTypes[0], &callState) ||
        !passCallArgs(params, builtinModuleFunc.funcType()->args(),
                      &callState)) {
      return false;
    }

    if (builtinModuleFunc.usesMemory()) {
      if (!passCallArg(memoryBase(0), MIRType::Pointer, &callState)) {
        return false;
      }
    }

    if (!finishCallArgs(&callState)) {
      return false;
    }

    bool hasResult = !builtinModuleFunc.funcType()->results().empty();
    MDefinition* result = nullptr;
    MDefinition** resultOutParam = hasResult ? &result : nullptr;
    if (!instanceCall(&callState, callee, readBytecodeOffset(),
                      resultOutParam)) {
      return false;
    }

    if (hasResult) {
      iter().setResult(result);
    }
    return true;
  }

  /*********************************************** Control flow generation */

  inline bool inDeadCode() const { return curBlock_ == nullptr; }

  [[nodiscard]] bool returnValues(DefVector&& values) {
    if (inDeadCode()) {
      return true;
    }

    // If we're inlined into another function, we must accumulate the returns
    // so that they can be patched into the caller function.
    if (isInlined()) {
      MGoto* jump = MGoto::New(alloc());
      if (!jump) {
        return false;
      }
      curBlock_->end(jump);
      curBlock_ = nullptr;
      return pendingInlineReturns_.emplaceBack(
          PendingInlineReturn(jump, std::move(values)));
    }

    if (values.empty()) {
      curBlock_->end(MWasmReturnVoid::New(alloc(), instancePointer_));
    } else {
      ResultType resultType = ResultType::Vector(funcType().results());
      ABIResultIter iter(resultType);
      // Switch to iterate in FIFO order instead of the default LIFO.
      while (!iter.done()) {
        iter.next();
      }
      iter.switchToPrev();
      for (uint32_t i = 0; !iter.done(); iter.prev(), i++) {
        if (!mirGen().ensureBallast()) {
          return false;
        }
        const ABIResult& result = iter.cur();
        if (result.onStack()) {
          MOZ_ASSERT(iter.remaining() > 1);
          auto* store = MWasmStoreStackResult::New(
              alloc(), stackResultPointer_, result.stackOffset(), values[i]);
          curBlock_->add(store);
        } else {
          MOZ_ASSERT(iter.remaining() == 1);
          MOZ_ASSERT(i + 1 == values.length());
          curBlock_->end(
              MWasmReturn::New(alloc(), values[i], instancePointer_));
        }
      }
    }
    curBlock_ = nullptr;
    return true;
  }

  void unreachableTrap() {
    if (inDeadCode()) {
      return;
    }

    auto* ins =
        MWasmTrap::New(alloc(), wasm::Trap::Unreachable, trapSiteDesc());
    curBlock_->end(ins);
    curBlock_ = nullptr;
  }

 private:
  static uint32_t numPushed(MBasicBlock* block) {
    return block->stackDepth() - block->info().firstStackSlot();
  }

 public:
  [[nodiscard]] bool pushDefs(const DefVector& defs) {
    if (inDeadCode()) {
      return true;
    }
    MOZ_ASSERT(numPushed(curBlock_) == 0);
    if (!curBlock_->ensureHasSlots(defs.length())) {
      return false;
    }
    for (MDefinition* def : defs) {
      MOZ_ASSERT(def->type() != MIRType::None);
      curBlock_->push(def);
    }
    return true;
  }

  [[nodiscard]] bool popPushedDefs(DefVector* defs) {
    size_t n = numPushed(curBlock_);
    if (!defs->resizeUninitialized(n)) {
      return false;
    }
    for (; n > 0; n--) {
      MDefinition* def = curBlock_->pop();
      MOZ_ASSERT(def->type() != MIRType::Value);
      (*defs)[n - 1] = def;
    }
    return true;
  }

 private:
  [[nodiscard]] bool addJoinPredecessor(const DefVector& defs,
                                        MBasicBlock** joinPred) {
    *joinPred = curBlock_;
    if (inDeadCode()) {
      return true;
    }
    return pushDefs(defs);
  }

 public:
  [[nodiscard]] bool branchAndStartThen(MDefinition* cond,
                                        MBasicBlock** elseBlock) {
    if (inDeadCode()) {
      *elseBlock = nullptr;
    } else {
      MBasicBlock* thenBlock;
      if (!newBlock(curBlock_, &thenBlock)) {
        return false;
      }
      if (!newBlock(curBlock_, elseBlock)) {
        return false;
      }

      curBlock_->end(MTest::New(alloc(), cond, thenBlock, *elseBlock));

      curBlock_ = thenBlock;
      mirGraph().moveBlockToEnd(curBlock_);
    }

    return startBlock();
  }

  [[nodiscard]] bool switchToElse(MBasicBlock* elseBlock,
                                  MBasicBlock** thenJoinPred) {
    DefVector values;
    if (!finishBlock(&values)) {
      return false;
    }

    if (!elseBlock) {
      *thenJoinPred = nullptr;
    } else {
      if (!addJoinPredecessor(values, thenJoinPred)) {
        return false;
      }

      curBlock_ = elseBlock;
      mirGraph().moveBlockToEnd(curBlock_);
    }

    return startBlock();
  }

  [[nodiscard]] bool joinIfElse(MBasicBlock* thenJoinPred, DefVector* defs) {
    DefVector values;
    if (!finishBlock(&values)) {
      return false;
    }

    if (!thenJoinPred && inDeadCode()) {
      return true;
    }

    MBasicBlock* elseJoinPred;
    if (!addJoinPredecessor(values, &elseJoinPred)) {
      return false;
    }

    mozilla::Array<MBasicBlock*, 2> blocks;
    size_t numJoinPreds = 0;
    if (thenJoinPred) {
      blocks[numJoinPreds++] = thenJoinPred;
    }
    if (elseJoinPred) {
      blocks[numJoinPreds++] = elseJoinPred;
    }

    if (numJoinPreds == 0) {
      return true;
    }

    MBasicBlock* join;
    if (!goToNewBlock(blocks[0], &join)) {
      return false;
    }
    for (size_t i = 1; i < numJoinPreds; ++i) {
      if (!goToExistingBlock(blocks[i], join)) {
        return false;
      }
    }

    curBlock_ = join;
    return popPushedDefs(defs);
  }

  [[nodiscard]] bool startBlock() {
    MOZ_ASSERT_IF(pendingBlockDepth_ < pendingBlocks_.length(),
                  pendingBlocks_[pendingBlockDepth_].patches.empty());
    pendingBlockDepth_++;
    return true;
  }

  [[nodiscard]] bool finishBlock(DefVector* defs) {
    MOZ_ASSERT(pendingBlockDepth_);
    uint32_t topLabel = --pendingBlockDepth_;
    return bindBranches(topLabel, defs);
  }

  [[nodiscard]] bool startLoop(MBasicBlock** loopHeader, size_t paramCount) {
    *loopHeader = nullptr;

    pendingBlockDepth_++;
    rootCompiler_.startLoop();

    if (inDeadCode()) {
      return true;
    }

    // Create the loop header.
    MOZ_ASSERT(curBlock_->loopDepth() == rootCompiler_.loopDepth() - 1);
    *loopHeader = MBasicBlock::New(mirGraph(), info(), curBlock_,
                                   MBasicBlock::PENDING_LOOP_HEADER);
    if (!*loopHeader) {
      return false;
    }

    (*loopHeader)->setLoopDepth(rootCompiler_.loopDepth());
    mirGraph().addBlock(*loopHeader);
    curBlock_->end(MGoto::New(alloc(), *loopHeader));

    DefVector loopParams;
    if (!iter().getResults(paramCount, &loopParams)) {
      return false;
    }

    // Eagerly create a phi for all loop params. setLoopBackedge will remove
    // any that were not necessary.
    for (size_t i = 0; i < paramCount; i++) {
      MPhi* phi = MPhi::New(alloc(), loopParams[i]->type());
      if (!phi) {
        return false;
      }
      if (!phi->reserveLength(2)) {
        return false;
      }
      (*loopHeader)->addPhi(phi);
      phi->addInput(loopParams[i]);
      loopParams[i] = phi;
    }
    iter().setResults(paramCount, loopParams);

    MBasicBlock* body;
    if (!goToNewBlock(*loopHeader, &body)) {
      return false;
    }
    curBlock_ = body;
    return true;
  }

 private:
  void fixupRedundantPhis(MBasicBlock* b) {
    for (size_t i = 0, depth = b->stackDepth(); i < depth; i++) {
      MDefinition* def = b->getSlot(i);
      if (def->isUnused()) {
        b->setSlot(i, def->toPhi()->getOperand(0));
      }
    }
  }

  [[nodiscard]] bool setLoopBackedge(MBasicBlock* loopEntry,
                                     MBasicBlock* loopBody,
                                     MBasicBlock* backedge, size_t paramCount) {
    if (!loopEntry->setBackedgeWasm(backedge, paramCount)) {
      return false;
    }

    // Entering a loop will eagerly create a phi node for all locals and loop
    // params. Now that we've closed the loop we can check which phi nodes
    // were actually needed by checking if the SSA definition flowing into the
    // loop header (operand 0) is different than the SSA definition coming from
    // the loop backedge (operand 1). If they are the same definition, the phi
    // is redundant and can be removed.
    //
    // To do this we mark all redundant phis as 'unused', then remove the phi's
    // from places in ourself the phis may have flowed into, then replace all
    // uses of the phi's in the MIR graph with the original SSA definition.
    for (MPhiIterator phi = loopEntry->phisBegin(); phi != loopEntry->phisEnd();
         phi++) {
      MOZ_ASSERT(phi->numOperands() == 2);
      if (phi->getOperand(0) == phi->getOperand(1)) {
        phi->setUnused();
      }
    }

    // Fix up phis stored in the slots Vector of pending blocks.
    for (PendingBlockTarget& pendingBlockTarget : pendingBlocks_) {
      for (ControlFlowPatch& p : pendingBlockTarget.patches) {
        MBasicBlock* block = p.ins->block();
        if (block->loopDepth() >= loopEntry->loopDepth()) {
          fixupRedundantPhis(block);
        }
      }
    }

    // The loop body, if any, might be referencing recycled phis too.
    if (loopBody) {
      fixupRedundantPhis(loopBody);
    }

    // Pending jumps to an enclosing try-catch may reference the recycled phis.
    // We have to search above all enclosing try blocks, as a delegate may move
    // patches around.
    for (uint32_t depth = 0; depth < iter().controlStackDepth(); depth++) {
      LabelKind kind = iter().controlKind(depth);
      if (kind != LabelKind::Try && kind != LabelKind::TryTable &&
          kind != LabelKind::Body) {
        continue;
      }
      Control& control = iter().controlItem(depth);
      if (!control.tryControl) {
        continue;
      }
      for (MControlInstruction* patch : control.tryControl->landingPadPatches) {
        MBasicBlock* block = patch->block();
        if (block->loopDepth() >= loopEntry->loopDepth()) {
          fixupRedundantPhis(block);
        }
      }
    }
    for (MControlInstruction* patch : bodyRethrowPadPatches_) {
      MBasicBlock* block = patch->block();
      if (block->loopDepth() >= loopEntry->loopDepth()) {
        fixupRedundantPhis(block);
      }
    }

    // If we're inlined into another function we are accumulating return values
    // in a vector, search through the results to see if any refer to a
    // redundant phi.
    for (PendingInlineReturn& pendingReturn : pendingInlineReturns_) {
      for (uint32_t resultIndex = 0;
           resultIndex < pendingReturn.results.length(); resultIndex++) {
        MDefinition** pendingResult = &pendingReturn.results[resultIndex];
        if ((*pendingResult)->isUnused()) {
          *pendingResult = (*pendingResult)->toPhi()->getOperand(0);
        }
      }
    }

    // Discard redundant phis and add to the free list.
    for (MPhiIterator phi = loopEntry->phisBegin();
         phi != loopEntry->phisEnd();) {
      MPhi* entryDef = *phi++;
      if (!entryDef->isUnused()) {
        continue;
      }

      entryDef->justReplaceAllUsesWith(entryDef->getOperand(0));
      loopEntry->discardPhi(entryDef);
      mirGraph().addPhiToFreeList(entryDef);
    }

    return true;
  }

 public:
  [[nodiscard]] bool closeLoop(MBasicBlock* loopHeader,
                               DefVector* loopResults) {
    MOZ_ASSERT(pendingBlockDepth_ >= 1);
    MOZ_ASSERT(rootCompiler_.loopDepth());

    uint32_t headerLabel = pendingBlockDepth_ - 1;

    if (!loopHeader) {
      MOZ_ASSERT(inDeadCode());
      MOZ_ASSERT(headerLabel >= pendingBlocks_.length() ||
                 pendingBlocks_[headerLabel].patches.empty());
      pendingBlockDepth_--;
      rootCompiler_.closeLoop();
      return true;
    }

    // Op::Loop doesn't have an implicit backedge so temporarily set
    // aside the end of the loop body to bind backedges.
    MBasicBlock* loopBody = curBlock_;
    curBlock_ = nullptr;

    // As explained in bug 1253544, Ion apparently has an invariant that
    // there is only one backedge to loop headers. To handle wasm's ability
    // to have multiple backedges to the same loop header, we bind all those
    // branches as forward jumps to a single backward jump. This is
    // unfortunate but the optimizer is able to fold these into single jumps
    // to backedges.
    DefVector backedgeValues;
    if (!bindBranches(headerLabel, &backedgeValues)) {
      return false;
    }

    MOZ_ASSERT(loopHeader->loopDepth() == rootCompiler_.loopDepth());

    if (curBlock_) {
      // We're on the loop backedge block, created by bindBranches.
      for (size_t i = 0, n = numPushed(curBlock_); i != n; i++) {
        curBlock_->pop();
      }

      if (!pushDefs(backedgeValues)) {
        return false;
      }

      MOZ_ASSERT(curBlock_->loopDepth() == rootCompiler_.loopDepth());
      curBlock_->end(MGoto::New(alloc(), loopHeader));
      if (!setLoopBackedge(loopHeader, loopBody, curBlock_,
                           backedgeValues.length())) {
        return false;
      }
    }

    curBlock_ = loopBody;

    rootCompiler_.closeLoop();

    // If the loop depth still at the inner loop body, correct it.
    if (curBlock_ && curBlock_->loopDepth() != rootCompiler_.loopDepth()) {
      MBasicBlock* out;
      if (!goToNewBlock(curBlock_, &out)) {
        return false;
      }
      curBlock_ = out;
    }

    pendingBlockDepth_ -= 1;
    return inDeadCode() || popPushedDefs(loopResults);
  }

  [[nodiscard]] bool addControlFlowPatch(
      MControlInstruction* ins, uint32_t relative, uint32_t index,
      BranchHint branchHint = BranchHint::Invalid) {
    MOZ_ASSERT(relative < pendingBlockDepth_);
    uint32_t absolute = pendingBlockDepth_ - 1 - relative;

    if (absolute >= pendingBlocks_.length() &&
        !pendingBlocks_.resize(absolute + 1)) {
      return false;
    }

    pendingBlocks_[absolute].hint = branchHint;
    return pendingBlocks_[absolute].patches.append(
        ControlFlowPatch(ins, index));
  }

  [[nodiscard]] bool br(uint32_t relativeDepth, const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    MGoto* jump = MGoto::New(alloc());
    if (!addControlFlowPatch(jump, relativeDepth, MGoto::TargetIndex)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(jump);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]] bool brIf(uint32_t relativeDepth, const DefVector& values,
                          MDefinition* condition, BranchHint branchHint) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* joinBlock = nullptr;
    if (!newBlock(curBlock_, &joinBlock)) {
      return false;
    }

    MTest* test = MTest::New(alloc(), condition, nullptr, joinBlock);
    if (!addControlFlowPatch(test, relativeDepth, MTest::TrueBranchIndex,
                             branchHint)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = joinBlock;

    return true;
  }

  [[nodiscard]] bool brTable(MDefinition* operand, uint32_t defaultDepth,
                             const Uint32Vector& depths,
                             const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    size_t numCases = depths.length();
    MOZ_ASSERT(numCases <= INT32_MAX);
    MOZ_ASSERT(numCases);

    MTableSwitch* table =
        MTableSwitch::New(alloc(), operand, 0, int32_t(numCases - 1));

    size_t defaultIndex;
    if (!table->addDefault(nullptr, &defaultIndex)) {
      return false;
    }
    if (!addControlFlowPatch(table, defaultDepth, defaultIndex)) {
      return false;
    }

    using IndexToCaseMap =
        HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy>;

    IndexToCaseMap indexToCase;
    if (!indexToCase.put(defaultDepth, defaultIndex)) {
      return false;
    }

    for (size_t i = 0; i < numCases; i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }

      uint32_t depth = depths[i];

      size_t caseIndex;
      IndexToCaseMap::AddPtr p = indexToCase.lookupForAdd(depth);
      if (!p) {
        if (!table->addSuccessor(nullptr, &caseIndex)) {
          return false;
        }
        if (!addControlFlowPatch(table, depth, caseIndex)) {
          return false;
        }
        if (!indexToCase.add(p, depth, caseIndex)) {
          return false;
        }
      } else {
        caseIndex = p->value();
      }

      if (!table->addCase(caseIndex)) {
        return false;
      }
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(table);
    curBlock_ = nullptr;

    return true;
  }

  /********************************************************** Exceptions ***/

  bool inTryBlockFrom(uint32_t fromRelativeDepth,
                      uint32_t* tryRelativeDepth) const {
    uint32_t relativeDepth;
    if (iter_.controlFindInnermostFrom(
            [](LabelKind kind, const Control& control) {
              return control.tryControl != nullptr &&
                     control.tryControl->inBody;
            },
            fromRelativeDepth, &relativeDepth)) {
      *tryRelativeDepth = relativeDepth;
      return true;
    }

    if (callerCompiler_ && callerCompiler_->inTryCode()) {
      *tryRelativeDepth = iter_.controlStackDepth() - 1;
      return true;
    }

    return false;
  }

  bool inTryBlockFrom(uint32_t fromRelativeDepth,
                      ControlInstructionVector** landingPadPatches) {
    uint32_t tryRelativeDepth;
    if (!inTryBlockFrom(fromRelativeDepth, &tryRelativeDepth)) {
      return false;
    }

    if (tryRelativeDepth == iter().controlStackDepth() - 1) {
      *landingPadPatches = &bodyRethrowPadPatches_;
    } else {
      *landingPadPatches =
          &iter().controlItem(tryRelativeDepth).tryControl->landingPadPatches;
    }
    return true;
  }

  bool inTryBlock(ControlInstructionVector** landingPadPatches) {
    return inTryBlockFrom(0, landingPadPatches);
  }

  bool inTryCode() const {
    uint32_t tryRelativeDepth;
    return inTryBlockFrom(0, &tryRelativeDepth);
  }

  MDefinition* loadTag(uint32_t tagIndex) {
    MWasmLoadInstanceDataField* tag = MWasmLoadInstanceDataField::New(
        alloc(), MIRType::WasmAnyRef,
        codeMeta().offsetOfTagInstanceData(tagIndex), true, instancePointer_);
    curBlock_->add(tag);
    return tag;
  }

  void loadPendingExceptionState(MDefinition** pendingException,
                                 MDefinition** pendingExceptionTag) {
    auto* exception = MWasmLoadInstance::New(
        alloc(), instancePointer_, wasm::Instance::offsetOfPendingException(),
        MIRType::WasmAnyRef, AliasSet::Load(AliasSet::WasmPendingException));
    curBlock_->add(exception);
    *pendingException = exception;

    auto* tag = MWasmLoadInstance::New(
        alloc(), instancePointer_,
        wasm::Instance::offsetOfPendingExceptionTag(), MIRType::WasmAnyRef,
        AliasSet::Load(AliasSet::WasmPendingException));
    curBlock_->add(tag);
    *pendingExceptionTag = tag;
  }

  [[nodiscard]] bool setPendingExceptionState(MDefinition* exception,
                                              MDefinition* tag) {
    // Set the pending exception object
    auto* exceptionAddr = MWasmDerivedPointer::New(
        alloc(), instancePointer_, Instance::offsetOfPendingException());
    curBlock_->add(exceptionAddr);
    auto* setException = MWasmStoreRef::New(
        alloc(), instancePointer_, exceptionAddr, /*valueOffset=*/0, exception,
        AliasSet::WasmPendingException, WasmPreBarrierKind::Normal);
    curBlock_->add(setException);
    if (!postBarrierEdgePrecise(/*lineOrBytecode=*/0, exceptionAddr,
                                exception)) {
      return false;
    }

    // Set the pending exception tag object
    auto* exceptionTagAddr = MWasmDerivedPointer::New(
        alloc(), instancePointer_, Instance::offsetOfPendingExceptionTag());
    curBlock_->add(exceptionTagAddr);
    auto* setExceptionTag = MWasmStoreRef::New(
        alloc(), instancePointer_, exceptionTagAddr, /*valueOffset=*/0, tag,
        AliasSet::WasmPendingException, WasmPreBarrierKind::Normal);
    curBlock_->add(setExceptionTag);
    return postBarrierEdgePrecise(/*lineOrBytecode=*/0, exceptionTagAddr, tag);
  }

  [[nodiscard]] bool endWithPadPatch(
      ControlInstructionVector* tryLandingPadPatches) {
    MGoto* jumpToLandingPad = MGoto::New(alloc());
    curBlock_->end(jumpToLandingPad);
    return tryLandingPadPatches->emplaceBack(jumpToLandingPad);
  }

  [[nodiscard]] bool delegatePadPatches(const ControlInstructionVector& patches,
                                        uint32_t relativeDepth) {
    if (patches.empty()) {
      return true;
    }

    // Find where we are delegating the pad patches to.
    ControlInstructionVector* targetPatches;
    if (!inTryBlockFrom(relativeDepth, &targetPatches)) {
      MOZ_ASSERT(relativeDepth <= pendingBlockDepth_ - 1);
      targetPatches = &bodyRethrowPadPatches_;
    }

    // Append the delegate's pad patches to the target's.
    for (MControlInstruction* ins : patches) {
      if (!targetPatches->emplaceBack(ins)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]]
  bool beginCatchableCall(CallCompileState* callState) {
    if (!inTryBlock(&callState->tryLandingPadPatches)) {
      MOZ_ASSERT(!callState->isCatchable());
      return true;
    }
    MOZ_ASSERT(callState->isCatchable());

    // Allocate a try note
    if (!rootCompiler_.addTryNote(&callState->tryNoteIndex)) {
      return false;
    }

    // Allocate blocks for fallthrough and exceptions
    return newBlock(curBlock_, &callState->fallthroughBlock) &&
           newBlock(curBlock_, &callState->prePadBlock);
  }

  [[nodiscard]]
  bool finishCatchableCall(CallCompileState* callState) {
    if (!callState->tryLandingPadPatches) {
      return true;
    }

    // Switch to the prePadBlock
    MBasicBlock* callBlock = curBlock_;
    curBlock_ = callState->prePadBlock;

    // Mark this as the landing pad for the call
    curBlock_->add(MWasmCallLandingPrePad::New(alloc(), callBlock,
                                               callState->tryNoteIndex));

    // End with a pending jump to the landing pad
    if (!endWithPadPatch(callState->tryLandingPadPatches)) {
      return false;
    }

    // Compilation continues in the fallthroughBlock.
    curBlock_ = callState->fallthroughBlock;
    return true;
  }

  // Create a landing pad for a try block. This is also used for the implicit
  // rethrow landing pad used for delegate instructions that target the
  // outermost label.
  [[nodiscard]]
  bool createTryLandingPad(ControlInstructionVector& landingPadPatches,
                           MBasicBlock** landingPad) {
    MOZ_ASSERT(!landingPadPatches.empty());

    // Bind the branches from exception throwing code to a new landing pad
    // block. This is done similarly to what is done in bindBranches.
    MControlInstruction* ins = landingPadPatches[0];
    MBasicBlock* pred = ins->block();
    if (!newBlock(pred, landingPad)) {
      return false;
    }
    ins->replaceSuccessor(MGoto::TargetIndex, *landingPad);
    for (size_t i = 1; i < landingPadPatches.length(); i++) {
      ins = landingPadPatches[i];
      pred = ins->block();
      if (!(*landingPad)->addPredecessor(alloc(), pred)) {
        return false;
      }
      ins->replaceSuccessor(MGoto::TargetIndex, *landingPad);
    }

    // Clear the now bound pad patches.
    landingPadPatches.clear();
    return true;
  }

  [[nodiscard]]
  bool createTryTableLandingPad(TryControl* tryControl) {
    // If there were no patches, then there were no throwing instructions and
    // we don't need to do anything.
    if (tryControl->landingPadPatches.empty()) {
      return true;
    }

    // Create the landing pad block and bind all the throwing instructions
    MBasicBlock* landingPad;
    if (!createTryLandingPad(tryControl->landingPadPatches, &landingPad)) {
      return false;
    }

    // Get the pending exception from the instance
    MDefinition* pendingException;
    MDefinition* pendingExceptionTag;
    if (!consumePendingException(&landingPad, &pendingException,
                                 &pendingExceptionTag)) {
      return false;
    }

    MBasicBlock* originalBlock = curBlock_;
    curBlock_ = landingPad;

    bool hadCatchAll = false;
    for (const TryTableCatch& tryTableCatch : tryControl->catches) {
      // Handle a catch_all by jumping to the target block
      if (tryTableCatch.tagIndex == CatchAllIndex) {
        // Capture the exnref value if we need to
        DefVector values;
        if (tryTableCatch.captureExnRef && !values.append(pendingException)) {
          return false;
        }

        // Branch to the catch_all code
        if (!br(tryTableCatch.labelRelativeDepth, values)) {
          return false;
        }

        // Break from the loop and skip the implicit rethrow that's needed
        // if we didn't have a catch_all
        hadCatchAll = true;
        break;
      }

      // Handle a tagged catch by doing a compare and branch on the tag index,
      // jumping to a catch block if they match, or else to a fallthrough block
      // to continue the landing pad.
      MBasicBlock* catchBlock = nullptr;
      MBasicBlock* fallthroughBlock = nullptr;
      if (!newBlock(curBlock_, &catchBlock) ||
          !newBlock(curBlock_, &fallthroughBlock)) {
        return false;
      }

      // Branch to the catch block if the exception's tag matches this catch
      // block's tag.
      MDefinition* catchTag = loadTag(tryTableCatch.tagIndex);
      MDefinition* matchesCatchTag =
          compare(pendingExceptionTag, catchTag, JSOp::Eq,
                  MCompare::Compare_WasmAnyRef);
      curBlock_->end(
          MTest::New(alloc(), matchesCatchTag, catchBlock, fallthroughBlock));

      // Set up the catch block by extracting the values from the exception
      // object.
      curBlock_ = catchBlock;

      // Extract the exception values for the catch block
      DefVector values;
      if (!loadExceptionValues(pendingException, tryTableCatch.tagIndex,
                               &values)) {
        return false;
      }
      if (tryTableCatch.captureExnRef && !values.append(pendingException)) {
        return false;
      }

      if (!br(tryTableCatch.labelRelativeDepth, values)) {
        return false;
      }

      curBlock_ = fallthroughBlock;
    }

    // If there was no catch_all, we must rethrow this exception.
    if (!hadCatchAll) {
      if (!throwFrom(pendingException, pendingExceptionTag)) {
        return false;
      }
    }

    curBlock_ = originalBlock;
    return true;
  }

  // Consume the pending exception state from instance. This will clear out the
  // previous value.
  [[nodiscard]]
  bool consumePendingException(MBasicBlock** landingPad,
                               MDefinition** pendingException,
                               MDefinition** pendingExceptionTag) {
    MBasicBlock* prevBlock = curBlock_;
    curBlock_ = *landingPad;

    // Load the pending exception and tag
    loadPendingExceptionState(pendingException, pendingExceptionTag);

    // Clear the pending exception and tag
    auto* null = constantNullRef(MaybeRefType());
    if (!setPendingExceptionState(null, null)) {
      return false;
    }

    // The landing pad may have changed from loading and clearing the pending
    // exception state.
    *landingPad = curBlock_;

    curBlock_ = prevBlock;
    return true;
  }

  [[nodiscard]] bool startTry() {
    Control& control = iter().controlItem();
    control.block = curBlock_;
    control.tryControl = rootCompiler_.newTryControl();
    if (!control.tryControl) {
      return false;
    }
    control.tryControl->inBody = true;
    return startBlock();
  }

  [[nodiscard]] bool startTryTable(TryTableCatchVector&& catches) {
    Control& control = iter().controlItem();
    control.block = curBlock_;
    control.tryControl = rootCompiler_.newTryControl();
    if (!control.tryControl) {
      return false;
    }
    control.tryControl->inBody = true;
    control.tryControl->catches = std::move(catches);
    return startBlock();
  }

  [[nodiscard]] bool joinTryOrCatchBlock(Control& control) {
    // If the try or catch block ended with dead code, there is no need to
    // do any control flow join.
    if (inDeadCode()) {
      return true;
    }

    // This is a split path which we'll need to join later, using a control
    // flow patch.
    MOZ_ASSERT(!curBlock_->hasLastIns());
    MGoto* jump = MGoto::New(alloc());
    if (!addControlFlowPatch(jump, 0, MGoto::TargetIndex)) {
      return false;
    }

    // Finish the current block with the control flow patch instruction.
    curBlock_->end(jump);
    return true;
  }

  // Finish the previous block (either a try or catch block) and then setup a
  // new catch block.
  [[nodiscard]] bool switchToCatch(Control& control, LabelKind fromKind,
                                   uint32_t tagIndex) {
    // Mark this control node as being no longer in the body of the try
    control.tryControl->inBody = false;

    // If there is no control block, then either:
    //   - the entry of the try block is dead code, or
    //   - there is no landing pad for the try-catch.
    // In either case, any catch will be dead code.
    if (!control.block) {
      MOZ_ASSERT(inDeadCode());
      return true;
    }

    // Join the previous try or catch block with a patch to the future join of
    // the whole try-catch block.
    if (!joinTryOrCatchBlock(control)) {
      return false;
    }

    // If we are switching from the try block, create the landing pad. This is
    // guaranteed to happen once and only once before processing catch blocks.
    if (fromKind == LabelKind::Try) {
      if (!control.tryControl->landingPadPatches.empty()) {
        // Create the landing pad block and bind all the throwing instructions
        MBasicBlock* padBlock = nullptr;
        if (!createTryLandingPad(control.tryControl->landingPadPatches,
                                 &padBlock)) {
          return false;
        }

        // Store the pending exception and tag on the control item for future
        // use in catch handlers.
        if (!consumePendingException(
                &padBlock, &control.tryControl->pendingException,
                &control.tryControl->pendingExceptionTag)) {
          return false;
        }

        // Set the control block for this try-catch to the landing pad.
        control.block = padBlock;
      } else {
        control.block = nullptr;
      }
    }

    // If there is no landing pad, then this and following catches are dead
    // code.
    if (!control.block) {
      curBlock_ = nullptr;
      return true;
    }

    // Switch to the landing pad.
    curBlock_ = control.block;

    // We should have a pending exception and tag if we were able to create a
    // landing pad.
    MOZ_ASSERT(control.tryControl->pendingException);
    MOZ_ASSERT(control.tryControl->pendingExceptionTag);

    // Handle a catch_all by immediately jumping to a new block. We require a
    // new block (as opposed to just emitting the catch_all code in the current
    // block) because rethrow requires the exception/tag to be present in the
    // landing pad's slots, while the catch_all block must not have the
    // exception/tag in slots.
    if (tagIndex == CatchAllIndex) {
      MBasicBlock* catchAllBlock = nullptr;
      if (!goToNewBlock(curBlock_, &catchAllBlock)) {
        return false;
      }
      // Compilation will continue in the catch_all block.
      curBlock_ = catchAllBlock;
      return true;
    }

    // Handle a tagged catch by doing a compare and branch on the tag index,
    // jumping to a catch block if they match, or else to a fallthrough block
    // to continue the landing pad.
    MBasicBlock* catchBlock = nullptr;
    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &catchBlock) ||
        !newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    // Branch to the catch block if the exception's tag matches this catch
    // block's tag.
    MDefinition* catchTag = loadTag(tagIndex);
    MDefinition* matchesCatchTag =
        compare(control.tryControl->pendingExceptionTag, catchTag, JSOp::Eq,
                MCompare::Compare_WasmAnyRef);
    curBlock_->end(
        MTest::New(alloc(), matchesCatchTag, catchBlock, fallthroughBlock));

    // The landing pad will continue in the fallthrough block
    control.block = fallthroughBlock;

    // Set up the catch block by extracting the values from the exception
    // object.
    curBlock_ = catchBlock;

    // Extract the exception values for the catch block
    DefVector values;
    if (!loadExceptionValues(control.tryControl->pendingException, tagIndex,
                             &values)) {
      return false;
    }
    iter().setResults(values.length(), values);
    return true;
  }

  [[nodiscard]] bool loadExceptionValues(MDefinition* exception,
                                         uint32_t tagIndex, DefVector* values) {
    SharedTagType tagType = codeMeta().tags[tagIndex].type;
    const ValTypeVector& params = tagType->argTypes();
    const TagOffsetVector& offsets = tagType->argOffsets();

    // Get the data pointer from the exception object
    auto* data = MWasmLoadField::New(
        alloc(), exception, nullptr, WasmExceptionObject::offsetOfData(),
        mozilla::Nothing(), MIRType::Pointer, MWideningOp::None,
        AliasSet::Load(AliasSet::Any));
    if (!data) {
      return false;
    }
    curBlock_->add(data);

    // Presize the values vector to the number of params
    if (!values->reserve(params.length())) {
      return false;
    }

    // Load each value from the data pointer
    for (size_t i = 0; i < params.length(); i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      auto* load =
          MWasmLoadField::New(alloc(), data, exception, offsets[i],
                              mozilla::Nothing(), params[i].toMIRType(),
                              MWideningOp::None, AliasSet::Load(AliasSet::Any),
                              mozilla::Nothing(), params[i].toMaybeRefType());
      if (!load || !values->append(load)) {
        return false;
      }
      curBlock_->add(load);
    }
    return true;
  }

  [[nodiscard]] bool finishTryCatch(LabelKind kind, Control& control,
                                    DefVector* defs) {
    switch (kind) {
      case LabelKind::Try: {
        // This is a catchless try, we must delegate all throwing instructions
        // to the nearest enclosing try block if one exists, or else to the
        // body block which will handle it in emitBodyRethrowPad. We
        // specify a relativeDepth of '1' to delegate outside of the still
        // active try block.
        uint32_t relativeDepth = 1;
        if (!delegatePadPatches(control.tryControl->landingPadPatches,
                                relativeDepth)) {
          return false;
        }
        break;
      }
      case LabelKind::Catch: {
        MOZ_ASSERT(!control.tryControl->inBody);
        // This is a try without a catch_all, we must have a rethrow at the end
        // of the landing pad (if any).
        MBasicBlock* padBlock = control.block;
        if (padBlock) {
          MBasicBlock* prevBlock = curBlock_;
          curBlock_ = padBlock;
          if (!throwFrom(control.tryControl->pendingException,
                         control.tryControl->pendingExceptionTag)) {
            return false;
          }
          curBlock_ = prevBlock;
        }
        break;
      }
      case LabelKind::CatchAll: {
        MOZ_ASSERT(!control.tryControl->inBody);
        // This is a try with a catch_all, and requires no special handling.
        break;
      }
      default:
        MOZ_CRASH();
    }

    // Finish the block, joining the try and catch blocks
    return finishBlock(defs);
  }

  [[nodiscard]] bool finishTryTable(Control& control, DefVector* defs) {
    // Mark this control as no longer in the body of the try
    control.tryControl->inBody = false;
    // Create a landing pad for all of the catches
    if (!createTryTableLandingPad(control.tryControl.get())) {
      return false;
    }
    // Finish the block, joining the try and catch blocks
    return finishBlock(defs);
  }

  [[nodiscard]] bool emitBodyRethrowPad(Control& control) {
    // If there are no throwing instructions pending, we don't need to do
    // anything
    if (bodyRethrowPadPatches_.empty()) {
      return true;
    }

    // Create a landing pad for any throwing instructions
    MBasicBlock* padBlock;
    if (!createTryLandingPad(bodyRethrowPadPatches_, &padBlock)) {
      return false;
    }

    // If we're inlined into another function, we save the landing pad to be
    // linked later directly to our caller's landing pad. See
    // `finishedInlinedCallDirect`.
    if (callerCompiler_ && callerCompiler_->inTryCode()) {
      pendingInlineCatchBlock_ = padBlock;
      return true;
    }

    // Otherwise we need to grab the pending exception and rethrow it.
    MDefinition* pendingException;
    MDefinition* pendingExceptionTag;
    if (!consumePendingException(&padBlock, &pendingException,
                                 &pendingExceptionTag)) {
      return false;
    }

    // Switch to the landing pad and rethrow the exception
    MBasicBlock* prevBlock = curBlock_;
    curBlock_ = padBlock;
    if (!throwFrom(pendingException, pendingExceptionTag)) {
      return false;
    }
    curBlock_ = prevBlock;

    MOZ_ASSERT(bodyRethrowPadPatches_.empty());
    return true;
  }

  [[nodiscard]] bool emitNewException(MDefinition* tag,
                                      MDefinition** exception) {
    return emitInstanceCall1(readBytecodeOffset(), SASigExceptionNew, tag,
                             exception);
  }

  [[nodiscard]] bool emitThrow(uint32_t tagIndex, const DefVector& argValues) {
    if (inDeadCode()) {
      return true;
    }
    uint32_t bytecodeOffset = readBytecodeOffset();

    // Load the tag
    MDefinition* tag = loadTag(tagIndex);
    if (!tag) {
      return false;
    }

    // Allocate an exception object
    MDefinition* exception;
    if (!emitNewException(tag, &exception)) {
      return false;
    }

    // Load the data pointer from the object
    auto* data = MWasmLoadField::New(
        alloc(), exception, nullptr, WasmExceptionObject::offsetOfData(),
        mozilla::Nothing(), MIRType::Pointer, MWideningOp::None,
        AliasSet::Load(AliasSet::Any));
    if (!data) {
      return false;
    }
    curBlock_->add(data);

    // Store the params into the data pointer
    SharedTagType tagType = codeMeta().tags[tagIndex].type;
    for (size_t i = 0; i < tagType->argOffsets().length(); i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      ValType type = tagType->argTypes()[i];
      uint32_t offset = tagType->argOffsets()[i];

      if (!type.isRefRepr()) {
        auto* store = MWasmStoreField::New(
            alloc(), data, exception, offset, mozilla::Nothing(), argValues[i],
            MNarrowingOp::None, AliasSet::Store(AliasSet::Any));
        if (!store) {
          return false;
        }
        curBlock_->add(store);
        continue;
      }

      // Store the new value
      auto* store = MWasmStoreFieldRef::New(
          alloc(), instancePointer_, data, exception, offset,
          mozilla::Nothing(), argValues[i], AliasSet::Store(AliasSet::Any),
          Nothing(), WasmPreBarrierKind::None);
      if (!store) {
        return false;
      }
      curBlock_->add(store);

      // Call the post-write barrier
      if (!postBarrierWholeCell(bytecodeOffset, exception, argValues[i])) {
        return false;
      }
    }

    // Throw the exception
    return throwFrom(exception, tag);
  }

  [[nodiscard]] bool emitThrowRef(MDefinition* exnRef) {
    if (inDeadCode()) {
      return true;
    }

    // The exception must be non-null
    exnRef = refAsNonNull(exnRef);
    if (!exnRef) {
      return false;
    }

    // Call Instance::throwException to perform tag unpacking and throw the
    // exception
    if (!emitInstanceCall1(readBytecodeOffset(), SASigThrowException, exnRef)) {
      return false;
    }
    unreachableTrap();

    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]] bool throwFrom(MDefinition* exn, MDefinition* tag) {
    if (inDeadCode()) {
      return true;
    }

    // Check if there is a local catching try control, and if so, then add a
    // pad-patch to its tryPadPatches.
    ControlInstructionVector* tryLandingPadPatches;
    if (inTryBlock(&tryLandingPadPatches)) {
      // Set the pending exception state, the landing pad will read from this
      if (!setPendingExceptionState(exn, tag)) {
        return false;
      }

      // End with a pending jump to the landing pad
      if (!endWithPadPatch(tryLandingPadPatches)) {
        return false;
      }
      curBlock_ = nullptr;
      return true;
    }

    // If there is no surrounding catching block, call an instance method to
    // throw the exception.
    if (!emitInstanceCall1(readBytecodeOffset(), SASigThrowException, exn)) {
      return false;
    }
    unreachableTrap();

    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]] bool emitRethrow(uint32_t relativeDepth) {
    if (inDeadCode()) {
      return true;
    }

    Control& control = iter().controlItem(relativeDepth);
    MOZ_ASSERT(iter().controlKind(relativeDepth) == LabelKind::Catch ||
               iter().controlKind(relativeDepth) == LabelKind::CatchAll);
    return throwFrom(control.tryControl->pendingException,
                     control.tryControl->pendingExceptionTag);
  }

  /******************************** WasmGC: low level load/store helpers ***/

  // Given a (StorageType, FieldExtension) pair, produce the (MIRType,
  // MWideningOp) pair that will give the correct operation for reading the
  // value from memory.
  static void fieldLoadInfoToMIR(StorageType type, FieldWideningOp wideningOp,
                                 MIRType* mirType, MWideningOp* mirWideningOp) {
    switch (type.kind()) {
      case StorageType::I8: {
        switch (wideningOp) {
          case FieldWideningOp::Signed:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromS8;
            return;
          case FieldWideningOp::Unsigned:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromU8;
            return;
          default:
            MOZ_CRASH();
        }
      }
      case StorageType::I16: {
        switch (wideningOp) {
          case FieldWideningOp::Signed:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromS16;
            return;
          case FieldWideningOp::Unsigned:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromU16;
            return;
          default:
            MOZ_CRASH();
        }
      }
      default: {
        switch (wideningOp) {
          case FieldWideningOp::None:
            *mirType = type.toMIRType();
            *mirWideningOp = MWideningOp::None;
            return;
          default:
            MOZ_CRASH();
        }
      }
    }
  }

  // Given a StorageType, return the Scale required when accessing array
  // elements of this type.
  static Scale scaleFromFieldType(StorageType type) {
    if (type.kind() == StorageType::V128) {
      // V128 is accessed differently, so this scale will not be used.
      return Scale::Invalid;
    }
    return ShiftToScale(type.indexingShift());
  }

  // Given a StorageType, produce the MNarrowingOp required for writing the
  // value to memory.
  static MNarrowingOp fieldStoreInfoToMIR(StorageType type) {
    switch (type.kind()) {
      case StorageType::I8:
        return MNarrowingOp::To8;
      case StorageType::I16:
        return MNarrowingOp::To16;
      default:
        return MNarrowingOp::None;
    }
  }

  // Generate a write of `value` at address `base + offset`, where `offset` is
  // known at JIT time.  If the written value is a reftype, the previous value
  // at `base + offset` will be retrieved and handed off to the post-write
  // barrier.  `keepAlive` will be referenced by the instruction so as to hold
  // it live (from the GC's point of view).
  [[nodiscard]] bool writeGcValueAtBasePlusOffset(
      uint32_t lineOrBytecode, StorageType type, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* value, MDefinition* base,
      uint32_t offset, uint32_t fieldIndex, bool needsTrapInfo,
      WasmPreBarrierKind preBarrierKind) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(type.widenToValType().toMIRType() == value->type());
    MNarrowingOp narrowingOp = fieldStoreInfoToMIR(type);

    if (!type.isRefRepr()) {
      MaybeTrapSiteDesc maybeTrap;
      if (needsTrapInfo) {
        maybeTrap.emplace(trapSiteDesc());
      }

      auto* store = MWasmStoreField::New(
          alloc(), base, keepAlive, offset, mozilla::Some(fieldIndex), value,
          narrowingOp, AliasSet::Store(aliasBitset), maybeTrap);
      if (!store) {
        return false;
      }
      curBlock_->add(store);
      return true;
    }

    // Otherwise it's a ref store.  Load the previous value so we can show it
    // to the post-write barrier.
    //
    // Optimisation opportunity: for the case where this field write results
    // from struct.new, the old value is always zero.  So we should synthesise
    // a suitable zero constant rather than reading it from the object.  See
    // also bug 1799999.
    MOZ_ASSERT(narrowingOp == MNarrowingOp::None);
    MOZ_ASSERT(type.widenToValType() == type.valType());

    // Store the new value
    auto* store = MWasmStoreFieldRef::New(
        alloc(), instancePointer_, base, keepAlive, offset,
        mozilla::Some(fieldIndex), value, AliasSet::Store(aliasBitset),
        mozilla::Some(trapSiteDesc()), preBarrierKind);
    if (!store) {
      return false;
    }
    curBlock_->add(store);

    // Call the post-write barrier
    return postBarrierWholeCell(lineOrBytecode, keepAlive, value);
  }

  // Generate a write of `value` at address `base + index * scale`, where
  // `scale` is known at JIT-time.  If the written value is a reftype, the
  // previous value at `base + index * scale` will be retrieved and handed off
  // to the post-write barrier.  `keepAlive` will be referenced by the
  // instruction so as to hold it live (from the GC's point of view).
  [[nodiscard]] bool writeGcValueAtBasePlusScaledIndex(
      uint32_t lineOrBytecode, StorageType type, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* value, MDefinition* base,
      uint32_t scale, MDefinition* index, WasmPreBarrierKind preBarrierKind) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(type.widenToValType().toMIRType() == value->type());
    MOZ_ASSERT(scale == 1 || scale == 2 || scale == 4 || scale == 8 ||
               scale == 16);

    MNarrowingOp narrowingOp = fieldStoreInfoToMIR(type);

    if (!type.isRefRepr()) {
      MaybeTrapSiteDesc maybeTrap;
      Scale scale = scaleFromFieldType(type);
      auto* store = MWasmStoreElement::New(
          alloc(), base, index, value, keepAlive, narrowingOp, scale,
          AliasSet::Store(aliasBitset), maybeTrap);
      if (!store) {
        return false;
      }
      curBlock_->add(store);
      return true;
    }

    // Otherwise it's a ref store.
    MOZ_ASSERT(narrowingOp == MNarrowingOp::None);
    MOZ_ASSERT(type.widenToValType() == type.valType());

    // Store the new value
    auto* store = MWasmStoreElementRef::New(
        alloc(), instancePointer_, base, index, value, keepAlive,
        AliasSet::Store(aliasBitset), mozilla::Some(trapSiteDesc()),
        preBarrierKind);
    if (!store) {
      return false;
    }
    curBlock_->add(store);

    return postBarrierEdgeAtIndex(lineOrBytecode, keepAlive, base, index,
                                  sizeof(void*), value);
  }

  // Generate a read from address `base + offset`, where `offset` is known at
  // JIT time.  The loaded value will be widened as described by `type` and
  // `fieldWideningOp`.  `keepAlive` will be referenced by the instruction so as
  // to hold it live (from the GC's point of view).
  [[nodiscard]] MDefinition* readGcValueAtBasePlusOffset(
      StorageType type, FieldWideningOp fieldWideningOp, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* base, uint32_t offset,
      uint32_t fieldIndex, bool needsTrapInfo) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);
    MIRType mirType;
    MWideningOp mirWideningOp;
    fieldLoadInfoToMIR(type, fieldWideningOp, &mirType, &mirWideningOp);
    MaybeTrapSiteDesc maybeTrap;
    if (needsTrapInfo) {
      maybeTrap.emplace(trapSiteDesc());
    }

    auto* load = MWasmLoadField::New(alloc(), base, keepAlive, offset,
                                     mozilla::Some(fieldIndex), mirType,
                                     mirWideningOp, AliasSet::Load(aliasBitset),
                                     maybeTrap, type.toMaybeRefType());
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  // Generate a read from address `base + index * scale`, where `scale` is
  // known at JIT-time.  The loaded value will be widened as described by
  // `type` and `fieldWideningOp`.  `keepAlive` will be referenced by the
  // instruction so as to hold it live (from the GC's point of view).
  [[nodiscard]] MDefinition* readGcArrayValueAtIndex(
      StorageType type, FieldWideningOp fieldWideningOp, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* base, MDefinition* index) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);

    MIRType mirType;
    MWideningOp mirWideningOp;
    fieldLoadInfoToMIR(type, fieldWideningOp, &mirType, &mirWideningOp);
    Scale scale = scaleFromFieldType(type);
    auto* load = MWasmLoadElement::New(
        alloc(), base, keepAlive, index, mirType, mirWideningOp, scale,
        AliasSet::Load(aliasBitset), mozilla::Some(trapSiteDesc()),
        type.toMaybeRefType());
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  /************************************************ WasmGC: type helpers ***/

  // Returns an MDefinition holding the supertype vector for `typeIndex`.
  [[nodiscard]] MDefinition* loadSuperTypeVector(uint32_t typeIndex) {
    uint32_t stvOffset = codeMeta().offsetOfSuperTypeVector(typeIndex);

    auto* load =
        MWasmLoadInstanceDataField::New(alloc(), MIRType::Pointer, stvOffset,
                                        /*isConst=*/true, instancePointer_);
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  uint32_t readAllocSiteIndex(uint32_t typeIndex) {
    if (!codeTailMeta() || !codeTailMeta()->hasFuncDefAllocSites()) {
      // For single tier of optimized compilation, there are no assigned alloc
      // sites, using type index as alloc site.
      return typeIndex;
    }
    AllocSitesRange rangeInModule =
        codeTailMeta()->getFuncDefAllocSites(funcIndex());
    uint32_t localIndex = numAllocSites_++;
    MOZ_RELEASE_ASSERT(localIndex < rangeInModule.length);
    return rangeInModule.begin + localIndex;
  }

  [[nodiscard]] MDefinition* loadAllocSiteInstanceData(
      uint32_t allocSiteIndex) {
    auto* allocSites = MWasmLoadInstance::New(
        alloc(), instancePointer_, Instance::offsetOfAllocSites(),
        MIRType::Pointer, AliasSet::None());
    if (!allocSites) {
      return nullptr;
    }
    curBlock_->add(allocSites);

    auto* result = MWasmDerivedPointer::New(
        alloc(), allocSites, allocSiteIndex * sizeof(gc::AllocSite));
    if (!result) {
      return nullptr;
    }
    curBlock_->add(result);
    return result;
  }

  /********************************************** WasmGC: struct helpers ***/

  [[nodiscard]] MDefinition* createStructObject(uint32_t typeIndex,
                                                uint32_t allocSiteIndex,
                                                bool zeroFields) {
    // Allocate an uninitialized struct.
    MDefinition* allocSite = loadAllocSiteInstanceData(allocSiteIndex);
    if (!allocSite) {
      return nullptr;
    }

    const TypeDef* typeDef = &(*codeMeta().types)[typeIndex];
    auto* structObject =
        MWasmNewStructObject::New(alloc(), instancePointer_, allocSite, typeDef,
                                  zeroFields, trapSiteDesc());
    if (!structObject) {
      return nullptr;
    }
    curBlock_->add(structObject);

    return structObject;
  }

  // Helper function for EmitStruct{New,Set}: given a MIR pointer to a
  // WasmStructObject, a MIR pointer to a value, and a field descriptor,
  // generate MIR to write the value to the relevant field in the object.
  [[nodiscard]] bool writeValueToStructField(
      uint32_t lineOrBytecode, const StructType& structType,
      uint32_t fieldIndex, MDefinition* structObject, MDefinition* value,
      WasmPreBarrierKind preBarrierKind) {
    StorageType fieldType = structType.fields_[fieldIndex].type;
    uint32_t fieldOffset = structType.fieldOffset(fieldIndex);

    bool areaIsOutline;
    uint32_t areaOffset;
    WasmStructObject::fieldOffsetToAreaAndOffset(fieldType, fieldOffset,
                                                 &areaIsOutline, &areaOffset);

    // Make `base` point at the first byte of either the struct object as a
    // whole or of the out-of-line data area.  And adjust `areaOffset`
    // accordingly.
    MDefinition* base;
    bool needsTrapInfo;
    if (areaIsOutline) {
      auto* loadDataPointer = MWasmLoadField::New(
          alloc(), structObject, nullptr,
          WasmStructObject::offsetOfOutlineData(), mozilla::Nothing(),
          MIRType::Pointer, MWideningOp::None,
          AliasSet::Load(AliasSet::WasmStructOutlineDataPointer),
          mozilla::Some(trapSiteDesc()));
      if (!loadDataPointer) {
        return false;
      }
      curBlock_->add(loadDataPointer);
      base = loadDataPointer;
      needsTrapInfo = false;
    } else {
      base = structObject;
      needsTrapInfo = true;
      areaOffset += WasmStructObject::offsetOfInlineData();
    }
    // The transaction is to happen at `base + areaOffset`, so to speak.
    // After this point we must ignore `fieldOffset`.

    // The alias set denoting the field's location, although lacking a
    // Load-vs-Store indication at this point.
    AliasSet::Flag fieldAliasSet = areaIsOutline
                                       ? AliasSet::WasmStructOutlineDataArea
                                       : AliasSet::WasmStructInlineDataArea;

    return writeGcValueAtBasePlusOffset(
        lineOrBytecode, fieldType, structObject, fieldAliasSet, value, base,
        areaOffset, fieldIndex, needsTrapInfo, preBarrierKind);
  }

  // Helper function for EmitStructGet: given a MIR pointer to a
  // WasmStructObject, a field descriptor and a field widening operation,
  // generate MIR to read the value from the relevant field in the object.
  [[nodiscard]] MDefinition* readValueFromStructField(
      const StructType& structType, uint32_t fieldIndex,
      FieldWideningOp wideningOp, MDefinition* structObject) {
    StorageType fieldType = structType.fields_[fieldIndex].type;
    uint32_t fieldOffset = structType.fieldOffset(fieldIndex);

    bool areaIsOutline;
    uint32_t areaOffset;
    WasmStructObject::fieldOffsetToAreaAndOffset(fieldType, fieldOffset,
                                                 &areaIsOutline, &areaOffset);

    // Make `base` point at the first byte of either the struct object as a
    // whole or of the out-of-line data area.  And adjust `areaOffset`
    // accordingly.
    MDefinition* base;
    bool needsTrapInfo;
    if (areaIsOutline) {
      auto* loadDataPointer = MWasmLoadField::New(
          alloc(), structObject, nullptr,
          WasmStructObject::offsetOfOutlineData(), mozilla::Nothing(),
          MIRType::Pointer, MWideningOp::None,
          AliasSet::Load(AliasSet::WasmStructOutlineDataPointer),
          mozilla::Some(trapSiteDesc()));
      if (!loadDataPointer) {
        return nullptr;
      }
      curBlock_->add(loadDataPointer);
      base = loadDataPointer;
      needsTrapInfo = false;
    } else {
      base = structObject;
      needsTrapInfo = true;
      areaOffset += WasmStructObject::offsetOfInlineData();
    }
    // The transaction is to happen at `base + areaOffset`, so to speak.
    // After this point we must ignore `fieldOffset`.

    // The alias set denoting the field's location, although lacking a
    // Load-vs-Store indication at this point.
    AliasSet::Flag fieldAliasSet = areaIsOutline
                                       ? AliasSet::WasmStructOutlineDataArea
                                       : AliasSet::WasmStructInlineDataArea;

    return readGcValueAtBasePlusOffset(fieldType, wideningOp, structObject,
                                       fieldAliasSet, base, areaOffset,
                                       fieldIndex, needsTrapInfo);
  }

  /********************************* WasmGC: address-arithmetic helpers ***/

  inline bool targetIs64Bit() const {
#ifdef JS_64BIT
    return true;
#else
    return false;
#endif
  }

  // Generate MIR to unsigned widen `val` out to the target word size.  If
  // `val` is already at the target word size, this is a no-op.  The only
  // other allowed case is where `val` is Int32 and we're compiling for a
  // 64-bit target, in which case a widen is generated.
  [[nodiscard]] MDefinition* unsignedWidenToTargetWord(MDefinition* val) {
    if (targetIs64Bit()) {
      if (val->type() == MIRType::Int32) {
        auto* ext = MExtendInt32ToInt64::New(alloc(), val, /*isUnsigned=*/true);
        if (!ext) {
          return nullptr;
        }
        curBlock_->add(ext);
        return ext;
      }
      MOZ_ASSERT(val->type() == MIRType::Int64);
      return val;
    }
    MOZ_ASSERT(val->type() == MIRType::Int32);
    return val;
  }

  /********************************************** WasmGC: array helpers ***/

  // Given `arrayObject`, the address of a WasmArrayObject, generate MIR to
  // return the contents of the WasmArrayObject::numElements_ field.
  // Adds trap site info for the null check.
  [[nodiscard]] MDefinition* getWasmArrayObjectNumElements(
      MDefinition* arrayObject) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);

    auto* numElements = MWasmLoadField::New(
        alloc(), arrayObject, nullptr, WasmArrayObject::offsetOfNumElements(),
        mozilla::Nothing(), MIRType::Int32, MWideningOp::None,
        AliasSet::Load(AliasSet::WasmArrayNumElements),
        mozilla::Some(trapSiteDesc()));
    if (!numElements) {
      return nullptr;
    }
    curBlock_->add(numElements);

    return numElements;
  }

  // Given `arrayObject`, the address of a WasmArrayObject, generate MIR to
  // return the contents of the WasmArrayObject::data_ field.
  [[nodiscard]] MDefinition* getWasmArrayObjectData(MDefinition* arrayObject) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);

    auto* data = MWasmLoadField::New(
        alloc(), arrayObject, nullptr, WasmArrayObject::offsetOfData(),
        mozilla::Nothing(), MIRType::WasmArrayData, MWideningOp::None,
        AliasSet::Load(AliasSet::WasmArrayDataPointer),
        mozilla::Some(trapSiteDesc()));
    if (!data) {
      return nullptr;
    }
    curBlock_->add(data);

    return data;
  }

  // Given a JIT-time-known type index `typeIndex` and a run-time known number
  // of elements `numElements`, create MIR to allocate a new wasm array,
  // possibly initialized with `typeIndex`s default value.
  [[nodiscard]] MDefinition* createArrayObject(uint32_t typeIndex,
                                               uint32_t allocSiteIndex,
                                               MDefinition* numElements,
                                               bool zeroFields) {
    MDefinition* allocSite = loadAllocSiteInstanceData(allocSiteIndex);
    if (!allocSite) {
      return nullptr;
    }

    const TypeDef* typeDef = &(*codeMeta().types)[typeIndex];
    auto* arrayObject = MWasmNewArrayObject::New(
        alloc(), instancePointer_, numElements, allocSite, typeDef, zeroFields,
        trapSiteDesc());
    if (!arrayObject) {
      return nullptr;
    }
    curBlock_->add(arrayObject);

    return arrayObject;
  }

  // This emits MIR to perform several actions common to array loads and
  // stores.  Given `arrayObject`, that points to a WasmArrayObject, and an
  // index value `index`, it:
  //
  // * Generates a trap if the array pointer is null
  // * Gets the size of the array
  // * Emits a bounds check of `index` against the array size
  // * Retrieves the OOL object pointer from the array
  // * Includes check for null via signal handler.
  //
  // The returned value is for the OOL object pointer.
  [[nodiscard]] MDefinition* setupForArrayAccess(MDefinition* arrayObject,
                                                 MDefinition* index) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(index->type() == MIRType::Int32);

    // Check for null is done in getWasmArrayObjectNumElements.

    // Get the size value for the array.
    MDefinition* numElements = getWasmArrayObjectNumElements(arrayObject);
    if (!numElements) {
      return nullptr;
    }

    // Create a bounds check.
    auto* boundsCheck =
        MWasmBoundsCheck::New(alloc(), index, numElements, trapSiteDesc(),
                              MWasmBoundsCheck::Target::Unknown);
    if (!boundsCheck) {
      return nullptr;
    }
    curBlock_->add(boundsCheck);

    // Get the address of the first byte of the (OOL) data area.
    return getWasmArrayObjectData(arrayObject);
  }

  [[nodiscard]] bool fillArray(uint32_t lineOrBytecode,
                               const ArrayType& arrayType,
                               MDefinition* arrayObject, MDefinition* index,
                               MDefinition* numElements, MDefinition* val,
                               WasmPreBarrierKind preBarrierKind) {
    mozilla::DebugOnly<MIRType> valMIRType = val->type();
    StorageType elemType = arrayType.elementType();
    MOZ_ASSERT(elemType.widenToValType().toMIRType() == valMIRType);

    uint32_t elemSize = elemType.size();
    MOZ_ASSERT(elemSize >= 1 && elemSize <= 16);

    // Make `arrayBase` point at the first byte of the (OOL) data area.
    MDefinition* arrayBase = getWasmArrayObjectData(arrayObject);
    if (!arrayBase) {
      return false;
    }

    // We have:
    //   arrayBase   : TargetWord
    //   index       : Int32
    //   numElements : Int32
    //   val         : <any StorageType>
    //   $elemSize = arrayType.elementType_.size(); 1, 2, 4, 8 or 16
    //
    // Generate MIR:
    //   <in current block>
    //     limit : Int32 = index + numElements
    //     if (limit == index) goto after; // skip loop if trip count == 0
    //   loop:
    //     indexPhi = phi(index, indexNext)
    //     arrayBase[index * $elemSize] = val
    //     indexNext = indexPhi + 1
    //     if (indexNext <u limit) goto loop;
    //   after:
    //
    // We construct the loop "manually" rather than using
    // FunctionCompiler::{startLoop,closeLoop} as the latter have awareness of
    // the wasm view of loops, whereas the loop we're building here is not a
    // wasm-level loop.
    // ==== Create the "loop" and "after" blocks ====
    MBasicBlock* loopBlock;
    if (!newBlock(curBlock_, &loopBlock, MBasicBlock::LOOP_HEADER)) {
      return false;
    }
    MBasicBlock* afterBlock;
    if (!newBlock(loopBlock, &afterBlock)) {
      return false;
    }

    // ==== Fill in the remainder of the block preceding the loop ====
    MAdd* limit = MAdd::NewWasm(alloc(), index, numElements, MIRType::Int32);
    if (!limit) {
      return false;
    }
    curBlock_->add(limit);

    // Note: the comparison (and eventually the entire initialisation loop) will
    // be folded out in the case where the number of elements is zero.
    // See MCompare::tryFoldEqualOperands.
    MDefinition* limitEqualsBase =
        compare(limit, index, JSOp::StrictEq, MCompare::Compare_UInt32);
    if (!limitEqualsBase) {
      return false;
    }
    MTest* skipIfLimitEqualsBase =
        MTest::New(alloc(), limitEqualsBase, afterBlock, loopBlock);
    if (!skipIfLimitEqualsBase) {
      return false;
    }
    curBlock_->end(skipIfLimitEqualsBase);
    if (!afterBlock->addPredecessor(alloc(), curBlock_)) {
      return false;
    }

    // ==== Fill in the loop block as best we can ====
    curBlock_ = loopBlock;
    MPhi* indexPhi = MPhi::New(alloc(), MIRType::Int32);
    if (!indexPhi) {
      return false;
    }
    if (!indexPhi->reserveLength(2)) {
      return false;
    }
    indexPhi->addInput(index);
    curBlock_->addPhi(indexPhi);
    curBlock_->setLoopDepth(rootCompiler_.loopDepth() + 1);

    if (!writeGcValueAtBasePlusScaledIndex(
            lineOrBytecode, elemType, arrayObject, AliasSet::WasmArrayDataArea,
            val, arrayBase, elemSize, indexPhi, preBarrierKind)) {
      return false;
    }

    auto* indexNext =
        MAdd::NewWasm(alloc(), indexPhi, constantI32(1), MIRType::Int32);
    if (!indexNext) {
      return false;
    }
    curBlock_->add(indexNext);
    indexPhi->addInput(indexNext);

    MDefinition* indexNextLtuLimit =
        compare(indexNext, limit, JSOp::Lt, MCompare::Compare_UInt32);
    if (!indexNextLtuLimit) {
      return false;
    }
    auto* continueIfIndexNextLtuLimit =
        MTest::New(alloc(), indexNextLtuLimit, loopBlock, afterBlock);
    if (!continueIfIndexNextLtuLimit) {
      return false;
    }
    curBlock_->end(continueIfIndexNextLtuLimit);
    if (!loopBlock->addPredecessor(alloc(), loopBlock)) {
      return false;
    }
    // ==== Loop block completed ====

    curBlock_ = afterBlock;
    return true;
  }

  // This routine generates all MIR required for `array.new`.  The returned
  // value is for the newly created array.
  [[nodiscard]] MDefinition* createArrayNewCallAndLoop(uint32_t lineOrBytecode,
                                                       uint32_t typeIndex,
                                                       uint32_t allocSiteIndex,
                                                       MDefinition* numElements,
                                                       MDefinition* fillValue) {
    // Create the array object, uninitialized.
    MDefinition* arrayObject =
        createArrayObject(typeIndex, allocSiteIndex, numElements,
                          /*zeroFields=*/false);
    if (!arrayObject) {
      return nullptr;
    }

    // Optimisation opportunity: if the fill value is zero, maybe we should
    // likewise skip over the initialisation loop entirely (and, if the zero
    // value is visible at JIT time, the loop will be removed).  For the
    // reftyped case, that would be a big win since each iteration requires a
    // call to the post-write barrier routine.

    const ArrayType& arrayType = (*codeMeta().types)[typeIndex].arrayType();
    if (!fillArray(lineOrBytecode, arrayType, arrayObject, constantI32(0),
                   numElements, fillValue, WasmPreBarrierKind::None)) {
      return nullptr;
    }

    return arrayObject;
  }

  [[nodiscard]] bool createArrayCopy(uint32_t lineOrBytecode,
                                     MDefinition* dstArrayObject,
                                     MDefinition* dstArrayIndex,
                                     MDefinition* srcArrayObject,
                                     MDefinition* srcArrayIndex,
                                     MDefinition* numElements, int32_t elemSize,
                                     bool elemsAreRefTyped) {
    // Check for null is done in getWasmArrayObjectNumElements.

    // Get the arrays' actual sizes.
    MDefinition* dstNumElements = getWasmArrayObjectNumElements(dstArrayObject);
    if (!dstNumElements) {
      return false;
    }
    MDefinition* srcNumElements = getWasmArrayObjectNumElements(srcArrayObject);
    if (!srcNumElements) {
      return false;
    }

    // Create the bounds checks.
    MInstruction* dstBoundsCheck = MWasmBoundsCheckRange32::New(
        alloc(), dstArrayIndex, numElements, dstNumElements, trapSiteDesc());
    if (!dstBoundsCheck) {
      return false;
    }
    curBlock_->add(dstBoundsCheck);

    MInstruction* srcBoundsCheck = MWasmBoundsCheckRange32::New(
        alloc(), srcArrayIndex, numElements, srcNumElements, trapSiteDesc());
    if (!srcBoundsCheck) {
      return false;
    }
    curBlock_->add(srcBoundsCheck);

    // Check if numElements != 0 -- optimization to not invoke builtins.
    MBasicBlock* copyBlock = nullptr;
    if (!newBlock(curBlock_, &copyBlock)) {
      return false;
    }
    MBasicBlock* joinBlock = nullptr;
    if (!newBlock(curBlock_, &joinBlock)) {
      return false;
    }

    MInstruction* condition =
        MCompare::NewWasm(alloc(), numElements, constantI32(0), JSOp::StrictEq,
                          MCompare::Compare_UInt32);
    curBlock_->add(condition);

    MTest* test = MTest::New(alloc(), condition, joinBlock, copyBlock);
    if (!test) {
      return false;
    }
    curBlock_->end(test);
    curBlock_ = copyBlock;

    MInstruction* dstData = MWasmLoadField::New(
        alloc(), dstArrayObject, nullptr, WasmArrayObject::offsetOfData(),
        mozilla::Nothing(), MIRType::WasmArrayData, MWideningOp::None,
        AliasSet::Load(AliasSet::WasmArrayDataPointer));
    if (!dstData) {
      return false;
    }
    curBlock_->add(dstData);

    MInstruction* srcData = MWasmLoadField::New(
        alloc(), srcArrayObject, nullptr, WasmArrayObject::offsetOfData(),
        mozilla::Nothing(), MIRType::WasmArrayData, MWideningOp::None,
        AliasSet::Load(AliasSet::WasmArrayDataPointer));
    if (!srcData) {
      return false;
    }
    curBlock_->add(srcData);

    if (elemsAreRefTyped) {
      MOZ_RELEASE_ASSERT(elemSize == sizeof(void*));

      if (!builtinCall5(SASigArrayRefsMove, lineOrBytecode, dstData,
                        dstArrayIndex, srcData, srcArrayIndex, numElements,
                        nullptr)) {
        return false;
      }
    } else {
      MDefinition* elemSizeDef = constantI32(elemSize);
      if (!elemSizeDef) {
        return false;
      }

      if (!builtinCall6(SASigArrayMemMove, lineOrBytecode, dstData,
                        dstArrayIndex, srcData, srcArrayIndex, elemSizeDef,
                        numElements, nullptr)) {
        return false;
      }
    }

    MGoto* fallthrough = MGoto::New(alloc(), joinBlock);
    if (!fallthrough) {
      return false;
    }
    curBlock_->end(fallthrough);
    if (!joinBlock->addPredecessor(alloc(), curBlock_)) {
      return false;
    }
    curBlock_ = joinBlock;
    return true;
  }

  [[nodiscard]] bool createArrayFill(uint32_t lineOrBytecode,
                                     uint32_t typeIndex,
                                     MDefinition* arrayObject,
                                     MDefinition* index, MDefinition* val,
                                     MDefinition* numElements) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(index->type() == MIRType::Int32);
    MOZ_ASSERT(numElements->type() == MIRType::Int32);

    const ArrayType& arrayType = (*codeMeta().types)[typeIndex].arrayType();

    // Check for null is done in getWasmArrayObjectNumElements.

    // Get the array's actual size.
    MDefinition* actualNumElements = getWasmArrayObjectNumElements(arrayObject);
    if (!actualNumElements) {
      return false;
    }

    // Create a bounds check.
    auto* boundsCheck = MWasmBoundsCheckRange32::New(
        alloc(), index, numElements, actualNumElements, trapSiteDesc());
    if (!boundsCheck) {
      return false;
    }
    curBlock_->add(boundsCheck);

    return fillArray(lineOrBytecode, arrayType, arrayObject, index, numElements,
                     val, WasmPreBarrierKind::Normal);
  }

  /*********************************************** WasmGC: other helpers ***/

  // Generate MIR that causes a trap of kind `trapKind` if `arg` is zero.
  // Currently `arg` may only be a MIRType::Int32, but that requirement could
  // be relaxed if needed in future.
  [[nodiscard]] bool trapIfZero(wasm::Trap trapKind, MDefinition* arg) {
    MOZ_ASSERT(arg->type() == MIRType::Int32);

    MBasicBlock* trapBlock = nullptr;
    if (!newBlock(curBlock_, &trapBlock)) {
      return false;
    }

    auto* trap = MWasmTrap::New(alloc(), trapKind, trapSiteDesc());
    if (!trap) {
      return false;
    }
    trapBlock->end(trap);

    MBasicBlock* joinBlock = nullptr;
    if (!newBlock(curBlock_, &joinBlock)) {
      return false;
    }

    auto* test = MTest::New(alloc(), arg, joinBlock, trapBlock);
    if (!test) {
      return false;
    }
    curBlock_->end(test);
    curBlock_ = joinBlock;
    return true;
  }

  // Generate MIR that attempts to cast `ref` to `castToTypeDef`.  If the
  // cast fails, we trap.  If it succeeds, then `ref` can be assumed to
  // have a type that is a subtype of (or the same as) `castToTypeDef` after
  // this point.
  [[nodiscard]] MDefinition* refCast(MDefinition* ref, RefType destType) {
    MInstruction* cast = nullptr;
    if (destType.isTypeRef()) {
      uint32_t typeIndex = codeMeta().types->indexOf(*destType.typeDef());
      MDefinition* superSTV = loadSuperTypeVector(typeIndex);
      if (!superSTV) {
        return nullptr;
      }
      cast = MWasmRefCastConcrete::New(alloc(), ref, superSTV, destType,
                                       trapSiteDesc());
    } else {
      cast = MWasmRefCastAbstract::New(alloc(), ref, destType, trapSiteDesc());
    }

    if (!cast) {
      return nullptr;
    }
    curBlock_->add(cast);
    return cast;
  }

  // Generate MIR that computes a boolean value indicating whether or not it
  // is possible to cast `ref` to `destType`.
  [[nodiscard]] MDefinition* refTest(MDefinition* ref, RefType destType) {
    MInstruction* isSubTypeOf = nullptr;
    if (destType.isTypeRef()) {
      uint32_t typeIndex = codeMeta().types->indexOf(*destType.typeDef());
      MDefinition* superSTV = loadSuperTypeVector(typeIndex);
      if (!superSTV) {
        return nullptr;
      }
      isSubTypeOf = MWasmRefTestConcrete::New(alloc(), ref, superSTV, destType);
    } else {
      isSubTypeOf = MWasmRefTestAbstract::New(alloc(), ref, destType);
    }
    MOZ_ASSERT(isSubTypeOf);

    curBlock_->add(isSubTypeOf);
    return isSubTypeOf;
  }

  // Generates MIR for br_on_cast and br_on_cast_fail.
  [[nodiscard]] bool brOnCastCommon(bool onSuccess, uint32_t labelRelativeDepth,
                                    RefType sourceType, RefType destType,
                                    const ResultType& labelType,
                                    const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    // `values` are the values in the top block-value on the stack.  Since the
    // argument to `br_on_cast{_fail}` is at the top of the stack, it is the
    // last element in `values`.
    //
    // For both br_on_cast and br_on_cast_fail, the OpIter validation routines
    // ensure that `values` is non-empty (by rejecting the case
    // `labelType->length() < 1`) and that the last value in `values` is
    // reftyped.
    MOZ_RELEASE_ASSERT(values.length() > 0);
    MDefinition* ref = values.back();
    MOZ_ASSERT(ref->type() == MIRType::WasmAnyRef);

    MDefinition* success = refTest(ref, destType);
    if (!success) {
      return false;
    }

    MTest* test;
    if (onSuccess) {
      test = MTest::New(alloc(), success, nullptr, fallthroughBlock);
      if (!test || !addControlFlowPatch(test, labelRelativeDepth,
                                        MTest::TrueBranchIndex)) {
        return false;
      }
    } else {
      test = MTest::New(alloc(), success, fallthroughBlock, nullptr);
      if (!test || !addControlFlowPatch(test, labelRelativeDepth,
                                        MTest::FalseBranchIndex)) {
        return false;
      }
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = fallthroughBlock;
    return true;
  }

  [[nodiscard]] bool brOnNonStruct(const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    MOZ_ASSERT(values.length() > 0);
    MOZ_ASSERT(values.back()->type() == MIRType::WasmAnyRef);

    MGoto* jump = MGoto::New(alloc(), fallthroughBlock);
    if (!jump) {
      return false;
    }
    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(jump);
    curBlock_ = fallthroughBlock;
    return true;
  }

  [[nodiscard]] MDefinition* convertAnyExtern(MDefinition* ref,
                                              wasm::RefType::Kind kind) {
    auto* converted = MWasmRefConvertAnyExtern::New(alloc(), ref, kind);
    if (!converted) {
      return nullptr;
    }
    curBlock_->add(converted);
    return converted;
  }

  /************************************************************ DECODING ***/

  // AsmJS adds a line number to `callSiteLineNums` for certain operations that
  // are represented by a JS call, such as math builtins. We use these line
  // numbers when calling builtins. This method will read from
  // `callSiteLineNums` when we are using AsmJS, or else return the current
  // bytecode offset.
  //
  // This method MUST be called from opcodes that AsmJS will emit a call site
  // line number for, or else the arrays will get out of sync. Other opcodes
  // must use `readBytecodeOffset` below.
  uint32_t readCallSiteLineOrBytecode() {
    if (!func_.callSiteLineNums.empty()) {
      return func_.callSiteLineNums[lastReadCallSite_++];
    }
    return iter_.lastOpcodeOffset();
  }

  // Return the current bytecode offset.
  uint32_t readBytecodeOffset() { return iter_.lastOpcodeOffset(); }

  CallRefHint readCallRefHint() {
    // We don't track anything if we're not using lazy tiering
    if (compilerEnv().mode() != CompileMode::LazyTiering) {
      return CallRefHint();
    }

    CallRefMetricsRange rangeInModule =
        codeTailMeta()->getFuncDefCallRefs(funcIndex());
    uint32_t localIndex = numCallRefs_++;
    MOZ_RELEASE_ASSERT(localIndex < rangeInModule.length);
    uint32_t moduleIndex = rangeInModule.begin + localIndex;
    return codeTailMeta()->getCallRefHint(moduleIndex);
  }

#if DEBUG
  bool done() const { return iter_.done(); }
#endif

  /*************************************************************************/
 private:
  [[nodiscard]] bool newBlock(MBasicBlock* pred, MBasicBlock** block,
                              MBasicBlock::Kind kind = MBasicBlock::NORMAL) {
    *block = MBasicBlock::New(mirGraph(), info(), pred, kind);
    if (!*block) {
      return false;
    }
    mirGraph().addBlock(*block);
    (*block)->setLoopDepth(rootCompiler_.loopDepth());
    return true;
  }

  [[nodiscard]] bool goToNewBlock(MBasicBlock* pred, MBasicBlock** block) {
    if (!newBlock(pred, block)) {
      return false;
    }
    pred->end(MGoto::New(alloc(), *block));
    return true;
  }

  [[nodiscard]] bool goToExistingBlock(MBasicBlock* prev, MBasicBlock* next) {
    MOZ_ASSERT(prev);
    MOZ_ASSERT(next);
    prev->end(MGoto::New(alloc(), next));
    return next->addPredecessor(alloc(), prev);
  }

  [[nodiscard]] bool bindBranches(uint32_t absolute, DefVector* defs) {
    if (absolute >= pendingBlocks_.length() ||
        pendingBlocks_[absolute].patches.empty()) {
      return inDeadCode() || popPushedDefs(defs);
    }

    ControlFlowPatchVector& patches = pendingBlocks_[absolute].patches;
    MControlInstruction* ins = patches[0].ins;
    MBasicBlock* pred = ins->block();

    MBasicBlock* join = nullptr;
    if (!newBlock(pred, &join)) {
      return false;
    }

    // Use branch hinting information if any.
    if (pendingBlocks_[absolute].hint != BranchHint::Invalid) {
      join->setBranchHinting(pendingBlocks_[absolute].hint);
    }

    pred->mark();
    ins->replaceSuccessor(patches[0].index, join);

    for (size_t i = 1; i < patches.length(); i++) {
      ins = patches[i].ins;

      pred = ins->block();
      if (!pred->isMarked()) {
        if (!join->addPredecessor(alloc(), pred)) {
          return false;
        }
        pred->mark();
      }

      ins->replaceSuccessor(patches[i].index, join);
    }

    MOZ_ASSERT_IF(curBlock_, !curBlock_->isMarked());
    for (uint32_t i = 0; i < join->numPredecessors(); i++) {
      join->getPredecessor(i)->unmark();
    }

    if (curBlock_ && !goToExistingBlock(curBlock_, join)) {
      return false;
    }

    curBlock_ = join;

    if (!popPushedDefs(defs)) {
      return false;
    }

    patches.clear();
    return true;
  }

  bool emitI32Const();
  bool emitI64Const();
  bool emitF32Const();
  bool emitF64Const();
  bool emitBlock();
  bool emitLoop();
  bool emitIf();
  bool emitElse();
  bool emitEnd();
  bool emitBr();
  bool emitBrIf();
  bool emitBrTable();
  bool emitReturn();
  bool emitUnreachable();
  bool emitTry();
  bool emitCatch();
  bool emitCatchAll();
  bool emitTryTable();
  bool emitDelegate();
  bool emitThrow();
  bool emitThrowRef();
  bool emitRethrow();
  bool emitInlineCall(const FuncType& funcType, uint32_t funcIndex,
                      InliningHeuristics::CallKind callKind,
                      const DefVector& args, DefVector* results);
  bool emitCall(bool asmJSFuncDef);
  bool emitCallIndirect(bool oldStyle);
  bool emitStackSwitch();
  bool emitReturnCall();
  bool emitReturnCallIndirect();
  bool emitReturnCallRef();
  bool emitGetLocal();
  bool emitSetLocal();
  bool emitTeeLocal();
  bool emitGetGlobal();
  bool emitSetGlobal();
  bool emitTeeGlobal();
  template <typename MIRClass>
  bool emitUnary(ValType operandType);
  template <typename MIRClass>
  bool emitConversion(ValType operandType, ValType resultType);
  template <typename MIRClass>
  bool emitUnaryWithType(ValType operandType, MIRType mirType);
  template <typename MIRClass>
  bool emitConversionWithType(ValType operandType, ValType resultType,
                              MIRType mirType);
  bool emitTruncate(ValType operandType, ValType resultType, bool isUnsigned,
                    bool isSaturating);
  bool emitSignExtend(uint32_t srcSize, uint32_t targetSize);
  bool emitExtendI32(bool isUnsigned);
  bool emitConvertI64ToFloatingPoint(ValType resultType, MIRType mirType,
                                     bool isUnsigned);
  bool emitReinterpret(ValType resultType, ValType operandType,
                       MIRType mirType);
  bool emitAdd(ValType type, MIRType mirType);
  bool emitSub(ValType type, MIRType mirType);
  bool emitRotate(ValType type, bool isLeftRotation);
  bool emitBitNot(ValType operandType, MIRType mirType);
  bool emitBitwiseAndOrXor(ValType operandType, MIRType mirType,
                           MWasmBinaryBitwise::SubOpcode subOpc);
  template <typename MIRClass>
  bool emitShift(ValType operandType, MIRType mirType);
  bool emitUrsh(ValType operandType, MIRType mirType);
  bool emitMul(ValType operandType, MIRType mirType);
  bool emitDiv(ValType operandType, MIRType mirType, bool isUnsigned);
  bool emitRem(ValType operandType, MIRType mirType, bool isUnsigned);
  bool emitMinMax(ValType operandType, MIRType mirType, bool isMax);
  bool emitCopySign(ValType operandType);
  bool emitComparison(ValType operandType, JSOp compareOp,
                      MCompare::CompareType compareType);
  bool emitSelect(bool typed);
  bool emitLoad(ValType type, Scalar::Type viewType);
  bool emitStore(ValType resultType, Scalar::Type viewType);
  bool emitTeeStore(ValType resultType, Scalar::Type viewType);
  bool emitTeeStoreWithCoercion(ValType resultType, Scalar::Type viewType);
  bool tryInlineUnaryBuiltin(SymbolicAddress callee, MDefinition* input);
  bool emitUnaryMathBuiltinCall(const SymbolicAddressSignature& callee);
  bool emitBinaryMathBuiltinCall(const SymbolicAddressSignature& callee);
  bool emitMemoryGrow();
  bool emitMemorySize();
  bool emitAtomicCmpXchg(ValType type, Scalar::Type viewType);
  bool emitAtomicLoad(ValType type, Scalar::Type viewType);
  bool emitAtomicRMW(ValType type, Scalar::Type viewType, jit::AtomicOp op);
  bool emitAtomicStore(ValType type, Scalar::Type viewType);
  bool emitWait(ValType type, uint32_t byteSize);
  bool emitFence();
  bool emitNotify();
  bool emitAtomicXchg(ValType type, Scalar::Type viewType);
  bool emitMemCopyCall(uint32_t dstMemIndex, uint32_t srcMemIndex,
                       MDefinition* dst, MDefinition* src, MDefinition* len);
  bool emitMemCopyInline(uint32_t memoryIndex, MDefinition* dst,
                         MDefinition* src, uint32_t length);
  bool emitMemCopy();
  bool emitTableCopy();
  bool emitDataOrElemDrop(bool isData);
  bool emitMemFillCall(uint32_t memoryIndex, MDefinition* start,
                       MDefinition* val, MDefinition* len);
  bool emitMemFillInline(uint32_t memoryIndex, MDefinition* start,
                         MDefinition* val, uint32_t length);
  bool emitMemFill();
  bool emitMemInit();
  bool emitTableInit();
  bool emitTableFill();
  bool emitMemDiscard();
  bool emitTableGet();
  bool emitTableGrow();
  bool emitTableSet();
  bool emitTableSize();
  bool emitRefFunc();
  bool emitRefNull();
  bool emitRefIsNull();
  bool emitConstSimd128();
  bool emitBinarySimd128(bool commutative, SimdOp op);
  bool emitTernarySimd128(wasm::SimdOp op);
  bool emitShiftSimd128(SimdOp op);
  bool emitSplatSimd128(ValType inType, SimdOp op);
  bool emitUnarySimd128(SimdOp op);
  bool emitReduceSimd128(SimdOp op);
  bool emitExtractLaneSimd128(ValType outType, uint32_t laneLimit, SimdOp op);
  bool emitReplaceLaneSimd128(ValType laneType, uint32_t laneLimit, SimdOp op);
  bool emitShuffleSimd128();
  bool emitLoadSplatSimd128(Scalar::Type viewType, wasm::SimdOp splatOp);
  bool emitLoadExtendSimd128(wasm::SimdOp op);
  bool emitLoadZeroSimd128(Scalar::Type viewType, size_t numBytes);
  bool emitLoadLaneSimd128(uint32_t laneSize);
  bool emitStoreLaneSimd128(uint32_t laneSize);
  bool emitRefAsNonNull();
  bool emitBrOnNull();
  bool emitBrOnNonNull();
  bool emitSpeculativeInlineCallRef(uint32_t bytecodeOffset,
                                    const FuncType& funcType,
                                    CallRefHint expectedFuncIndices,
                                    MDefinition* actualCalleeFunc,
                                    const DefVector& args, DefVector* results);
  bool emitCallRef();
  bool emitStructNew();
  bool emitStructNewDefault();
  bool emitStructSet();
  bool emitStructGet(FieldWideningOp wideningOp);
  bool emitArrayNew();
  bool emitArrayNewDefault();
  bool emitArrayNewFixed();
  bool emitArrayNewData();
  bool emitArrayNewElem();
  bool emitArrayInitData();
  bool emitArrayInitElem();
  bool emitArraySet();
  bool emitArrayGet(FieldWideningOp wideningOp);
  bool emitArrayLen();
  bool emitArrayCopy();
  bool emitArrayFill();
  bool emitRefI31();
  bool emitI31Get(FieldWideningOp wideningOp);
  bool emitRefTest(bool nullable);
  bool emitRefCast(bool nullable);
  bool emitBrOnCast(bool onSuccess);
  bool emitAnyConvertExtern();
  bool emitExternConvertAny();
  bool emitCallBuiltinModuleFunc();

 public:
  bool emitBodyExprs();
};

template <>
MDefinition* FunctionCompiler::unary<MToFloat32>(MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MToFloat32::New(alloc(), op, mustPreserveNaN(op->type()));
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MWasmBuiltinTruncateToInt32>(
    MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MWasmBuiltinTruncateToInt32::New(
      alloc(), op, instancePointer_, trapSiteDescWithCallSiteLineNumber());
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MNot>(MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MNot::NewInt32(alloc(), op);
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MAbs>(MDefinition* op, MIRType type) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MAbs::NewWasm(alloc(), op, type);
  curBlock_->add(ins);
  return ins;
}

bool FunctionCompiler::emitI32Const() {
  int32_t i32;
  if (!iter().readI32Const(&i32)) {
    return false;
  }

  iter().setResult(constantI32(i32));
  return true;
}

bool FunctionCompiler::emitI64Const() {
  int64_t i64;
  if (!iter().readI64Const(&i64)) {
    return false;
  }

  iter().setResult(constantI64(i64));
  return true;
}

bool FunctionCompiler::emitF32Const() {
  float f32;
  if (!iter().readF32Const(&f32)) {
    return false;
  }

  iter().setResult(constantF32(f32));
  return true;
}

bool FunctionCompiler::emitF64Const() {
  double f64;
  if (!iter().readF64Const(&f64)) {
    return false;
  }

  iter().setResult(constantF64(f64));
  return true;
}

bool FunctionCompiler::emitBlock() {
  BlockType type;
  return iter().readBlock(&type) && startBlock();
}

bool FunctionCompiler::emitLoop() {
  BlockType type;
  if (!iter().readLoop(&type)) {
    return false;
  }

  MBasicBlock* loopHeader;
  if (!startLoop(&loopHeader, type.params().length())) {
    return false;
  }

  addInterruptCheck();

  iter().controlItem().block = loopHeader;
  return true;
}

bool FunctionCompiler::emitIf() {
  BranchHint branchHint =
      iter().getBranchHint(funcIndex(), relativeBytecodeOffset());

  BlockType type;
  MDefinition* condition = nullptr;
  if (!iter().readIf(&type, &condition)) {
    return false;
  }

  MBasicBlock* elseBlock;
  if (!branchAndStartThen(condition, &elseBlock)) {
    return false;
  }

  // Store the branch hint in the basic block.
  if (!inDeadCode() && branchHint != BranchHint::Invalid) {
    getCurBlock()->setBranchHinting(branchHint);
  }

  iter().controlItem().block = elseBlock;
  return true;
}

bool FunctionCompiler::emitElse() {
  ResultType paramType;
  ResultType resultType;
  DefVector thenValues;
  if (!iter().readElse(&paramType, &resultType, &thenValues)) {
    return false;
  }

  if (!pushDefs(thenValues)) {
    return false;
  }

  Control& control = iter().controlItem();
  return switchToElse(control.block, &control.block);
}

bool FunctionCompiler::emitEnd() {
  LabelKind kind;
  ResultType type;
  DefVector preJoinDefs;
  DefVector resultsForEmptyElse;
  if (!iter().readEnd(&kind, &type, &preJoinDefs, &resultsForEmptyElse)) {
    return false;
  }

  Control& control = iter().controlItem();
  MBasicBlock* block = control.block;

  if (!pushDefs(preJoinDefs)) {
    return false;
  }

  // Every label case is responsible to pop the control item at the appropriate
  // time for the label case
  DefVector postJoinDefs;
  switch (kind) {
    case LabelKind::Body: {
      MOZ_ASSERT(!control.tryControl);
      if (!emitBodyRethrowPad(control)) {
        return false;
      }
      if (!finishBlock(&postJoinDefs)) {
        return false;
      }
      if (!returnValues(std::move(postJoinDefs))) {
        return false;
      }
      iter().popEnd();
      MOZ_ASSERT(iter().controlStackEmpty());
      return iter().endFunction(iter().end());
    }
    case LabelKind::Block:
      MOZ_ASSERT(!control.tryControl);
      if (!finishBlock(&postJoinDefs)) {
        return false;
      }
      iter().popEnd();
      break;
    case LabelKind::Loop:
      MOZ_ASSERT(!control.tryControl);
      if (!closeLoop(block, &postJoinDefs)) {
        return false;
      }
      iter().popEnd();
      break;
    case LabelKind::Then: {
      MOZ_ASSERT(!control.tryControl);
      // If we didn't see an Else, create a trivial else block so that we create
      // a diamond anyway, to preserve Ion invariants.
      if (!switchToElse(block, &block)) {
        return false;
      }

      if (!pushDefs(resultsForEmptyElse)) {
        return false;
      }

      if (!joinIfElse(block, &postJoinDefs)) {
        return false;
      }
      iter().popEnd();
      break;
    }
    case LabelKind::Else:
      MOZ_ASSERT(!control.tryControl);
      if (!joinIfElse(block, &postJoinDefs)) {
        return false;
      }
      iter().popEnd();
      break;
    case LabelKind::Try:
    case LabelKind::Catch:
    case LabelKind::CatchAll:
      MOZ_ASSERT(control.tryControl);
      if (!finishTryCatch(kind, control, &postJoinDefs)) {
        return false;
      }
      rootCompiler().freeTryControl(std::move(control.tryControl));
      iter().popEnd();
      break;
    case LabelKind::TryTable:
      MOZ_ASSERT(control.tryControl);
      if (!finishTryTable(control, &postJoinDefs)) {
        return false;
      }
      rootCompiler().freeTryControl(std::move(control.tryControl));
      iter().popEnd();
      break;
  }

  MOZ_ASSERT_IF(!inDeadCode(), postJoinDefs.length() == type.length());
  iter().setResults(postJoinDefs.length(), postJoinDefs);

  return true;
}

bool FunctionCompiler::emitBr() {
  uint32_t relativeDepth;
  ResultType type;
  DefVector values;
  if (!iter().readBr(&relativeDepth, &type, &values)) {
    return false;
  }

  return br(relativeDepth, values);
}

bool FunctionCompiler::emitBrIf() {
  uint32_t relativeDepth;
  ResultType type;
  DefVector values;
  MDefinition* condition;

  BranchHint branchHint =
      iter().getBranchHint(funcIndex(), relativeBytecodeOffset());

  if (!iter().readBrIf(&relativeDepth, &type, &values, &condition)) {
    return false;
  }

  return brIf(relativeDepth, values, condition, branchHint);
}

bool FunctionCompiler::emitBrTable() {
  Uint32Vector depths;
  uint32_t defaultDepth;
  ResultType branchValueType;
  DefVector branchValues;
  MDefinition* index;
  if (!iter().readBrTable(&depths, &defaultDepth, &branchValueType,
                          &branchValues, &index)) {
    return false;
  }

  // If all the targets are the same, or there are no targets, we can just
  // use a goto. This is not just an optimization: MaybeFoldConditionBlock
  // assumes that tables have more than one successor.
  bool allSameDepth = true;
  for (uint32_t depth : depths) {
    if (depth != defaultDepth) {
      allSameDepth = false;
      break;
    }
  }

  if (allSameDepth) {
    return br(defaultDepth, branchValues);
  }

  return brTable(index, defaultDepth, depths, branchValues);
}

bool FunctionCompiler::emitReturn() {
  DefVector values;
  if (!iter().readReturn(&values)) {
    return false;
  }

  return returnValues(std::move(values));
}

bool FunctionCompiler::emitUnreachable() {
  if (!iter().readUnreachable()) {
    return false;
  }

  unreachableTrap();
  return true;
}

bool FunctionCompiler::emitTry() {
  BlockType type;
  if (!iter().readTry(&type)) {
    return false;
  }

  return startTry();
}

bool FunctionCompiler::emitCatch() {
  LabelKind kind;
  uint32_t tagIndex;
  ResultType paramType, resultType;
  DefVector tryValues;
  if (!iter().readCatch(&kind, &tagIndex, &paramType, &resultType,
                        &tryValues)) {
    return false;
  }

  // Pushing the results of the previous block, to properly join control flow
  // after the try and after each handler, as well as potential control flow
  // patches from other instrunctions. This is similar to what is done for
  // if-then-else control flow and for most other control control flow joins.
  if (!pushDefs(tryValues)) {
    return false;
  }

  return switchToCatch(iter().controlItem(), kind, tagIndex);
}

bool FunctionCompiler::emitCatchAll() {
  LabelKind kind;
  ResultType paramType, resultType;
  DefVector tryValues;
  if (!iter().readCatchAll(&kind, &paramType, &resultType, &tryValues)) {
    return false;
  }

  // Pushing the results of the previous block, to properly join control flow
  // after the try and after each handler, as well as potential control flow
  // patches from other instrunctions.
  if (!pushDefs(tryValues)) {
    return false;
  }

  return switchToCatch(iter().controlItem(), kind, CatchAllIndex);
}

bool FunctionCompiler::emitTryTable() {
  BlockType type;
  TryTableCatchVector catches;
  if (!iter().readTryTable(&type, &catches)) {
    return false;
  }

  return startTryTable(std::move(catches));
}

bool FunctionCompiler::emitDelegate() {
  uint32_t relativeDepth;
  ResultType resultType;
  DefVector tryValues;
  if (!iter().readDelegate(&relativeDepth, &resultType, &tryValues)) {
    return false;
  }

  Control& control = iter().controlItem();
  MBasicBlock* block = control.block;
  MOZ_ASSERT(control.tryControl);

  // Unless the entire try-delegate is dead code, delegate any pad-patches from
  // this try to the next try-block above relativeDepth.
  if (block) {
    ControlInstructionVector& padPatches =
        control.tryControl->landingPadPatches;
    if (!delegatePadPatches(padPatches, relativeDepth)) {
      return false;
    }
  }
  rootCompiler().freeTryControl(std::move(control.tryControl));
  iter().popDelegate();

  // Push the results of the previous block, and join control flow with
  // potential control flow patches from other instrunctions in the try code.
  // This is similar to what is done for EmitEnd.
  if (!pushDefs(tryValues)) {
    return false;
  }
  DefVector postJoinDefs;
  if (!finishBlock(&postJoinDefs)) {
    return false;
  }
  MOZ_ASSERT_IF(!inDeadCode(), postJoinDefs.length() == resultType.length());
  iter().setResults(postJoinDefs.length(), postJoinDefs);

  return true;
}

bool FunctionCompiler::emitThrow() {
  uint32_t tagIndex;
  DefVector argValues;
  if (!iter().readThrow(&tagIndex, &argValues)) {
    return false;
  }

  return emitThrow(tagIndex, argValues);
}

bool FunctionCompiler::emitThrowRef() {
  MDefinition* exnRef;
  if (!iter().readThrowRef(&exnRef)) {
    return false;
  }

  return emitThrowRef(exnRef);
}

bool FunctionCompiler::emitRethrow() {
  uint32_t relativeDepth;
  if (!iter().readRethrow(&relativeDepth)) {
    return false;
  }

  return emitRethrow(relativeDepth);
}

bool FunctionCompiler::emitInlineCall(const FuncType& funcType,
                                      uint32_t funcIndex,
                                      InliningHeuristics::CallKind callKind,
                                      const DefVector& args,
                                      DefVector* results) {
  UniqueChars error;
  const BytecodeRange& funcRange = codeTailMeta()->funcDefRange(funcIndex);
  BytecodeSpan funcBytecode = codeTailMeta()->funcDefBody(funcIndex);
  FuncCompileInput func(funcIndex, funcRange.start, funcBytecode.data(),
                        funcBytecode.data() + funcBytecode.size(),
                        Uint32Vector());
  Decoder d(func.begin, func.end, func.lineOrBytecode, &error);

  ValTypeVector locals;
  if (!DecodeLocalEntriesWithParams(d, codeMeta(), funcIndex, &locals)) {
    return false;
  }

  CompileInfo* compileInfo = rootCompiler().startInlineCall(
      this->funcIndex(), bytecodeOffset(), funcIndex, locals.length(),
      funcRange.size(), callKind);
  if (!compileInfo) {
    return false;
  }

  FunctionCompiler calleeCompiler(this, d, func, locals, *compileInfo);
  if (!calleeCompiler.initInline(args)) {
    MOZ_ASSERT(!error);
    return false;
  }

  if (!calleeCompiler.startBlock()) {
    MOZ_ASSERT(!error);
    return false;
  }

  if (!calleeCompiler.emitBodyExprs()) {
    MOZ_ASSERT(!error);
    return false;
  }

  calleeCompiler.finish();
  rootCompiler_.finishInlineCall();

  return finishInlinedCallDirect(calleeCompiler, results);
}

bool FunctionCompiler::emitCall(bool asmJSFuncDef) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t funcIndex;
  DefVector args;
  if (asmJSFuncDef) {
    if (!iter().readOldCallDirect(codeMeta().numFuncImports, &funcIndex,
                                  &args)) {
      return false;
    }
  } else {
    if (!iter().readCall(&funcIndex, &args)) {
      return false;
    }
  }

  if (inDeadCode()) {
    return true;
  }

  const FuncType& funcType = codeMeta().getFuncType(funcIndex);

  DefVector results;
  if (codeMeta().funcIsImport(funcIndex)) {
    BuiltinModuleFuncId knownFuncImport = codeMeta().knownFuncImport(funcIndex);
    if (knownFuncImport != BuiltinModuleFuncId::None) {
      const BuiltinModuleFunc& builtinModuleFunc =
          BuiltinModuleFuncs::getFromId(knownFuncImport);
      return callBuiltinModuleFunc(builtinModuleFunc, args);
    }

    uint32_t instanceDataOffset =
        codeMeta().offsetOfFuncImportInstanceData(funcIndex);
    if (!callImport(instanceDataOffset, lineOrBytecode, funcType, args,
                    &results)) {
      return false;
    }
  } else {
    const auto callKind = InliningHeuristics::CallKind::Direct;
    // Make up a single-entry CallRefHint and enquire about its inlineability.
    CallRefHint hints;
    hints.append(funcIndex);
    hints = auditInlineableCallees(callKind, hints);
    if (!hints.empty()) {
      // Inlining of `funcIndex` was approved.
      if (!emitInlineCall(funcType, funcIndex, callKind, args, &results)) {
        return false;
      }
    } else {
      if (!callDirect(funcType, funcIndex, lineOrBytecode, args, &results)) {
        return false;
      }
    }
  }

  iter().setResults(results.length(), results);
  return true;
}

bool FunctionCompiler::emitCallIndirect(bool oldStyle) {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t funcTypeIndex;
  uint32_t tableIndex;
  MDefinition* callee;
  DefVector args;
  if (oldStyle) {
    tableIndex = 0;
    if (!iter().readOldCallIndirect(&funcTypeIndex, &callee, &args)) {
      return false;
    }
  } else {
    if (!iter().readCallIndirect(&funcTypeIndex, &tableIndex, &callee, &args)) {
      return false;
    }
  }

  if (inDeadCode()) {
    return true;
  }

  DefVector results;
  if (!callIndirect(funcTypeIndex, tableIndex, callee, lineOrBytecode, args,
                    &results)) {
    return false;
  }

  iter().setResults(results.length(), results);
  return true;
}

#ifdef ENABLE_WASM_JSPI
bool FunctionCompiler::emitStackSwitch() {
  StackSwitchKind kind;
  MDefinition* suspender;
  MDefinition* fn;
  MDefinition* data;
  if (!iter().readStackSwitch(&kind, &suspender, &fn, &data)) {
    return false;
  }
  MDefinition* result = stackSwitch(suspender, fn, data, kind);
  if (!result) {
    return false;
  }

  if (kind == StackSwitchKind::SwitchToMain) {
    iter().setResult(result);
  }
  return true;
}
#endif

bool FunctionCompiler::emitReturnCall() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t funcIndex;
  DefVector args;
  if (!iter().readReturnCall(&funcIndex, &args)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  const FuncType& funcType = codeMeta().getFuncType(funcIndex);

  DefVector results;
  if (codeMeta().funcIsImport(funcIndex)) {
    uint32_t globalDataOffset =
        codeMeta().offsetOfFuncImportInstanceData(funcIndex);
    if (!returnCallImport(globalDataOffset, lineOrBytecode, funcType, args,
                          &results)) {
      return false;
    }
  } else {
    if (!returnCallDirect(funcType, funcIndex, lineOrBytecode, args,
                          &results)) {
      return false;
    }
  }
  return true;
}

bool FunctionCompiler::emitReturnCallIndirect() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t funcTypeIndex;
  uint32_t tableIndex;
  MDefinition* callee;
  DefVector args;
  if (!iter().readReturnCallIndirect(&funcTypeIndex, &tableIndex, &callee,
                                     &args)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  DefVector results;
  return returnCallIndirect(funcTypeIndex, tableIndex, callee, lineOrBytecode,
                            args, &results);
}

bool FunctionCompiler::emitReturnCallRef() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t funcTypeIndex;
  MDefinition* callee;
  DefVector args;

  if (!iter().readReturnCallRef(&funcTypeIndex, &callee, &args)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  const FuncType& funcType = codeMeta().types->type(funcTypeIndex).funcType();
  DefVector results;
  return returnCallRef(funcType, callee, lineOrBytecode, args, &results);
}

bool FunctionCompiler::emitGetLocal() {
  uint32_t id;
  if (!iter().readGetLocal(&id)) {
    return false;
  }

  iter().setResult(getLocalDef(id));
  return true;
}

bool FunctionCompiler::emitSetLocal() {
  uint32_t id;
  MDefinition* value;
  if (!iter().readSetLocal(&id, &value)) {
    return false;
  }

  assign(id, value);
  return true;
}

bool FunctionCompiler::emitTeeLocal() {
  uint32_t id;
  MDefinition* value;
  if (!iter().readTeeLocal(&id, &value)) {
    return false;
  }

  assign(id, value);
  return true;
}

bool FunctionCompiler::emitGetGlobal() {
  uint32_t id;
  if (!iter().readGetGlobal(&id)) {
    return false;
  }

  const GlobalDesc& global = codeMeta().globals[id];
  if (!global.isConstant()) {
    iter().setResult(loadGlobalVar(global));
    return true;
  }

  LitVal value = global.constantValue();

  MDefinition* result;
  switch (value.type().kind()) {
    case ValType::I32:
      result = constantI32(int32_t(value.i32()));
      break;
    case ValType::I64:
      result = constantI64(int64_t(value.i64()));
      break;
    case ValType::F32:
      result = constantF32(value.f32());
      break;
    case ValType::F64:
      result = constantF64(value.f64());
      break;
    case ValType::V128:
#ifdef ENABLE_WASM_SIMD
      result = constantV128(value.v128());
      break;
#else
      return iter().fail("Ion has no SIMD support yet");
#endif
    case ValType::Ref:
      MOZ_ASSERT(value.ref().isNull());
      result = constantNullRef(MaybeRefType(value.type().refType()));
      break;
    default:
      MOZ_CRASH("unexpected type in EmitGetGlobal");
  }

  iter().setResult(result);
  return true;
}

bool FunctionCompiler::emitSetGlobal() {
  uint32_t bytecodeOffset = readBytecodeOffset();

  uint32_t id;
  MDefinition* value;
  if (!iter().readSetGlobal(&id, &value)) {
    return false;
  }

  const GlobalDesc& global = codeMeta().globals[id];
  MOZ_ASSERT(global.isMutable());
  return storeGlobalVar(bytecodeOffset, global, value);
}

bool FunctionCompiler::emitTeeGlobal() {
  uint32_t bytecodeOffset = readBytecodeOffset();

  uint32_t id;
  MDefinition* value;
  if (!iter().readTeeGlobal(&id, &value)) {
    return false;
  }

  const GlobalDesc& global = codeMeta().globals[id];
  MOZ_ASSERT(global.isMutable());

  return storeGlobalVar(bytecodeOffset, global, value);
}

template <typename MIRClass>
bool FunctionCompiler::emitUnary(ValType operandType) {
  MDefinition* input;
  if (!iter().readUnary(operandType, &input)) {
    return false;
  }

  iter().setResult(unary<MIRClass>(input));
  return true;
}

template <typename MIRClass>
bool FunctionCompiler::emitConversion(ValType operandType, ValType resultType) {
  MDefinition* input;
  if (!iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  iter().setResult(unary<MIRClass>(input));
  return true;
}

template <typename MIRClass>
bool FunctionCompiler::emitUnaryWithType(ValType operandType, MIRType mirType) {
  MDefinition* input;
  if (!iter().readUnary(operandType, &input)) {
    return false;
  }

  iter().setResult(unary<MIRClass>(input, mirType));
  return true;
}

template <typename MIRClass>
bool FunctionCompiler::emitConversionWithType(ValType operandType,
                                              ValType resultType,
                                              MIRType mirType) {
  MDefinition* input;
  if (!iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  iter().setResult(unary<MIRClass>(input, mirType));
  return true;
}

bool FunctionCompiler::emitTruncate(ValType operandType, ValType resultType,
                                    bool isUnsigned, bool isSaturating) {
  MDefinition* input = nullptr;
  if (!iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  TruncFlags flags = 0;
  if (isUnsigned) {
    flags |= TRUNC_UNSIGNED;
  }
  if (isSaturating) {
    flags |= TRUNC_SATURATING;
  }
  if (resultType == ValType::I32) {
    if (codeMeta().isAsmJS()) {
      if (inDeadCode()) {
        // The read callsite line, produced by prepareCall, has to be
        // consumed -- the MWasmBuiltinTruncateToInt32 and MTruncateToInt32
        // will not create MIR node.
        (void)readCallSiteLineOrBytecode();
        iter().setResult(nullptr);
      } else if (input && (input->type() == MIRType::Double ||
                           input->type() == MIRType::Float32)) {
        iter().setResult(unary<MWasmBuiltinTruncateToInt32>(input));
      } else {
        iter().setResult(unary<MTruncateToInt32>(input));
      }
    } else {
      iter().setResult(truncate<MWasmTruncateToInt32>(input, flags));
    }
  } else {
    MOZ_ASSERT(resultType == ValType::I64);
    MOZ_ASSERT(!codeMeta().isAsmJS());
#if defined(JS_CODEGEN_ARM)
    iter().setResult(truncateWithInstance(input, flags));
#else
    iter().setResult(truncate<MWasmTruncateToInt64>(input, flags));
#endif
  }
  return true;
}

bool FunctionCompiler::emitSignExtend(uint32_t srcSize, uint32_t targetSize) {
  MDefinition* input;
  ValType type = targetSize == 4 ? ValType::I32 : ValType::I64;
  if (!iter().readConversion(type, type, &input)) {
    return false;
  }

  iter().setResult(signExtend(input, srcSize, targetSize));
  return true;
}

bool FunctionCompiler::emitExtendI32(bool isUnsigned) {
  MDefinition* input;
  if (!iter().readConversion(ValType::I32, ValType::I64, &input)) {
    return false;
  }

  iter().setResult(extendI32(input, isUnsigned));
  return true;
}

bool FunctionCompiler::emitConvertI64ToFloatingPoint(ValType resultType,
                                                     MIRType mirType,
                                                     bool isUnsigned) {
  MDefinition* input;
  if (!iter().readConversion(ValType::I64, resultType, &input)) {
    return false;
  }

  iter().setResult(convertI64ToFloatingPoint(input, mirType, isUnsigned));
  return true;
}

bool FunctionCompiler::emitReinterpret(ValType resultType, ValType operandType,
                                       MIRType mirType) {
  MDefinition* input;
  if (!iter().readConversion(operandType, resultType, &input)) {
    return false;
  }

  iter().setResult(unary<MReinterpretCast>(input, mirType));
  return true;
}

bool FunctionCompiler::emitAdd(ValType type, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(type, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(add(lhs, rhs, mirType));
  return true;
}

bool FunctionCompiler::emitSub(ValType type, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(type, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(sub(lhs, rhs, mirType));
  return true;
}

bool FunctionCompiler::emitRotate(ValType type, bool isLeftRotation) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(type, &lhs, &rhs)) {
    return false;
  }

  MDefinition* result = rotate(lhs, rhs, type.toMIRType(), isLeftRotation);
  iter().setResult(result);
  return true;
}

bool FunctionCompiler::emitBitNot(ValType operandType, MIRType mirType) {
  MDefinition* input;
  if (!iter().readUnary(operandType, &input)) {
    return false;
  }

  iter().setResult(bitnot(input, mirType));
  return true;
}

bool FunctionCompiler::emitBitwiseAndOrXor(
    ValType operandType, MIRType mirType,
    MWasmBinaryBitwise::SubOpcode subOpc) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(binary<MWasmBinaryBitwise>(lhs, rhs, mirType, subOpc));
  return true;
}

template <typename MIRClass>
bool FunctionCompiler::emitShift(ValType operandType, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(binary<MIRClass>(lhs, rhs, mirType));
  return true;
}

bool FunctionCompiler::emitUrsh(ValType operandType, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(ursh(lhs, rhs, mirType));
  return true;
}

bool FunctionCompiler::emitMul(ValType operandType, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(
      mul(lhs, rhs, mirType,
          mirType == MIRType::Int32 ? MMul::Integer : MMul::Normal));
  return true;
}

bool FunctionCompiler::emitDiv(ValType operandType, MIRType mirType,
                               bool isUnsigned) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(div(lhs, rhs, mirType, isUnsigned));
  return true;
}

bool FunctionCompiler::emitRem(ValType operandType, MIRType mirType,
                               bool isUnsigned) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(mod(lhs, rhs, mirType, isUnsigned));
  return true;
}

bool FunctionCompiler::emitMinMax(ValType operandType, MIRType mirType,
                                  bool isMax) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(minMax(lhs, rhs, mirType, isMax));
  return true;
}

bool FunctionCompiler::emitCopySign(ValType operandType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(binary<MCopySign>(lhs, rhs, operandType.toMIRType()));
  return true;
}

bool FunctionCompiler::emitComparison(ValType operandType, JSOp compareOp,
                                      MCompare::CompareType compareType) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readComparison(operandType, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(compare(lhs, rhs, compareOp, compareType));
  return true;
}

bool FunctionCompiler::emitSelect(bool typed) {
  StackType type;
  MDefinition* trueValue;
  MDefinition* falseValue;
  MDefinition* condition;
  if (!iter().readSelect(typed, &type, &trueValue, &falseValue, &condition)) {
    return false;
  }

  iter().setResult(select(trueValue, falseValue, condition));
  return true;
}

bool FunctionCompiler::emitLoad(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoad(type, Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));
  auto* ins = load(addr.base, &access, type);
  if (!inDeadCode() && !ins) {
    return false;
  }

  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitStore(ValType resultType, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!iter().readStore(resultType, Scalar::byteSize(viewType), &addr,
                        &value)) {
    return false;
  }

  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));

  store(addr.base, &access, value);
  return true;
}

bool FunctionCompiler::emitTeeStore(ValType resultType, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!iter().readTeeStore(resultType, Scalar::byteSize(viewType), &addr,
                           &value)) {
    return false;
  }

  MOZ_ASSERT(isMem32(addr.memoryIndex));  // asm.js opcode
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));

  store(addr.base, &access, value);
  return true;
}

bool FunctionCompiler::emitTeeStoreWithCoercion(ValType resultType,
                                                Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!iter().readTeeStore(resultType, Scalar::byteSize(viewType), &addr,
                           &value)) {
    return false;
  }

  if (resultType == ValType::F32 && viewType == Scalar::Float64) {
    value = unary<MToDouble>(value);
  } else if (resultType == ValType::F64 && viewType == Scalar::Float32) {
    value = unary<MToFloat32>(value);
  } else {
    MOZ_CRASH("unexpected coerced store");
  }

  MOZ_ASSERT(isMem32(addr.memoryIndex));  // asm.js opcode
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));

  store(addr.base, &access, value);
  return true;
}

bool FunctionCompiler::tryInlineUnaryBuiltin(SymbolicAddress callee,
                                             MDefinition* input) {
  if (!input) {
    return false;
  }

  MOZ_ASSERT(IsFloatingPointType(input->type()));

  RoundingMode mode;
  if (!IsRoundingFunction(callee, &mode)) {
    return false;
  }

  if (!MNearbyInt::HasAssemblerSupport(mode)) {
    return false;
  }

  iter().setResult(nearbyInt(input, mode));
  return true;
}

bool FunctionCompiler::emitUnaryMathBuiltinCall(
    const SymbolicAddressSignature& callee) {
  MOZ_ASSERT(callee.numArgs == 1);

  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  MDefinition* input;
  if (!iter().readUnary(ValType::fromMIRType(callee.argTypes[0]), &input)) {
    return false;
  }

  if (tryInlineUnaryBuiltin(callee.identity, input)) {
    return true;
  }

  MDefinition* def;
  if (!builtinCall1(callee, lineOrBytecode, input, &def)) {
    return false;
  }

  iter().setResult(def);
  return true;
}

bool FunctionCompiler::emitBinaryMathBuiltinCall(
    const SymbolicAddressSignature& callee) {
  MOZ_ASSERT(callee.numArgs == 2);
  MOZ_ASSERT(callee.argTypes[0] == callee.argTypes[1]);

  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  MDefinition* lhs;
  MDefinition* rhs;
  // This call to readBinary assumes both operands have the same type.
  if (!iter().readBinary(ValType::fromMIRType(callee.argTypes[0]), &lhs,
                         &rhs)) {
    return false;
  }

  MDefinition* def;
  if (!builtinCall2(callee, lineOrBytecode, lhs, rhs, &def)) {
    return false;
  }

  iter().setResult(def);
  return true;
}

bool FunctionCompiler::emitMemoryGrow() {
  uint32_t bytecodeOffset = readBytecodeOffset();

  MDefinition* delta;
  uint32_t memoryIndex;
  if (!iter().readMemoryGrow(&memoryIndex, &delta)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* memoryIndexValue = constantI32(int32_t(memoryIndex));
  if (!memoryIndexValue) {
    return false;
  }

  const SymbolicAddressSignature& callee =
      isMem32(memoryIndex) ? SASigMemoryGrowM32 : SASigMemoryGrowM64;

  MDefinition* ret;
  if (!emitInstanceCall2(bytecodeOffset, callee, delta, memoryIndexValue,
                         &ret)) {
    return false;
  }

  iter().setResult(ret);
  return true;
}

bool FunctionCompiler::emitMemorySize() {
  uint32_t bytecodeOffset = readBytecodeOffset();

  uint32_t memoryIndex;
  if (!iter().readMemorySize(&memoryIndex)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* memoryIndexValue = constantI32(int32_t(memoryIndex));
  if (!memoryIndexValue) {
    return false;
  }

  const SymbolicAddressSignature& callee =
      isMem32(memoryIndex) ? SASigMemorySizeM32 : SASigMemorySizeM64;

  MDefinition* ret;
  if (!emitInstanceCall1(bytecodeOffset, callee, memoryIndexValue, &ret)) {
    return false;
  }

  iter().setResult(ret);
  return true;
}

bool FunctionCompiler::emitAtomicCmpXchg(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* oldValue;
  MDefinition* newValue;
  if (!iter().readAtomicCmpXchg(&addr, type, byteSize(viewType), &oldValue,
                                &newValue)) {
    return false;
  }

  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Full());
  auto* ins =
      atomicCompareExchangeHeap(addr.base, &access, type, oldValue, newValue);
  if (!inDeadCode() && !ins) {
    return false;
  }

  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitAtomicLoad(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readAtomicLoad(&addr, type, byteSize(viewType))) {
    return false;
  }

  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Load());
  auto* ins = load(addr.base, &access, type);
  if (!inDeadCode() && !ins) {
    return false;
  }

  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitAtomicRMW(ValType type, Scalar::Type viewType,
                                     jit::AtomicOp op) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!iter().readAtomicRMW(&addr, type, byteSize(viewType), &value)) {
    return false;
  }

  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Full());
  auto* ins = atomicBinopHeap(op, addr.base, &access, type, value);
  if (!inDeadCode() && !ins) {
    return false;
  }

  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitAtomicStore(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!iter().readAtomicStore(&addr, type, byteSize(viewType), &value)) {
    return false;
  }

  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Store());
  store(addr.base, &access, value);
  return true;
}

bool FunctionCompiler::emitWait(ValType type, uint32_t byteSize) {
  MOZ_ASSERT(type == ValType::I32 || type == ValType::I64);
  MOZ_ASSERT(type.size() == byteSize);

  uint32_t bytecodeOffset = readBytecodeOffset();

  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* expected;
  MDefinition* timeout;
  if (!iter().readWait(&addr, type, byteSize, &expected, &timeout)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MemoryAccessDesc access(addr.memoryIndex,
                          type == ValType::I32 ? Scalar::Int32 : Scalar::Int64,
                          addr.align, addr.offset, trapSiteDesc(),
                          hugeMemoryEnabled(addr.memoryIndex));
  MDefinition* ptr = computeEffectiveAddress(addr.base, &access);
  if (!ptr) {
    return false;
  }

  MDefinition* memoryIndex = constantI32(int32_t(addr.memoryIndex));
  if (!memoryIndex) {
    return false;
  }

  const SymbolicAddressSignature& callee =
      isMem32(addr.memoryIndex)
          ? (type == ValType::I32 ? SASigWaitI32M32 : SASigWaitI64M32)
          : (type == ValType::I32 ? SASigWaitI32M64 : SASigWaitI64M64);

  MDefinition* ret;
  if (!emitInstanceCall4(bytecodeOffset, callee, ptr, expected, timeout,
                         memoryIndex, &ret)) {
    return false;
  }

  iter().setResult(ret);
  return true;
}

bool FunctionCompiler::emitFence() {
  if (!iter().readFence()) {
    return false;
  }

  fence();
  return true;
}

bool FunctionCompiler::emitNotify() {
  uint32_t bytecodeOffset = readBytecodeOffset();

  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* count;
  if (!iter().readNotify(&addr, &count)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MemoryAccessDesc access(addr.memoryIndex, Scalar::Int32, addr.align,
                          addr.offset, trapSiteDesc(),
                          hugeMemoryEnabled(addr.memoryIndex));
  MDefinition* ptr = computeEffectiveAddress(addr.base, &access);
  if (!ptr) {
    return false;
  }

  MDefinition* memoryIndex = constantI32(int32_t(addr.memoryIndex));
  if (!memoryIndex) {
    return false;
  }

  const SymbolicAddressSignature& callee =
      isMem32(addr.memoryIndex) ? SASigWakeM32 : SASigWakeM64;

  MDefinition* ret;
  if (!emitInstanceCall3(bytecodeOffset, callee, ptr, count, memoryIndex,
                         &ret)) {
    return false;
  }

  iter().setResult(ret);
  return true;
}

bool FunctionCompiler::emitAtomicXchg(ValType type, Scalar::Type viewType) {
  LinearMemoryAddress<MDefinition*> addr;
  MDefinition* value;
  if (!iter().readAtomicRMW(&addr, type, byteSize(viewType), &value)) {
    return false;
  }

  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex),
                          Synchronization::Full());
  MDefinition* ins = atomicExchangeHeap(addr.base, &access, type, value);
  if (!inDeadCode() && !ins) {
    return false;
  }

  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitMemCopyCall(uint32_t dstMemIndex,
                                       uint32_t srcMemIndex, MDefinition* dst,
                                       MDefinition* src, MDefinition* len) {
  uint32_t bytecodeOffset = readBytecodeOffset();

  if (dstMemIndex == srcMemIndex) {
    const SymbolicAddressSignature& callee =
        (codeMeta().usesSharedMemory(dstMemIndex)
             ? (isMem32(dstMemIndex) ? SASigMemCopySharedM32
                                     : SASigMemCopySharedM64)
             : (isMem32(dstMemIndex) ? SASigMemCopyM32 : SASigMemCopyM64));
    MDefinition* base = memoryBase(dstMemIndex);
    if (!base) {
      return false;
    }
    return emitInstanceCall4(bytecodeOffset, callee, dst, src, len, base);
  }

  AddressType dstIndexType = codeMeta().memories[dstMemIndex].addressType();
  AddressType srcIndexType = codeMeta().memories[srcMemIndex].addressType();

  if (dstIndexType == AddressType::I32) {
    dst = extendI32(dst, /*isUnsigned=*/true);
    if (!dst) {
      return false;
    }
  }
  if (srcIndexType == AddressType::I32) {
    src = extendI32(src, /*isUnsigned=*/true);
    if (!src) {
      return false;
    }
  }
  if (dstIndexType == AddressType::I32 || srcIndexType == AddressType::I32) {
    len = extendI32(len, /*isUnsigned=*/true);
    if (!len) {
      return false;
    }
  }

  MDefinition* dstMemIndexValue = constantI32(int32_t(dstMemIndex));
  if (!dstMemIndexValue) {
    return false;
  }

  MDefinition* srcMemIndexValue = constantI32(int32_t(srcMemIndex));
  if (!srcMemIndexValue) {
    return false;
  }

  return emitInstanceCall5(bytecodeOffset, SASigMemCopyAny, dst, src, len,
                           dstMemIndexValue, srcMemIndexValue);
}

bool FunctionCompiler::emitMemCopyInline(uint32_t memoryIndex, MDefinition* dst,
                                         MDefinition* src, uint32_t length) {
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryCopyLength);

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef ENABLE_WASM_SIMD
  size_t numCopies16 = 0;
  if (MacroAssembler::SupportsFastUnalignedFPAccesses()) {
    numCopies16 = remainder / sizeof(V128);
    remainder %= sizeof(V128);
  }
#endif
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Load all source bytes from low to high using the widest transfer width we
  // can for the system. We will trap without writing anything if any source
  // byte is out-of-bounds.
  size_t offset = 0;
  DefVector loadedValues;

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::V128);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(V128);
  }
#endif

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I64);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(uint64_t);
  }
#endif

  for (uint32_t i = 0; i < numCopies4; i++) {
    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I32);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(uint32_t);
  }

  if (numCopies2) {
    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I32);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(uint16_t);
  }

  if (numCopies1) {
    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I32);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }
  }

  // Store all source bytes to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  offset = length;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    store(dst, &access, value);
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    store(dst, &access, value);
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    store(dst, &access, value);
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    store(dst, &access, value);
  }
#endif

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    offset -= sizeof(V128);

    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    store(dst, &access, value);
  }
#endif

  return true;
}

bool FunctionCompiler::emitMemCopy() {
  MDefinition *dst, *src, *len;
  uint32_t dstMemIndex;
  uint32_t srcMemIndex;
  if (!iter().readMemOrTableCopy(true, &dstMemIndex, &dst, &srcMemIndex, &src,
                                 &len)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  if (dstMemIndex == srcMemIndex && len->isConstant()) {
    uint64_t length = isMem32(dstMemIndex) ? len->toConstant()->toInt32()
                                           : len->toConstant()->toInt64();
    static_assert(MaxInlineMemoryCopyLength <= UINT32_MAX);
    if (length != 0 && length <= MaxInlineMemoryCopyLength) {
      return emitMemCopyInline(dstMemIndex, dst, src, uint32_t(length));
    }
  }

  return emitMemCopyCall(dstMemIndex, srcMemIndex, dst, src, len);
}

bool FunctionCompiler::emitTableCopy() {
  MDefinition *dst, *src, *len;
  uint32_t dstTableIndex;
  uint32_t srcTableIndex;
  if (!iter().readMemOrTableCopy(false, &dstTableIndex, &dst, &srcTableIndex,
                                 &src, &len)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();
  const TableDesc& dstTable = codeMeta().tables[dstTableIndex];
  const TableDesc& srcTable = codeMeta().tables[srcTableIndex];

  AddressType dstAddressType = dstTable.addressType();
  AddressType srcAddressType = srcTable.addressType();
  AddressType lenAddressType =
      dstAddressType == AddressType::I64 && srcAddressType == AddressType::I64
          ? AddressType::I64
          : AddressType::I32;

  MDefinition* dst32 = tableAddressToI32(dstAddressType, dst);
  if (!dst32) {
    return false;
  }

  MDefinition* src32 = tableAddressToI32(srcAddressType, src);
  if (!src32) {
    return false;
  }

  MDefinition* len32 = tableAddressToI32(lenAddressType, len);
  if (!len32) {
    return false;
  }

  MDefinition* dti = constantI32(int32_t(dstTableIndex));
  MDefinition* sti = constantI32(int32_t(srcTableIndex));

  return emitInstanceCall5(bytecodeOffset, SASigTableCopy, dst32, src32, len32,
                           dti, sti);
}

bool FunctionCompiler::emitDataOrElemDrop(bool isData) {
  uint32_t segIndexVal = 0;
  if (!iter().readDataOrElemDrop(isData, &segIndexVal)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();

  MDefinition* segIndex = constantI32(int32_t(segIndexVal));

  const SymbolicAddressSignature& callee =
      isData ? SASigDataDrop : SASigElemDrop;
  return emitInstanceCall1(bytecodeOffset, callee, segIndex);
}

bool FunctionCompiler::emitMemFillCall(uint32_t memoryIndex, MDefinition* start,
                                       MDefinition* val, MDefinition* len) {
  MDefinition* base = memoryBase(memoryIndex);

  uint32_t bytecodeOffset = readBytecodeOffset();
  const SymbolicAddressSignature& callee =
      (codeMeta().usesSharedMemory(memoryIndex)
           ? (isMem32(memoryIndex) ? SASigMemFillSharedM32
                                   : SASigMemFillSharedM64)
           : (isMem32(memoryIndex) ? SASigMemFillM32 : SASigMemFillM64));
  return emitInstanceCall4(bytecodeOffset, callee, start, val, len, base);
}

bool FunctionCompiler::emitMemFillInline(uint32_t memoryIndex,
                                         MDefinition* start, MDefinition* val,
                                         uint32_t length) {
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryFillLength);
  uint32_t value = val->toConstant()->toInt32();

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef ENABLE_WASM_SIMD
  size_t numCopies16 = 0;
  if (MacroAssembler::SupportsFastUnalignedFPAccesses()) {
    numCopies16 = remainder / sizeof(V128);
    remainder %= sizeof(V128);
  }
#endif
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Generate splatted definitions for wider fills as needed
#ifdef ENABLE_WASM_SIMD
  MDefinition* val16 = numCopies16 ? constantV128(V128(value)) : nullptr;
#endif
#ifdef JS_64BIT
  MDefinition* val8 =
      numCopies8 ? constantI64(int64_t(SplatByteToUInt<uint64_t>(value, 8)))
                 : nullptr;
#endif
  MDefinition* val4 =
      numCopies4 ? constantI32(int32_t(SplatByteToUInt<uint32_t>(value, 4)))
                 : nullptr;
  MDefinition* val2 =
      numCopies2 ? constantI32(int32_t(SplatByteToUInt<uint32_t>(value, 2)))
                 : nullptr;

  // Store the fill value to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  size_t offset = length;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    store(start, &access, val);
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    store(start, &access, val2);
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    store(start, &access, val4);
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    store(start, &access, val8);
  }
#endif

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    offset -= sizeof(V128);

    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    store(start, &access, val16);
  }
#endif

  return true;
}

bool FunctionCompiler::emitMemFill() {
  uint32_t memoryIndex;
  MDefinition *start, *val, *len;
  if (!iter().readMemFill(&memoryIndex, &start, &val, &len)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  if (len->isConstant() && val->isConstant()) {
    uint64_t length = isMem32(memoryIndex) ? len->toConstant()->toInt32()
                                           : len->toConstant()->toInt64();
    static_assert(MaxInlineMemoryFillLength <= UINT32_MAX);
    if (length != 0 && length <= MaxInlineMemoryFillLength) {
      return emitMemFillInline(memoryIndex, start, val, uint32_t(length));
    }
  }

  return emitMemFillCall(memoryIndex, start, val, len);
}

bool FunctionCompiler::emitMemInit() {
  uint32_t segIndexVal = 0, dstMemIndex = 0;
  MDefinition *dstOff, *srcOff, *len;
  if (!iter().readMemOrTableInit(true, &segIndexVal, &dstMemIndex, &dstOff,
                                 &srcOff, &len)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();
  const SymbolicAddressSignature& callee =
      (isMem32(dstMemIndex) ? SASigMemInitM32 : SASigMemInitM64);

  MDefinition* segIndex = constantI32(int32_t(segIndexVal));
  if (!segIndex) {
    return false;
  }

  MDefinition* dti = constantI32(int32_t(dstMemIndex));
  if (!dti) {
    return false;
  }

  return emitInstanceCall5(bytecodeOffset, callee, dstOff, srcOff, len,
                           segIndex, dti);
}

bool FunctionCompiler::emitTableInit() {
  uint32_t segIndexVal = 0, dstTableIndex = 0;
  MDefinition *dstOff, *srcOff, *len;
  if (!iter().readMemOrTableInit(false, &segIndexVal, &dstTableIndex, &dstOff,
                                 &srcOff, &len)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();
  const TableDesc& table = codeMeta().tables[dstTableIndex];

  MDefinition* dstOff32 = tableAddressToI32(table.addressType(), dstOff);
  if (!dstOff32) {
    return false;
  }

  MDefinition* segIndex = constantI32(int32_t(segIndexVal));
  if (!segIndex) {
    return false;
  }

  MDefinition* dti = constantI32(int32_t(dstTableIndex));
  if (!dti) {
    return false;
  }

  return emitInstanceCall5(bytecodeOffset, SASigTableInit, dstOff32, srcOff,
                           len, segIndex, dti);
}

bool FunctionCompiler::emitTableFill() {
  uint32_t tableIndex;
  MDefinition *start, *val, *len;
  if (!iter().readTableFill(&tableIndex, &start, &val, &len)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();
  const TableDesc& table = codeMeta().tables[tableIndex];

  MDefinition* start32 = tableAddressToI32(table.addressType(), start);
  if (!start32) {
    return false;
  }

  MDefinition* len32 = tableAddressToI32(table.addressType(), len);
  if (!len32) {
    return false;
  }

  MDefinition* tableIndexArg = constantI32(int32_t(tableIndex));
  if (!tableIndexArg) {
    return false;
  }

  return emitInstanceCall4(bytecodeOffset, SASigTableFill, start32, val, len32,
                           tableIndexArg);
}

#if ENABLE_WASM_MEMORY_CONTROL
bool FunctionCompiler::emitMemDiscard() {
  uint32_t memoryIndex;
  MDefinition *start, *len;
  if (!iter().readMemDiscard(&memoryIndex, &start, &len)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();

  MDefinition* base = memoryBase(memoryIndex);
  bool mem32 = isMem32(memoryIndex);

  const SymbolicAddressSignature& callee =
      (codeMeta().usesSharedMemory(memoryIndex)
           ? (mem32 ? SASigMemDiscardSharedM32 : SASigMemDiscardSharedM64)
           : (mem32 ? SASigMemDiscardM32 : SASigMemDiscardM64));
  return emitInstanceCall3(bytecodeOffset, callee, start, len, base);
}
#endif

bool FunctionCompiler::emitTableGet() {
  uint32_t tableIndex;
  MDefinition* address;
  if (!iter().readTableGet(&tableIndex, &address)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  const TableDesc& table = codeMeta().tables[tableIndex];

  MDefinition* address32 = tableAddressToI32(table.addressType(), address);
  if (!address32) {
    return false;
  }

  if (table.elemType.tableRepr() == TableRepr::Ref) {
    MDefinition* ret = tableGetAnyRef(tableIndex, address32);
    if (!ret) {
      return false;
    }
    iter().setResult(ret);
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();

  MDefinition* tableIndexArg = constantI32(int32_t(tableIndex));
  if (!tableIndexArg) {
    return false;
  }

  // The return value here is either null, denoting an error, or a short-lived
  // pointer to a location containing a possibly-null ref.
  MDefinition* ret;
  if (!emitInstanceCall2(bytecodeOffset, SASigTableGet, address32,
                         tableIndexArg, &ret)) {
    return false;
  }

  iter().setResult(ret);
  return true;
}

bool FunctionCompiler::emitTableGrow() {
  uint32_t tableIndex;
  MDefinition* initValue;
  MDefinition* delta;
  if (!iter().readTableGrow(&tableIndex, &initValue, &delta)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();
  const TableDesc& table = codeMeta().tables[tableIndex];

  MDefinition* delta32 = tableAddressToI32(table.addressType(), delta);
  if (!delta32) {
    return false;
  }

  MDefinition* tableIndexArg = constantI32(int32_t(tableIndex));
  if (!tableIndexArg) {
    return false;
  }

  MDefinition* ret;
  if (!emitInstanceCall3(bytecodeOffset, SASigTableGrow, initValue, delta32,
                         tableIndexArg, &ret)) {
    return false;
  }

  if (table.addressType() == AddressType::I64) {
    ret = extendI32(ret, false);
    if (!ret) {
      return false;
    }
  }

  iter().setResult(ret);
  return true;
}

bool FunctionCompiler::emitTableSet() {
  uint32_t tableIndex;
  MDefinition* address;
  MDefinition* value;
  if (!iter().readTableSet(&tableIndex, &address, &value)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();

  const TableDesc& table = codeMeta().tables[tableIndex];

  MDefinition* address32 = tableAddressToI32(table.addressType(), address);
  if (!address32) {
    return false;
  }

  if (table.elemType.tableRepr() == TableRepr::Ref) {
    return tableSetAnyRef(tableIndex, address32, value, bytecodeOffset);
  }

  MDefinition* tableIndexArg = constantI32(int32_t(tableIndex));
  if (!tableIndexArg) {
    return false;
  }

  return emitInstanceCall3(bytecodeOffset, SASigTableSet, address32, value,
                           tableIndexArg);
}

bool FunctionCompiler::emitTableSize() {
  uint32_t tableIndex;
  if (!iter().readTableSize(&tableIndex)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* length = loadTableLength(tableIndex);
  if (!length) {
    return false;
  }

  if (codeMeta().tables[tableIndex].addressType() == AddressType::I64) {
    length = extendI32(length, true);
    if (!length) {
      return false;
    }
  }

  iter().setResult(length);
  return true;
}

bool FunctionCompiler::emitRefFunc() {
  uint32_t funcIndex;
  if (!iter().readRefFunc(&funcIndex)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  uint32_t bytecodeOffset = readBytecodeOffset();

  MDefinition* funcIndexArg = constantI32(int32_t(funcIndex));
  if (!funcIndexArg) {
    return false;
  }

  // The return value here is either null, denoting an error, or a short-lived
  // pointer to a location containing a possibly-null ref.
  MDefinition* ret;
  if (!emitInstanceCall1(bytecodeOffset, SASigRefFunc, funcIndexArg, &ret)) {
    return false;
  }

  iter().setResult(ret);
  return true;
}

bool FunctionCompiler::emitRefNull() {
  RefType type;
  if (!iter().readRefNull(&type)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* nullVal = constantNullRef(MaybeRefType(type));
  if (!nullVal) {
    return false;
  }
  iter().setResult(nullVal);
  return true;
}

bool FunctionCompiler::emitRefIsNull() {
  MDefinition* input;
  if (!iter().readRefIsNull(&input)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* nullVal = constantNullRef(MaybeRefType());
  if (!nullVal) {
    return false;
  }
  iter().setResult(
      compare(input, nullVal, JSOp::Eq, MCompare::Compare_WasmAnyRef));
  return true;
}

#ifdef ENABLE_WASM_SIMD
bool FunctionCompiler::emitConstSimd128() {
  V128 v128;
  if (!iter().readV128Const(&v128)) {
    return false;
  }

  iter().setResult(constantV128(v128));
  return true;
}

bool FunctionCompiler::emitBinarySimd128(bool commutative, SimdOp op) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(ValType::V128, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(binarySimd128(lhs, rhs, commutative, op));
  return true;
}

bool FunctionCompiler::emitTernarySimd128(wasm::SimdOp op) {
  MDefinition* v0;
  MDefinition* v1;
  MDefinition* v2;
  if (!iter().readTernary(ValType::V128, &v0, &v1, &v2)) {
    return false;
  }

  iter().setResult(ternarySimd128(v0, v1, v2, op));
  return true;
}

bool FunctionCompiler::emitShiftSimd128(SimdOp op) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readVectorShift(&lhs, &rhs)) {
    return false;
  }

  iter().setResult(shiftSimd128(lhs, rhs, op));
  return true;
}

bool FunctionCompiler::emitSplatSimd128(ValType inType, SimdOp op) {
  MDefinition* src;
  if (!iter().readConversion(inType, ValType::V128, &src)) {
    return false;
  }

  iter().setResult(scalarToSimd128(src, op));
  return true;
}

bool FunctionCompiler::emitUnarySimd128(SimdOp op) {
  MDefinition* src;
  if (!iter().readUnary(ValType::V128, &src)) {
    return false;
  }

  iter().setResult(unarySimd128(src, op));
  return true;
}

bool FunctionCompiler::emitReduceSimd128(SimdOp op) {
  MDefinition* src;
  if (!iter().readConversion(ValType::V128, ValType::I32, &src)) {
    return false;
  }

  iter().setResult(reduceSimd128(src, op, ValType::I32));
  return true;
}

bool FunctionCompiler::emitExtractLaneSimd128(ValType outType,
                                              uint32_t laneLimit, SimdOp op) {
  uint32_t laneIndex;
  MDefinition* src;
  if (!iter().readExtractLane(outType, laneLimit, &laneIndex, &src)) {
    return false;
  }

  iter().setResult(reduceSimd128(src, op, outType, laneIndex));
  return true;
}

bool FunctionCompiler::emitReplaceLaneSimd128(ValType laneType,
                                              uint32_t laneLimit, SimdOp op) {
  uint32_t laneIndex;
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readReplaceLane(laneType, laneLimit, &laneIndex, &lhs, &rhs)) {
    return false;
  }

  iter().setResult(replaceLaneSimd128(lhs, rhs, laneIndex, op));
  return true;
}

bool FunctionCompiler::emitShuffleSimd128() {
  MDefinition* v1;
  MDefinition* v2;
  V128 control;
  if (!iter().readVectorShuffle(&v1, &v2, &control)) {
    return false;
  }

  iter().setResult(shuffleSimd128(v1, v2, control));
  return true;
}

bool FunctionCompiler::emitLoadSplatSimd128(Scalar::Type viewType,
                                            wasm::SimdOp splatOp) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  auto* ins = loadSplatSimd128(viewType, addr, splatOp);
  if (!inDeadCode() && !ins) {
    return false;
  }
  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitLoadExtendSimd128(wasm::SimdOp op) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadExtend(&addr)) {
    return false;
  }

  auto* ins = loadExtendSimd128(addr, op);
  if (!inDeadCode() && !ins) {
    return false;
  }
  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitLoadZeroSimd128(Scalar::Type viewType,
                                           size_t numBytes) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadSplat(numBytes, &addr)) {
    return false;
  }

  auto* ins = loadZeroSimd128(viewType, numBytes, addr);
  if (!inDeadCode() && !ins) {
    return false;
  }
  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitLoadLaneSimd128(uint32_t laneSize) {
  uint32_t laneIndex;
  MDefinition* src;
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadLane(laneSize, &addr, &laneIndex, &src)) {
    return false;
  }

  auto* ins = loadLaneSimd128(laneSize, addr, laneIndex, src);
  if (!inDeadCode() && !ins) {
    return false;
  }
  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitStoreLaneSimd128(uint32_t laneSize) {
  uint32_t laneIndex;
  MDefinition* src;
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readStoreLane(laneSize, &addr, &laneIndex, &src)) {
    return false;
  }

  storeLaneSimd128(laneSize, addr, laneIndex, src);
  return true;
}

#endif  // ENABLE_WASM_SIMD

bool FunctionCompiler::emitRefAsNonNull() {
  MDefinition* ref;
  if (!iter().readRefAsNonNull(&ref)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* ins = refAsNonNull(ref);
  if (!ins) {
    return false;
  }

  iter().setResult(ins);
  return true;
}

bool FunctionCompiler::emitBrOnNull() {
  uint32_t relativeDepth;
  ResultType type;
  DefVector values;
  MDefinition* condition;
  if (!iter().readBrOnNull(&relativeDepth, &type, &values, &condition)) {
    return false;
  }

  return brOnNull(relativeDepth, values, type, condition);
}

bool FunctionCompiler::emitBrOnNonNull() {
  uint32_t relativeDepth;
  ResultType type;
  DefVector values;
  MDefinition* condition;
  if (!iter().readBrOnNonNull(&relativeDepth, &type, &values, &condition)) {
    return false;
  }

  return brOnNonNull(relativeDepth, values, type, condition);
}

// Speculatively inline a call_refs that are likely to target the expected
// function index in this module. A fallback for if the actual callee is not
// any of the speculated expected callees is always generated. This leads to a
// control flow chain that is roughly:
//
// if (ref.func $expectedFuncIndex_1) == actualCalleeFunc:
//   (call_inline $expectedFuncIndex1)
// else if (ref.func $expectedFuncIndex_2) == actualCalleeFunc:
//   (call_inline $expectedFuncIndex2)
// ...
// else:
//   (call_ref actualCalleeFunc)
//
bool FunctionCompiler::emitSpeculativeInlineCallRef(
    uint32_t bytecodeOffset, const FuncType& funcType,
    CallRefHint expectedFuncIndices, MDefinition* actualCalleeFunc,
    const DefVector& args, DefVector* results) {
  // There must be at least one speculative target.
  MOZ_ASSERT(!expectedFuncIndices.empty());

  // Perform an up front null check on the callee function reference.
  actualCalleeFunc = refAsNonNull(actualCalleeFunc);
  if (!actualCalleeFunc) {
    return false;
  }

  constexpr size_t numElseBlocks = CallRefHint::NUM_ENTRIES + 1;
  Vector<MBasicBlock*, numElseBlocks, SystemAllocPolicy> elseBlocks;
  if (!elseBlocks.reserve(numElseBlocks)) {
    return false;
  }

  for (uint32_t i = 0; i < expectedFuncIndices.length(); i++) {
    uint32_t funcIndex = expectedFuncIndices.get(i);

    // Load the cached value of `ref.func $expectedFuncIndex` for comparing
    // against `actualCalleeFunc`. This cached value may be null if the
    // `ref.func` for the expected function has not been executed in this
    // runtime session.
    //
    // This is okay because we have done a null check on the `actualCalleeFunc`
    // already and so comparing it against a null expected callee func will
    // return false and fall back to the general case. This can only happen if
    // we've deserialized a cached module in a different session, and then run
    // the code without ever acquiring a reference to the expected function. In
    // that case, the expected callee could never be the target of this
    // call_ref, so performing the fallback path is the right thing to do
    // anyways.
    MDefinition* expectedCalleeFunc = loadCachedRefFunc(funcIndex);
    if (!expectedCalleeFunc) {
      return false;
    }

    // Check if the callee funcref we have is equals to the expected callee
    // funcref we're inlining.
    MDefinition* isExpectedCallee =
        compare(actualCalleeFunc, expectedCalleeFunc, JSOp::Eq,
                MCompare::Compare_WasmAnyRef);
    if (!isExpectedCallee) {
      return false;
    }

    // Start a 'then' block, which will have the inlined code
    MBasicBlock* elseBlock;
    if (!branchAndStartThen(isExpectedCallee, &elseBlock)) {
      return false;
    }

    // Inline the expected callee as we do with direct calls
    DefVector inlineResults;
    if (!emitInlineCall(funcType, funcIndex,
                        InliningHeuristics::CallKind::CallRef, args,
                        &inlineResults)) {
      return false;
    }

    // Push the results for joining with the 'else' block
    if (!pushDefs(inlineResults)) {
      return false;
    }

    // Switch to the 'else' block which will have, either the check for the
    // next target, or the fallback `call_ref` if we're out of targets.
    if (!switchToElse(elseBlock, &elseBlock)) {
      return false;
    }

    elseBlocks.infallibleAppend(elseBlock);
  }

  DefVector callResults;
  if (!callRef(funcType, actualCalleeFunc, bytecodeOffset, args,
               &callResults)) {
    return false;
  }

  // Push the results for joining with the 'then' block
  if (!pushDefs(callResults)) {
    return false;
  }

  // Join the various branches together
  for (uint32_t i = elseBlocks.length() - 1; i != 0; i--) {
    DefVector results;
    if (!joinIfElse(elseBlocks[i], &results) || !pushDefs(results)) {
      return false;
    }
  }
  return joinIfElse(elseBlocks[0], results);
}

bool FunctionCompiler::emitCallRef() {
  uint32_t bytecodeOffset = readBytecodeOffset();
  uint32_t funcTypeIndex;
  MDefinition* callee;
  DefVector args;

  if (!iter().readCallRef(&funcTypeIndex, &callee, &args)) {
    return false;
  }

  // We must unconditionally read a call_ref hint so that we stay in sync with
  // how baseline generates them.
  CallRefHint hint = readCallRefHint();

  if (inDeadCode()) {
    return true;
  }

  const FuncType& funcType = codeMeta().types->type(funcTypeIndex).funcType();

  // Ask the inlining heuristics which entries in `hint` we are allowed to
  // inline.
  CallRefHint approved =
      auditInlineableCallees(InliningHeuristics::CallKind::CallRef, hint);
  if (!approved.empty()) {
    DefVector results;
    if (!emitSpeculativeInlineCallRef(bytecodeOffset, funcType, approved,
                                      callee, args, &results)) {
      return false;
    }
    iter().setResults(results.length(), results);
    return true;
  }

  DefVector results;
  if (!callRef(funcType, callee, bytecodeOffset, args, &results)) {
    return false;
  }

  iter().setResults(results.length(), results);
  return true;
}

bool FunctionCompiler::emitStructNew() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex;
  DefVector args;
  if (!iter().readStructNew(&typeIndex, &args)) {
    return false;
  }

  uint32_t allocSiteIndex = readAllocSiteIndex(typeIndex);

  if (inDeadCode()) {
    return true;
  }

  const TypeDef& typeDef = (*codeMeta().types)[typeIndex];
  const StructType& structType = typeDef.structType();
  MOZ_ASSERT(args.length() == structType.fields_.length());

  MDefinition* structObject =
      createStructObject(typeIndex, allocSiteIndex, false);
  if (!structObject) {
    return false;
  }

  // And fill in the fields.
  for (uint32_t fieldIndex = 0; fieldIndex < structType.fields_.length();
       fieldIndex++) {
    if (!mirGen().ensureBallast()) {
      return false;
    }
    if (!writeValueToStructField(lineOrBytecode, structType, fieldIndex,
                                 structObject, args[fieldIndex],
                                 WasmPreBarrierKind::None)) {
      return false;
    }
  }

  iter().setResult(structObject);
  return true;
}

bool FunctionCompiler::emitStructNewDefault() {
  uint32_t typeIndex;
  if (!iter().readStructNewDefault(&typeIndex)) {
    return false;
  }

  uint32_t allocSiteIndex = readAllocSiteIndex(typeIndex);

  if (inDeadCode()) {
    return true;
  }

  MDefinition* structObject =
      createStructObject(typeIndex, allocSiteIndex, true);
  if (!structObject) {
    return false;
  }

  iter().setResult(structObject);
  return true;
}

bool FunctionCompiler::emitStructSet() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex;
  uint32_t fieldIndex;
  MDefinition* structObject;
  MDefinition* value;
  if (!iter().readStructSet(&typeIndex, &fieldIndex, &structObject, &value)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // Check for null is done at writeValueToStructField.

  // And fill in the field.
  const StructType& structType = (*codeMeta().types)[typeIndex].structType();
  return writeValueToStructField(lineOrBytecode, structType, fieldIndex,
                                 structObject, value,
                                 WasmPreBarrierKind::Normal);
}

bool FunctionCompiler::emitStructGet(FieldWideningOp wideningOp) {
  uint32_t typeIndex;
  uint32_t fieldIndex;
  MDefinition* structObject;
  if (!iter().readStructGet(&typeIndex, &fieldIndex, wideningOp,
                            &structObject)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // Check for null is done at readValueFromStructField.

  // And fetch the data.
  const StructType& structType = (*codeMeta().types)[typeIndex].structType();
  MDefinition* load = readValueFromStructField(structType, fieldIndex,
                                               wideningOp, structObject);
  if (!load) {
    return false;
  }

  iter().setResult(load);
  return true;
}

bool FunctionCompiler::emitArrayNew() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex;
  MDefinition* numElements;
  MDefinition* fillValue;
  if (!iter().readArrayNew(&typeIndex, &numElements, &fillValue)) {
    return false;
  }

  uint32_t allocSiteIndex = readAllocSiteIndex(typeIndex);

  if (inDeadCode()) {
    return true;
  }

  // If the requested size exceeds MaxArrayPayloadBytes, the MIR generated by
  // this helper will trap.
  MDefinition* arrayObject = createArrayNewCallAndLoop(
      lineOrBytecode, typeIndex, allocSiteIndex, numElements, fillValue);
  if (!arrayObject) {
    return false;
  }

  iter().setResult(arrayObject);
  return true;
}

bool FunctionCompiler::emitArrayNewDefault() {
  // This is almost identical to EmitArrayNew, except we skip the
  // initialisation loop.
  uint32_t typeIndex;
  MDefinition* numElements;
  if (!iter().readArrayNewDefault(&typeIndex, &numElements)) {
    return false;
  }

  uint32_t allocSiteIndex = readAllocSiteIndex(typeIndex);

  if (inDeadCode()) {
    return true;
  }

  // Create the array object, default-initialized.
  MDefinition* arrayObject =
      createArrayObject(typeIndex, allocSiteIndex, numElements,
                        /*zeroFields=*/true);
  if (!arrayObject) {
    return false;
  }

  iter().setResult(arrayObject);
  return true;
}

bool FunctionCompiler::emitArrayNewFixed() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex, numElements;
  DefVector values;

  if (!iter().readArrayNewFixed(&typeIndex, &numElements, &values)) {
    return false;
  }
  MOZ_ASSERT(values.length() == numElements);

  uint32_t allocSiteIndex = readAllocSiteIndex(typeIndex);

  if (inDeadCode()) {
    return true;
  }

  MDefinition* numElementsDef = constantI32(int32_t(numElements));
  if (!numElementsDef) {
    return false;
  }

  // Create the array object, uninitialized.
  const ArrayType& arrayType = (*codeMeta().types)[typeIndex].arrayType();
  StorageType elemType = arrayType.elementType();
  uint32_t elemSize = elemType.size();
  MDefinition* arrayObject =
      createArrayObject(typeIndex, allocSiteIndex, numElementsDef,
                        /*zeroFields=*/false);
  if (!arrayObject) {
    return false;
  }

  // Make `base` point at the first byte of the (OOL) data area.
  MDefinition* base = getWasmArrayObjectData(arrayObject);
  if (!base) {
    return false;
  }

  // Write each element in turn.

  // How do we know that the offset expression `i * elemSize` below remains
  // within 2^31 (signed-i32) range?  In the worst case we will have 16-byte
  // values, and there can be at most MaxFunctionBytes expressions, if it were
  // theoretically possible to generate one expression per instruction byte.
  // Hence the max offset we can be expected to generate is
  // `16 * MaxFunctionBytes`.
  static_assert(16 /* sizeof v128 */ * MaxFunctionBytes <=
                MaxArrayPayloadBytes);
  MOZ_RELEASE_ASSERT(numElements <= MaxFunctionBytes);

  for (uint32_t i = 0; i < numElements; i++) {
    if (!mirGen().ensureBallast()) {
      return false;
    }
    // `i * elemSize` is made safe by the assertions above.
    if (!writeGcValueAtBasePlusOffset(
            lineOrBytecode, elemType, arrayObject, AliasSet::WasmArrayDataArea,
            values[numElements - 1 - i], base, i * elemSize, i, false,
            WasmPreBarrierKind::None)) {
      return false;
    }
  }

  iter().setResult(arrayObject);
  return true;
}

bool FunctionCompiler::emitArrayNewData() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex, segIndex;
  MDefinition* segByteOffset;
  MDefinition* numElements;
  if (!iter().readArrayNewData(&typeIndex, &segIndex, &segByteOffset,
                               &numElements)) {
    return false;
  }

  uint32_t allocSiteIndex = readAllocSiteIndex(typeIndex);

  if (inDeadCode()) {
    return true;
  }

  MDefinition* typeIndexValue = constantI32(int32_t(typeIndex));
  if (!typeIndexValue) {
    return false;
  }

  MDefinition* allocSite = loadAllocSiteInstanceData(allocSiteIndex);
  if (!allocSite) {
    return false;
  }

  // Other values we need to pass to the instance call:
  MDefinition* segIndexM = constantI32(int32_t(segIndex));
  if (!segIndexM) {
    return false;
  }

  // Create call:
  // arrayObject = Instance::arrayNewData(segByteOffset:u32, numElements:u32,
  //                                      typeDefData:word, segIndex:u32)
  // If the requested size exceeds MaxArrayPayloadBytes, the MIR generated by
  // this call will trap.
  MDefinition* arrayObject;
  if (!emitInstanceCall5(lineOrBytecode, SASigArrayNewData, segByteOffset,
                         numElements, typeIndexValue, allocSite, segIndexM,
                         &arrayObject)) {
    return false;
  }

  iter().setResult(arrayObject);
  return true;
}

bool FunctionCompiler::emitArrayNewElem() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex, segIndex;
  MDefinition* segElemIndex;
  MDefinition* numElements;
  if (!iter().readArrayNewElem(&typeIndex, &segIndex, &segElemIndex,
                               &numElements)) {
    return false;
  }

  uint32_t allocSiteIndex = readAllocSiteIndex(typeIndex);

  if (inDeadCode()) {
    return true;
  }

  MDefinition* typeIndexValue = constantI32(int32_t(typeIndex));
  if (!typeIndexValue) {
    return false;
  }

  MDefinition* allocSite = loadAllocSiteInstanceData(allocSiteIndex);
  if (!allocSite) {
    return false;
  }

  // Other values we need to pass to the instance call:
  MDefinition* segIndexM = constantI32(int32_t(segIndex));
  if (!segIndexM) {
    return false;
  }

  // Create call:
  // arrayObject = Instance::arrayNewElem(segElemIndex:u32, numElements:u32,
  //                                      typeDefData:word, segIndex:u32)
  // If the requested size exceeds MaxArrayPayloadBytes, the MIR generated by
  // this call will trap.
  MDefinition* arrayObject;
  if (!emitInstanceCall5(lineOrBytecode, SASigArrayNewElem, segElemIndex,
                         numElements, typeIndexValue, allocSite, segIndexM,
                         &arrayObject)) {
    return false;
  }

  iter().setResult(arrayObject);
  return true;
}

bool FunctionCompiler::emitArrayInitData() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t unusedTypeIndex, segIndex;
  MDefinition* array;
  MDefinition* arrayIndex;
  MDefinition* segOffset;
  MDefinition* length;
  if (!iter().readArrayInitData(&unusedTypeIndex, &segIndex, &array,
                                &arrayIndex, &segOffset, &length)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // Other values we need to pass to the instance call:
  MDefinition* segIndexM = constantI32(int32_t(segIndex));
  if (!segIndexM) {
    return false;
  }

  // Create call:
  // Instance::arrayInitData(array:word, index:u32, segByteOffset:u32,
  // numElements:u32, segIndex:u32) If the requested size exceeds
  // MaxArrayPayloadBytes, the MIR generated by this call will trap.
  return emitInstanceCall5(lineOrBytecode, SASigArrayInitData, array,
                           arrayIndex, segOffset, length, segIndexM);
}

bool FunctionCompiler::emitArrayInitElem() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex, segIndex;
  MDefinition* array;
  MDefinition* arrayIndex;
  MDefinition* segOffset;
  MDefinition* length;
  if (!iter().readArrayInitElem(&typeIndex, &segIndex, &array, &arrayIndex,
                                &segOffset, &length)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* typeIndexValue = constantI32(int32_t(typeIndex));
  if (!typeIndexValue) {
    return false;
  }

  // Other values we need to pass to the instance call:
  MDefinition* segIndexM = constantI32(int32_t(segIndex));
  if (!segIndexM) {
    return false;
  }

  // Create call:
  // Instance::arrayInitElem(array:word, index:u32, segByteOffset:u32,
  // numElements:u32, typeDefData:word, segIndex:u32) If the requested size
  // exceeds MaxArrayPayloadBytes, the MIR generated by this call will trap.
  return emitInstanceCall6(lineOrBytecode, SASigArrayInitElem, array,
                           arrayIndex, segOffset, length, typeIndexValue,
                           segIndexM);
}

bool FunctionCompiler::emitArraySet() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex;
  MDefinition* value;
  MDefinition* index;
  MDefinition* arrayObject;
  if (!iter().readArraySet(&typeIndex, &value, &index, &arrayObject)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // Check for null is done at setupForArrayAccess.

  // Create the object null check and the array bounds check and get the OOL
  // data pointer.
  MDefinition* base = setupForArrayAccess(arrayObject, index);
  if (!base) {
    return false;
  }

  // And do the store.
  const ArrayType& arrayType = (*codeMeta().types)[typeIndex].arrayType();
  StorageType elemType = arrayType.elementType();
  uint32_t elemSize = elemType.size();
  MOZ_ASSERT(elemSize >= 1 && elemSize <= 16);

  return writeGcValueAtBasePlusScaledIndex(
      lineOrBytecode, elemType, arrayObject, AliasSet::WasmArrayDataArea, value,
      base, elemSize, index, WasmPreBarrierKind::Normal);
}

bool FunctionCompiler::emitArrayGet(FieldWideningOp wideningOp) {
  uint32_t typeIndex;
  MDefinition* index;
  MDefinition* arrayObject;
  if (!iter().readArrayGet(&typeIndex, wideningOp, &index, &arrayObject)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // Check for null is done at setupForArrayAccess.

  // Create the object null check and the array bounds check and get the data
  // pointer.
  MDefinition* base = setupForArrayAccess(arrayObject, index);
  if (!base) {
    return false;
  }

  // And do the load.
  const ArrayType& arrayType = (*codeMeta().types)[typeIndex].arrayType();
  StorageType elemType = arrayType.elementType();

  MDefinition* load =
      readGcArrayValueAtIndex(elemType, wideningOp, arrayObject,
                              AliasSet::WasmArrayDataArea, base, index);
  if (!load) {
    return false;
  }

  iter().setResult(load);
  return true;
}

bool FunctionCompiler::emitArrayLen() {
  MDefinition* arrayObject;
  if (!iter().readArrayLen(&arrayObject)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // Check for null is done at getWasmArrayObjectNumElements.

  // Get the size value for the array
  MDefinition* numElements = getWasmArrayObjectNumElements(arrayObject);
  if (!numElements) {
    return false;
  }

  iter().setResult(numElements);
  return true;
}

bool FunctionCompiler::emitArrayCopy() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t dstArrayTypeIndex;
  uint32_t srcArrayTypeIndex;
  MDefinition* dstArrayObject;
  MDefinition* dstArrayIndex;
  MDefinition* srcArrayObject;
  MDefinition* srcArrayIndex;
  MDefinition* numElements;
  if (!iter().readArrayCopy(&dstArrayTypeIndex, &srcArrayTypeIndex,
                            &dstArrayObject, &dstArrayIndex, &srcArrayObject,
                            &srcArrayIndex, &numElements)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  const ArrayType& dstArrayType =
      codeMeta().types->type(dstArrayTypeIndex).arrayType();
  StorageType dstElemType = dstArrayType.elementType();
  int32_t elemSize = int32_t(dstElemType.size());
  bool elemsAreRefTyped = dstElemType.isRefType();

  return createArrayCopy(lineOrBytecode, dstArrayObject, dstArrayIndex,
                         srcArrayObject, srcArrayIndex, numElements, elemSize,
                         elemsAreRefTyped);
}

bool FunctionCompiler::emitArrayFill() {
  uint32_t lineOrBytecode = readCallSiteLineOrBytecode();

  uint32_t typeIndex;
  MDefinition* array;
  MDefinition* index;
  MDefinition* val;
  MDefinition* numElements;
  if (!iter().readArrayFill(&typeIndex, &array, &index, &val, &numElements)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  return createArrayFill(lineOrBytecode, typeIndex, array, index, val,
                         numElements);
}

bool FunctionCompiler::emitRefI31() {
  MDefinition* input;
  if (!iter().readConversion(ValType::I32,
                             ValType(RefType::i31().asNonNullable()), &input)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* output = refI31(input);
  if (!output) {
    return false;
  }
  iter().setResult(output);
  return true;
}

bool FunctionCompiler::emitI31Get(FieldWideningOp wideningOp) {
  MOZ_ASSERT(wideningOp != FieldWideningOp::None);

  MDefinition* input;
  if (!iter().readConversion(ValType(RefType::i31()), ValType::I32, &input)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  input = refAsNonNull(input);
  if (!input) {
    return false;
  }
  MDefinition* output = i31Get(input, wideningOp);
  if (!output) {
    return false;
  }
  iter().setResult(output);
  return true;
}

bool FunctionCompiler::emitRefTest(bool nullable) {
  MDefinition* ref;
  RefType sourceType;
  RefType destType;
  if (!iter().readRefTest(nullable, &sourceType, &destType, &ref)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* success = refTest(ref, destType);
  if (!success) {
    return false;
  }

  iter().setResult(success);
  return true;
}

bool FunctionCompiler::emitRefCast(bool nullable) {
  MDefinition* ref;
  RefType sourceType;
  RefType destType;
  if (!iter().readRefCast(nullable, &sourceType, &destType, &ref)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* castedRef = refCast(ref, destType);
  if (!castedRef) {
    return false;
  }

  iter().setResult(castedRef);
  return true;
}

bool FunctionCompiler::emitBrOnCast(bool onSuccess) {
  uint32_t labelRelativeDepth;
  RefType sourceType;
  RefType destType;
  ResultType labelType;
  DefVector values;
  if (!iter().readBrOnCast(onSuccess, &labelRelativeDepth, &sourceType,
                           &destType, &labelType, &values)) {
    return false;
  }

  return brOnCastCommon(onSuccess, labelRelativeDepth, sourceType, destType,
                        labelType, values);
}

bool FunctionCompiler::emitAnyConvertExtern() {
  MDefinition* ref;
  if (!iter().readRefConversion(RefType::extern_(), RefType::any(), &ref)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* conversion = convertAnyExtern(ref, wasm::RefType::Kind::Any);
  if (!conversion) {
    return false;
  }

  iter().setResult(conversion);
  return true;
}

bool FunctionCompiler::emitExternConvertAny() {
  MDefinition* ref;
  if (!iter().readRefConversion(RefType::any(), RefType::extern_(), &ref)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* conversion = convertAnyExtern(ref, wasm::RefType::Kind::Extern);
  if (!conversion) {
    return false;
  }

  iter().setResult(conversion);
  return true;
}

bool FunctionCompiler::emitCallBuiltinModuleFunc() {
  const BuiltinModuleFunc* builtinModuleFunc;

  DefVector params;
  if (!iter().readCallBuiltinModuleFunc(&builtinModuleFunc, &params)) {
    return false;
  }

  return callBuiltinModuleFunc(*builtinModuleFunc, params);
}

bool FunctionCompiler::emitBodyExprs() {
  if (!iter().startFunction(funcIndex())) {
    return false;
  }

#define CHECK(c)          \
  if (!(c)) return false; \
  break

  while (true) {
    if (!mirGen().ensureBallast()) {
      return false;
    }

    OpBytes op;
    if (!iter().readOp(&op)) {
      return false;
    }

    switch (op.b0) {
      case uint16_t(Op::End):
        if (!emitEnd()) {
          return false;
        }
        if (iter().controlStackEmpty()) {
          return true;
        }
        break;

      // Control opcodes
      case uint16_t(Op::Unreachable):
        CHECK(emitUnreachable());
      case uint16_t(Op::Nop):
        CHECK(iter().readNop());
      case uint16_t(Op::Block):
        CHECK(emitBlock());
      case uint16_t(Op::Loop):
        CHECK(emitLoop());
      case uint16_t(Op::If):
        CHECK(emitIf());
      case uint16_t(Op::Else):
        CHECK(emitElse());
      case uint16_t(Op::Try):
        CHECK(emitTry());
      case uint16_t(Op::Catch):
        CHECK(emitCatch());
      case uint16_t(Op::CatchAll):
        CHECK(emitCatchAll());
      case uint16_t(Op::Delegate):
        CHECK(emitDelegate());
      case uint16_t(Op::Throw):
        CHECK(emitThrow());
      case uint16_t(Op::Rethrow):
        CHECK(emitRethrow());
      case uint16_t(Op::ThrowRef):
        if (!codeMeta().exnrefEnabled()) {
          return iter().unrecognizedOpcode(&op);
        }
        CHECK(emitThrowRef());
      case uint16_t(Op::TryTable):
        if (!codeMeta().exnrefEnabled()) {
          return iter().unrecognizedOpcode(&op);
        }
        CHECK(emitTryTable());
      case uint16_t(Op::Br):
        CHECK(emitBr());
      case uint16_t(Op::BrIf):
        CHECK(emitBrIf());
      case uint16_t(Op::BrTable):
        CHECK(emitBrTable());
      case uint16_t(Op::Return):
        CHECK(emitReturn());

      // Calls
      case uint16_t(Op::Call):
        CHECK(emitCall(/* asmJSFuncDef = */ false));
      case uint16_t(Op::CallIndirect):
        CHECK(emitCallIndirect(/* oldStyle = */ false));

      // Parametric operators
      case uint16_t(Op::Drop):
        CHECK(iter().readDrop());
      case uint16_t(Op::SelectNumeric):
        CHECK(emitSelect(/*typed*/ false));
      case uint16_t(Op::SelectTyped):
        CHECK(emitSelect(/*typed*/ true));

      // Locals and globals
      case uint16_t(Op::LocalGet):
        CHECK(emitGetLocal());
      case uint16_t(Op::LocalSet):
        CHECK(emitSetLocal());
      case uint16_t(Op::LocalTee):
        CHECK(emitTeeLocal());
      case uint16_t(Op::GlobalGet):
        CHECK(emitGetGlobal());
      case uint16_t(Op::GlobalSet):
        CHECK(emitSetGlobal());
      case uint16_t(Op::TableGet):
        CHECK(emitTableGet());
      case uint16_t(Op::TableSet):
        CHECK(emitTableSet());

      // Memory-related operators
      case uint16_t(Op::I32Load):
        CHECK(emitLoad(ValType::I32, Scalar::Int32));
      case uint16_t(Op::I64Load):
        CHECK(emitLoad(ValType::I64, Scalar::Int64));
      case uint16_t(Op::F32Load):
        CHECK(emitLoad(ValType::F32, Scalar::Float32));
      case uint16_t(Op::F64Load):
        CHECK(emitLoad(ValType::F64, Scalar::Float64));
      case uint16_t(Op::I32Load8S):
        CHECK(emitLoad(ValType::I32, Scalar::Int8));
      case uint16_t(Op::I32Load8U):
        CHECK(emitLoad(ValType::I32, Scalar::Uint8));
      case uint16_t(Op::I32Load16S):
        CHECK(emitLoad(ValType::I32, Scalar::Int16));
      case uint16_t(Op::I32Load16U):
        CHECK(emitLoad(ValType::I32, Scalar::Uint16));
      case uint16_t(Op::I64Load8S):
        CHECK(emitLoad(ValType::I64, Scalar::Int8));
      case uint16_t(Op::I64Load8U):
        CHECK(emitLoad(ValType::I64, Scalar::Uint8));
      case uint16_t(Op::I64Load16S):
        CHECK(emitLoad(ValType::I64, Scalar::Int16));
      case uint16_t(Op::I64Load16U):
        CHECK(emitLoad(ValType::I64, Scalar::Uint16));
      case uint16_t(Op::I64Load32S):
        CHECK(emitLoad(ValType::I64, Scalar::Int32));
      case uint16_t(Op::I64Load32U):
        CHECK(emitLoad(ValType::I64, Scalar::Uint32));
      case uint16_t(Op::I32Store):
        CHECK(emitStore(ValType::I32, Scalar::Int32));
      case uint16_t(Op::I64Store):
        CHECK(emitStore(ValType::I64, Scalar::Int64));
      case uint16_t(Op::F32Store):
        CHECK(emitStore(ValType::F32, Scalar::Float32));
      case uint16_t(Op::F64Store):
        CHECK(emitStore(ValType::F64, Scalar::Float64));
      case uint16_t(Op::I32Store8):
        CHECK(emitStore(ValType::I32, Scalar::Int8));
      case uint16_t(Op::I32Store16):
        CHECK(emitStore(ValType::I32, Scalar::Int16));
      case uint16_t(Op::I64Store8):
        CHECK(emitStore(ValType::I64, Scalar::Int8));
      case uint16_t(Op::I64Store16):
        CHECK(emitStore(ValType::I64, Scalar::Int16));
      case uint16_t(Op::I64Store32):
        CHECK(emitStore(ValType::I64, Scalar::Int32));
      case uint16_t(Op::MemorySize):
        CHECK(emitMemorySize());
      case uint16_t(Op::MemoryGrow):
        CHECK(emitMemoryGrow());

      // Constants
      case uint16_t(Op::I32Const):
        CHECK(emitI32Const());
      case uint16_t(Op::I64Const):
        CHECK(emitI64Const());
      case uint16_t(Op::F32Const):
        CHECK(emitF32Const());
      case uint16_t(Op::F64Const):
        CHECK(emitF64Const());

      // Comparison operators
      case uint16_t(Op::I32Eqz):
        CHECK(emitConversion<MNot>(ValType::I32, ValType::I32));
      case uint16_t(Op::I32Eq):
        CHECK(emitComparison(ValType::I32, JSOp::Eq, MCompare::Compare_Int32));
      case uint16_t(Op::I32Ne):
        CHECK(emitComparison(ValType::I32, JSOp::Ne, MCompare::Compare_Int32));
      case uint16_t(Op::I32LtS):
        CHECK(emitComparison(ValType::I32, JSOp::Lt, MCompare::Compare_Int32));
      case uint16_t(Op::I32LtU):
        CHECK(emitComparison(ValType::I32, JSOp::Lt, MCompare::Compare_UInt32));
      case uint16_t(Op::I32GtS):
        CHECK(emitComparison(ValType::I32, JSOp::Gt, MCompare::Compare_Int32));
      case uint16_t(Op::I32GtU):
        CHECK(emitComparison(ValType::I32, JSOp::Gt, MCompare::Compare_UInt32));
      case uint16_t(Op::I32LeS):
        CHECK(emitComparison(ValType::I32, JSOp::Le, MCompare::Compare_Int32));
      case uint16_t(Op::I32LeU):
        CHECK(emitComparison(ValType::I32, JSOp::Le, MCompare::Compare_UInt32));
      case uint16_t(Op::I32GeS):
        CHECK(emitComparison(ValType::I32, JSOp::Ge, MCompare::Compare_Int32));
      case uint16_t(Op::I32GeU):
        CHECK(emitComparison(ValType::I32, JSOp::Ge, MCompare::Compare_UInt32));
      case uint16_t(Op::I64Eqz):
        CHECK(emitConversion<MNot>(ValType::I64, ValType::I32));
      case uint16_t(Op::I64Eq):
        CHECK(emitComparison(ValType::I64, JSOp::Eq, MCompare::Compare_Int64));
      case uint16_t(Op::I64Ne):
        CHECK(emitComparison(ValType::I64, JSOp::Ne, MCompare::Compare_Int64));
      case uint16_t(Op::I64LtS):
        CHECK(emitComparison(ValType::I64, JSOp::Lt, MCompare::Compare_Int64));
      case uint16_t(Op::I64LtU):
        CHECK(emitComparison(ValType::I64, JSOp::Lt, MCompare::Compare_UInt64));
      case uint16_t(Op::I64GtS):
        CHECK(emitComparison(ValType::I64, JSOp::Gt, MCompare::Compare_Int64));
      case uint16_t(Op::I64GtU):
        CHECK(emitComparison(ValType::I64, JSOp::Gt, MCompare::Compare_UInt64));
      case uint16_t(Op::I64LeS):
        CHECK(emitComparison(ValType::I64, JSOp::Le, MCompare::Compare_Int64));
      case uint16_t(Op::I64LeU):
        CHECK(emitComparison(ValType::I64, JSOp::Le, MCompare::Compare_UInt64));
      case uint16_t(Op::I64GeS):
        CHECK(emitComparison(ValType::I64, JSOp::Ge, MCompare::Compare_Int64));
      case uint16_t(Op::I64GeU):
        CHECK(emitComparison(ValType::I64, JSOp::Ge, MCompare::Compare_UInt64));
      case uint16_t(Op::F32Eq):
        CHECK(
            emitComparison(ValType::F32, JSOp::Eq, MCompare::Compare_Float32));
      case uint16_t(Op::F32Ne):
        CHECK(
            emitComparison(ValType::F32, JSOp::Ne, MCompare::Compare_Float32));
      case uint16_t(Op::F32Lt):
        CHECK(
            emitComparison(ValType::F32, JSOp::Lt, MCompare::Compare_Float32));
      case uint16_t(Op::F32Gt):
        CHECK(
            emitComparison(ValType::F32, JSOp::Gt, MCompare::Compare_Float32));
      case uint16_t(Op::F32Le):
        CHECK(
            emitComparison(ValType::F32, JSOp::Le, MCompare::Compare_Float32));
      case uint16_t(Op::F32Ge):
        CHECK(
            emitComparison(ValType::F32, JSOp::Ge, MCompare::Compare_Float32));
      case uint16_t(Op::F64Eq):
        CHECK(emitComparison(ValType::F64, JSOp::Eq, MCompare::Compare_Double));
      case uint16_t(Op::F64Ne):
        CHECK(emitComparison(ValType::F64, JSOp::Ne, MCompare::Compare_Double));
      case uint16_t(Op::F64Lt):
        CHECK(emitComparison(ValType::F64, JSOp::Lt, MCompare::Compare_Double));
      case uint16_t(Op::F64Gt):
        CHECK(emitComparison(ValType::F64, JSOp::Gt, MCompare::Compare_Double));
      case uint16_t(Op::F64Le):
        CHECK(emitComparison(ValType::F64, JSOp::Le, MCompare::Compare_Double));
      case uint16_t(Op::F64Ge):
        CHECK(emitComparison(ValType::F64, JSOp::Ge, MCompare::Compare_Double));

      // Numeric operators
      case uint16_t(Op::I32Clz):
        CHECK(emitUnaryWithType<MClz>(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Ctz):
        CHECK(emitUnaryWithType<MCtz>(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Popcnt):
        CHECK(emitUnaryWithType<MPopcnt>(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Add):
        CHECK(emitAdd(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Sub):
        CHECK(emitSub(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Mul):
        CHECK(emitMul(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32DivS):
      case uint16_t(Op::I32DivU):
        CHECK(emitDiv(ValType::I32, MIRType::Int32, Op(op.b0) == Op::I32DivU));
      case uint16_t(Op::I32RemS):
      case uint16_t(Op::I32RemU):
        CHECK(emitRem(ValType::I32, MIRType::Int32, Op(op.b0) == Op::I32RemU));
      case uint16_t(Op::I32And):
        CHECK(emitBitwiseAndOrXor(ValType::I32, MIRType::Int32,
                                  MWasmBinaryBitwise::SubOpcode::And));
      case uint16_t(Op::I32Or):
        CHECK(emitBitwiseAndOrXor(ValType::I32, MIRType::Int32,
                                  MWasmBinaryBitwise::SubOpcode::Or));
      case uint16_t(Op::I32Xor):
        CHECK(emitBitwiseAndOrXor(ValType::I32, MIRType::Int32,
                                  MWasmBinaryBitwise::SubOpcode::Xor));
      case uint16_t(Op::I32Shl):
        CHECK(emitShift<MLsh>(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32ShrS):
        CHECK(emitShift<MRsh>(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32ShrU):
        CHECK(emitUrsh(ValType::I32, MIRType::Int32));
      case uint16_t(Op::I32Rotl):
      case uint16_t(Op::I32Rotr):
        CHECK(emitRotate(ValType::I32, Op(op.b0) == Op::I32Rotl));
      case uint16_t(Op::I64Clz):
        CHECK(emitUnaryWithType<MClz>(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Ctz):
        CHECK(emitUnaryWithType<MCtz>(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Popcnt):
        CHECK(emitUnaryWithType<MPopcnt>(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Add):
        CHECK(emitAdd(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Sub):
        CHECK(emitSub(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Mul):
        CHECK(emitMul(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64DivS):
      case uint16_t(Op::I64DivU):
        CHECK(emitDiv(ValType::I64, MIRType::Int64, Op(op.b0) == Op::I64DivU));
      case uint16_t(Op::I64RemS):
      case uint16_t(Op::I64RemU):
        CHECK(emitRem(ValType::I64, MIRType::Int64, Op(op.b0) == Op::I64RemU));
      case uint16_t(Op::I64And):
        CHECK(emitBitwiseAndOrXor(ValType::I64, MIRType::Int64,
                                  MWasmBinaryBitwise::SubOpcode::And));
      case uint16_t(Op::I64Or):
        CHECK(emitBitwiseAndOrXor(ValType::I64, MIRType::Int64,
                                  MWasmBinaryBitwise::SubOpcode::Or));
      case uint16_t(Op::I64Xor):
        CHECK(emitBitwiseAndOrXor(ValType::I64, MIRType::Int64,
                                  MWasmBinaryBitwise::SubOpcode::Xor));
      case uint16_t(Op::I64Shl):
        CHECK(emitShift<MLsh>(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64ShrS):
        CHECK(emitShift<MRsh>(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64ShrU):
        CHECK(emitUrsh(ValType::I64, MIRType::Int64));
      case uint16_t(Op::I64Rotl):
      case uint16_t(Op::I64Rotr):
        CHECK(emitRotate(ValType::I64, Op(op.b0) == Op::I64Rotl));
      case uint16_t(Op::F32Abs):
        CHECK(emitUnaryWithType<MAbs>(ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Neg):
        CHECK(emitUnaryWithType<MWasmNeg>(ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Ceil):
        CHECK(emitUnaryMathBuiltinCall(SASigCeilF));
      case uint16_t(Op::F32Floor):
        CHECK(emitUnaryMathBuiltinCall(SASigFloorF));
      case uint16_t(Op::F32Trunc):
        CHECK(emitUnaryMathBuiltinCall(SASigTruncF));
      case uint16_t(Op::F32Nearest):
        CHECK(emitUnaryMathBuiltinCall(SASigNearbyIntF));
      case uint16_t(Op::F32Sqrt):
        CHECK(emitUnaryWithType<MSqrt>(ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Add):
        CHECK(emitAdd(ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Sub):
        CHECK(emitSub(ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Mul):
        CHECK(emitMul(ValType::F32, MIRType::Float32));
      case uint16_t(Op::F32Div):
        CHECK(emitDiv(ValType::F32, MIRType::Float32,
                      /* isUnsigned = */ false));
      case uint16_t(Op::F32Min):
      case uint16_t(Op::F32Max):
        CHECK(emitMinMax(ValType::F32, MIRType::Float32,
                         Op(op.b0) == Op::F32Max));
      case uint16_t(Op::F32CopySign):
        CHECK(emitCopySign(ValType::F32));
      case uint16_t(Op::F64Abs):
        CHECK(emitUnaryWithType<MAbs>(ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Neg):
        CHECK(emitUnaryWithType<MWasmNeg>(ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Ceil):
        CHECK(emitUnaryMathBuiltinCall(SASigCeilD));
      case uint16_t(Op::F64Floor):
        CHECK(emitUnaryMathBuiltinCall(SASigFloorD));
      case uint16_t(Op::F64Trunc):
        CHECK(emitUnaryMathBuiltinCall(SASigTruncD));
      case uint16_t(Op::F64Nearest):
        CHECK(emitUnaryMathBuiltinCall(SASigNearbyIntD));
      case uint16_t(Op::F64Sqrt):
        CHECK(emitUnaryWithType<MSqrt>(ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Add):
        CHECK(emitAdd(ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Sub):
        CHECK(emitSub(ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Mul):
        CHECK(emitMul(ValType::F64, MIRType::Double));
      case uint16_t(Op::F64Div):
        CHECK(emitDiv(ValType::F64, MIRType::Double,
                      /* isUnsigned = */ false));
      case uint16_t(Op::F64Min):
      case uint16_t(Op::F64Max):
        CHECK(
            emitMinMax(ValType::F64, MIRType::Double, Op(op.b0) == Op::F64Max));
      case uint16_t(Op::F64CopySign):
        CHECK(emitCopySign(ValType::F64));

      // Conversions
      case uint16_t(Op::I32WrapI64):
        CHECK(emitConversion<MWrapInt64ToInt32>(ValType::I64, ValType::I32));
      case uint16_t(Op::I32TruncF32S):
      case uint16_t(Op::I32TruncF32U):
        CHECK(emitTruncate(ValType::F32, ValType::I32,
                           Op(op.b0) == Op::I32TruncF32U, false));
      case uint16_t(Op::I32TruncF64S):
      case uint16_t(Op::I32TruncF64U):
        CHECK(emitTruncate(ValType::F64, ValType::I32,
                           Op(op.b0) == Op::I32TruncF64U, false));
      case uint16_t(Op::I64ExtendI32S):
      case uint16_t(Op::I64ExtendI32U):
        CHECK(emitExtendI32(Op(op.b0) == Op::I64ExtendI32U));
      case uint16_t(Op::I64TruncF32S):
      case uint16_t(Op::I64TruncF32U):
        CHECK(emitTruncate(ValType::F32, ValType::I64,
                           Op(op.b0) == Op::I64TruncF32U, false));
      case uint16_t(Op::I64TruncF64S):
      case uint16_t(Op::I64TruncF64U):
        CHECK(emitTruncate(ValType::F64, ValType::I64,
                           Op(op.b0) == Op::I64TruncF64U, false));
      case uint16_t(Op::F32ConvertI32S):
        CHECK(emitConversion<MToFloat32>(ValType::I32, ValType::F32));
      case uint16_t(Op::F32ConvertI32U):
        CHECK(
            emitConversion<MWasmUnsignedToFloat32>(ValType::I32, ValType::F32));
      case uint16_t(Op::F32ConvertI64S):
      case uint16_t(Op::F32ConvertI64U):
        CHECK(emitConvertI64ToFloatingPoint(ValType::F32, MIRType::Float32,
                                            Op(op.b0) == Op::F32ConvertI64U));
      case uint16_t(Op::F32DemoteF64):
        CHECK(emitConversion<MToFloat32>(ValType::F64, ValType::F32));
      case uint16_t(Op::F64ConvertI32S):
        CHECK(emitConversion<MToDouble>(ValType::I32, ValType::F64));
      case uint16_t(Op::F64ConvertI32U):
        CHECK(
            emitConversion<MWasmUnsignedToDouble>(ValType::I32, ValType::F64));
      case uint16_t(Op::F64ConvertI64S):
      case uint16_t(Op::F64ConvertI64U):
        CHECK(emitConvertI64ToFloatingPoint(ValType::F64, MIRType::Double,
                                            Op(op.b0) == Op::F64ConvertI64U));
      case uint16_t(Op::F64PromoteF32):
        CHECK(emitConversion<MToDouble>(ValType::F32, ValType::F64));

      // Reinterpretations
      case uint16_t(Op::I32ReinterpretF32):
        CHECK(emitReinterpret(ValType::I32, ValType::F32, MIRType::Int32));
      case uint16_t(Op::I64ReinterpretF64):
        CHECK(emitReinterpret(ValType::I64, ValType::F64, MIRType::Int64));
      case uint16_t(Op::F32ReinterpretI32):
        CHECK(emitReinterpret(ValType::F32, ValType::I32, MIRType::Float32));
      case uint16_t(Op::F64ReinterpretI64):
        CHECK(emitReinterpret(ValType::F64, ValType::I64, MIRType::Double));

      case uint16_t(Op::RefEq):
        CHECK(emitComparison(RefType::eq(), JSOp::Eq,
                             MCompare::Compare_WasmAnyRef));
      case uint16_t(Op::RefFunc):
        CHECK(emitRefFunc());
      case uint16_t(Op::RefNull):
        CHECK(emitRefNull());
      case uint16_t(Op::RefIsNull):
        CHECK(emitRefIsNull());

      // Sign extensions
      case uint16_t(Op::I32Extend8S):
        CHECK(emitSignExtend(1, 4));
      case uint16_t(Op::I32Extend16S):
        CHECK(emitSignExtend(2, 4));
      case uint16_t(Op::I64Extend8S):
        CHECK(emitSignExtend(1, 8));
      case uint16_t(Op::I64Extend16S):
        CHECK(emitSignExtend(2, 8));
      case uint16_t(Op::I64Extend32S):
        CHECK(emitSignExtend(4, 8));

      case uint16_t(Op::ReturnCall): {
        CHECK(emitReturnCall());
      }
      case uint16_t(Op::ReturnCallIndirect): {
        CHECK(emitReturnCallIndirect());
      }

      case uint16_t(Op::RefAsNonNull):
        CHECK(emitRefAsNonNull());
      case uint16_t(Op::BrOnNull): {
        CHECK(emitBrOnNull());
      }
      case uint16_t(Op::BrOnNonNull): {
        CHECK(emitBrOnNonNull());
      }
      case uint16_t(Op::CallRef): {
        CHECK(emitCallRef());
      }

      case uint16_t(Op::ReturnCallRef): {
        CHECK(emitReturnCallRef());
      }

      // Gc operations
      case uint16_t(Op::GcPrefix): {
        switch (op.b1) {
          case uint32_t(GcOp::StructNew):
            CHECK(emitStructNew());
          case uint32_t(GcOp::StructNewDefault):
            CHECK(emitStructNewDefault());
          case uint32_t(GcOp::StructSet):
            CHECK(emitStructSet());
          case uint32_t(GcOp::StructGet):
            CHECK(emitStructGet(FieldWideningOp::None));
          case uint32_t(GcOp::StructGetS):
            CHECK(emitStructGet(FieldWideningOp::Signed));
          case uint32_t(GcOp::StructGetU):
            CHECK(emitStructGet(FieldWideningOp::Unsigned));
          case uint32_t(GcOp::ArrayNew):
            CHECK(emitArrayNew());
          case uint32_t(GcOp::ArrayNewDefault):
            CHECK(emitArrayNewDefault());
          case uint32_t(GcOp::ArrayNewFixed):
            CHECK(emitArrayNewFixed());
          case uint32_t(GcOp::ArrayNewData):
            CHECK(emitArrayNewData());
          case uint32_t(GcOp::ArrayNewElem):
            CHECK(emitArrayNewElem());
          case uint32_t(GcOp::ArrayInitData):
            CHECK(emitArrayInitData());
          case uint32_t(GcOp::ArrayInitElem):
            CHECK(emitArrayInitElem());
          case uint32_t(GcOp::ArraySet):
            CHECK(emitArraySet());
          case uint32_t(GcOp::ArrayGet):
            CHECK(emitArrayGet(FieldWideningOp::None));
          case uint32_t(GcOp::ArrayGetS):
            CHECK(emitArrayGet(FieldWideningOp::Signed));
          case uint32_t(GcOp::ArrayGetU):
            CHECK(emitArrayGet(FieldWideningOp::Unsigned));
          case uint32_t(GcOp::ArrayLen):
            CHECK(emitArrayLen());
          case uint32_t(GcOp::ArrayCopy):
            CHECK(emitArrayCopy());
          case uint32_t(GcOp::ArrayFill):
            CHECK(emitArrayFill());
          case uint32_t(GcOp::RefI31):
            CHECK(emitRefI31());
          case uint32_t(GcOp::I31GetS):
            CHECK(emitI31Get(FieldWideningOp::Signed));
          case uint32_t(GcOp::I31GetU):
            CHECK(emitI31Get(FieldWideningOp::Unsigned));
          case uint32_t(GcOp::BrOnCast):
            CHECK(emitBrOnCast(/*onSuccess=*/true));
          case uint32_t(GcOp::BrOnCastFail):
            CHECK(emitBrOnCast(/*onSuccess=*/false));
          case uint32_t(GcOp::RefTest):
            CHECK(emitRefTest(/*nullable=*/false));
          case uint32_t(GcOp::RefTestNull):
            CHECK(emitRefTest(/*nullable=*/true));
          case uint32_t(GcOp::RefCast):
            CHECK(emitRefCast(/*nullable=*/false));
          case uint32_t(GcOp::RefCastNull):
            CHECK(emitRefCast(/*nullable=*/true));
          case uint16_t(GcOp::AnyConvertExtern):
            CHECK(emitAnyConvertExtern());
          case uint16_t(GcOp::ExternConvertAny):
            CHECK(emitExternConvertAny());
          default:
            return iter().unrecognizedOpcode(&op);
        }  // switch (op.b1)
        break;
      }

      // SIMD operations
#ifdef ENABLE_WASM_SIMD
      case uint16_t(Op::SimdPrefix): {
        if (!codeMeta().simdAvailable()) {
          return iter().unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(SimdOp::V128Const):
            CHECK(emitConstSimd128());
          case uint32_t(SimdOp::V128Load):
            CHECK(emitLoad(ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V128Store):
            CHECK(emitStore(ValType::V128, Scalar::Simd128));
          case uint32_t(SimdOp::V128And):
          case uint32_t(SimdOp::V128Or):
          case uint32_t(SimdOp::V128Xor):
          case uint32_t(SimdOp::I8x16AvgrU):
          case uint32_t(SimdOp::I16x8AvgrU):
          case uint32_t(SimdOp::I8x16Add):
          case uint32_t(SimdOp::I8x16AddSatS):
          case uint32_t(SimdOp::I8x16AddSatU):
          case uint32_t(SimdOp::I8x16MinS):
          case uint32_t(SimdOp::I8x16MinU):
          case uint32_t(SimdOp::I8x16MaxS):
          case uint32_t(SimdOp::I8x16MaxU):
          case uint32_t(SimdOp::I16x8Add):
          case uint32_t(SimdOp::I16x8AddSatS):
          case uint32_t(SimdOp::I16x8AddSatU):
          case uint32_t(SimdOp::I16x8Mul):
          case uint32_t(SimdOp::I16x8MinS):
          case uint32_t(SimdOp::I16x8MinU):
          case uint32_t(SimdOp::I16x8MaxS):
          case uint32_t(SimdOp::I16x8MaxU):
          case uint32_t(SimdOp::I32x4Add):
          case uint32_t(SimdOp::I32x4Mul):
          case uint32_t(SimdOp::I32x4MinS):
          case uint32_t(SimdOp::I32x4MinU):
          case uint32_t(SimdOp::I32x4MaxS):
          case uint32_t(SimdOp::I32x4MaxU):
          case uint32_t(SimdOp::I64x2Add):
          case uint32_t(SimdOp::I64x2Mul):
          case uint32_t(SimdOp::F32x4Add):
          case uint32_t(SimdOp::F32x4Mul):
          case uint32_t(SimdOp::F32x4Min):
          case uint32_t(SimdOp::F32x4Max):
          case uint32_t(SimdOp::F64x2Add):
          case uint32_t(SimdOp::F64x2Mul):
          case uint32_t(SimdOp::F64x2Min):
          case uint32_t(SimdOp::F64x2Max):
          case uint32_t(SimdOp::I8x16Eq):
          case uint32_t(SimdOp::I8x16Ne):
          case uint32_t(SimdOp::I16x8Eq):
          case uint32_t(SimdOp::I16x8Ne):
          case uint32_t(SimdOp::I32x4Eq):
          case uint32_t(SimdOp::I32x4Ne):
          case uint32_t(SimdOp::I64x2Eq):
          case uint32_t(SimdOp::I64x2Ne):
          case uint32_t(SimdOp::F32x4Eq):
          case uint32_t(SimdOp::F32x4Ne):
          case uint32_t(SimdOp::F64x2Eq):
          case uint32_t(SimdOp::F64x2Ne):
          case uint32_t(SimdOp::I32x4DotI16x8S):
          case uint32_t(SimdOp::I16x8ExtmulLowI8x16S):
          case uint32_t(SimdOp::I16x8ExtmulHighI8x16S):
          case uint32_t(SimdOp::I16x8ExtmulLowI8x16U):
          case uint32_t(SimdOp::I16x8ExtmulHighI8x16U):
          case uint32_t(SimdOp::I32x4ExtmulLowI16x8S):
          case uint32_t(SimdOp::I32x4ExtmulHighI16x8S):
          case uint32_t(SimdOp::I32x4ExtmulLowI16x8U):
          case uint32_t(SimdOp::I32x4ExtmulHighI16x8U):
          case uint32_t(SimdOp::I64x2ExtmulLowI32x4S):
          case uint32_t(SimdOp::I64x2ExtmulHighI32x4S):
          case uint32_t(SimdOp::I64x2ExtmulLowI32x4U):
          case uint32_t(SimdOp::I64x2ExtmulHighI32x4U):
          case uint32_t(SimdOp::I16x8Q15MulrSatS):
            CHECK(emitBinarySimd128(/* commutative= */ true, SimdOp(op.b1)));
          case uint32_t(SimdOp::V128AndNot):
          case uint32_t(SimdOp::I8x16Sub):
          case uint32_t(SimdOp::I8x16SubSatS):
          case uint32_t(SimdOp::I8x16SubSatU):
          case uint32_t(SimdOp::I16x8Sub):
          case uint32_t(SimdOp::I16x8SubSatS):
          case uint32_t(SimdOp::I16x8SubSatU):
          case uint32_t(SimdOp::I32x4Sub):
          case uint32_t(SimdOp::I64x2Sub):
          case uint32_t(SimdOp::F32x4Sub):
          case uint32_t(SimdOp::F32x4Div):
          case uint32_t(SimdOp::F64x2Sub):
          case uint32_t(SimdOp::F64x2Div):
          case uint32_t(SimdOp::I8x16NarrowI16x8S):
          case uint32_t(SimdOp::I8x16NarrowI16x8U):
          case uint32_t(SimdOp::I16x8NarrowI32x4S):
          case uint32_t(SimdOp::I16x8NarrowI32x4U):
          case uint32_t(SimdOp::I8x16LtS):
          case uint32_t(SimdOp::I8x16LtU):
          case uint32_t(SimdOp::I8x16GtS):
          case uint32_t(SimdOp::I8x16GtU):
          case uint32_t(SimdOp::I8x16LeS):
          case uint32_t(SimdOp::I8x16LeU):
          case uint32_t(SimdOp::I8x16GeS):
          case uint32_t(SimdOp::I8x16GeU):
          case uint32_t(SimdOp::I16x8LtS):
          case uint32_t(SimdOp::I16x8LtU):
          case uint32_t(SimdOp::I16x8GtS):
          case uint32_t(SimdOp::I16x8GtU):
          case uint32_t(SimdOp::I16x8LeS):
          case uint32_t(SimdOp::I16x8LeU):
          case uint32_t(SimdOp::I16x8GeS):
          case uint32_t(SimdOp::I16x8GeU):
          case uint32_t(SimdOp::I32x4LtS):
          case uint32_t(SimdOp::I32x4LtU):
          case uint32_t(SimdOp::I32x4GtS):
          case uint32_t(SimdOp::I32x4GtU):
          case uint32_t(SimdOp::I32x4LeS):
          case uint32_t(SimdOp::I32x4LeU):
          case uint32_t(SimdOp::I32x4GeS):
          case uint32_t(SimdOp::I32x4GeU):
          case uint32_t(SimdOp::I64x2LtS):
          case uint32_t(SimdOp::I64x2GtS):
          case uint32_t(SimdOp::I64x2LeS):
          case uint32_t(SimdOp::I64x2GeS):
          case uint32_t(SimdOp::F32x4Lt):
          case uint32_t(SimdOp::F32x4Gt):
          case uint32_t(SimdOp::F32x4Le):
          case uint32_t(SimdOp::F32x4Ge):
          case uint32_t(SimdOp::F64x2Lt):
          case uint32_t(SimdOp::F64x2Gt):
          case uint32_t(SimdOp::F64x2Le):
          case uint32_t(SimdOp::F64x2Ge):
          case uint32_t(SimdOp::I8x16Swizzle):
          case uint32_t(SimdOp::F32x4PMax):
          case uint32_t(SimdOp::F32x4PMin):
          case uint32_t(SimdOp::F64x2PMax):
          case uint32_t(SimdOp::F64x2PMin):
            CHECK(emitBinarySimd128(/* commutative= */ false, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16Splat):
          case uint32_t(SimdOp::I16x8Splat):
          case uint32_t(SimdOp::I32x4Splat):
            CHECK(emitSplatSimd128(ValType::I32, SimdOp(op.b1)));
          case uint32_t(SimdOp::I64x2Splat):
            CHECK(emitSplatSimd128(ValType::I64, SimdOp(op.b1)));
          case uint32_t(SimdOp::F32x4Splat):
            CHECK(emitSplatSimd128(ValType::F32, SimdOp(op.b1)));
          case uint32_t(SimdOp::F64x2Splat):
            CHECK(emitSplatSimd128(ValType::F64, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16Neg):
          case uint32_t(SimdOp::I16x8Neg):
          case uint32_t(SimdOp::I16x8ExtendLowI8x16S):
          case uint32_t(SimdOp::I16x8ExtendHighI8x16S):
          case uint32_t(SimdOp::I16x8ExtendLowI8x16U):
          case uint32_t(SimdOp::I16x8ExtendHighI8x16U):
          case uint32_t(SimdOp::I32x4Neg):
          case uint32_t(SimdOp::I32x4ExtendLowI16x8S):
          case uint32_t(SimdOp::I32x4ExtendHighI16x8S):
          case uint32_t(SimdOp::I32x4ExtendLowI16x8U):
          case uint32_t(SimdOp::I32x4ExtendHighI16x8U):
          case uint32_t(SimdOp::I32x4TruncSatF32x4S):
          case uint32_t(SimdOp::I32x4TruncSatF32x4U):
          case uint32_t(SimdOp::I64x2Neg):
          case uint32_t(SimdOp::I64x2ExtendLowI32x4S):
          case uint32_t(SimdOp::I64x2ExtendHighI32x4S):
          case uint32_t(SimdOp::I64x2ExtendLowI32x4U):
          case uint32_t(SimdOp::I64x2ExtendHighI32x4U):
          case uint32_t(SimdOp::F32x4Abs):
          case uint32_t(SimdOp::F32x4Neg):
          case uint32_t(SimdOp::F32x4Sqrt):
          case uint32_t(SimdOp::F32x4ConvertI32x4S):
          case uint32_t(SimdOp::F32x4ConvertI32x4U):
          case uint32_t(SimdOp::F64x2Abs):
          case uint32_t(SimdOp::F64x2Neg):
          case uint32_t(SimdOp::F64x2Sqrt):
          case uint32_t(SimdOp::V128Not):
          case uint32_t(SimdOp::I8x16Popcnt):
          case uint32_t(SimdOp::I8x16Abs):
          case uint32_t(SimdOp::I16x8Abs):
          case uint32_t(SimdOp::I32x4Abs):
          case uint32_t(SimdOp::I64x2Abs):
          case uint32_t(SimdOp::F32x4Ceil):
          case uint32_t(SimdOp::F32x4Floor):
          case uint32_t(SimdOp::F32x4Trunc):
          case uint32_t(SimdOp::F32x4Nearest):
          case uint32_t(SimdOp::F64x2Ceil):
          case uint32_t(SimdOp::F64x2Floor):
          case uint32_t(SimdOp::F64x2Trunc):
          case uint32_t(SimdOp::F64x2Nearest):
          case uint32_t(SimdOp::F32x4DemoteF64x2Zero):
          case uint32_t(SimdOp::F64x2PromoteLowF32x4):
          case uint32_t(SimdOp::F64x2ConvertLowI32x4S):
          case uint32_t(SimdOp::F64x2ConvertLowI32x4U):
          case uint32_t(SimdOp::I32x4TruncSatF64x2SZero):
          case uint32_t(SimdOp::I32x4TruncSatF64x2UZero):
          case uint32_t(SimdOp::I16x8ExtaddPairwiseI8x16S):
          case uint32_t(SimdOp::I16x8ExtaddPairwiseI8x16U):
          case uint32_t(SimdOp::I32x4ExtaddPairwiseI16x8S):
          case uint32_t(SimdOp::I32x4ExtaddPairwiseI16x8U):
            CHECK(emitUnarySimd128(SimdOp(op.b1)));
          case uint32_t(SimdOp::V128AnyTrue):
          case uint32_t(SimdOp::I8x16AllTrue):
          case uint32_t(SimdOp::I16x8AllTrue):
          case uint32_t(SimdOp::I32x4AllTrue):
          case uint32_t(SimdOp::I64x2AllTrue):
          case uint32_t(SimdOp::I8x16Bitmask):
          case uint32_t(SimdOp::I16x8Bitmask):
          case uint32_t(SimdOp::I32x4Bitmask):
          case uint32_t(SimdOp::I64x2Bitmask):
            CHECK(emitReduceSimd128(SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16Shl):
          case uint32_t(SimdOp::I8x16ShrS):
          case uint32_t(SimdOp::I8x16ShrU):
          case uint32_t(SimdOp::I16x8Shl):
          case uint32_t(SimdOp::I16x8ShrS):
          case uint32_t(SimdOp::I16x8ShrU):
          case uint32_t(SimdOp::I32x4Shl):
          case uint32_t(SimdOp::I32x4ShrS):
          case uint32_t(SimdOp::I32x4ShrU):
          case uint32_t(SimdOp::I64x2Shl):
          case uint32_t(SimdOp::I64x2ShrS):
          case uint32_t(SimdOp::I64x2ShrU):
            CHECK(emitShiftSimd128(SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16ExtractLaneS):
          case uint32_t(SimdOp::I8x16ExtractLaneU):
            CHECK(emitExtractLaneSimd128(ValType::I32, 16, SimdOp(op.b1)));
          case uint32_t(SimdOp::I16x8ExtractLaneS):
          case uint32_t(SimdOp::I16x8ExtractLaneU):
            CHECK(emitExtractLaneSimd128(ValType::I32, 8, SimdOp(op.b1)));
          case uint32_t(SimdOp::I32x4ExtractLane):
            CHECK(emitExtractLaneSimd128(ValType::I32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::I64x2ExtractLane):
            CHECK(emitExtractLaneSimd128(ValType::I64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::F32x4ExtractLane):
            CHECK(emitExtractLaneSimd128(ValType::F32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::F64x2ExtractLane):
            CHECK(emitExtractLaneSimd128(ValType::F64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16ReplaceLane):
            CHECK(emitReplaceLaneSimd128(ValType::I32, 16, SimdOp(op.b1)));
          case uint32_t(SimdOp::I16x8ReplaceLane):
            CHECK(emitReplaceLaneSimd128(ValType::I32, 8, SimdOp(op.b1)));
          case uint32_t(SimdOp::I32x4ReplaceLane):
            CHECK(emitReplaceLaneSimd128(ValType::I32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::I64x2ReplaceLane):
            CHECK(emitReplaceLaneSimd128(ValType::I64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::F32x4ReplaceLane):
            CHECK(emitReplaceLaneSimd128(ValType::F32, 4, SimdOp(op.b1)));
          case uint32_t(SimdOp::F64x2ReplaceLane):
            CHECK(emitReplaceLaneSimd128(ValType::F64, 2, SimdOp(op.b1)));
          case uint32_t(SimdOp::V128Bitselect):
            CHECK(emitTernarySimd128(SimdOp(op.b1)));
          case uint32_t(SimdOp::I8x16Shuffle):
            CHECK(emitShuffleSimd128());
          case uint32_t(SimdOp::V128Load8Splat):
            CHECK(emitLoadSplatSimd128(Scalar::Uint8, SimdOp::I8x16Splat));
          case uint32_t(SimdOp::V128Load16Splat):
            CHECK(emitLoadSplatSimd128(Scalar::Uint16, SimdOp::I16x8Splat));
          case uint32_t(SimdOp::V128Load32Splat):
            CHECK(emitLoadSplatSimd128(Scalar::Float32, SimdOp::I32x4Splat));
          case uint32_t(SimdOp::V128Load64Splat):
            CHECK(emitLoadSplatSimd128(Scalar::Float64, SimdOp::I64x2Splat));
          case uint32_t(SimdOp::V128Load8x8S):
          case uint32_t(SimdOp::V128Load8x8U):
          case uint32_t(SimdOp::V128Load16x4S):
          case uint32_t(SimdOp::V128Load16x4U):
          case uint32_t(SimdOp::V128Load32x2S):
          case uint32_t(SimdOp::V128Load32x2U):
            CHECK(emitLoadExtendSimd128(SimdOp(op.b1)));
          case uint32_t(SimdOp::V128Load32Zero):
            CHECK(emitLoadZeroSimd128(Scalar::Float32, 4));
          case uint32_t(SimdOp::V128Load64Zero):
            CHECK(emitLoadZeroSimd128(Scalar::Float64, 8));
          case uint32_t(SimdOp::V128Load8Lane):
            CHECK(emitLoadLaneSimd128(1));
          case uint32_t(SimdOp::V128Load16Lane):
            CHECK(emitLoadLaneSimd128(2));
          case uint32_t(SimdOp::V128Load32Lane):
            CHECK(emitLoadLaneSimd128(4));
          case uint32_t(SimdOp::V128Load64Lane):
            CHECK(emitLoadLaneSimd128(8));
          case uint32_t(SimdOp::V128Store8Lane):
            CHECK(emitStoreLaneSimd128(1));
          case uint32_t(SimdOp::V128Store16Lane):
            CHECK(emitStoreLaneSimd128(2));
          case uint32_t(SimdOp::V128Store32Lane):
            CHECK(emitStoreLaneSimd128(4));
          case uint32_t(SimdOp::V128Store64Lane):
            CHECK(emitStoreLaneSimd128(8));
#  ifdef ENABLE_WASM_RELAXED_SIMD
          case uint32_t(SimdOp::F32x4RelaxedMadd):
          case uint32_t(SimdOp::F32x4RelaxedNmadd):
          case uint32_t(SimdOp::F64x2RelaxedMadd):
          case uint32_t(SimdOp::F64x2RelaxedNmadd):
          case uint32_t(SimdOp::I8x16RelaxedLaneSelect):
          case uint32_t(SimdOp::I16x8RelaxedLaneSelect):
          case uint32_t(SimdOp::I32x4RelaxedLaneSelect):
          case uint32_t(SimdOp::I64x2RelaxedLaneSelect):
          case uint32_t(SimdOp::I32x4RelaxedDotI8x16I7x16AddS): {
            if (!codeMeta().v128RelaxedEnabled()) {
              return iter().unrecognizedOpcode(&op);
            }
            CHECK(emitTernarySimd128(SimdOp(op.b1)));
          }
          case uint32_t(SimdOp::F32x4RelaxedMin):
          case uint32_t(SimdOp::F32x4RelaxedMax):
          case uint32_t(SimdOp::F64x2RelaxedMin):
          case uint32_t(SimdOp::F64x2RelaxedMax): {
            if (!codeMeta().v128RelaxedEnabled()) {
              return iter().unrecognizedOpcode(&op);
            }
            // These aren't really commutative, because at least on Intel, the
            // behaviour in the presence of NaNs depends on the order of the
            // operands.  And we need to have that ordering fixed, so that we
            // can produce the same results as baseline.  See bug 1946618.
            CHECK(emitBinarySimd128(/* commutative= */ false, SimdOp(op.b1)));
          }
          case uint32_t(SimdOp::I16x8RelaxedQ15MulrS): {
            if (!codeMeta().v128RelaxedEnabled()) {
              return iter().unrecognizedOpcode(&op);
            }
            CHECK(emitBinarySimd128(/* commutative= */ true, SimdOp(op.b1)));
          }
          case uint32_t(SimdOp::I32x4RelaxedTruncF32x4S):
          case uint32_t(SimdOp::I32x4RelaxedTruncF32x4U):
          case uint32_t(SimdOp::I32x4RelaxedTruncF64x2SZero):
          case uint32_t(SimdOp::I32x4RelaxedTruncF64x2UZero): {
            if (!codeMeta().v128RelaxedEnabled()) {
              return iter().unrecognizedOpcode(&op);
            }
            CHECK(emitUnarySimd128(SimdOp(op.b1)));
          }
          case uint32_t(SimdOp::I8x16RelaxedSwizzle):
          case uint32_t(SimdOp::I16x8RelaxedDotI8x16I7x16S): {
            if (!codeMeta().v128RelaxedEnabled()) {
              return iter().unrecognizedOpcode(&op);
            }
            CHECK(emitBinarySimd128(/* commutative= */ false, SimdOp(op.b1)));
          }
#  endif

          default:
            return iter().unrecognizedOpcode(&op);
        }  // switch (op.b1)
        break;
      }
#endif

      // Miscellaneous operations
      case uint16_t(Op::MiscPrefix): {
        switch (op.b1) {
          case uint32_t(MiscOp::I32TruncSatF32S):
          case uint32_t(MiscOp::I32TruncSatF32U):
            CHECK(emitTruncate(ValType::F32, ValType::I32,
                               MiscOp(op.b1) == MiscOp::I32TruncSatF32U, true));
          case uint32_t(MiscOp::I32TruncSatF64S):
          case uint32_t(MiscOp::I32TruncSatF64U):
            CHECK(emitTruncate(ValType::F64, ValType::I32,
                               MiscOp(op.b1) == MiscOp::I32TruncSatF64U, true));
          case uint32_t(MiscOp::I64TruncSatF32S):
          case uint32_t(MiscOp::I64TruncSatF32U):
            CHECK(emitTruncate(ValType::F32, ValType::I64,
                               MiscOp(op.b1) == MiscOp::I64TruncSatF32U, true));
          case uint32_t(MiscOp::I64TruncSatF64S):
          case uint32_t(MiscOp::I64TruncSatF64U):
            CHECK(emitTruncate(ValType::F64, ValType::I64,
                               MiscOp(op.b1) == MiscOp::I64TruncSatF64U, true));
          case uint32_t(MiscOp::MemoryCopy):
            CHECK(emitMemCopy());
          case uint32_t(MiscOp::DataDrop):
            CHECK(emitDataOrElemDrop(/*isData=*/true));
          case uint32_t(MiscOp::MemoryFill):
            CHECK(emitMemFill());
          case uint32_t(MiscOp::MemoryInit):
            CHECK(emitMemInit());
          case uint32_t(MiscOp::TableCopy):
            CHECK(emitTableCopy());
          case uint32_t(MiscOp::ElemDrop):
            CHECK(emitDataOrElemDrop(/*isData=*/false));
          case uint32_t(MiscOp::TableInit):
            CHECK(emitTableInit());
          case uint32_t(MiscOp::TableFill):
            CHECK(emitTableFill());
#if ENABLE_WASM_MEMORY_CONTROL
          case uint32_t(MiscOp::MemoryDiscard): {
            if (!codeMeta().memoryControlEnabled()) {
              return iter().unrecognizedOpcode(&op);
            }
            CHECK(emitMemDiscard());
          }
#endif
          case uint32_t(MiscOp::TableGrow):
            CHECK(emitTableGrow());
          case uint32_t(MiscOp::TableSize):
            CHECK(emitTableSize());
          default:
            return iter().unrecognizedOpcode(&op);
        }
        break;
      }

      // Thread operations
      case uint16_t(Op::ThreadPrefix): {
        // Though thread ops can be used on nonshared memories, we make them
        // unavailable if shared memory has been disabled in the prefs, for
        // maximum predictability and safety and consistency with JS.
        if (codeMeta().sharedMemoryEnabled() == Shareable::False) {
          return iter().unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(ThreadOp::Notify):
            CHECK(emitNotify());

          case uint32_t(ThreadOp::I32Wait):
            CHECK(emitWait(ValType::I32, 4));
          case uint32_t(ThreadOp::I64Wait):
            CHECK(emitWait(ValType::I64, 8));
          case uint32_t(ThreadOp::Fence):
            CHECK(emitFence());

          case uint32_t(ThreadOp::I32AtomicLoad):
            CHECK(emitAtomicLoad(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicLoad):
            CHECK(emitAtomicLoad(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicLoad8U):
            CHECK(emitAtomicLoad(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicLoad16U):
            CHECK(emitAtomicLoad(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad8U):
            CHECK(emitAtomicLoad(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicLoad16U):
            CHECK(emitAtomicLoad(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicLoad32U):
            CHECK(emitAtomicLoad(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicStore):
            CHECK(emitAtomicStore(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicStore):
            CHECK(emitAtomicStore(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicStore8U):
            CHECK(emitAtomicStore(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicStore16U):
            CHECK(emitAtomicStore(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore8U):
            CHECK(emitAtomicStore(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicStore16U):
            CHECK(emitAtomicStore(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicStore32U):
            CHECK(emitAtomicStore(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicAdd):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Add));
          case uint32_t(ThreadOp::I32AtomicAdd8U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Add));
          case uint32_t(ThreadOp::I32AtomicAdd16U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd8U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd16U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Add));
          case uint32_t(ThreadOp::I64AtomicAdd32U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Add));

          case uint32_t(ThreadOp::I32AtomicSub):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Sub));
          case uint32_t(ThreadOp::I32AtomicSub8U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Sub));
          case uint32_t(ThreadOp::I32AtomicSub16U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub8U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub16U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Sub));
          case uint32_t(ThreadOp::I64AtomicSub32U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Sub));

          case uint32_t(ThreadOp::I32AtomicAnd):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::And));
          case uint32_t(ThreadOp::I32AtomicAnd8U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::And));
          case uint32_t(ThreadOp::I32AtomicAnd16U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd8U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd16U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::And));
          case uint32_t(ThreadOp::I64AtomicAnd32U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::And));

          case uint32_t(ThreadOp::I32AtomicOr):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Or));
          case uint32_t(ThreadOp::I32AtomicOr8U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Or));
          case uint32_t(ThreadOp::I32AtomicOr16U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr8U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr16U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Or));
          case uint32_t(ThreadOp::I64AtomicOr32U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Or));

          case uint32_t(ThreadOp::I32AtomicXor):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Int32, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Int64, AtomicOp::Xor));
          case uint32_t(ThreadOp::I32AtomicXor8U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint8, AtomicOp::Xor));
          case uint32_t(ThreadOp::I32AtomicXor16U):
            CHECK(emitAtomicRMW(ValType::I32, Scalar::Uint16, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor8U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint8, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor16U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint16, AtomicOp::Xor));
          case uint32_t(ThreadOp::I64AtomicXor32U):
            CHECK(emitAtomicRMW(ValType::I64, Scalar::Uint32, AtomicOp::Xor));

          case uint32_t(ThreadOp::I32AtomicXchg):
            CHECK(emitAtomicXchg(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicXchg):
            CHECK(emitAtomicXchg(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicXchg8U):
            CHECK(emitAtomicXchg(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicXchg16U):
            CHECK(emitAtomicXchg(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg8U):
            CHECK(emitAtomicXchg(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicXchg16U):
            CHECK(emitAtomicXchg(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicXchg32U):
            CHECK(emitAtomicXchg(ValType::I64, Scalar::Uint32));

          case uint32_t(ThreadOp::I32AtomicCmpXchg):
            CHECK(emitAtomicCmpXchg(ValType::I32, Scalar::Int32));
          case uint32_t(ThreadOp::I64AtomicCmpXchg):
            CHECK(emitAtomicCmpXchg(ValType::I64, Scalar::Int64));
          case uint32_t(ThreadOp::I32AtomicCmpXchg8U):
            CHECK(emitAtomicCmpXchg(ValType::I32, Scalar::Uint8));
          case uint32_t(ThreadOp::I32AtomicCmpXchg16U):
            CHECK(emitAtomicCmpXchg(ValType::I32, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg8U):
            CHECK(emitAtomicCmpXchg(ValType::I64, Scalar::Uint8));
          case uint32_t(ThreadOp::I64AtomicCmpXchg16U):
            CHECK(emitAtomicCmpXchg(ValType::I64, Scalar::Uint16));
          case uint32_t(ThreadOp::I64AtomicCmpXchg32U):
            CHECK(emitAtomicCmpXchg(ValType::I64, Scalar::Uint32));

          default:
            return iter().unrecognizedOpcode(&op);
        }
        break;
      }

      // asm.js-specific operators
      case uint16_t(Op::MozPrefix): {
        if (op.b1 == uint32_t(MozOp::CallBuiltinModuleFunc)) {
          if (!codeMeta().isBuiltinModule()) {
            return iter().unrecognizedOpcode(&op);
          }
          CHECK(emitCallBuiltinModuleFunc());
        }
#ifdef ENABLE_WASM_JSPI
        if (op.b1 == uint32_t(MozOp::StackSwitch)) {
          if (!codeMeta().isBuiltinModule() ||
              !codeMeta().jsPromiseIntegrationEnabled()) {
            return iter().unrecognizedOpcode(&op);
          }
          CHECK(emitStackSwitch());
        }
#endif

        if (!codeMeta().isAsmJS()) {
          return iter().unrecognizedOpcode(&op);
        }
        switch (op.b1) {
          case uint32_t(MozOp::TeeGlobal):
            CHECK(emitTeeGlobal());
          case uint32_t(MozOp::I32Min):
          case uint32_t(MozOp::I32Max):
            CHECK(emitMinMax(ValType::I32, MIRType::Int32,
                             MozOp(op.b1) == MozOp::I32Max));
          case uint32_t(MozOp::I32Neg):
            CHECK(emitUnaryWithType<MWasmNeg>(ValType::I32, MIRType::Int32));
          case uint32_t(MozOp::I32BitNot):
            CHECK(emitBitNot(ValType::I32, MIRType::Int32));
          case uint32_t(MozOp::I32Abs):
            CHECK(emitUnaryWithType<MAbs>(ValType::I32, MIRType::Int32));
          case uint32_t(MozOp::F32TeeStoreF64):
            CHECK(emitTeeStoreWithCoercion(ValType::F32, Scalar::Float64));
          case uint32_t(MozOp::F64TeeStoreF32):
            CHECK(emitTeeStoreWithCoercion(ValType::F64, Scalar::Float32));
          case uint32_t(MozOp::I32TeeStore8):
            CHECK(emitTeeStore(ValType::I32, Scalar::Int8));
          case uint32_t(MozOp::I32TeeStore16):
            CHECK(emitTeeStore(ValType::I32, Scalar::Int16));
          case uint32_t(MozOp::I64TeeStore8):
            CHECK(emitTeeStore(ValType::I64, Scalar::Int8));
          case uint32_t(MozOp::I64TeeStore16):
            CHECK(emitTeeStore(ValType::I64, Scalar::Int16));
          case uint32_t(MozOp::I64TeeStore32):
            CHECK(emitTeeStore(ValType::I64, Scalar::Int32));
          case uint32_t(MozOp::I32TeeStore):
            CHECK(emitTeeStore(ValType::I32, Scalar::Int32));
          case uint32_t(MozOp::I64TeeStore):
            CHECK(emitTeeStore(ValType::I64, Scalar::Int64));
          case uint32_t(MozOp::F32TeeStore):
            CHECK(emitTeeStore(ValType::F32, Scalar::Float32));
          case uint32_t(MozOp::F64TeeStore):
            CHECK(emitTeeStore(ValType::F64, Scalar::Float64));
          case uint32_t(MozOp::F64Mod):
            CHECK(emitRem(ValType::F64, MIRType::Double,
                          /* isUnsigned = */ false));
          case uint32_t(MozOp::F64SinNative):
            CHECK(emitUnaryMathBuiltinCall(SASigSinNativeD));
          case uint32_t(MozOp::F64SinFdlibm):
            CHECK(emitUnaryMathBuiltinCall(SASigSinFdlibmD));
          case uint32_t(MozOp::F64CosNative):
            CHECK(emitUnaryMathBuiltinCall(SASigCosNativeD));
          case uint32_t(MozOp::F64CosFdlibm):
            CHECK(emitUnaryMathBuiltinCall(SASigCosFdlibmD));
          case uint32_t(MozOp::F64TanNative):
            CHECK(emitUnaryMathBuiltinCall(SASigTanNativeD));
          case uint32_t(MozOp::F64TanFdlibm):
            CHECK(emitUnaryMathBuiltinCall(SASigTanFdlibmD));
          case uint32_t(MozOp::F64Asin):
            CHECK(emitUnaryMathBuiltinCall(SASigASinD));
          case uint32_t(MozOp::F64Acos):
            CHECK(emitUnaryMathBuiltinCall(SASigACosD));
          case uint32_t(MozOp::F64Atan):
            CHECK(emitUnaryMathBuiltinCall(SASigATanD));
          case uint32_t(MozOp::F64Exp):
            CHECK(emitUnaryMathBuiltinCall(SASigExpD));
          case uint32_t(MozOp::F64Log):
            CHECK(emitUnaryMathBuiltinCall(SASigLogD));
          case uint32_t(MozOp::F64Pow):
            CHECK(emitBinaryMathBuiltinCall(SASigPowD));
          case uint32_t(MozOp::F64Atan2):
            CHECK(emitBinaryMathBuiltinCall(SASigATan2D));
          case uint32_t(MozOp::OldCallDirect):
            CHECK(emitCall(/* asmJSFuncDef = */ true));
          case uint32_t(MozOp::OldCallIndirect):
            CHECK(emitCallIndirect(/* oldStyle = */ true));

          default:
            return iter().unrecognizedOpcode(&op);
        }
        break;
      }

      default:
        return iter().unrecognizedOpcode(&op);
    }
  }

  MOZ_CRASH("unreachable");

#undef CHECK
}

}  // end anonymous namespace

bool RootCompiler::generate() {
  // Initialize global information used for optimization
  if (codeMeta_.numMemories() > 0) {
    if (codeMeta_.memories[0].addressType() == AddressType::I32) {
      mirGen_.initMinWasmMemory0Length(codeMeta_.memories[0].initialLength32());
    } else {
      mirGen_.initMinWasmMemory0Length(codeMeta_.memories[0].initialLength64());
    }
  }

  // Only activate branch hinting if the option is enabled and some hints were
  // parsed.
  if (codeMeta_.branchHintingEnabled() && !codeMeta_.branchHints.isEmpty()) {
    compileInfo_.setBranchHinting(true);
  }

  // Figure out what the inlining budget for this function is.  If we've
  // already exceeded the module-level limit, the budget is zero.  See
  // "[SMDOC] Per-function and per-module inlining limits" (WasmHeuristics.h)
  if (codeTailMeta_) {
    auto guard = codeTailMeta_->inliningBudget.lock();

    if (guard.get() > 0) {
      localInliningBudget_ =
          int64_t(codeMeta_.codeSectionSize()) * PerFunctionMaxInliningRatio;
      localInliningBudget_ =
          std::min<int64_t>(localInliningBudget_, guard.get());
    } else {
      localInliningBudget_ = 0;
    }
    MOZ_ASSERT(localInliningBudget_ >= 0);
  } else {
    localInliningBudget_ = 0;
  }

  // Build the MIR graph
  FunctionCompiler funcCompiler(*this, decoder_, func_, locals_, compileInfo_);
  if (!funcCompiler.initRoot() || !funcCompiler.startBlock() ||
      !funcCompiler.emitBodyExprs()) {
    return false;
  }
  funcCompiler.finish();
  observedFeatures_ = funcCompiler.featureUsage();

  MOZ_ASSERT(loopDepth_ == 0);

  funcStats_.numFuncs += 1;
  funcStats_.bytecodeSize += func_.bytecodeSize();
  funcStats_.inlinedDirectCallCount += inliningStats_.inlinedDirectFunctions;
  funcStats_.inlinedCallRefCount += inliningStats_.inlinedCallRefFunctions;
  funcStats_.inlinedDirectCallBytecodeSize +=
      inliningStats_.inlinedDirectBytecodeSize;
  funcStats_.inlinedCallRefBytecodeSize +=
      inliningStats_.inlinedCallRefBytecodeSize;
  funcStats_.numLargeFunctionBackoffs +=
      inliningStats_.largeFunctionBackoff ? 1 : 0;

  if (codeTailMeta_) {
    auto guard = codeTailMeta_->inliningBudget.lock();
    // Update the module's inlining budget accordingly.  If it is already
    // negative, no more inlining for the module can happen, so there's no
    // point in updating it further.
    if (guard.get() >= 0) {
      guard.get() -= int64_t(inliningStats_.inlinedDirectBytecodeSize);
      guard.get() -= int64_t(inliningStats_.inlinedCallRefBytecodeSize);
      if (guard.get() < 0) {
        JS_LOG(wasmPerf, Info,
               "CM=..%06lx  RC::generate            "
               "Inlining budget for entire module exceeded",
               0xFFFFFF & (unsigned long)uintptr_t(&codeMeta_));
      }
    }
    // If this particular root function overran the function-level
    // limit, note that in the module too.
    if (localInliningBudget_ < 0) {
      funcStats_.numInliningBudgetOverruns += 1;
    }
  }

  return true;
}

CompileInfo* RootCompiler::startInlineCall(
    uint32_t callerFuncIndex, BytecodeOffset callerOffset,
    uint32_t calleeFuncIndex, uint32_t numLocals, size_t inlineeBytecodeSize,
    InliningHeuristics::CallKind callKind) {
  if (callKind == InliningHeuristics::CallKind::Direct) {
    inliningStats_.inlinedDirectBytecodeSize += inlineeBytecodeSize;
    inliningStats_.inlinedDirectFunctions += 1;
  } else {
    MOZ_ASSERT(callKind == InliningHeuristics::CallKind::CallRef);
    inliningStats_.inlinedCallRefBytecodeSize += inlineeBytecodeSize;
    inliningStats_.inlinedCallRefFunctions += 1;
  }

  // Update the inlining budget accordingly.  If it is already negative, no
  // more inlining within this root function can happen, so there's no
  // point in updating it further.
  if (localInliningBudget_ >= 0) {
    localInliningBudget_ -= int64_t(inlineeBytecodeSize);
#ifdef JS_JITSPEW
    if (localInliningBudget_ <= 0) {
      JS_LOG(wasmPerf, Info,
             "CM=..%06lx  RC::startInlineCall     "
             "Inlining budget for fI=%u exceeded",
             0xFFFFFF & (unsigned long)uintptr_t(&codeMeta_), callerFuncIndex);
    }
#endif
  }

  // Add the callers offset to the stack of inlined caller offsets
  if (!inlinedCallerOffsets_.append(callerOffset)) {
    return nullptr;
  }

  // Cache a copy of the current stack of inlined caller offsets that can be
  // shared across all call sites
  InlinedCallerOffsets inlinedCallerOffsets;
  if (!inlinedCallerOffsets.appendAll(inlinedCallerOffsets_)) {
    return nullptr;
  }

  if (!inliningContext_.append(std::move(inlinedCallerOffsets),
                               &inlinedCallerOffsetsIndex_)) {
    return nullptr;
  }

  UniqueCompileInfo compileInfo = MakeUnique<CompileInfo>(numLocals);
  if (!compileInfo || !compileInfos_.append(std::move(compileInfo))) {
    return nullptr;
  }
  return compileInfos_[compileInfos_.length() - 1].get();
}

void RootCompiler::finishInlineCall() { inlinedCallerOffsets_.popBack(); }

bool wasm::IonCompileFunctions(const CodeMetadata& codeMeta,
                               const CodeTailMetadata* codeTailMeta,
                               const CompilerEnvironment& compilerEnv,
                               LifoAlloc& lifo,
                               const FuncCompileInputVector& inputs,
                               CompiledCode* code, UniqueChars* error) {
  MOZ_ASSERT(compilerEnv.tier() == Tier::Optimized);
  MOZ_ASSERT(compilerEnv.debug() == DebugEnabled::False);
  MOZ_ASSERT_IF(compilerEnv.mode() == CompileMode::LazyTiering, !!codeTailMeta);

  // We should not interact with the GC heap, nor allocate from it when we are
  // compiling wasm code. Ion data structures have some fields for GC objects
  // that we do not use, yet can confuse the static analysis here. Disable it
  // for this function.
  JS::AutoSuppressGCAnalysis nogc;

  TempAllocator alloc(&lifo);
  JitContext jitContext;
  MOZ_ASSERT(IsCompilingWasm());
  WasmMacroAssembler masm(alloc);
#if defined(JS_CODEGEN_ARM64)
  masm.SetStackPointer64(PseudoStackPointer64);
#endif

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());
  if (!code->swap(masm)) {
    return false;
  }

  // Create a description of the stack layout created by GenerateTrapExit().
  RegisterOffsets trapExitLayout;
  size_t trapExitLayoutNumWords;
  GenerateTrapExitRegisterOffsets(&trapExitLayout, &trapExitLayoutNumWords);

  for (const FuncCompileInput& func : inputs) {
    JitSpewCont(JitSpew_Codegen, "\n");
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "==================================");
    JitSpew(JitSpew_Codegen, "# ==");
    JitSpew(JitSpew_Codegen,
            "# wasm::IonCompileFunctions: starting on function index %d",
            (int)func.index);

    Decoder d(func.begin, func.end, func.lineOrBytecode, error);

    // Build the local types vector.
    ValTypeVector locals;
    if (!DecodeLocalEntriesWithParams(d, codeMeta, func.index, &locals)) {
      return false;
    }

    // Set up for Ion compilation.
    RootCompiler rootCompiler(compilerEnv, codeMeta, codeTailMeta, alloc,
                              locals, func, d, masm.tryNotes(),
                              masm.inliningContext());
    if (!rootCompiler.generate()) {
      return false;
    }

    // Record observed feature usage
    FeatureUsage observedFeatures = rootCompiler.observedFeatures();
    code->featureUsage |= observedFeatures;

    // Compile MIR graph
    {
      jit::SpewBeginWasmFunction(&rootCompiler.mirGen(), func.index);
      jit::AutoSpewEndFunction spewEndFunction(&rootCompiler.mirGen());

      if (!OptimizeMIR(&rootCompiler.mirGen())) {
        return false;
      }

      LIRGraph* lir = GenerateLIR(&rootCompiler.mirGen());
      if (!lir) {
        return false;
      }

      size_t unwindInfoBefore = masm.codeRangeUnwindInfos().length();

      CodeGenerator codegen(&rootCompiler.mirGen(), lir, &masm, &codeMeta);

      TrapSiteDesc prologueTrapSiteDesc(
          wasm::BytecodeOffset(func.lineOrBytecode));
      FuncOffsets offsets;
      ArgTypeVector args(codeMeta.getFuncType(func.index));
      IonPerfSpewer spewer;
      if (!codegen.generateWasm(CallIndirectId::forFunc(codeMeta, func.index),
                                prologueTrapSiteDesc, args, trapExitLayout,
                                trapExitLayoutNumWords, &offsets,
                                &code->stackMaps, &d, &spewer)) {
        return false;
      }

      bool hasUnwindInfo =
          unwindInfoBefore != masm.codeRangeUnwindInfos().length();

      // Record this function's code range
      if (!code->codeRanges.emplaceBack(func.index, offsets, hasUnwindInfo)) {
        return false;
      }

      if (PerfEnabled()) {
        if (!code->funcIonSpewers.emplaceBack(func.index, std::move(spewer))) {
          return false;
        }
      }
    }

    // Record this function's compilation stats
    code->compileStats.merge(rootCompiler.funcStats());

    // Record this function's specific feature usage
    if (!code->funcs.emplaceBack(func.index, observedFeatures)) {
      return false;
    }

    JitSpew(JitSpew_Codegen,
            "# wasm::IonCompileFunctions: completed function index %d",
            (int)func.index);
    JitSpew(JitSpew_Codegen, "# ==");
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "==================================");
    JitSpewCont(JitSpew_Codegen, "\n");
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}

bool wasm::IonDumpFunction(const CompilerEnvironment& compilerEnv,
                           const CodeMetadata& codeMeta,
                           const FuncCompileInput& func,
                           IonDumpContents contents, GenericPrinter& out,
                           UniqueChars* error) {
  LifoAlloc lifo(TempAllocator::PreferredLifoChunkSize,
                 js::BackgroundMallocArena);
  TempAllocator alloc(&lifo);
  JitContext jitContext;
  Decoder d(func.begin, func.end, func.lineOrBytecode, error);

  // Decode the locals.
  ValTypeVector locals;
  if (!DecodeLocalEntriesWithParams(d, codeMeta, func.index, &locals)) {
    return false;
  }

  TryNoteVector tryNotes;
  InliningContext inliningContext;
  RootCompiler rootCompiler(compilerEnv, codeMeta, nullptr, alloc, locals, func,
                            d, tryNotes, inliningContext);
  if (!rootCompiler.generate()) {
    return false;
  }

  if (contents == IonDumpContents::UnoptimizedMIR) {
    rootCompiler.mirGraph().dump(out);
    return true;
  }

  // Optimize the MIR graph
  if (!OptimizeMIR(&rootCompiler.mirGen())) {
    return false;
  }

  if (contents == IonDumpContents::OptimizedMIR) {
    rootCompiler.mirGraph().dump(out);
    return true;
  }

#ifdef JS_JITSPEW
  // Generate the LIR graph
  LIRGraph* lir = GenerateLIR(&rootCompiler.mirGen());
  if (!lir) {
    return false;
  }

  MOZ_ASSERT(contents == IonDumpContents::LIR);
  lir->dump(out);
#else
  out.printf("cannot dump LIR without --enable-jitspew");
#endif
  return true;
}

bool js::wasm::IonPlatformSupport() {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||       \
    defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS64) ||    \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  return true;
#else
  return false;
#endif
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_WarpBuilder_h
#define jit_WarpBuilder_h

#include <initializer_list>

#include "ds/InlineTable.h"
#include "jit/JitContext.h"
#include "jit/MIR.h"
#include "jit/WarpBuilderShared.h"
#include "jit/WarpSnapshot.h"
#include "vm/Opcodes.h"

namespace js {
namespace jit {

// JSOps not yet supported by WarpBuilder. See warning at the end of the list.
#define WARP_UNSUPPORTED_OPCODE_LIST(_)  \
  /* Intentionally not implemented */    \
  _(ForceInterpreter)                    \
  /* With */                             \
  _(EnterWith)                           \
  _(LeaveWith)                           \
  /* Eval */                             \
  _(Eval)                                \
  _(StrictEval)                          \
  _(SpreadEval)                          \
  _(StrictSpreadEval)                    \
  /* Super */                            \
  _(SetPropSuper)                        \
  _(SetElemSuper)                        \
  _(StrictSetPropSuper)                  \
  _(StrictSetElemSuper)                  \
  /* Environments (bug 1366470) */       \
  _(PushVarEnv)                          \
  /* Compound assignment */              \
  _(GetBoundName)                        \
  /* Generators / Async (bug 1317690) */ \
  _(IsGenClosing)                        \
  _(Resume)                              \
  /* try-finally */                      \
  _(Finally)                             \
  _(Gosub)                               \
  _(Retsub)                              \
  /* Misc */                             \
  _(DelName)                             \
  _(SetIntrinsic)                        \
  /* Private Fields */                   \
  _(InitLockedElem)                      \
  _(GetAliasedDebugVar)                  \
  // === !! WARNING WARNING WARNING !! ===
  // Do you really want to sacrifice performance by not implementing this
  // operation in the optimizing compiler?

class MIRGenerator;
class MIRGraph;
class WarpSnapshot;

enum class CacheKind : uint8_t;

// [SMDOC] Control Flow handling in WarpBuilder.
//
// WarpBuilder traverses the script's bytecode and compiles each instruction to
// corresponding MIR instructions. Handling control flow bytecode ops requires
// some special machinery:
//
// Forward branches
// ----------------
// Most branches in the bytecode are forward branches to a JSOp::JumpTarget
// instruction that we have not inspected yet. We compile them in two phases:
//
// 1) When compiling the source instruction: the MBasicBlock is terminated
//    with a control instruction that has a nullptr successor block. We also add
//    a PendingEdge instance to the PendingEdges list for the target bytecode
//    location.
//
// 2) When finally compiling the JSOp::JumpTarget: WarpBuilder::build_JumpTarget
//    creates the target block and uses the list of PendingEdges to 'link' the
//    blocks.
//
// Loops
// -----
// Loops may be nested within other loops, so each WarpBuilder has a LoopState
// stack. This is used to link the backedge to the loop's header block.
//
// Unreachable/dead code
// ---------------------
// Some bytecode instructions never fall through to the next instruction, for
// example JSOp::Return, JSOp::Goto, or JSOp::Throw. Code after such
// instructions is guaranteed to be dead so WarpBuilder skips it until it gets
// to a jump target instruction with pending edges.
//
// Note: The frontend may generate unnecessary JSOp::JumpTarget instructions we
// can ignore when they have no incoming pending edges.
//
// Try-catch
// ---------
// WarpBuilder supports scripts with try-catch by only compiling the try-block
// and bailing out (to the Baseline Interpreter) from the exception handler
// whenever we need to execute the catch-block.
//
// Because we don't compile the catch-block and the code after the try-catch may
// only be reachable via the catch-block, Baseline's BytecodeAnalysis ensures
// Baseline does not attempt OSR into Warp at loops that are only reachable via
// catch/finally blocks.
//
// Finally-blocks are currently not supported by WarpBuilder.

// PendingEdge is used whenever a block is terminated with a forward branch in
// the bytecode. When we reach the jump target we use this information to link
// the block to the jump target's block.
class PendingEdge {
 public:
  enum class Kind : uint8_t {
    // MTest true-successor.
    TestTrue,

    // MTest false-successor.
    TestFalse,

    // MGoto successor.
    Goto,
  };

 private:
  MBasicBlock* block_;
  Kind kind_;
  JSOp testOp_ = JSOp::Undefined;

  PendingEdge(MBasicBlock* block, Kind kind, JSOp testOp = JSOp::Undefined)
      : block_(block), kind_(kind), testOp_(testOp) {}

 public:
  static PendingEdge NewTestTrue(MBasicBlock* block, JSOp op) {
    return PendingEdge(block, Kind::TestTrue, op);
  }
  static PendingEdge NewTestFalse(MBasicBlock* block, JSOp op) {
    return PendingEdge(block, Kind::TestFalse, op);
  }
  static PendingEdge NewGoto(MBasicBlock* block) {
    return PendingEdge(block, Kind::Goto);
  }

  MBasicBlock* block() const { return block_; }
  Kind kind() const { return kind_; }

  JSOp testOp() const {
    MOZ_ASSERT(kind_ == Kind::TestTrue || kind_ == Kind::TestFalse);
    return testOp_;
  }
};

// PendingEdgesMap maps a bytecode instruction to a Vector of PendingEdges
// targeting it. We use InlineMap<> for this because most of the time there are
// only a few pending edges but there can be many when switch-statements are
// involved.
using PendingEdges = Vector<PendingEdge, 2, SystemAllocPolicy>;
using PendingEdgesMap =
    InlineMap<jsbytecode*, PendingEdges, 8, PointerHasher<jsbytecode*>,
              SystemAllocPolicy>;

// LoopState stores information about a loop that's being compiled to MIR.
class LoopState {
  MBasicBlock* header_ = nullptr;

 public:
  explicit LoopState(MBasicBlock* header) : header_(header) {}

  MBasicBlock* header() const { return header_; }
};
using LoopStateStack = Vector<LoopState, 4, JitAllocPolicy>;

// Data that is shared across all WarpBuilders for a given compilation.
class MOZ_STACK_CLASS WarpCompilation {
  // The total loop depth, including loops in the caller while
  // compiling inlined functions.
  uint32_t loopDepth_ = 0;

  // Loop phis for iterators that need to be kept alive.
  PhiVector iterators_;

 public:
  explicit WarpCompilation(TempAllocator& alloc) : iterators_(alloc) {}

  uint32_t loopDepth() const { return loopDepth_; }
  void incLoopDepth() { loopDepth_++; }
  void decLoopDepth() {
    MOZ_ASSERT(loopDepth() > 0);
    loopDepth_--;
  }

  PhiVector* iterators() { return &iterators_; }
};

// WarpBuilder builds a MIR graph from WarpSnapshot. Unlike WarpOracle,
// WarpBuilder can run off-thread.
class MOZ_STACK_CLASS WarpBuilder : public WarpBuilderShared {
  WarpCompilation* warpCompilation_;
  MIRGraph& graph_;
  const CompileInfo& info_;
  const WarpScriptSnapshot* scriptSnapshot_;
  JSScript* script_;

  // Pointer to a WarpOpSnapshot or nullptr if we reached the end of the list.
  // Because bytecode is compiled from first to last instruction (and
  // WarpOpSnapshot is sorted the same way), the iterator always moves forward.
  const WarpOpSnapshot* opSnapshotIter_ = nullptr;

  // Note: loopStack_ is builder-specific. loopStack_.length is the
  // depth relative to the current script.  The overall loop depth is
  // stored in the WarpCompilation.
  LoopStateStack loopStack_;
  PendingEdgesMap pendingEdges_;

  // These are only initialized when building an inlined script.
  WarpBuilder* callerBuilder_ = nullptr;
  MResumePoint* callerResumePoint_ = nullptr;
  CallInfo* inlineCallInfo_ = nullptr;

  WarpCompilation* warpCompilation() const { return warpCompilation_; }
  MIRGraph& graph() { return graph_; }
  const CompileInfo& info() const { return info_; }
  const WarpScriptSnapshot* scriptSnapshot() const { return scriptSnapshot_; }

  uint32_t loopDepth() const { return warpCompilation_->loopDepth(); }
  void incLoopDepth() { warpCompilation_->incLoopDepth(); }
  void decLoopDepth() { warpCompilation_->decLoopDepth(); }
  PhiVector* iterators() { return warpCompilation_->iterators(); }

  WarpBuilder* callerBuilder() const { return callerBuilder_; }
  MResumePoint* callerResumePoint() const { return callerResumePoint_; }

  BytecodeSite* newBytecodeSite(BytecodeLocation loc);

  const WarpOpSnapshot* getOpSnapshotImpl(BytecodeLocation loc,
                                          WarpOpSnapshot::Kind kind);

  template <typename T>
  const T* getOpSnapshot(BytecodeLocation loc) {
    const WarpOpSnapshot* snapshot = getOpSnapshotImpl(loc, T::ThisKind);
    return snapshot ? snapshot->as<T>() : nullptr;
  }

  void initBlock(MBasicBlock* block);
  [[nodiscard]] bool startNewEntryBlock(size_t stackDepth,
                                        BytecodeLocation loc);
  [[nodiscard]] bool startNewBlock(MBasicBlock* predecessor,
                                   BytecodeLocation loc, size_t numToPop = 0);
  [[nodiscard]] bool startNewLoopHeaderBlock(BytecodeLocation loopHead);
  [[nodiscard]] bool startNewOsrPreHeaderBlock(BytecodeLocation loopHead);

  bool hasTerminatedBlock() const { return current == nullptr; }
  void setTerminatedBlock() { current = nullptr; }

  [[nodiscard]] bool addPendingEdge(const PendingEdge& edge,
                                    BytecodeLocation target);
  [[nodiscard]] bool buildForwardGoto(BytecodeLocation target);
  [[nodiscard]] bool buildBackedge();
  [[nodiscard]] bool buildTestBackedge(BytecodeLocation loc);

  [[nodiscard]] bool addIteratorLoopPhis(BytecodeLocation loopHead);

  [[nodiscard]] bool buildPrologue();
  [[nodiscard]] bool buildBody();

  [[nodiscard]] bool buildInlinePrologue();

  [[nodiscard]] bool buildIC(BytecodeLocation loc, CacheKind kind,
                             std::initializer_list<MDefinition*> inputs);
  [[nodiscard]] bool buildBailoutForColdIC(BytecodeLocation loc,
                                           CacheKind kind);

  [[nodiscard]] bool buildEnvironmentChain();
  MInstruction* buildNamedLambdaEnv(MDefinition* callee, MDefinition* env,
                                    NamedLambdaObject* templateObj);
  MInstruction* buildCallObject(MDefinition* callee, MDefinition* env,
                                CallObject* templateObj);
  MInstruction* buildLoadSlot(MDefinition* obj, uint32_t numFixedSlots,
                              uint32_t slot);

  MConstant* globalLexicalEnvConstant();
  MDefinition* getCallee();

  [[nodiscard]] bool buildUnaryOp(BytecodeLocation loc);
  [[nodiscard]] bool buildBinaryOp(BytecodeLocation loc);
  [[nodiscard]] bool buildCompareOp(BytecodeLocation loc);
  [[nodiscard]] bool buildTestOp(BytecodeLocation loc);
  [[nodiscard]] bool buildCallOp(BytecodeLocation loc);

  [[nodiscard]] bool buildInitPropGetterSetterOp(BytecodeLocation loc);
  [[nodiscard]] bool buildInitElemGetterSetterOp(BytecodeLocation loc);

  [[nodiscard]] bool buildSuspend(BytecodeLocation loc, MDefinition* gen,
                                  MDefinition* retVal);

  void buildCopyLexicalEnvOp(bool copySlots);
  void buildCheckLexicalOp(BytecodeLocation loc);

  bool usesEnvironmentChain() const;
  MDefinition* walkEnvironmentChain(uint32_t numHops);

  [[nodiscard]] bool transpileCall(BytecodeLocation loc,
                                   const WarpCacheIR* cacheIRSnapshot,
                                   CallInfo* callInfo);

  [[nodiscard]] bool buildInlinedCall(BytecodeLocation loc,
                                      const WarpInlinedCall* snapshot,
                                      CallInfo& callInfo);

  MDefinition* patchInlinedReturns(CompileInfo* calleeCompileInfo,
                                   CallInfo& callInfo, MIRGraphReturns& exits,
                                   MBasicBlock* returnBlock);
  MDefinition* patchInlinedReturn(CompileInfo* calleeCompileInfo,
                                  CallInfo& callInfo, MBasicBlock* exit,
                                  MBasicBlock* returnBlock);

#define BUILD_OP(OP, ...) [[nodiscard]] bool build_##OP(BytecodeLocation loc);
  FOR_EACH_OPCODE(BUILD_OP)
#undef BUILD_OP

 public:
  WarpBuilder(WarpSnapshot& snapshot, MIRGenerator& mirGen,
              WarpCompilation* warpCompilation);
  WarpBuilder(WarpBuilder* caller, WarpScriptSnapshot* snapshot,
              CompileInfo& compileInfo, CallInfo* inlineCallInfo,
              MResumePoint* callerResumePoint);

  [[nodiscard]] bool build();
  [[nodiscard]] bool buildInline();

  CallInfo* inlineCallInfo() const { return inlineCallInfo_; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_WarpBuilder_h */

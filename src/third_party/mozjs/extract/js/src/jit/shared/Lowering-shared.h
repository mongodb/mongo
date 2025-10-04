/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_Lowering_shared_h
#define jit_shared_Lowering_shared_h

// This file declares the structures that are used for attaching LIR to a
// MIRGraph.

#include "jit/LIR.h"
#include "jit/MIRGenerator.h"

namespace js {
namespace jit {

class MIRGenerator;
class MIRGraph;
class MDefinition;
class MInstruction;
class LOsiPoint;

class LIRGeneratorShared {
 protected:
  MIRGenerator* gen;
  MIRGraph& graph;
  LIRGraph& lirGraph_;
  LBlock* current;
  MResumePoint* lastResumePoint_;
  LRecoverInfo* cachedRecoverInfo_;
  LOsiPoint* osiPoint_;

  LIRGeneratorShared(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : gen(gen),
        graph(graph),
        lirGraph_(lirGraph),
        current(nullptr),
        lastResumePoint_(nullptr),
        cachedRecoverInfo_(nullptr),
        osiPoint_(nullptr) {}

  MIRGenerator* mir() { return gen; }

  // Abort errors are caught at end of visitInstruction. It is possible for
  // multiple errors to be detected before the end of visitInstruction. In
  // this case, we only report the first back to the MIRGenerator.
  bool errored() { return gen->getOffThreadStatus().isErr(); }
  void abort(AbortReason r, const char* message, ...) MOZ_FORMAT_PRINTF(3, 4) {
    if (errored()) {
      return;
    }

    va_list ap;
    va_start(ap, message);
    auto reason_ = gen->abortFmt(r, message, ap);
    va_end(ap);
    gen->setOffThreadStatus(reason_);
  }
  void abort(AbortReason r) {
    if (errored()) {
      return;
    }

    auto reason_ = gen->abort(r);
    gen->setOffThreadStatus(reason_);
  }

  static void ReorderCommutative(MDefinition** lhsp, MDefinition** rhsp,
                                 MInstruction* ins);
  static bool ShouldReorderCommutative(MDefinition* lhs, MDefinition* rhs,
                                       MInstruction* ins);

  // A backend can decide that an instruction should be emitted at its uses,
  // rather than at its definition. To communicate this, set the
  // instruction's virtual register set to 0. When using the instruction,
  // its virtual register is temporarily reassigned. To know to clear it
  // after constructing the use information, the worklist bit is temporarily
  // unset.
  //
  // The backend can use the worklist bit to determine whether or not a
  // definition should be created.
  inline void emitAtUses(MInstruction* mir);

  // The lowest-level calls to use, those that do not wrap another call to
  // use(), must prefix grabbing virtual register IDs by these calls.
  inline void ensureDefined(MDefinition* mir);

  void visitEmittedAtUses(MInstruction* ins);

  // These all create a use of a virtual register, with an optional
  // allocation policy.
  //
  // Some of these use functions have atStart variants.
  // - non-atStart variants will tell the register allocator that the input
  // allocation must be different from any Temp or Definition also needed for
  // this LInstruction.
  // - atStart variants relax that restriction and allow the input to be in
  // the same register as any output Definition (but not Temps) used by the
  // LInstruction. Note that it doesn't *imply* this will actually happen,
  // but gives a hint to the register allocator that it can do it.
  //
  // TL;DR: Use non-atStart variants only if you need the input value after
  // writing to any definitions (excluding temps), during code generation of
  // this LInstruction. Otherwise, use atStart variants, which will lower
  // register pressure.
  //
  // There is an additional constraint.  Consider a MIR node with two
  // MDefinition* operands, op1 and op2.  If the node reuses the register of op1
  // for its output then op1 must be used as atStart.  Then, if op1 and op2
  // represent the same LIR node then op2 must be an atStart use too; otherwise
  // op2 must be a non-atStart use.  There is however not always a 1-1 mapping
  // from MDefinition* to LNode*, so to determine whether two MDefinition* map
  // to the same LNode*, ALWAYS go via the willHaveDifferentLIRNodes()
  // predicate.  Do not use pointer equality on the MIR nodes.
  //
  // Do not add other conditions when using willHaveDifferentLIRNodes().  The
  // predicate is the source of truth about whether to use atStart or not, no
  // other conditions may apply in contexts when it is appropriate to use it.
  inline LUse use(MDefinition* mir, LUse policy);
  inline LUse use(MDefinition* mir);
  inline LUse useAtStart(MDefinition* mir);
  inline LUse useRegister(MDefinition* mir);
  inline LUse useRegisterAtStart(MDefinition* mir);
  inline LUse useFixed(MDefinition* mir, Register reg);
  inline LUse useFixed(MDefinition* mir, FloatRegister reg);
  inline LUse useFixed(MDefinition* mir, AnyRegister reg);
  inline LUse useFixedAtStart(MDefinition* mir, Register reg);
  inline LUse useFixedAtStart(MDefinition* mir, AnyRegister reg);
  inline LAllocation useOrConstant(MDefinition* mir);
  inline LAllocation useOrConstantAtStart(MDefinition* mir);
  // "Any" is architecture dependent, and will include registers and stack
  // slots on X86, and only registers on ARM.
  inline LAllocation useAny(MDefinition* mir);
  inline LAllocation useAnyAtStart(MDefinition* mir);
  inline LAllocation useAnyOrConstant(MDefinition* mir);
  // "Storable" is architecture dependend, and will include registers and
  // constants on X86 and only registers on ARM.  This is a generic "things
  // we can expect to write into memory in 1 instruction".
  inline LAllocation useStorable(MDefinition* mir);
  inline LAllocation useStorableAtStart(MDefinition* mir);
  inline LAllocation useKeepalive(MDefinition* mir);
  inline LAllocation useKeepaliveOrConstant(MDefinition* mir);
  inline LAllocation useRegisterOrConstant(MDefinition* mir);
  inline LAllocation useRegisterOrConstantAtStart(MDefinition* mir);
  inline LAllocation useRegisterOrZeroAtStart(MDefinition* mir);
  inline LAllocation useRegisterOrZero(MDefinition* mir);
  inline LAllocation useRegisterOrNonDoubleConstant(MDefinition* mir);

  // These methods accept either an Int32 or IntPtr value. A constant is used if
  // the value fits in an int32.
  inline LAllocation useRegisterOrInt32Constant(MDefinition* mir);
  inline LAllocation useAnyOrInt32Constant(MDefinition* mir);

  // Like useRegisterOrInt32Constant, but uses a constant only if
  // |int32val * Scalar::byteSize(type) + offsetAdjustment| doesn't overflow
  // int32.
  LAllocation useRegisterOrIndexConstant(MDefinition* mir, Scalar::Type type,
                                         int32_t offsetAdjustment = 0);

  inline LUse useRegisterForTypedLoad(MDefinition* mir, MIRType type);

#ifdef JS_NUNBOX32
  inline LUse useType(MDefinition* mir, LUse::Policy policy);
  inline LUse usePayload(MDefinition* mir, LUse::Policy policy);
  inline LUse usePayloadAtStart(MDefinition* mir, LUse::Policy policy);
  inline LUse usePayloadInRegisterAtStart(MDefinition* mir);

  // Adds a box input to an instruction, setting operand |n| to the type and
  // |n+1| to the payload. Does not modify the operands, instead expecting a
  // policy to already be set.
  inline void fillBoxUses(LInstruction* lir, size_t n, MDefinition* mir);
#endif

  // Test whether mir1 and mir2 may give rise to different LIR nodes even if
  // mir1 == mir2; use it to guide the selection of the use directive for one of
  // the nodes in the context of a reused input.  See comments above about why
  // it's important to use this predicate and not pointer equality.
  //
  // This predicate may be called before or after the application of a use
  // directive to the first of the nodes, but it is meaningless to call it after
  // the application of a directive to the second node.
  inline bool willHaveDifferentLIRNodes(MDefinition* mir1, MDefinition* mir2);

  // These create temporary register requests.
  inline LDefinition temp(LDefinition::Type type = LDefinition::GENERAL,
                          LDefinition::Policy policy = LDefinition::REGISTER);
  inline LInt64Definition tempInt64(
      LDefinition::Policy policy = LDefinition::REGISTER);
  inline LDefinition tempFloat32();
  inline LDefinition tempDouble();
#ifdef ENABLE_WASM_SIMD
  inline LDefinition tempSimd128();
#endif
  inline LDefinition tempCopy(MDefinition* input, uint32_t reusedInput);

  // Note that the fixed register has a GENERAL type,
  // unless the arg is of FloatRegister type
  inline LDefinition tempFixed(Register reg);
  inline LDefinition tempFixed(FloatRegister reg);
  inline LInt64Definition tempInt64Fixed(Register64 reg);

  template <size_t Ops, size_t Temps>
  inline void defineFixed(LInstructionHelper<1, Ops, Temps>* lir,
                          MDefinition* mir, const LAllocation& output);

  template <size_t Temps>
  inline void defineBox(
      details::LInstructionFixedDefsTempsHelper<BOX_PIECES, Temps>* lir,
      MDefinition* mir, LDefinition::Policy policy = LDefinition::REGISTER);

  template <size_t Ops, size_t Temps>
  inline void defineInt64(LInstructionHelper<INT64_PIECES, Ops, Temps>* lir,
                          MDefinition* mir,
                          LDefinition::Policy policy = LDefinition::REGISTER);

  template <size_t Ops, size_t Temps>
  inline void defineInt64Fixed(
      LInstructionHelper<INT64_PIECES, Ops, Temps>* lir, MDefinition* mir,
      const LInt64Allocation& output);

  inline void defineReturn(LInstruction* lir, MDefinition* mir);

  template <size_t X>
  inline void define(details::LInstructionFixedDefsTempsHelper<1, X>* lir,
                     MDefinition* mir,
                     LDefinition::Policy policy = LDefinition::REGISTER);
  template <size_t X>
  inline void define(details::LInstructionFixedDefsTempsHelper<1, X>* lir,
                     MDefinition* mir, const LDefinition& def);

  template <size_t Ops, size_t Temps>
  inline void defineReuseInput(LInstructionHelper<1, Ops, Temps>* lir,
                               MDefinition* mir, uint32_t operand);

  template <size_t Ops, size_t Temps>
  inline void defineBoxReuseInput(
      LInstructionHelper<BOX_PIECES, Ops, Temps>* lir, MDefinition* mir,
      uint32_t operand);

  template <size_t Ops, size_t Temps>
  inline void defineInt64ReuseInput(
      LInstructionHelper<INT64_PIECES, Ops, Temps>* lir, MDefinition* mir,
      uint32_t operand);

  // Returns a box allocation for a Value-typed instruction.
  inline LBoxAllocation useBox(MDefinition* mir,
                               LUse::Policy policy = LUse::REGISTER,
                               bool useAtStart = false);

  // Returns a box allocation. The use is either typed, a Value, or
  // a constant (if useConstant is true).
  inline LBoxAllocation useBoxOrTypedOrConstant(MDefinition* mir,
                                                bool useConstant,
                                                bool useAtStart = false);
  inline LBoxAllocation useBoxOrTyped(MDefinition* mir,
                                      bool useAtStart = false);

  // Returns an int64 allocation for an Int64-typed instruction.
  inline LInt64Allocation useInt64(MDefinition* mir, LUse::Policy policy,
                                   bool useAtStart);
  inline LInt64Allocation useInt64(MDefinition* mir, bool useAtStart = false);
  inline LInt64Allocation useInt64AtStart(MDefinition* mir);
  inline LInt64Allocation useInt64OrConstant(MDefinition* mir,
                                             bool useAtStart = false);
  inline LInt64Allocation useInt64Register(MDefinition* mir,
                                           bool useAtStart = false);
  inline LInt64Allocation useInt64RegisterOrConstant(MDefinition* mir,
                                                     bool useAtStart = false);
  inline LInt64Allocation useInt64Fixed(MDefinition* mir, Register64 regs,
                                        bool useAtStart = false);
  inline LInt64Allocation useInt64FixedAtStart(MDefinition* mir,
                                               Register64 regs);

  inline LInt64Allocation useInt64RegisterAtStart(MDefinition* mir);
  inline LInt64Allocation useInt64RegisterOrConstantAtStart(MDefinition* mir);
  inline LInt64Allocation useInt64OrConstantAtStart(MDefinition* mir);

  // Rather than defining a new virtual register, sets |ins| to have the same
  // virtual register as |as|.
  inline void redefine(MDefinition* ins, MDefinition* as);

  template <typename LClass, typename... Args>
  inline LClass* allocateVariadic(uint32_t numOperands, Args&&... args);

  TempAllocator& alloc() const { return graph.alloc(); }

  uint32_t getVirtualRegister() {
    uint32_t vreg = lirGraph_.getVirtualRegister();

    // If we run out of virtual registers, mark code generation as having
    // failed and return a dummy vreg. Include a + 1 here for NUNBOX32
    // platforms that expect Value vregs to be adjacent.
    if (vreg + 1 >= MAX_VIRTUAL_REGISTERS) {
      abort(AbortReason::Alloc, "max virtual registers");
      return 1;
    }
    return vreg;
  }

  template <typename T>
  void annotate(T* ins);
  template <typename T>
  void add(T* ins, MInstruction* mir = nullptr);

  void lowerTypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block,
                          size_t lirIndex);

  void definePhiOneRegister(MPhi* phi, size_t lirIndex);
#ifdef JS_NUNBOX32
  void definePhiTwoRegisters(MPhi* phi, size_t lirIndex);
#endif

  void defineTypedPhi(MPhi* phi, size_t lirIndex) {
    // One register containing the payload.
    definePhiOneRegister(phi, lirIndex);
  }
  void defineUntypedPhi(MPhi* phi, size_t lirIndex) {
#ifdef JS_NUNBOX32
    // Two registers: one for the type, one for the payload.
    definePhiTwoRegisters(phi, lirIndex);
#else
    // One register containing the full Value.
    definePhiOneRegister(phi, lirIndex);
#endif
  }

  LOsiPoint* popOsiPoint() {
    LOsiPoint* tmp = osiPoint_;
    osiPoint_ = nullptr;
    return tmp;
  }

  LRecoverInfo* getRecoverInfo(MResumePoint* rp);
  LSnapshot* buildSnapshot(MResumePoint* rp, BailoutKind kind);
  bool assignPostSnapshot(MInstruction* mir, LInstruction* ins);

  // Marks this instruction as fallible, meaning that before it performs
  // effects (if any), it may check pre-conditions and bailout if they do not
  // hold. This function informs the register allocator that it will need to
  // capture appropriate state.
  void assignSnapshot(LInstruction* ins, BailoutKind kind);

  // Marks this instruction as needing to call into either the VM or GC. This
  // function may build a snapshot that captures the result of its own
  // instruction, and as such, should generally be called after define*().
  void assignSafepoint(LInstruction* ins, MInstruction* mir,
                       BailoutKind kind = BailoutKind::DuringVMCall);

  // Marks this instruction as needing a wasm safepoint.
  void assignWasmSafepoint(LInstruction* ins);

  inline void lowerConstantDouble(double d, MInstruction* mir);
  inline void lowerConstantFloat32(float f, MInstruction* mir);

  bool canSpecializeWasmCompareAndSelect(MCompare::CompareType compTy,
                                         MIRType insTy);
  void lowerWasmCompareAndSelect(MWasmSelect* ins, MDefinition* lhs,
                                 MDefinition* rhs, MCompare::CompareType compTy,
                                 JSOp jsop);

 public:
  // Whether to generate typed reads for element accesses with hole checks.
  static bool allowTypedElementHoleCheck() { return false; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_shared_Lowering_shared_h */

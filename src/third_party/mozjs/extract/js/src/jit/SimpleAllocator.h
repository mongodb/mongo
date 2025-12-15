/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SimpleAllocator_h
#define jit_SimpleAllocator_h

#include "mozilla/Array.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include "jit/RegisterAllocator.h"
#include "jit/SparseBitSet.h"
#include "jit/StackSlotAllocator.h"

namespace js {
namespace jit {

class SimpleAllocator : protected RegisterAllocator {
  // Information about a virtual register.
  class VirtualRegister {
    // The definition and the id of the LIR instruction that contains it.
    LDefinition* def_;
    uint32_t insId_;

    // The id of the LIR instruction that's considered the 'last use' of this
    // virtual register. After this instruction we free any register or stack
    // slot allocated for this virtual register and we mark this virtual
    // register as dead.
    uint32_t lastUseInsId_;

    // Either UINT32_MAX (meaning no stack slot has been allocated) or the stack
    // slot offset and width of a slot we allocated for this virtual register.
    static constexpr uint32_t NoStackSlotAndWidth = UINT32_MAX;
    uint32_t stackSlotAndWidth_ = NoStackSlotAndWidth;

    // If this virtual register has fixed register uses, we use this register
    // hint when allocating registers to help avoid moves.
    // This has three states:
    // - Nothing: we didn't see any fixed register uses.
    // - Some(valid AnyRegister): there are fixed register uses and they all use
    //   this register.
    // - Some(Invalid AnyRegister): there are multiple fixed register uses for
    //   different registers and we gave up on assigning a register hint.
    mozilla::Maybe<AnyRegister> fixedUseHint_;

    // The index in the allocator's allocatedRegs_ vector, or NoRegisterIndex if
    // this vreg has no allocated register.
    //
    // In some uncommon cases a vreg can have multiple registers assigned to it
    // and in this case the index is for one of these registers.
    // See hasMultipleRegsForVreg_.
    static constexpr size_t NoRegisterIndex = UINT8_MAX;
    uint8_t registerIndex_ = NoRegisterIndex;

    // Assert registerIndex_ fits in a byte. There must be at most one
    // AllocatedRegister entry for each AnyRegister value.
    static_assert(sizeof(AnyRegister) == sizeof(registerIndex_));
    static_assert(AnyRegister::Invalid == NoRegisterIndex);

    // Whether this virtual register is for a Temp.
    bool isTemp_ : 1;

    // Whether this virtual register has a stack location. This location is
    // either a stack slot we allocated (see stackSlotAndWidth_) or this virtual
    // register came with an assigned stack location (for example for stack
    // arguments).
    bool hasStackLocation_ : 1;

    // Whether this virtual register is for a GC type that needs to be added to
    // safepoints.
    bool isGCType_ : 1;

    // Whether this virtual register is dead. This is set after allocating
    // registers for the lastUseInsId_ instruction.
    bool isDead_ : 1;

   public:
    VirtualRegister() = default;

    VirtualRegister(VirtualRegister&) = delete;
    void operator=(VirtualRegister&) = delete;

    void init(LNode* ins, LDefinition* def, bool isTemp) {
      def_ = def;
      insId_ = ins->id();
      lastUseInsId_ = ins->id();
      isTemp_ = isTemp;
      hasStackLocation_ = false;
      isGCType_ = def->isSafepointGCType(ins);
      isDead_ = false;
    }

    LDefinition* def() const { return def_; }
    uint32_t insId() const { return insId_; }
    uint32_t lastUseInsId() const { return lastUseInsId_; }

    uint8_t registerIndex() const {
      MOZ_ASSERT(hasRegister());
      return registerIndex_;
    }
    bool hasRegister() const {
      MOZ_ASSERT(!isDead());
      return registerIndex_ != NoRegisterIndex;
    }
    void setRegisterIndex(uint8_t registerIndex) {
      MOZ_ASSERT(!isDead());
      MOZ_ASSERT(registerIndex < NoRegisterIndex);
      registerIndex_ = registerIndex;
    }
    void clearRegisterIndex() {
      MOZ_ASSERT(!isDead());
      registerIndex_ = NoRegisterIndex;
    }

    LAllocation stackLocation() const {
      MOZ_ASSERT(hasStackLocation());
      if (hasAllocatedStackSlot()) {
        return LStackSlot(stackSlot());
      }
      MOZ_ASSERT(def()->policy() == LDefinition::FIXED);
      MOZ_ASSERT(!def()->output()->isAnyRegister());
      return *def()->output();
    }
    void setHasStackLocation() {
      MOZ_ASSERT(!isDead());
      MOZ_ASSERT(!isTemp());
      MOZ_ASSERT(!hasStackLocation_);
      MOZ_ASSERT(stackSlotAndWidth_ == NoStackSlotAndWidth);
      hasStackLocation_ = true;
    }

    bool hasAllocatedStackSlot() const {
      MOZ_ASSERT(!isDead());
      MOZ_ASSERT_IF(stackSlotAndWidth_ != NoStackSlotAndWidth,
                    hasStackLocation_);
      return stackSlotAndWidth_ != NoStackSlotAndWidth;
    }
    LStackSlot::SlotAndWidth stackSlot() const {
      MOZ_ASSERT(hasAllocatedStackSlot());
      return LStackSlot::SlotAndWidth::fromData(stackSlotAndWidth_);
    }
    void setAllocatedStackSlot(LStackSlot::SlotAndWidth slot) {
      setHasStackLocation();
      MOZ_ASSERT(stackSlotAndWidth_ == NoStackSlotAndWidth);
      MOZ_ASSERT(slot.data() != NoStackSlotAndWidth);
      stackSlotAndWidth_ = slot.data();
    }

    void markDead() {
      MOZ_ASSERT(!isDead());
      isDead_ = true;
    }

    mozilla::Maybe<AnyRegister> fixedUseHint() const { return fixedUseHint_; }
    void setFixedUseHint(AnyRegister reg) {
      fixedUseHint_ = mozilla::Some(reg);
    }

    bool hasStackLocation() const { return hasStackLocation_; }
    bool isGCType() const { return isGCType_; }
    bool isTemp() const { return isTemp_; }
    bool isDead() const { return isDead_; }

    void updateLastUseId(uint32_t useInsId) {
      MOZ_ASSERT(!isTemp());
      MOZ_ASSERT(useInsId > insId_);
      if (useInsId > lastUseInsId_) {
        lastUseInsId_ = useInsId;
      }
    }
  };

  // Information about virtual registers.
  Vector<VirtualRegister, 0, JitAllocPolicy> vregs_;

  // Information about an allocated register.
  class AllocatedRegister {
    static constexpr size_t VregBits = 24;
    static constexpr size_t RegisterBits = 8;

    // The virtual register.
    uint32_t vregId_ : VregBits;

    // The AnyRegister code. We don't use AnyRegister directly because the
    // compiler adds additional padding bytes on Windows if we do that.
    uint32_t reg_ : RegisterBits;

    // Id of the last LIR instruction that used this allocated register. Used
    // for eviction heuristics.
    uint32_t lastUsedAtInsId_;

    static_assert(MAX_VIRTUAL_REGISTERS <= (1 << VregBits) - 1);
    static_assert(sizeof(AnyRegister::Code) * 8 == RegisterBits);

   public:
    AllocatedRegister(uint32_t vreg, AnyRegister reg, uint32_t lastUsedAtInsId)
        : vregId_(vreg), reg_(reg.code()), lastUsedAtInsId_(lastUsedAtInsId) {}

    uint32_t vregId() const { return vregId_; }
    AnyRegister reg() const { return AnyRegister::FromCode(reg_); }
    uint32_t lastUsedAtInsId() const { return lastUsedAtInsId_; }
    void setLastUsedAtInsId(uint32_t insId) {
      MOZ_ASSERT(insId >= lastUsedAtInsId_);
      lastUsedAtInsId_ = insId;
    }
  };
  static_assert(sizeof(AllocatedRegister) == 2 * sizeof(uint32_t),
                "AllocatedRegister should not have unnecessary padding");

  // Information about allocated physical registers.
  using AllocatedRegisterVector =
      Vector<AllocatedRegister, 16, BackgroundSystemAllocPolicy>;
  AllocatedRegisterVector allocatedRegs_;
  AllocatableRegisterSet availableRegs_;

  // Whether there might be multiple entries in allocatedRegs_ for the same
  // vreg. This can happen for instance if an instruction has multiple fixed
  // register uses for the same vreg.
  //
  // This is uncommon and we try to avoid it, but if it does happen we fix
  // it up at the end of allocateForInstruction.
  bool hasMultipleRegsForVreg_ = false;

  // All registers allocated for the current LIR instruction.
  AllocatableRegisterSet currentInsRegs_;

  // Like currentInsRegs_, but does not include registers that are only used
  // for at-start uses.
  AllocatableRegisterSet currentInsRegsNotAtStart_;

  // Registers required for fixed outputs/temps of the current LIR instruction.
  AllocatableRegisterSet fixedTempRegs_;
  AllocatableRegisterSet fixedOutputAndTempRegs_;

  // The set of live GC things at the start of each basic block.
  using VirtualRegBitSet = SparseBitSet<BackgroundSystemAllocPolicy>;
  Vector<VirtualRegBitSet, 0, JitAllocPolicy> liveGCIn_;

  // Vector sorted by instructionId in descending order. This is used by
  // freeDeadVregsAfterInstruction to mark virtual registers as dead after their
  // last use.
  struct VregLastUse {
    uint32_t instructionId;
    uint32_t vregId;
    VregLastUse(uint32_t instructionId, uint32_t vregId)
        : instructionId(instructionId), vregId(vregId) {}
  };
  Vector<VregLastUse, 0, JitAllocPolicy> vregLastUses_;

  // Vector used for spilling instruction outputs that are used across blocks.
  Vector<LDefinition*, 4, BackgroundSystemAllocPolicy> eagerSpillOutputs_;

  // Allocator used to allocate and free stack slots.
  StackSlotAllocator stackSlotAllocator_;

  // Register state for a small number of previous blocks. Blocks with a single
  // predecessor block can optionally reuse this state.
  //
  // BlockStateLength should be small enough so that iterating over the array is
  // very cheap. It's currently 4 and this was sufficient for a hit rate of
  // 88.6% on a very large Wasm module.
  static constexpr size_t BlockStateLength = 4;
  struct BlockState {
    uint32_t blockIndex = UINT32_MAX;
    AllocatedRegisterVector allocatedRegs;
    AllocatableRegisterSet availableRegs;
  };
  mozilla::Array<BlockState, BlockStateLength> blockStates_;
  size_t nextBlockStateIndex_ = 0;

  // Data used to insert a move for the input of a MUST_REUSE_INPUT definition.
  struct ReusedInputReg {
    LAllocation* source;
    AnyRegister dest;
    LDefinition::Type type;
    ReusedInputReg(LAllocation* source, AnyRegister dest,
                   LDefinition::Type type)
        : source(source), dest(dest), type(type) {}
  };
  Vector<ReusedInputReg, 4, BackgroundSystemAllocPolicy> reusedInputs_;

  enum class AllocationKind { UseAtStart, Output, UseOrTemp };

  [[nodiscard]] bool addAllocatedReg(LInstruction* ins, uint32_t vregId,
                                     bool usedAtStart, AnyRegister reg) {
    currentInsRegs_.addUnchecked(reg);
    if (!usedAtStart) {
      currentInsRegsNotAtStart_.addUnchecked(reg);
    }
    availableRegs_.take(reg);
    MOZ_ASSERT_IF(vregs_[vregId].hasRegister(), hasMultipleRegsForVreg_);
    vregs_[vregId].setRegisterIndex(allocatedRegs_.length());
    return allocatedRegs_.emplaceBack(vregId, reg, ins->id());
  }
  void markUseOfAllocatedReg(LInstruction* ins, AllocatedRegister& allocated,
                             bool usedAtStart) {
    MOZ_ASSERT(!availableRegs_.has(allocated.reg()));
    allocated.setLastUsedAtInsId(ins->id());
    currentInsRegs_.addUnchecked(allocated.reg());
    if (!usedAtStart) {
      currentInsRegsNotAtStart_.addUnchecked(allocated.reg());
    }
  }

  [[nodiscard]] bool init();
  [[nodiscard]] bool analyzeLiveness();
  [[nodiscard]] bool allocateRegisters();

  void removeAllocatedRegisterAtIndex(size_t index);

  [[nodiscard]] bool tryReuseRegistersFromPredecessor(MBasicBlock* block);
  void saveAndClearAllocatedRegisters(MBasicBlock* block);

  void freeDeadVregsAfterInstruction(VirtualRegBitSet& liveGC, LNode* ins);

  [[nodiscard]] bool allocateForBlockEnd(LBlock* block, LInstruction* ins);

  LAllocation ensureStackLocation(uint32_t vregId);
  LAllocation registerOrStackLocation(LInstruction* ins, uint32_t vregId,
                                      bool trackRegUse);

  [[nodiscard]] bool spillRegister(LInstruction* ins,
                                   AllocatedRegister allocated);

  [[nodiscard]] bool evictRegister(LInstruction* ins, AnyRegister reg);

  void scanDefinition(LInstruction* ins, LDefinition* def, bool isTemp);

  [[nodiscard]] bool allocateForNonFixedDefOrUse(LInstruction* ins,
                                                 uint32_t vregId,
                                                 AllocationKind kind,
                                                 AnyRegister* reg);
  [[nodiscard]] bool allocateForFixedUse(LInstruction* ins, const LUse* use,
                                         AnyRegister* reg);
  [[nodiscard]] bool allocateForRegisterUse(LInstruction* ins, const LUse* use,
                                            AnyRegister* reg);

  [[nodiscard]] bool allocateForDefinition(uint32_t blockLastId,
                                           LInstruction* ins, LDefinition* def,
                                           bool isTemp);
  [[nodiscard]] bool allocateForInstruction(VirtualRegBitSet& liveGC,
                                            uint32_t blockLastId,
                                            LInstruction* ins);

  [[nodiscard]] bool addLiveRegisterToSafepoint(LSafepoint* safepoint,
                                                AllocatedRegister allocated);
  [[nodiscard]] bool populateSafepoint(VirtualRegBitSet& liveGC,
                                       LInstruction* ins,
                                       LSafepoint* safepoint);

  void assertValidRegisterStateBeforeInstruction() const;

 public:
  SimpleAllocator(MIRGenerator* mir, LIRGenerator* lir, LIRGraph& graph)
      : RegisterAllocator(mir, lir, graph),
        vregs_(mir->alloc()),
        liveGCIn_(mir->alloc()),
        vregLastUses_(mir->alloc()) {}

  [[nodiscard]] bool go();
};

}  // namespace jit
}  // namespace js

#endif /* jit_SimpleAllocator_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/SimpleAllocator.h"

#include "mozilla/Maybe.h"

#include <algorithm>

#include "jit/BitSet.h"
#include "jit/CompileInfo.h"
#include "js/Printf.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::Maybe;

// [SMDOC] Simple Register Allocator
//
// This is an alternative register allocator for Ion that's simpler than the
// backtracking allocator. It attempts to perform register allocation quickly
// while still producing fairly decent code within basic blocks.
//
// The Backtracking allocator is usually the slowest part of the Ion compiler
// backend, whereas this Simple allocator is often faster than codegen and/or
// GVN.
//
// This allocator lets us experiment with different compilation strategies and
// it provides a useful baseline for measuring and optimizing the performance of
// the backtracking allocator.
//
// This allocator also helps document our LIR => Register Allocator interface
// and will make it easier to experiment with alternative register allocators in
// the future.
//
// Phases
// ======
// This allocator has 3 phases with the following main goals:
//
// (1) init: initializes a VirtualRegister instance for each virtual register.
//
// (2) analyzeLiveness: iterates over all LIR instructions in reverse order to
//     collect the following information:
//
//     * For each virtual register we record where it's last used (its
//       lastUseInsId_). Register allocation uses this information to free
//       dead registers and stack slots in freeDeadVregsAfterInstruction.
//
//     * For each basic block it records the set of live GC things (liveGCIn_)
//       at the start of the block. This is used by the register allocation pass
//       to populate safepoints (used for GC tracing).
//
//     * For virtual registers with fixed register uses, it assigns a register
//       hint to help avoid moves (fixedUseHint_).
//
//     This function is based on BacktrackingAllocator::buildLivenessInfo.
//
// (3) allocateRegisters: this iterates over all LIR instructions (from first to
//     last) to allocate registers and to populate safepoints.
//
// Register allocation
// ===================
// Producing the fastest possible code is not a goal for this allocator, so it
// makes the following trade-offs:
//
// * It tries to keep values in registers within a basic block, but values that
//   are used across blocks are spilled in the block where they're defined.
//   We try to spill these values as early as possible (instead of all at the
//   end of the block) to help avoid CPU store buffer stalls.
//
// * Blocks with a single predecessor can reuse the register state at the end of
//   that predecessor block. Values in these registers must have a stack
//   location too so reusing this state is optional. This is based on a small
//   array with state for just four recent blocks in it, but in practice this is
//   good enough to eliminate a lot of memory loads.
//
// * Phis are always assigned a stack slot and phi operands are stored to this
//   location at the end of the predecessor blocks (in allocateForBlockEnd).
//
// * Stack slots are freed when virtual registers die and can then be reused.
//
// In practice this results in fairly decent code within basic blocks. This
// allocator generates relatively poor code for tight loops or for code with
// many short blocks.

static AnyRegister MaybeGetRegisterFromSet(AllocatableRegisterSet regs,
                                           LDefinition::Type type) {
  // Get an available register from `regs`, or return an invalid register if no
  // register is available.
  switch (type) {
    case LDefinition::Type::FLOAT32:
      if (regs.hasAny<RegTypeName::Float32>()) {
        return AnyRegister(regs.getAnyFloat<RegTypeName::Float32>());
      }
      break;
    case LDefinition::Type::DOUBLE:
      if (regs.hasAny<RegTypeName::Float64>()) {
        return AnyRegister(regs.getAnyFloat<RegTypeName::Float64>());
      }
      break;
    case LDefinition::Type::SIMD128:
      if (regs.hasAny<RegTypeName::Vector128>()) {
        return AnyRegister(regs.getAnyFloat<RegTypeName::Vector128>());
      }
      break;
    default:
      MOZ_ASSERT(!LDefinition::isFloatReg(type));
      if (regs.hasAny<RegTypeName::GPR>()) {
        return AnyRegister(regs.getAnyGeneral());
      }
      break;
  }
  return AnyRegister();
}

bool SimpleAllocator::init() {
  size_t numBlocks = graph.numBlocks();
  if (!liveGCIn_.growBy(numBlocks)) {
    return false;
  }

  size_t numVregs = graph.numVirtualRegisters();
  if (!vregs_.initCapacity(numVregs)) {
    return false;
  }
  for (size_t i = 0; i < numVregs; i++) {
    vregs_.infallibleEmplaceBack();
  }

  // Initialize virtual registers.
  for (size_t blockIndex = 0; blockIndex < numBlocks; blockIndex++) {
    if (mir->shouldCancel("init (block loop)")) {
      return false;
    }

    LBlock* block = graph.getBlock(blockIndex);

    for (uint32_t i = 0, numPhis = block->numPhis(); i < numPhis; i++) {
      LPhi* phi = block->getPhi(i);
      LDefinition* def = phi->getDef(0);
      vregs_[def->virtualRegister()].init(phi, def, /* isTemp = */ false);
    }

    for (LInstructionIterator iter = block->begin(); iter != block->end();
         iter++) {
      LInstruction* ins = *iter;
      for (LInstruction::OutputIter output(ins); !output.done(); output++) {
        LDefinition* def = *output;
        vregs_[def->virtualRegister()].init(ins, def, /* isTemp = */ false);
      }
      for (LInstruction::TempIter temp(ins); !temp.done(); temp++) {
        LDefinition* def = *temp;
        vregs_[def->virtualRegister()].init(ins, def, /* isTemp = */ true);
      }
    }
  }

  return true;
}

bool SimpleAllocator::analyzeLiveness() {
  JitSpew(JitSpew_RegAlloc, "Beginning liveness analysis");

  // Stack of active loops.
  struct LoopState {
    // The first instruction of the loop header.
    uint32_t firstId;
    // The last instruction of the backedge.
    uint32_t lastId;
    explicit LoopState(LBlock* header, uint32_t lastId)
        : firstId(header->firstId()), lastId(lastId) {}
  };
  Vector<LoopState, 4, BackgroundSystemAllocPolicy> loopStack;

#ifdef DEBUG
  // In debug builds, assert that each instruction has a smaller ID than the
  // previous instructions we saw (since we're iterating over instructions in
  // reverse order). Our simple liveness analysis relies on this for the vreg's
  // lastUseInsId_.
  uint32_t lastInsId = UINT32_MAX;
#endif

  for (size_t i = graph.numBlocks(); i > 0; i--) {
    if (mir->shouldCancel("analyzeLiveness (block loop)")) {
      return false;
    }

    LBlock* block = graph.getBlock(i - 1);
    MBasicBlock* mblock = block->mir();
    VirtualRegBitSet& liveGC = liveGCIn_[mblock->id()];

    if (mblock->isLoopBackedge()) {
      if (!loopStack.emplaceBack(mblock->loopHeaderOfBackedge()->lir(),
                                 block->lastId())) {
        return false;
      }
    }

    // Propagate liveGCIn from our successors to us. Skip backedges, as we fix
    // them up at the loop header.
    for (size_t i = 0; i < mblock->lastIns()->numSuccessors(); i++) {
      MBasicBlock* successor = mblock->lastIns()->getSuccessor(i);
      if (mblock->id() < successor->id()) {
        if (!liveGC.insertAll(liveGCIn_[successor->id()])) {
          return false;
        }
      }
    }

    uint32_t blockFirstId = block->firstId();

    auto handleUseOfVreg = [&](uint32_t insId, uint32_t vregId) {
      uint32_t defId = vregs_[vregId].insId();
      const LoopState* outerLoop = nullptr;
      if (defId < blockFirstId) {
        // This vreg is defined before the current block. We need to add it to
        // the block's liveGC set if it's a GC type.
        if (vregs_[vregId].isGCType() && !liveGC.insert(vregId)) {
          return false;
        }
        // If we're inside a loop, search for the outermost loop that has this
        // vreg live across the entire loop. We need to extend the vreg's range
        // to cover this loop.
        for (size_t i = loopStack.length(); i > 0; i--) {
          const LoopState& loop = loopStack[i - 1];
          if (defId >= loop.firstId) {
            break;
          }
          outerLoop = &loop;
        }
      }
      vregs_[vregId].updateLastUseId(outerLoop ? outerLoop->lastId : insId);
      return true;
    };

    // If this block has a successor with phis, the phi operands will be used at
    // the end of the current block.
    if (MBasicBlock* successor = mblock->successorWithPhis()) {
      LBlock* phiSuccessor = successor->lir();
      uint32_t blockLastInsId = block->lastId();
      for (size_t j = 0; j < phiSuccessor->numPhis(); j++) {
        LPhi* phi = phiSuccessor->getPhi(j);
        LAllocation* use = phi->getOperand(mblock->positionInPhiSuccessor());
        if (!handleUseOfVreg(blockLastInsId, use->toUse()->virtualRegister())) {
          return false;
        }
      }
    }

    // Handle instruction uses and outputs.
    for (LInstructionReverseIterator ins = block->rbegin();
         ins != block->rend(); ins++) {
      if (mir->shouldCancel("analyzeLiveness (instruction loop)")) {
        return false;
      }

#ifdef DEBUG
      MOZ_ASSERT(ins->id() < lastInsId);
      lastInsId = ins->id();
#endif

      for (LInstruction::OutputIter output(*ins); !output.done(); output++) {
        uint32_t vregId = output->virtualRegister();
        if (vregs_[vregId].isGCType()) {
          liveGC.remove(vregId);
        }
      }
      for (LInstruction::InputIter inputAlloc(**ins); inputAlloc.more();
           inputAlloc.next()) {
        if (!inputAlloc->isUse()) {
          continue;
        }
        LUse* use = inputAlloc->toUse();
        uint32_t vregId = use->virtualRegister();
        if (!handleUseOfVreg(ins->id(), vregId)) {
          return false;
        }
        if (use->policy() == LUse::FIXED) {
          // Assign a register hint to the vreg. We'll prefer allocating this
          // register later to avoid unnecessary moves.
          VirtualRegister& vreg = vregs_[vregId];
          AnyRegister fixedReg = GetFixedRegister(vreg.def(), use);
          if (vreg.fixedUseHint().isNothing()) {
            vreg.setFixedUseHint(fixedReg);
          } else if (*vreg.fixedUseHint() != fixedReg) {
            // Conflicting fixed register uses. Clear the hint.
            vreg.setFixedUseHint(AnyRegister());
          }
        }
      }
    }

    // Handle phi definitions.
    for (size_t i = 0; i < block->numPhis(); i++) {
      LPhi* phi = block->getPhi(i);
      LDefinition* def = phi->getDef(0);
      uint32_t vregId = def->virtualRegister();
      if (vregs_[vregId].isGCType()) {
        liveGC.remove(vregId);
      }
      // Loop header phis must not be freed before the end of the backedge
      // because the backedge might store to the phi's stack slot.
      if (mblock->isLoopHeader()) {
        vregs_[vregId].updateLastUseId(loopStack.back().lastId);
      }
    }

    if (mblock->isLoopHeader()) {
      MBasicBlock* backedge = mblock->backedge();
      MOZ_ASSERT(loopStack.back().firstId == block->firstId());
      loopStack.popBack();

      // Propagate the loop header's liveGCIn set to all blocks within the loop.
      if (mblock != backedge && !liveGC.empty()) {
        // Start at the block after |mblock|.
        MOZ_ASSERT(graph.getBlock(i - 1) == mblock->lir());
        size_t j = i;
        while (true) {
          MBasicBlock* loopBlock = graph.getBlock(j)->mir();
          if (!liveGCIn_[loopBlock->id()].insertAll(liveGC)) {
            return false;
          }
          if (loopBlock == backedge) {
            break;
          }
          j++;
        }
      }
    }

    MOZ_ASSERT_IF(!mblock->numPredecessors(), liveGC.empty());
  }

  MOZ_ASSERT(loopStack.empty());

  // Initialize vregLastUses_ vector and sort it by instructionId in descending
  // order. This will be used in freeDeadVregsAfterInstruction.
  uint32_t numVregs = vregs_.length();
  if (!vregLastUses_.reserve(numVregs)) {
    return false;
  }
  for (uint32_t vregId = 1; vregId < numVregs; vregId++) {
    vregLastUses_.infallibleEmplaceBack(vregs_[vregId].lastUseInsId(), vregId);
  }
  auto compareEntries = [](VregLastUse a, VregLastUse b) {
    return a.instructionId > b.instructionId;
  };
  std::sort(vregLastUses_.begin(), vregLastUses_.end(), compareEntries);
  return true;
}

void SimpleAllocator::removeAllocatedRegisterAtIndex(size_t index) {
  // Release the register for this entry.
  AllocatedRegister allocated = allocatedRegs_[index];
  uint32_t vregId = allocated.vregId();
  availableRegs_.add(allocated.reg());

  // Use the swap-and-pop idiom to remove the entry so that we don't change the
  // register index of other entries.
  size_t lastIndex = allocatedRegs_.length() - 1;
  if (index != lastIndex) {
    uint32_t lastVregId = allocatedRegs_.back().vregId();
    allocatedRegs_[index] = allocatedRegs_.back();
    if (vregs_[lastVregId].registerIndex() == lastIndex) {
      vregs_[lastVregId].setRegisterIndex(index);
    }
  }
  allocatedRegs_.popBack();
  vregs_[vregId].clearRegisterIndex();

  // In the uncommon case where the vreg might have another allocated register,
  // assign the other register to this vreg if needed.
  if (MOZ_UNLIKELY(hasMultipleRegsForVreg_)) {
    for (size_t j = 0; j < allocatedRegs_.length(); j++) {
      if (allocatedRegs_[j].vregId() == vregId) {
        vregs_[vregId].setRegisterIndex(j);
        break;
      }
    }
  }
}

LAllocation SimpleAllocator::ensureStackLocation(uint32_t vregId) {
  // Allocate a stack slot for this virtual register if needed.
  VirtualRegister& vreg = vregs_[vregId];
  if (vreg.hasStackLocation()) {
    return vreg.stackLocation();
  }
  LStackSlot::Width width = LStackSlot::width(vreg.def()->type());
  LStackSlot::SlotAndWidth slot(stackSlotAllocator_.allocateSlot(width), width);
  vreg.setAllocatedStackSlot(slot);
  return LStackSlot(slot);
}

LAllocation SimpleAllocator::registerOrStackLocation(LInstruction* ins,
                                                     uint32_t vregId,
                                                     bool trackRegUse) {
  const VirtualRegister& vreg = vregs_[vregId];
  if (vreg.hasRegister()) {
    AllocatedRegister& allocated = allocatedRegs_[vreg.registerIndex()];
    if (trackRegUse) {
      allocated.setLastUsedAtInsId(ins->id());
    }
    return LAllocation(allocated.reg());
  }
  return vreg.stackLocation();
}

bool SimpleAllocator::spillRegister(LInstruction* ins,
                                    AllocatedRegister allocated) {
  VirtualRegister& vreg = vregs_[allocated.vregId()];
  MOZ_ASSERT(vreg.insId() < ins->id());
  if (vreg.hasStackLocation()) {
    return true;
  }
  // Allocate a new stack slot and insert a register => stack move.
  LMoveGroup* input = getInputMoveGroup(ins);
  LAllocation dest = ensureStackLocation(allocated.vregId());
  return input->addAfter(LAllocation(allocated.reg()), dest,
                         vreg.def()->type());
}

bool SimpleAllocator::allocateForBlockEnd(LBlock* block, LInstruction* ins) {
#ifdef DEBUG
  // All vregs that are live after this block must have a stack location at this
  // point.
  for (const AllocatedRegister& allocated : allocatedRegs_) {
    MOZ_ASSERT_IF(vregs_[allocated.vregId()].lastUseInsId() > ins->id(),
                  vregs_[allocated.vregId()].hasStackLocation());
  }
#endif

  MBasicBlock* successor = block->mir()->successorWithPhis();
  if (!successor) {
    return true;
  }

  // The current block has a successor with phis. For each successor phi, insert
  // a move at the end of the current block to store the phi's operand into the
  // phi's stack slot.

  uint32_t position = block->mir()->positionInPhiSuccessor();
  LBlock* lirSuccessor = successor->lir();
  LMoveGroup* group = nullptr;

  for (size_t i = 0, numPhis = lirSuccessor->numPhis(); i < numPhis; i++) {
    LPhi* phi = lirSuccessor->getPhi(i);

    uint32_t sourceVreg = phi->getOperand(position)->toUse()->virtualRegister();
    uint32_t destVreg = phi->getDef(0)->virtualRegister();
    if (sourceVreg == destVreg) {
      continue;
    }

    if (!group) {
      // The moves we insert here need to happen simultaneously with each other,
      // yet after any existing moves before the instruction.
      LMoveGroup* input = getInputMoveGroup(ins);
      if (input->numMoves() == 0) {
        group = input;
      } else {
        group = LMoveGroup::New(alloc());
        block->insertAfter(input, group);
      }
    }

    LAllocation source =
        registerOrStackLocation(ins, sourceVreg, /* trackRegUse = */ true);
    LAllocation dest = ensureStackLocation(destVreg);
    if (!group->add(source, dest, phi->getDef(0)->type())) {
      return false;
    }
  }

  return true;
}

void SimpleAllocator::scanDefinition(LInstruction* ins, LDefinition* def,
                                     bool isTemp) {
  // Record fixed registers to make sure we don't assign these registers to
  // uses.
  if (def->policy() == LDefinition::FIXED && def->output()->isAnyRegister()) {
    AnyRegister reg = def->output()->toAnyRegister();
    if (isTemp) {
      fixedTempRegs_.add(reg);
    }
    // Note: addUnchecked because some instructions use the same fixed register
    // for a Temp and an Output (eg LCallNative). See bug 1962671.
    fixedOutputAndTempRegs_.addUnchecked(reg);
    return;
  }
  if (def->policy() == LDefinition::MUST_REUSE_INPUT) {
    ins->changePolicyOfReusedInputToAny(def);
  }
}

bool SimpleAllocator::allocateForNonFixedDefOrUse(LInstruction* ins,
                                                  uint32_t vregId,
                                                  AllocationKind kind,
                                                  AnyRegister* reg) {
  // This function is responsible for finding a new register for a (non-fixed)
  // register def or use. If no register is available, it will evict something.

  // This should only be called if the vreg doesn't have a register yet, unless
  // the caller set hasMultipleRegsForVreg_ to true.
  const VirtualRegister& vreg = vregs_[vregId];
  MOZ_ASSERT_IF(!hasMultipleRegsForVreg_, !vreg.hasRegister());

  // Determine the set of available registers. We can pick any register in
  // availableRegs_ except for fixed output/temp registers that we need to
  // allocate later. Note that UseAtStart inputs may use the same register as
  // outputs (but not temps).
  bool isUseAtStart = (kind == AllocationKind::UseAtStart);
  RegisterSet fixedDefs =
      isUseAtStart ? fixedTempRegs_.set() : fixedOutputAndTempRegs_.set();
  AllocatableRegisterSet available(
      RegisterSet::Subtract(availableRegs_.set(), fixedDefs));

  // If the virtual register has a register hint, pick that register if it's
  // available.
  if (vreg.fixedUseHint().isSome() && vreg.fixedUseHint()->isValid()) {
    AnyRegister regHint = *vreg.fixedUseHint();
    if (available.has(regHint)) {
      *reg = regHint;
      return addAllocatedReg(ins, vregId, isUseAtStart, *reg);
    }
  }

  // Try to get an arbitrary register from the set.
  *reg = MaybeGetRegisterFromSet(available, vreg.def()->type());
  if (reg->isValid()) {
    return addAllocatedReg(ins, vregId, isUseAtStart, *reg);
  }

  // No register is available. We need to evict something.

  // Determine which registers we must not evict. These are the registers in
  // fixedDefs (same as above) + registers we've already allocated for this
  // instruction. Note again that outputs can be assigned the same register as
  // UseAtStart inputs and we rely on this to not run out of registers on 32-bit
  // x86.
  AllocatableRegisterSet notEvictable;
  if (kind == AllocationKind::Output) {
    notEvictable.set() =
        RegisterSet::Union(fixedDefs, currentInsRegsNotAtStart_.set());
  } else {
    notEvictable.set() = RegisterSet::Union(fixedDefs, currentInsRegs_.set());
  }

  // Search for the best register to evict.
  LDefinition* def = vreg.def();
  const AllocatedRegister* bestToEvict = nullptr;
  for (size_t i = 0, len = allocatedRegs_.length(); i < len; i++) {
    AllocatedRegister& allocated = allocatedRegs_[i];
    if (!def->isCompatibleReg(allocated.reg()) ||
        notEvictable.has(allocated.reg())) {
      continue;
    }
    // Heuristics: prefer evicting registers where the vreg is also in another
    // register (uncommon), registers that already have a stack location, or
    // registers that haven't been used recently.
    if (!bestToEvict || vregs_[allocated.vregId()].registerIndex() != i ||
        (vregs_[allocated.vregId()].hasStackLocation() &&
         !vregs_[bestToEvict->vregId()].hasStackLocation()) ||
        allocated.lastUsedAtInsId() < bestToEvict->lastUsedAtInsId()) {
      bestToEvict = &allocated;
    }
  }

  if (bestToEvict) {
    *reg = bestToEvict->reg();
  } else {
    // We didn't find a compatible register to evict. This can happen for
    // aliased registers, for example if we allocated all Float32 registers and
    // now need a Double register. Pick an arbitrary register that's evictable
    // and evict all of its aliases.
    AllocatableRegisterSet evictable;
    evictable.set() =
        RegisterSet::Subtract(allRegisters_.set(), notEvictable.set());
    *reg = MaybeGetRegisterFromSet(evictable, def->type());
    MOZ_ASSERT(reg->numAliased() > 1);
  }
  if (!evictRegister(ins, *reg)) {
    return false;
  }
  return addAllocatedReg(ins, vregId, isUseAtStart, *reg);
}

bool SimpleAllocator::allocateForFixedUse(LInstruction* ins, const LUse* use,
                                          AnyRegister* reg) {
  uint32_t vregId = use->virtualRegister();
  VirtualRegister& vreg = vregs_[vregId];
  *reg = GetFixedRegister(vreg.def(), use);

  // Determine the vreg's current location.
  LAllocation alloc;
  if (vreg.hasRegister()) {
    // Check if the vreg is already using the fixed register.
    AllocatedRegister& allocated = allocatedRegs_[vreg.registerIndex()];
    if (allocated.reg() == *reg) {
      markUseOfAllocatedReg(ins, allocated, use->usedAtStart());
      return true;
    }

    // Try to avoid having multiple registers for the same vreg by replacing the
    // current register with the new one. If this is not possible (this is
    // uncommon) we set hasMultipleRegsForVreg_ and fix this up at the end of
    // allocateForInstruction.
    alloc = LAllocation(allocated.reg());
    if (currentInsRegs_.has(allocated.reg())) {
      hasMultipleRegsForVreg_ = true;
    } else {
      removeAllocatedRegisterAtIndex(vreg.registerIndex());
    }
  } else {
    alloc = vreg.stackLocation();
  }

  // If the fixed register is not available we have to evict it.
  if (!availableRegs_.has(*reg) && !evictRegister(ins, *reg)) {
    return false;
  }

  // Mark register as allocated and load the value in it.
  if (!addAllocatedReg(ins, vregId, use->usedAtStart(), *reg)) {
    return false;
  }
  LMoveGroup* input = getInputMoveGroup(ins);
  return input->addAfter(alloc, LAllocation(*reg), vreg.def()->type());
}

bool SimpleAllocator::allocateForRegisterUse(LInstruction* ins, const LUse* use,
                                             AnyRegister* reg) {
  uint32_t vregId = use->virtualRegister();
  VirtualRegister& vreg = vregs_[vregId];
  bool useAtStart = use->usedAtStart();

  // Determine the vreg's current location.
  LAllocation alloc;
  if (vreg.hasRegister()) {
    // The vreg already has a register. We can use it if we won't need this
    // register later for a fixed output or temp.
    AllocatedRegister& allocated = allocatedRegs_[vreg.registerIndex()];
    bool isReserved = useAtStart ? fixedTempRegs_.has(allocated.reg())
                                 : fixedOutputAndTempRegs_.has(allocated.reg());
    if (!isReserved) {
      markUseOfAllocatedReg(ins, allocated, useAtStart);
      *reg = allocated.reg();
      return true;
    }

    // Try to avoid having multiple registers for the same vreg by replacing the
    // current register with the new one. If this is not possible (this is
    // uncommon) we set hasMultipleRegsForVreg_ and fix this up at the end of
    // allocateForInstruction.
    alloc = LAllocation(allocated.reg());
    if (currentInsRegs_.has(allocated.reg())) {
      hasMultipleRegsForVreg_ = true;
    } else {
      removeAllocatedRegisterAtIndex(vreg.registerIndex());
    }
  } else {
    alloc = vreg.stackLocation();
  }

  // This vreg is not in a register or it's in a reserved register. Allocate a
  // new register.
  AllocationKind kind =
      useAtStart ? AllocationKind::UseAtStart : AllocationKind::UseOrTemp;
  if (!allocateForNonFixedDefOrUse(ins, vregId, kind, reg)) {
    return false;
  }
  LMoveGroup* input = getInputMoveGroup(ins);
  return input->addAfter(alloc, LAllocation(*reg), vreg.def()->type());
}

bool SimpleAllocator::evictRegister(LInstruction* ins, AnyRegister reg) {
  // The caller wants to use `reg` but it's not available. Spill this register
  // (and its aliases) to make it available.

  MOZ_ASSERT(reg.isValid());
  MOZ_ASSERT(!availableRegs_.has(reg));

  for (size_t i = 0; i < allocatedRegs_.length();) {
    AllocatedRegister allocated = allocatedRegs_[i];
    if (!allocated.reg().aliases(reg)) {
      i++;
      continue;
    }

    // Registers are added to the safepoint in `populateSafepoint`, but if we're
    // evicting a register that's being used at-start we lose information about
    // this so we have to add it to the safepoint now.
    if (ins->safepoint() && !ins->isCall() &&
        currentInsRegs_.has(allocated.reg())) {
      MOZ_ASSERT(!currentInsRegsNotAtStart_.has(allocated.reg()));
      if (!addLiveRegisterToSafepoint(ins->safepoint(), allocated)) {
        return false;
      }
    }

    // Spill this register unless we know this vreg also lives in a different
    // register (this is uncommon).
    uint32_t vregId = allocated.vregId();
    if (vregs_[vregId].registerIndex() == i) {
      if (!spillRegister(ins, allocated)) {
        return false;
      }
    } else {
      MOZ_ASSERT(hasMultipleRegsForVreg_);
    }
    removeAllocatedRegisterAtIndex(i);

    if (availableRegs_.has(reg)) {
      return true;
    }
  }

  MOZ_CRASH("failed to evict register");
}

bool SimpleAllocator::allocateForDefinition(uint32_t blockLastId,
                                            LInstruction* ins, LDefinition* def,
                                            bool isTemp) {
  uint32_t vregId = def->virtualRegister();

  // Allocate a register for this definition. Return early for definitions that
  // don't need a register.
  AnyRegister reg;
  switch (def->policy()) {
    case LDefinition::FIXED: {
      if (!def->output()->isAnyRegister()) {
        MOZ_ASSERT(!isTemp);
        vregs_[vregId].setHasStackLocation();
        return true;
      }

      // We need a fixed register. Evict it if it's not available.
      reg = def->output()->toAnyRegister();
      if (!availableRegs_.has(reg)) {
        // For call instructions we allow Temps and Outputs to use the same
        // fixed register. That should probably be changed at some point, but
        // for now we can handle this edge case by removing the temp's register.
        // See bug 1962671.
        if (!isTemp && fixedTempRegs_.has(reg)) {
          MOZ_ASSERT(ins->isCall());
          for (size_t i = 0; i < allocatedRegs_.length(); i++) {
            if (allocatedRegs_[i].reg() == reg) {
              removeAllocatedRegisterAtIndex(i);
              break;
            }
          }
          MOZ_ASSERT(availableRegs_.has(reg));
        } else {
          if (!evictRegister(ins, reg)) {
            return false;
          }
        }
      }
      if (!addAllocatedReg(ins, vregId, /* usedAtStart = */ false, reg)) {
        return false;
      }
      break;
    }
    case LDefinition::REGISTER: {
      AllocationKind kind =
          isTemp ? AllocationKind::UseOrTemp : AllocationKind::Output;
      if (!allocateForNonFixedDefOrUse(ins, vregId, kind, &reg)) {
        return false;
      }
      break;
    }
    case LDefinition::MUST_REUSE_INPUT: {
      // Allocate a new register and add an entry to reusedInputs_ to move the
      // input into this register when we're done allocating registers.
      AllocationKind kind =
          isTemp ? AllocationKind::UseOrTemp : AllocationKind::Output;
      if (!allocateForNonFixedDefOrUse(ins, vregId, kind, &reg)) {
        return false;
      }
      LAllocation* useAlloc = ins->getOperand(def->getReusedInput());
      uint32_t useVregId = useAlloc->toUse()->virtualRegister();
      LDefinition::Type type = vregs_[useVregId].def()->type();
      if (!reusedInputs_.emplaceBack(useAlloc, reg, type)) {
        return false;
      }
      break;
    }
    case LDefinition::STACK: {
      // This is a Wasm stack area or stack result. This is used when calling
      // functions with multiple return values.
      MOZ_ASSERT(!isTemp);
      if (def->type() == LDefinition::STACKRESULTS) {
        LStackArea alloc(ins->toInstruction());
        stackSlotAllocator_.allocateStackArea(&alloc);
        def->setOutput(alloc);
      } else {
        // Because the definitions are visited in order, the area has been
        // allocated before we reach this result, so we know the operand is an
        // LStackArea.
        const LUse* use = ins->getOperand(0)->toUse();
        VirtualRegister& area = vregs_[use->virtualRegister()];
        const LStackArea* areaAlloc = area.def()->output()->toStackArea();
        def->setOutput(areaAlloc->resultAlloc(ins, def));
      }
      vregs_[vregId].setHasStackLocation();
      return true;
    }
  }

  def->setOutput(LAllocation(reg));

  // If this is an output register for a vreg that's used in a different block,
  // we have to spill it to the stack at some point and we want to do that as
  // early as possible. We can't do it here though because we're not allowed to
  // insert moves right before LOsiPoint instructions. Add the outputs to a
  // vector that we check at the start of allocateForInstruction.
  if (!isTemp && vregs_[vregId].lastUseInsId() > blockLastId) {
    if (!eagerSpillOutputs_.append(def)) {
      return false;
    }
  }

  return true;
}

bool SimpleAllocator::allocateForInstruction(VirtualRegBitSet& liveGC,
                                             uint32_t blockLastId,
                                             LInstruction* ins) {
  if (!alloc().ensureBallast()) {
    return false;
  }

  assertValidRegisterStateBeforeInstruction();
  MOZ_ASSERT(reusedInputs_.empty());

  // If we have to spill output registers of a previous instruction, do that now
  // if the instruction is not an LOsiPoint instruction. See the comment at the
  // end of allocateForDefinition.
  if (!eagerSpillOutputs_.empty() && !ins->isOsiPoint()) {
    LMoveGroup* moves = getInputMoveGroup(ins);
    for (LDefinition* def : eagerSpillOutputs_) {
      MOZ_ASSERT(!vregs_[def->virtualRegister()].hasStackLocation());
      LAllocation dest = ensureStackLocation(def->virtualRegister());
      if (!moves->add(*def->output(), dest, def->type())) {
        return false;
      }
    }
    eagerSpillOutputs_.clear();
  }

  // Clear per-instruction register sets.
  currentInsRegs_ = AllocatableRegisterSet();
  currentInsRegsNotAtStart_ = AllocatableRegisterSet();
  fixedTempRegs_ = AllocatableRegisterSet();
  fixedOutputAndTempRegs_ = AllocatableRegisterSet();

  // Allocate all fixed inputs first.
  for (LInstruction::NonSnapshotInputIter alloc(*ins); alloc.more();
       alloc.next()) {
    if (!alloc->isUse() || alloc->toUse()->policy() != LUse::FIXED) {
      continue;
    }
    AnyRegister reg;
    if (!allocateForFixedUse(ins, alloc->toUse(), &reg)) {
      return false;
    }
    alloc.replace(LAllocation(reg));
  }

  // Scan all definitions. If any of these require fixed registers, it will
  // affect which registers are available for the inputs.
  for (LInstruction::TempIter temp(ins); !temp.done(); temp++) {
    scanDefinition(ins, *temp, /* isTemp = */ true);
  }
  for (LInstruction::OutputIter output(ins); !output.done(); output++) {
    scanDefinition(ins, *output, /* isTemp = */ false);
  }

  // Allocate inputs which are required to be in non-fixed registers.
  for (LInstruction::NonSnapshotInputIter alloc(*ins); alloc.more();
       alloc.next()) {
    if (!alloc->isUse() || alloc->toUse()->policy() != LUse::REGISTER) {
      continue;
    }
    AnyRegister reg;
    if (!allocateForRegisterUse(ins, alloc->toUse(), &reg)) {
      return false;
    }
    alloc.replace(LAllocation(reg));
  }

  // Allocate all definitions.
  for (LInstruction::TempIter temp(ins); !temp.done(); temp++) {
    if (!allocateForDefinition(blockLastId, ins, *temp, /* isTemp = */ true)) {
      return false;
    }
  }
  for (LInstruction::OutputIter output(ins); !output.done(); output++) {
    LDefinition* def = *output;
    if (!allocateForDefinition(blockLastId, ins, def, /* isTemp = */ false)) {
      return false;
    }
    if (vregs_[def->virtualRegister()].isGCType()) {
      if (!liveGC.insert(def->virtualRegister())) {
        return false;
      }
    }
  }

  // If this instruction is a call, we need to evict all registers except for
  // preserved registers or the instruction's definitions. We have to do this
  // before the next step because call instructions are allowed to clobber
  // their input registers so we shouldn't use the same register for snapshot
  // inputs.
  if (ins->isCall()) {
    for (size_t i = 0; i < allocatedRegs_.length();) {
      AllocatedRegister allocated = allocatedRegs_[i];
      if (ins->isCallPreserved(allocated.reg()) ||
          vregs_[allocated.vregId()].insId() == ins->id()) {
        i++;
        continue;
      }
      if (!spillRegister(ins, allocated)) {
        return false;
      }
      removeAllocatedRegisterAtIndex(i);
    }
  }

  // Allocate for remaining inputs which do not need to be in registers.
  for (LInstruction::InputIter alloc(*ins); alloc.more(); alloc.next()) {
    if (!alloc->isUse()) {
      continue;
    }
    LUse* use = alloc->toUse();
    MOZ_ASSERT(use->policy() != LUse::REGISTER && use->policy() != LUse::FIXED);
    uint32_t vreg = use->virtualRegister();
    // KEEPALIVE uses are very common in JS code for snapshots and these uses
    // don't prefer a register. Don't update the register's last-use (used for
    // eviction heuristics) for them.
    bool trackRegUse = (use->policy() != LUse::KEEPALIVE);
    LAllocation allocated = registerOrStackLocation(ins, vreg, trackRegUse);
    alloc.replace(allocated);
  }

  // If this instruction had MUST_REUSE_INPUT definitions, we've now assigned a
  // register or stack slot for the input and we allocated a register for the
  // output. We can now insert a move and then rewrite the input LAllocation to
  // match the LDefinition (code generation relies on this).
  while (!reusedInputs_.empty()) {
    auto entry = reusedInputs_.popCopy();
    LMoveGroup* input = getInputMoveGroup(ins);
    if (!input->addAfter(*entry.source, LAllocation(entry.dest), entry.type)) {
      return false;
    }
    *entry.source = LAllocation(entry.dest);
  }

  // Add registers and stack locations to the instruction's safepoint if needed.
  if (LSafepoint* safepoint = ins->safepoint()) {
    if (!populateSafepoint(liveGC, ins, safepoint)) {
      return false;
    }
  }

  // In the uncommon case where a vreg might have multiple allocated registers,
  // discard the registers not linked from the vreg. This simplifies other parts
  // of the allocator.
  if (hasMultipleRegsForVreg_) {
    for (size_t i = 0; i < allocatedRegs_.length();) {
      VirtualRegister& vreg = vregs_[allocatedRegs_[i].vregId()];
      if (vreg.registerIndex() != i) {
        removeAllocatedRegisterAtIndex(i);
        continue;
      }
      i++;
    }
    hasMultipleRegsForVreg_ = false;
  }

  return true;
}

bool SimpleAllocator::addLiveRegisterToSafepoint(LSafepoint* safepoint,
                                                 AllocatedRegister allocated) {
  safepoint->addLiveRegister(allocated.reg());
  const VirtualRegister& vreg = vregs_[allocated.vregId()];
  if (vreg.isGCType()) {
    if (!safepoint->addGCAllocation(allocated.vregId(), vreg.def(),
                                    LAllocation(allocated.reg()))) {
      return false;
    }
  }
  return true;
}

bool SimpleAllocator::populateSafepoint(VirtualRegBitSet& liveGC,
                                        LInstruction* ins,
                                        LSafepoint* safepoint) {
  // Add allocated registers to the safepoint. Safepoints for call instructions
  // never include any registers.
  if (!ins->isCall()) {
    for (AllocatedRegister allocated : allocatedRegs_) {
      // Outputs (but not temps) of the current instruction are ignored, except
      // for the clobberedRegs set that's used for debug assertions.
      const VirtualRegister& vreg = vregs_[allocated.vregId()];
      if (vreg.insId() == ins->id()) {
#ifdef CHECK_OSIPOINT_REGISTERS
        safepoint->addClobberedRegister(allocated.reg());
#endif
        if (!vreg.isTemp()) {
          continue;
        }
      }
      if (!addLiveRegisterToSafepoint(safepoint, allocated)) {
        return false;
      }
    }
  }

  // Also add stack locations that store GC things.
  for (VirtualRegBitSet::Iterator liveRegId(liveGC); liveRegId; ++liveRegId) {
    uint32_t vregId = *liveRegId;
    const VirtualRegister& vreg = vregs_[vregId];
    MOZ_ASSERT(vreg.isGCType());
    if (!vreg.hasStackLocation() || vreg.insId() == ins->id()) {
      continue;
    }
    if (!safepoint->addGCAllocation(vregId, vreg.def(), vreg.stackLocation())) {
      return false;
    }
  }

  return true;
}

void SimpleAllocator::freeDeadVregsAfterInstruction(VirtualRegBitSet& liveGC,
                                                    LNode* ins) {
  // Mark virtual registers that won't be used after `ins` as dead. This also
  // frees their stack slots and registers.
  while (!vregLastUses_.empty() &&
         vregLastUses_.back().instructionId <= ins->id()) {
    VregLastUse entry = vregLastUses_.popCopy();
    VirtualRegister& vreg = vregs_[entry.vregId];
    if (vreg.hasRegister()) {
      removeAllocatedRegisterAtIndex(vreg.registerIndex());
    }
    if (vreg.hasAllocatedStackSlot()) {
      LStackSlot::SlotAndWidth stackSlot = vreg.stackSlot();
      stackSlotAllocator_.freeSlot(stackSlot.width(), stackSlot.slot());
    }
    if (vreg.isGCType()) {
      liveGC.remove(entry.vregId);
    }
    vreg.markDead();
  }
}

bool SimpleAllocator::tryReuseRegistersFromPredecessor(MBasicBlock* block) {
  // Try to reuse the register state from our predecessor block if we have a
  // single predecessor. Note that this is completely optional because all live
  // vregs must have a stack location at this point.

  if (block->numPredecessors() != 1) {
    return true;
  }

  auto findBlockState = [this](uint32_t predId) -> const BlockState* {
    for (const BlockState& state : blockStates_) {
      if (state.blockIndex == predId) {
        return &state;
      }
    }
    return nullptr;
  };
  const BlockState* state = findBlockState(block->getPredecessor(0)->id());
  if (!state) {
    return true;
  }

  MOZ_ASSERT(allocatedRegs_.empty());
  availableRegs_ = state->availableRegs;
  for (AllocatedRegister allocated : state->allocatedRegs) {
    // Ignore registers for vregs that were marked dead between the
    // predecessor block and the current block.
    if (vregs_[allocated.vregId()].isDead()) {
      availableRegs_.add(allocated.reg());
    } else {
      VirtualRegister& vreg = vregs_[allocated.vregId()];
      MOZ_ASSERT(!vreg.hasRegister());
      vreg.setRegisterIndex(allocatedRegs_.length());
      if (!allocatedRegs_.append(allocated)) {
        return false;
      }
    }
  }
  return true;
}

void SimpleAllocator::saveAndClearAllocatedRegisters(MBasicBlock* block) {
  if (allocatedRegs_.empty()) {
    return;
  }

  for (AllocatedRegister allocated : allocatedRegs_) {
    vregs_[allocated.vregId()].clearRegisterIndex();
  }

  // Ensure at least one successor has a single predecessor block that could use
  // our register state later in tryReuseRegistersFromPredecessor.
  bool shouldSave = false;
  for (size_t i = 0; i < block->numSuccessors(); i++) {
    if (block->getSuccessor(i)->numPredecessors() == 1 &&
        block->getSuccessor(i)->id() > block->id()) {
      shouldSave = true;
      break;
    }
  }
  if (shouldSave) {
    // Save current register state in the circular buffer. We use std::swap for
    // the vectors to try to reuse malloc buffers.
    BlockState& state = blockStates_[nextBlockStateIndex_];
    std::swap(allocatedRegs_, state.allocatedRegs);
    state.availableRegs = availableRegs_;
    state.blockIndex = block->id();
    nextBlockStateIndex_ = (nextBlockStateIndex_ + 1) % BlockStateLength;
  }
  allocatedRegs_.clear();
  availableRegs_ = allRegisters_;
}

bool SimpleAllocator::allocateRegisters() {
  // Initialize register state.
  MOZ_ASSERT(allocatedRegs_.empty());
  availableRegs_ = allRegisters_;

  size_t numBlocks = graph.numBlocks();

  // It's very common for JS code to have a basic block that jumps to the next
  // block and the next block doesn't have any other predecessors. We treat
  // such blocks as a single block.
  bool fuseWithNextBlock = false;

  for (size_t blockIndex = 0; blockIndex < numBlocks; blockIndex++) {
    if (mir->shouldCancel("allocateRegisters (block loop)")) {
      return false;
    }

    LBlock* block = graph.getBlock(blockIndex);
    MBasicBlock* mblock = block->mir();
    MOZ_ASSERT(mblock->id() == blockIndex);

    VirtualRegBitSet& liveGC = liveGCIn_[blockIndex];

    // Try to reuse the register state from our predecessor. If we fused this
    // block with the previous block we're already using its register state.
    if (!fuseWithNextBlock) {
      MOZ_ASSERT(allocatedRegs_.empty());
      if (!tryReuseRegistersFromPredecessor(mblock)) {
        return false;
      }
    }

    // Note: sometimes two blocks appear fusable but the successor block still
    // has an (unnecessary) phi. Don't fuse in this edge case.
    fuseWithNextBlock = mblock->numSuccessors() == 1 &&
                        mblock->getSuccessor(0)->id() == blockIndex + 1 &&
                        mblock->getSuccessor(0)->numPredecessors() == 1 &&
                        graph.getBlock(blockIndex + 1)->numPhis() == 0;

    uint32_t blockLastId = fuseWithNextBlock
                               ? graph.getBlock(blockIndex + 1)->lastId()
                               : block->lastId();

    // Allocate stack slots for phis.
    for (uint32_t i = 0, numPhis = block->numPhis(); i < numPhis; i++) {
      LPhi* phi = block->getPhi(i);
      LDefinition* def = phi->getDef(0);
      uint32_t vregId = def->virtualRegister();
      bool isGCType = vregs_[vregId].isGCType();
      def->setOutput(ensureStackLocation(vregId));
      if (isGCType && !liveGC.insert(vregId)) {
        return false;
      }
      if (i == numPhis - 1) {
        freeDeadVregsAfterInstruction(liveGC, phi);
      }
    }

    // Allocate registers for each instruction.
    for (LInstructionIterator iter = block->begin(); iter != block->end();
         iter++) {
      if (mir->shouldCancel("allocateRegisters (instruction loop)")) {
        return false;
      }

      LInstruction* ins = *iter;
      if (!allocateForInstruction(liveGC, blockLastId, ins)) {
        return false;
      }
      if (ins == *block->rbegin() && !fuseWithNextBlock) {
        if (!allocateForBlockEnd(block, ins)) {
          return false;
        }
      }
      freeDeadVregsAfterInstruction(liveGC, ins);
    }

    if (!fuseWithNextBlock) {
      saveAndClearAllocatedRegisters(mblock);
    }
  }

  // All vregs must now be dead.
  MOZ_ASSERT(vregLastUses_.empty());
#ifdef DEBUG
  for (size_t i = 1; i < vregs_.length(); i++) {
    MOZ_ASSERT(vregs_[i].isDead());
  }
#endif

  graph.setLocalSlotsSize(stackSlotAllocator_.stackHeight());
  return true;
}

void SimpleAllocator::assertValidRegisterStateBeforeInstruction() const {
#ifdef DEBUG
  MOZ_ASSERT(!hasMultipleRegsForVreg_);

  // Assert allocatedRegs_ does not contain duplicate registers and matches the
  // availableRegs_ set.
  AllocatableRegisterSet available = allRegisters_;
  for (size_t i = 0; i < allocatedRegs_.length(); i++) {
    AllocatedRegister allocated = allocatedRegs_[i];
    available.take(allocated.reg());
    const VirtualRegister& vreg = vregs_[allocated.vregId()];
    MOZ_ASSERT(vreg.registerIndex() == i);
    MOZ_ASSERT(!vreg.isTemp());
  }
  MOZ_ASSERT(availableRegs_.set() == available.set());

  // Assert vregs have a valid register index. Limit this to the first 20 vregs
  // to not slow down debug builds too much.
  size_t numVregsToCheck = std::min<size_t>(20, vregs_.length());
  for (size_t i = 1; i < numVregsToCheck; i++) {
    if (!vregs_[i].isDead() && vregs_[i].hasRegister()) {
      size_t index = vregs_[i].registerIndex();
      MOZ_ASSERT(allocatedRegs_[index].vregId() == i);
    }
  }
#endif
}

bool SimpleAllocator::go() {
  JitSpewCont(JitSpew_RegAlloc, "\n");
  JitSpew(JitSpew_RegAlloc, "Beginning register allocation");

  JitSpewCont(JitSpew_RegAlloc, "\n");
  if (JitSpewEnabled(JitSpew_RegAlloc)) {
    dumpInstructions("(Pre-allocation LIR)");
  }

  if (!init()) {
    return false;
  }
  if (!analyzeLiveness()) {
    return false;
  }
  if (!allocateRegisters()) {
    return false;
  }
  return true;
}

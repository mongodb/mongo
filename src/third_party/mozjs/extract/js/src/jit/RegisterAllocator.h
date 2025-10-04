/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RegisterAllocator_h
#define jit_RegisterAllocator_h

#include "mozilla/MathAlgorithms.h"

#include "jit/LIR.h"
#include "jit/MIRGenerator.h"
#include "jit/MIRGraph.h"

// Generic structures and functions for use by register allocators.

namespace js {
namespace jit {

class LIRGenerator;

#ifdef DEBUG
// Structure for running a liveness analysis on a finished register allocation.
// This analysis can be used for two purposes:
//
// - Check the integrity of the allocation, i.e. that the reads and writes of
//   physical values preserve the semantics of the original virtual registers.
//
// - Populate safepoints with live registers, GC thing and value data, to
//   streamline the process of prototyping new allocators.
struct AllocationIntegrityState {
  explicit AllocationIntegrityState(LIRGraph& graph) : graph(graph) {}

  // Record all virtual registers in the graph. This must be called before
  // register allocation, to pick up the original LUses.
  [[nodiscard]] bool record();

  // Perform the liveness analysis on the graph, and assert on an invalid
  // allocation. This must be called after register allocation, to pick up
  // all assigned physical values.
  [[nodiscard]] bool check();

 private:
  LIRGraph& graph;

  // For all instructions and phis in the graph, keep track of the virtual
  // registers for all inputs and outputs of the nodes. These are overwritten
  // in place during register allocation. This information is kept on the
  // side rather than in the instructions and phis themselves to avoid
  // debug-builds-only bloat in the size of the involved structures.

  struct InstructionInfo {
    Vector<LAllocation, 2, SystemAllocPolicy> inputs;
    Vector<LDefinition, 0, SystemAllocPolicy> temps;
    Vector<LDefinition, 1, SystemAllocPolicy> outputs;

    InstructionInfo() = default;

    InstructionInfo(const InstructionInfo& o) {
      AutoEnterOOMUnsafeRegion oomUnsafe;
      if (!inputs.appendAll(o.inputs) || !temps.appendAll(o.temps) ||
          !outputs.appendAll(o.outputs)) {
        oomUnsafe.crash("InstructionInfo::InstructionInfo");
      }
    }
  };
  Vector<InstructionInfo, 0, SystemAllocPolicy> instructions;

  struct BlockInfo {
    Vector<InstructionInfo, 5, SystemAllocPolicy> phis;
    BlockInfo() = default;
    BlockInfo(const BlockInfo& o) {
      AutoEnterOOMUnsafeRegion oomUnsafe;
      if (!phis.appendAll(o.phis)) {
        oomUnsafe.crash("BlockInfo::BlockInfo");
      }
    }
  };
  Vector<BlockInfo, 0, SystemAllocPolicy> blocks;

  Vector<LDefinition*, 20, SystemAllocPolicy> virtualRegisters;

  // Describes a correspondence that should hold at the end of a block.
  // The value which was written to vreg in the original LIR should be
  // physically stored in alloc after the register allocation.
  struct IntegrityItem {
    LBlock* block;
    uint32_t vreg;
    LAllocation alloc;

    // Order of insertion into seen, for sorting.
    uint32_t index;

    using Lookup = IntegrityItem;
    static HashNumber hash(const IntegrityItem& item) {
      HashNumber hash = item.alloc.hash();
      hash = mozilla::RotateLeft(hash, 4) ^ item.vreg;
      hash = mozilla::RotateLeft(hash, 4) ^ HashNumber(item.block->mir()->id());
      return hash;
    }
    static bool match(const IntegrityItem& one, const IntegrityItem& two) {
      return one.block == two.block && one.vreg == two.vreg &&
             one.alloc == two.alloc;
    }
  };

  // Items still to be processed.
  Vector<IntegrityItem, 10, SystemAllocPolicy> worklist;

  // Set of all items that have already been processed.
  typedef HashSet<IntegrityItem, IntegrityItem, SystemAllocPolicy>
      IntegrityItemSet;
  IntegrityItemSet seen;

  [[nodiscard]] bool checkIntegrity(LBlock* block, LInstruction* ins,
                                    uint32_t vreg, LAllocation alloc);
  void checkSafepointAllocation(LInstruction* ins, uint32_t vreg,
                                LAllocation alloc);
  [[nodiscard]] bool addPredecessor(LBlock* block, uint32_t vreg,
                                    LAllocation alloc);

  void dump();
};
#endif  // DEBUG

// Represents with better-than-instruction precision a position in the
// instruction stream.
//
// An issue comes up when performing register allocation as to how to represent
// information such as "this register is only needed for the input of
// this instruction, it can be clobbered in the output". Just having ranges
// of instruction IDs is insufficiently expressive to denote all possibilities.
// This class solves this issue by associating an extra bit with the instruction
// ID which indicates whether the position is the input half or output half of
// an instruction.
class CodePosition {
 private:
  constexpr explicit CodePosition(uint32_t bits) : bits_(bits) {}

  static const unsigned int INSTRUCTION_SHIFT = 1;
  static const unsigned int SUBPOSITION_MASK = 1;
  uint32_t bits_;

 public:
  static const CodePosition MAX;
  static const CodePosition MIN;

  // This is the half of the instruction this code position represents, as
  // described in the huge comment above.
  enum SubPosition { INPUT, OUTPUT };

  constexpr CodePosition() : bits_(0) {}

  CodePosition(uint32_t instruction, SubPosition where) {
    MOZ_ASSERT(instruction < 0x80000000u);
    MOZ_ASSERT(((uint32_t)where & SUBPOSITION_MASK) == (uint32_t)where);
    bits_ = (instruction << INSTRUCTION_SHIFT) | (uint32_t)where;
  }

  uint32_t ins() const { return bits_ >> INSTRUCTION_SHIFT; }

  uint32_t bits() const { return bits_; }

  SubPosition subpos() const { return (SubPosition)(bits_ & SUBPOSITION_MASK); }

  bool operator<(CodePosition other) const { return bits_ < other.bits_; }

  bool operator<=(CodePosition other) const { return bits_ <= other.bits_; }

  bool operator!=(CodePosition other) const { return bits_ != other.bits_; }

  bool operator==(CodePosition other) const { return bits_ == other.bits_; }

  bool operator>(CodePosition other) const { return bits_ > other.bits_; }

  bool operator>=(CodePosition other) const { return bits_ >= other.bits_; }

  uint32_t operator-(CodePosition other) const {
    MOZ_ASSERT(bits_ >= other.bits_);
    return bits_ - other.bits_;
  }

  CodePosition previous() const {
    MOZ_ASSERT(*this != MIN);
    return CodePosition(bits_ - 1);
  }
  CodePosition next() const {
    MOZ_ASSERT(*this != MAX);
    return CodePosition(bits_ + 1);
  }
};

// Structure to track all moves inserted next to instructions in a graph.
class InstructionDataMap {
  FixedList<LNode*> insData_;

 public:
  InstructionDataMap() {}

  [[nodiscard]] bool init(MIRGenerator* gen, uint32_t numInstructions) {
    if (!insData_.init(gen->alloc(), numInstructions)) {
      return false;
    }
    memset(&insData_[0], 0, sizeof(LNode*) * numInstructions);
    return true;
  }

  LNode*& operator[](CodePosition pos) { return operator[](pos.ins()); }
  LNode* const& operator[](CodePosition pos) const {
    return operator[](pos.ins());
  }
  LNode*& operator[](uint32_t ins) { return insData_[ins]; }
  LNode* const& operator[](uint32_t ins) const { return insData_[ins]; }
};

// Common superclass for register allocators.
class RegisterAllocator {
  void operator=(const RegisterAllocator&) = delete;
  RegisterAllocator(const RegisterAllocator&) = delete;

 protected:
  // Context
  MIRGenerator* mir;
  LIRGenerator* lir;
  LIRGraph& graph;

  // Pool of all registers that should be considered allocateable
  AllocatableRegisterSet allRegisters_;

  // Computed data
  InstructionDataMap insData;
  Vector<CodePosition, 12, SystemAllocPolicy> entryPositions;
  Vector<CodePosition, 12, SystemAllocPolicy> exitPositions;

  RegisterAllocator(MIRGenerator* mir, LIRGenerator* lir, LIRGraph& graph)
      : mir(mir), lir(lir), graph(graph), allRegisters_(RegisterSet::All()) {
    MOZ_ASSERT(!allRegisters_.has(FramePointer));
    if (mir->compilingWasm()) {
      takeWasmRegisters(allRegisters_);
    }
  }

  [[nodiscard]] bool init();

  TempAllocator& alloc() const { return mir->alloc(); }

  CodePosition outputOf(const LNode* ins) const {
    return ins->isPhi() ? outputOf(ins->toPhi())
                        : outputOf(ins->toInstruction());
  }
  CodePosition outputOf(const LPhi* ins) const {
    // All phis in a block write their outputs after all of them have
    // read their inputs. Consequently, it doesn't make sense to talk
    // about code positions in the middle of a series of phis.
    LBlock* block = ins->block();
    return CodePosition(block->getPhi(block->numPhis() - 1)->id(),
                        CodePosition::OUTPUT);
  }
  CodePosition outputOf(const LInstruction* ins) const {
    return CodePosition(ins->id(), CodePosition::OUTPUT);
  }
  CodePosition inputOf(const LNode* ins) const {
    return ins->isPhi() ? inputOf(ins->toPhi()) : inputOf(ins->toInstruction());
  }
  CodePosition inputOf(const LPhi* ins) const {
    // All phis in a block read their inputs before any of them write their
    // outputs. Consequently, it doesn't make sense to talk about code
    // positions in the middle of a series of phis.
    return CodePosition(ins->block()->getPhi(0)->id(), CodePosition::INPUT);
  }
  CodePosition inputOf(const LInstruction* ins) const {
    return CodePosition(ins->id(), CodePosition::INPUT);
  }
  CodePosition entryOf(const LBlock* block) {
    return entryPositions[block->mir()->id()];
  }
  CodePosition exitOf(const LBlock* block) {
    return exitPositions[block->mir()->id()];
  }

  LMoveGroup* getInputMoveGroup(LInstruction* ins);
  LMoveGroup* getFixReuseMoveGroup(LInstruction* ins);
  LMoveGroup* getMoveGroupAfter(LInstruction* ins);

  // Atomic group helper.  See comments in BacktrackingAllocator.cpp.
  CodePosition minimalDefEnd(LNode* ins) const;

  void dumpInstructions(const char* who);

 public:
  template <typename TakeableSet>
  static void takeWasmRegisters(TakeableSet& regs) {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) ||        \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS32) ||   \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
    regs.take(HeapReg);
#endif
    MOZ_ASSERT(!regs.has(FramePointer));
  }
};

static inline AnyRegister GetFixedRegister(const LDefinition* def,
                                           const LUse* use) {
  return def->isFloatReg()
             ? AnyRegister(FloatRegister::FromCode(use->registerCode()))
             : AnyRegister(Register::FromCode(use->registerCode()));
}

}  // namespace jit
}  // namespace js

#endif /* jit_RegisterAllocator_h */

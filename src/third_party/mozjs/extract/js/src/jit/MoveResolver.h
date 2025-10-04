/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MoveResolver_h
#define jit_MoveResolver_h

#include <algorithm>

#include "jit/InlineList.h"
#include "jit/JitAllocPolicy.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/shared/Assembler-shared.h"

namespace js {
namespace jit {

class MacroAssembler;

// This is similar to Operand, but carries more information. We're also not
// guaranteed that Operand looks like this on all ISAs.
class MoveOperand {
 public:
  enum class Kind : uint8_t {
    // A register in the "integer", aka "general purpose", class.
    Reg,
#ifdef JS_CODEGEN_REGISTER_PAIR
    // Two consecutive "integer" registers (aka "general purpose"). The even
    // register contains the lower part, the odd register has the high bits
    // of the content.
    RegPair,
#endif
    // A register in the "float" register class.
    FloatReg,
    // A memory region.
    Memory,
    // The address of a memory region.
    EffectiveAddress
  };

 private:
  Kind kind_;
  uint8_t code_;
  int32_t disp_;

  static_assert(std::max(Registers::Total, FloatRegisters::Total) <= UINT8_MAX,
                "Any register code must fit in code_");

 public:
  MoveOperand() = delete;
  explicit MoveOperand(Register reg)
      : kind_(Kind::Reg), code_(reg.code()), disp_(0) {}
  explicit MoveOperand(FloatRegister reg)
      : kind_(Kind::FloatReg), code_(reg.code()), disp_(0) {}
  MoveOperand(Register reg, int32_t disp, Kind kind = Kind::Memory)
      : kind_(kind), code_(reg.code()), disp_(disp) {
    MOZ_ASSERT(isMemoryOrEffectiveAddress());

    // With a zero offset, this is a plain reg-to-reg move.
    if (disp == 0 && kind_ == Kind::EffectiveAddress) {
      kind_ = Kind::Reg;
    }
  }
  explicit MoveOperand(const Address& addr, Kind kind = Kind::Memory)
      : MoveOperand(AsRegister(addr.base), addr.offset, kind) {}
  MoveOperand(MacroAssembler& masm, const ABIArg& arg);
  MoveOperand(const MoveOperand& other) = default;
  bool isFloatReg() const { return kind_ == Kind::FloatReg; }
  bool isGeneralReg() const { return kind_ == Kind::Reg; }
  bool isGeneralRegPair() const {
#ifdef JS_CODEGEN_REGISTER_PAIR
    return kind_ == Kind::RegPair;
#else
    return false;
#endif
  }
  bool isMemory() const { return kind_ == Kind::Memory; }
  bool isEffectiveAddress() const { return kind_ == Kind::EffectiveAddress; }
  bool isMemoryOrEffectiveAddress() const {
    return isMemory() || isEffectiveAddress();
  }
  Register reg() const {
    MOZ_ASSERT(isGeneralReg());
    return Register::FromCode(code_);
  }
  Register evenReg() const {
    MOZ_ASSERT(isGeneralRegPair());
    return Register::FromCode(code_);
  }
  Register oddReg() const {
    MOZ_ASSERT(isGeneralRegPair());
    return Register::FromCode(code_ + 1);
  }
  FloatRegister floatReg() const {
    MOZ_ASSERT(isFloatReg());
    return FloatRegister::FromCode(code_);
  }
  Register base() const {
    MOZ_ASSERT(isMemoryOrEffectiveAddress());
    return Register::FromCode(code_);
  }
  int32_t disp() const {
    MOZ_ASSERT(isMemoryOrEffectiveAddress());
    return disp_;
  }

  bool aliases(MoveOperand other) const {
    // These are not handled presently, but Memory and EffectiveAddress
    // only appear in controlled circumstances in the trampoline code
    // which ensures these cases never come up.

    MOZ_ASSERT_IF(isMemoryOrEffectiveAddress() && other.isGeneralReg(),
                  base() != other.reg());
    MOZ_ASSERT_IF(other.isMemoryOrEffectiveAddress() && isGeneralReg(),
                  other.base() != reg());

    // Check if one of the operand is a registe rpair, in which case, we
    // have to check any other register, or register pair.
    if (isGeneralRegPair() || other.isGeneralRegPair()) {
      if (isGeneralRegPair() && other.isGeneralRegPair()) {
        // Assume that register pairs are aligned on even registers.
        MOZ_ASSERT(!evenReg().aliases(other.oddReg()));
        MOZ_ASSERT(!oddReg().aliases(other.evenReg()));
        // Pair of registers are composed of consecutive registers, thus
        // if the first registers are aliased, then the second registers
        // are aliased too.
        MOZ_ASSERT(evenReg().aliases(other.evenReg()) ==
                   oddReg().aliases(other.oddReg()));
        return evenReg().aliases(other.evenReg());
      } else if (other.isGeneralReg()) {
        MOZ_ASSERT(isGeneralRegPair());
        return evenReg().aliases(other.reg()) || oddReg().aliases(other.reg());
      } else if (isGeneralReg()) {
        MOZ_ASSERT(other.isGeneralRegPair());
        return other.evenReg().aliases(reg()) || other.oddReg().aliases(reg());
      }
      return false;
    }

    if (kind_ != other.kind_) {
      return false;
    }
    if (kind_ == Kind::FloatReg) {
      return floatReg().aliases(other.floatReg());
    }
    if (code_ != other.code_) {
      return false;
    }
    if (isMemoryOrEffectiveAddress()) {
      return disp_ == other.disp_;
    }
    return true;
  }

  bool operator==(const MoveOperand& other) const {
    if (kind_ != other.kind_) {
      return false;
    }
    if (code_ != other.code_) {
      return false;
    }
    if (isMemoryOrEffectiveAddress()) {
      return disp_ == other.disp_;
    }
    return true;
  }
  bool operator!=(const MoveOperand& other) const { return !operator==(other); }
};

// This represents a move operation.
class MoveOp {
 protected:
  MoveOperand from_;
  MoveOperand to_;
  int32_t cycleBeginSlot_ = -1;
  int32_t cycleEndSlot_ = -1;
  bool cycleBegin_ = false;
  bool cycleEnd_ = false;

 public:
  enum Type : uint8_t { GENERAL, INT32, FLOAT32, DOUBLE, SIMD128 };

 protected:
  Type type_;

  // If cycleBegin_ is true, endCycleType_ is the type of the move at the end
  // of the cycle. For example, given these moves:
  //       INT32 move a -> b
  //     GENERAL move b -> a
  // the move resolver starts by copying b into a temporary location, so that
  // the last move can read it. This copy needs to use use type GENERAL.
  Type endCycleType_;

 public:
  MoveOp() = delete;
  MoveOp(const MoveOperand& from, const MoveOperand& to, Type type)
      : from_(from),
        to_(to),
        type_(type),
        endCycleType_(GENERAL)  // initialize to silence UBSan warning
  {}

  bool isCycleBegin() const { return cycleBegin_; }
  bool isCycleEnd() const { return cycleEnd_; }
  uint32_t cycleBeginSlot() const {
    MOZ_ASSERT(cycleBeginSlot_ != -1);
    return cycleBeginSlot_;
  }
  uint32_t cycleEndSlot() const {
    MOZ_ASSERT(cycleEndSlot_ != -1);
    return cycleEndSlot_;
  }
  const MoveOperand& from() const { return from_; }
  const MoveOperand& to() const { return to_; }
  Type type() const { return type_; }
  Type endCycleType() const {
    MOZ_ASSERT(isCycleBegin());
    return endCycleType_;
  }
  bool aliases(const MoveOperand& op) const {
    return from().aliases(op) || to().aliases(op);
  }
  bool aliases(const MoveOp& other) const {
    return aliases(other.from()) || aliases(other.to());
  }
#ifdef JS_CODEGEN_ARM
  void overwrite(MoveOperand& from, MoveOperand& to, Type type) {
    from_ = from;
    to_ = to;
    type_ = type;
  }
#endif
};

class MoveResolver {
 private:
  struct PendingMove : public MoveOp,
                       public TempObject,
                       public InlineListNode<PendingMove> {
    PendingMove() = delete;

    PendingMove(const MoveOperand& from, const MoveOperand& to, Type type)
        : MoveOp(from, to, type) {}

    void setCycleBegin(Type endCycleType, int cycleSlot) {
      MOZ_ASSERT(!cycleBegin_);
      cycleBegin_ = true;
      cycleBeginSlot_ = cycleSlot;
      endCycleType_ = endCycleType;
    }
    void setCycleEnd(int cycleSlot) {
      MOZ_ASSERT(!cycleEnd_);
      cycleEnd_ = true;
      cycleEndSlot_ = cycleSlot;
    }
  };

  using PendingMoveIterator = InlineList<MoveResolver::PendingMove>::iterator;

  js::Vector<MoveOp, 16, SystemAllocPolicy> orderedMoves_;
  int numCycles_;
  int curCycles_;
  TempObjectPool<PendingMove> movePool_;

  InlineList<PendingMove> pending_;

  PendingMove* findBlockingMove(const PendingMove* last);
  PendingMove* findCycledMove(PendingMoveIterator* stack,
                              PendingMoveIterator end,
                              const PendingMove* first);
  [[nodiscard]] bool addOrderedMove(const MoveOp& move);
  void reorderMove(size_t from, size_t to);

  // Internal reset function. Does not clear lists.
  void resetState();

#ifdef JS_CODEGEN_ARM
  bool isDoubleAliasedAsSingle(const MoveOperand& move);
#endif

 public:
  MoveResolver();

  // Resolves a move group into two lists of ordered moves. These moves must
  // be executed in the order provided. Some moves may indicate that they
  // participate in a cycle. For every cycle there are two such moves, and it
  // is guaranteed that cycles do not nest inside each other in the list.
  //
  // After calling addMove() for each parallel move, resolve() performs the
  // cycle resolution algorithm. Calling addMove() again resets the resolver.
  [[nodiscard]] bool addMove(const MoveOperand& from, const MoveOperand& to,
                             MoveOp::Type type);
  [[nodiscard]] bool resolve();
  void sortMemoryToMemoryMoves();

  size_t numMoves() const { return orderedMoves_.length(); }
  const MoveOp& getMove(size_t i) const { return orderedMoves_[i]; }
  uint32_t numCycles() const { return numCycles_; }
  bool hasNoPendingMoves() const { return pending_.empty(); }
  void setAllocator(TempAllocator& alloc) { movePool_.setAllocator(alloc); }
};

}  // namespace jit
}  // namespace js

#endif /* jit_MoveResolver_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MoveResolver_h
#define jit_MoveResolver_h

#include "jit/InlineList.h"
#include "jit/JitAllocPolicy.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

class MacroAssembler;

// This is similar to Operand, but carries more information. We're also not
// guaranteed that Operand looks like this on all ISAs.
class MoveOperand
{
  public:
    enum Kind {
        // A register in the "integer", aka "general purpose", class.
        REG,
#ifdef JS_CODEGEN_REGISTER_PAIR
        // Two consecutive "integer" register (aka "general purpose"). The even
        // register contains the lower part, the odd register has the high bits
        // of the content.
        REG_PAIR,
#endif
        // A register in the "float" register class.
        FLOAT_REG,
        // A memory region.
        MEMORY,
        // The address of a memory region.
        EFFECTIVE_ADDRESS
    };

  private:
    Kind kind_;
    uint32_t code_;
    int32_t disp_;

  public:
    MoveOperand()
    { }
    explicit MoveOperand(Register reg) : kind_(REG), code_(reg.code())
    { }
    explicit MoveOperand(FloatRegister reg) : kind_(FLOAT_REG), code_(reg.code())
    { }
    MoveOperand(Register reg, int32_t disp, Kind kind = MEMORY)
        : kind_(kind),
        code_(reg.code()),
        disp_(disp)
    {
        MOZ_ASSERT(isMemoryOrEffectiveAddress());

        // With a zero offset, this is a plain reg-to-reg move.
        if (disp == 0 && kind_ == EFFECTIVE_ADDRESS)
            kind_ = REG;
    }
    MoveOperand(MacroAssembler& masm, const ABIArg& arg);
    MoveOperand(const MoveOperand& other)
      : kind_(other.kind_),
        code_(other.code_),
        disp_(other.disp_)
    { }
    bool isFloatReg() const {
        return kind_ == FLOAT_REG;
    }
    bool isGeneralReg() const {
        return kind_ == REG;
    }
    bool isGeneralRegPair() const {
#ifdef JS_CODEGEN_REGISTER_PAIR
        return kind_ == REG_PAIR;
#else
        return false;
#endif
    }
    bool isMemory() const {
        return kind_ == MEMORY;
    }
    bool isEffectiveAddress() const {
        return kind_ == EFFECTIVE_ADDRESS;
    }
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

        // These are not handled presently, but MEMORY and EFFECTIVE_ADDRESS
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
                MOZ_ASSERT(evenReg().aliases(other.evenReg()) == oddReg().aliases(other.oddReg()));
                return evenReg().aliases(other.evenReg());
            } else if (other.isGeneralReg()) {
                MOZ_ASSERT(isGeneralRegPair());
                return evenReg().aliases(other.reg()) ||
                       oddReg().aliases(other.reg());
            } else if (isGeneralReg()) {
                MOZ_ASSERT(other.isGeneralRegPair());
                return other.evenReg().aliases(reg()) ||
                       other.oddReg().aliases(reg());
            }
            return false;
        }

        if (kind_ != other.kind_)
            return false;
        if (kind_ == FLOAT_REG)
            return floatReg().aliases(other.floatReg());
        if (code_ != other.code_)
            return false;
        if (isMemoryOrEffectiveAddress())
            return disp_ == other.disp_;
        return true;
    }

    bool operator ==(const MoveOperand& other) const {
        if (kind_ != other.kind_)
            return false;
        if (code_ != other.code_)
            return false;
        if (isMemoryOrEffectiveAddress())
            return disp_ == other.disp_;
        return true;
    }
    bool operator !=(const MoveOperand& other) const {
        return !operator==(other);
    }
};

// This represents a move operation.
class MoveOp
{
  protected:
    MoveOperand from_;
    MoveOperand to_;
    bool cycleBegin_;
    bool cycleEnd_;
    int cycleBeginSlot_;
    int cycleEndSlot_;
  public:
    enum Type {
        GENERAL,
        INT32,
        FLOAT32,
        DOUBLE,
        INT32X4,
        FLOAT32X4
    };

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
    MoveOp()
    { }
    MoveOp(const MoveOperand& from, const MoveOperand& to, Type type)
      : from_(from),
        to_(to),
        cycleBegin_(false),
        cycleEnd_(false),
        cycleBeginSlot_(-1),
        cycleEndSlot_(-1),
        type_(type)
    { }

    bool isCycleBegin() const {
        return cycleBegin_;
    }
    bool isCycleEnd() const {
        return cycleEnd_;
    }
    uint32_t cycleBeginSlot() const {
        MOZ_ASSERT(cycleBeginSlot_ != -1);
        return cycleBeginSlot_;
    }
    uint32_t cycleEndSlot() const {
        MOZ_ASSERT(cycleEndSlot_ != -1);
        return cycleEndSlot_;
    }
    const MoveOperand& from() const {
        return from_;
    }
    const MoveOperand& to() const {
        return to_;
    }
    Type type() const {
        return type_;
    }
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
};

class MoveResolver
{
  private:
    struct PendingMove
      : public MoveOp,
        public TempObject,
        public InlineListNode<PendingMove>
    {
        PendingMove()
        { }
        PendingMove(const MoveOperand& from, const MoveOperand& to, Type type)
          : MoveOp(from, to, type)
        { }

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

    typedef InlineList<MoveResolver::PendingMove>::iterator PendingMoveIterator;

  private:
    js::Vector<MoveOp, 16, SystemAllocPolicy> orderedMoves_;
    int numCycles_;
    int curCycles_;
    TempObjectPool<PendingMove> movePool_;

    InlineList<PendingMove> pending_;

    PendingMove* findBlockingMove(const PendingMove* last);
    PendingMove* findCycledMove(PendingMoveIterator* stack, PendingMoveIterator end, const PendingMove* first);
    bool addOrderedMove(const MoveOp& move);
    void reorderMove(size_t from, size_t to);

    // Internal reset function. Does not clear lists.
    void resetState();

  public:
    MoveResolver();

    // Resolves a move group into two lists of ordered moves. These moves must
    // be executed in the order provided. Some moves may indicate that they
    // participate in a cycle. For every cycle there are two such moves, and it
    // is guaranteed that cycles do not nest inside each other in the list.
    //
    // After calling addMove() for each parallel move, resolve() performs the
    // cycle resolution algorithm. Calling addMove() again resets the resolver.
    bool addMove(const MoveOperand& from, const MoveOperand& to, MoveOp::Type type);
    bool resolve();
    void sortMemoryToMemoryMoves();

    size_t numMoves() const {
        return orderedMoves_.length();
    }
    const MoveOp& getMove(size_t i) const {
        return orderedMoves_[i];
    }
    uint32_t numCycles() const {
        return numCycles_;
    }
    void setAllocator(TempAllocator& alloc) {
        movePool_.setAllocator(alloc);
    }
};

} // namespace jit
} // namespace js

#endif /* jit_MoveResolver_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Recover_h
#define jit_Recover_h

#include "mozilla/Attributes.h"

#include "builtin/Array.h"

#include "jit/MIR.h"
#include "jit/Snapshots.h"

namespace js {
namespace jit {

// [SMDOC] IonMonkey Recover Instructions
//
// This file contains all recover instructions.
//
// A recover instruction is an equivalent of a MIR instruction which is executed
// before the reconstruction of a baseline frame. Recover instructions are used
// by resume points to fill the value which are not produced by the code
// compiled by IonMonkey. For example, if a value is optimized away by
// IonMonkey, but required by Baseline, then we should have a recover
// instruction to fill the missing baseline frame slot.
//
// Recover instructions are executed either during a bailout, or under a call
// when the stack frame is introspected. If the stack is introspected, then any
// use of recover instruction must lead to an invalidation of the code.
//
// For each MIR instruction where |canRecoverOnBailout| might return true, we
// have a RInstruction of the same name.
//
// Recover instructions are encoded by the code generator into a compact buffer
// (RecoverWriter). The MIR instruction method |writeRecoverData| should write a
// tag in the |CompactBufferWriter| which is used by
// |RInstruction::readRecoverData| to dispatch to the right Recover
// instruction. Then |writeRecoverData| writes any local fields which are
// necessary for the execution of the |recover| method. These fields are decoded
// by the Recover instruction constructor which has a |CompactBufferReader| as
// argument. The constructor of the Recover instruction should follow the same
// sequence as the |writeRecoverData| method of the MIR instruction.
//
// Recover instructions are decoded by the |SnapshotIterator| (RecoverReader),
// which is given as argument of the |recover| methods, in order to read the
// operands.  The number of operands read should be the same as the result of
// |numOperands|, which corresponds to the number of operands of the MIR
// instruction.  Operands should be decoded in the same order as the operands of
// the MIR instruction.
//
// The result of the |recover| method should either be a failure, or a value
// stored on the |SnapshotIterator|, by using the |storeInstructionResult|
// method.

#define RECOVER_OPCODE_LIST(_)    \
  _(ResumePoint)                  \
  _(BitNot)                       \
  _(BitAnd)                       \
  _(BitOr)                        \
  _(BitXor)                       \
  _(Lsh)                          \
  _(Rsh)                          \
  _(Ursh)                         \
  _(SignExtendInt32)              \
  _(Add)                          \
  _(Sub)                          \
  _(Mul)                          \
  _(Div)                          \
  _(Mod)                          \
  _(Not)                          \
  _(BigIntAdd)                    \
  _(BigIntSub)                    \
  _(BigIntMul)                    \
  _(BigIntDiv)                    \
  _(BigIntMod)                    \
  _(BigIntPow)                    \
  _(BigIntBitAnd)                 \
  _(BigIntBitOr)                  \
  _(BigIntBitXor)                 \
  _(BigIntLsh)                    \
  _(BigIntRsh)                    \
  _(BigIntIncrement)              \
  _(BigIntDecrement)              \
  _(BigIntNegate)                 \
  _(BigIntBitNot)                 \
  _(Concat)                       \
  _(StringLength)                 \
  _(ArgumentsLength)              \
  _(Floor)                        \
  _(Ceil)                         \
  _(Round)                        \
  _(Trunc)                        \
  _(CharCodeAt)                   \
  _(FromCharCode)                 \
  _(Pow)                          \
  _(PowHalf)                      \
  _(MinMax)                       \
  _(Abs)                          \
  _(Sqrt)                         \
  _(Atan2)                        \
  _(Hypot)                        \
  _(NearbyInt)                    \
  _(Sign)                         \
  _(MathFunction)                 \
  _(Random)                       \
  _(StringSplit)                  \
  _(NaNToZero)                    \
  _(RegExpMatcher)                \
  _(RegExpSearcher)               \
  _(RegExpTester)                 \
  _(StringReplace)                \
  _(TypeOf)                       \
  _(ToDouble)                     \
  _(ToFloat32)                    \
  _(TruncateToInt32)              \
  _(NewObject)                    \
  _(NewPlainObject)               \
  _(NewArrayObject)               \
  _(NewTypedArray)                \
  _(NewArray)                     \
  _(NewIterator)                  \
  _(NewCallObject)                \
  _(CreateThisWithTemplate)       \
  _(Lambda)                       \
  _(LambdaArrow)                  \
  _(FunctionWithProto)            \
  _(ObjectState)                  \
  _(ArrayState)                   \
  _(SetArrayLength)               \
  _(AtomicIsLockFree)             \
  _(BigIntAsIntN)                 \
  _(BigIntAsUintN)                \
  _(CreateArgumentsObject)        \
  _(CreateInlinedArgumentsObject) \
  _(AssertRecoveredOnBailout)

class RResumePoint;
class SnapshotIterator;

class MOZ_NON_PARAM RInstruction {
 public:
  enum Opcode {
#define DEFINE_OPCODES_(op) Recover_##op,
    RECOVER_OPCODE_LIST(DEFINE_OPCODES_)
#undef DEFINE_OPCODES_
        Recover_Invalid
  };

  virtual Opcode opcode() const = 0;

  // As opposed to the MIR, there is no need to add more methods as every
  // other instruction is well abstracted under the "recover" method.
  bool isResumePoint() const { return opcode() == Recover_ResumePoint; }
  inline const RResumePoint* toResumePoint() const;

  // Call the copy constructor of a specific RInstruction, to do a copy of the
  // RInstruction content.
  virtual void cloneInto(RInstructionStorage* raw) const = 0;

  // Number of allocations which are encoded in the Snapshot for recovering
  // the current instruction.
  virtual uint32_t numOperands() const = 0;

  // Function used to recover the value computed by this instruction. This
  // function reads its arguments from the allocations listed on the snapshot
  // iterator and stores its returned value on the snapshot iterator too.
  [[nodiscard]] virtual bool recover(JSContext* cx,
                                     SnapshotIterator& iter) const = 0;

  // Decode an RInstruction on top of the reserved storage space, based on the
  // tag written by the writeRecoverData function of the corresponding MIR
  // instruction.
  static void readRecoverData(CompactBufferReader& reader,
                              RInstructionStorage* raw);
};

#define RINSTRUCTION_HEADER_(op)                                        \
 private:                                                               \
  friend class RInstruction;                                            \
  explicit R##op(CompactBufferReader& reader);                          \
  explicit R##op(const R##op& src) = default;                           \
                                                                        \
 public:                                                                \
  Opcode opcode() const override { return RInstruction::Recover_##op; } \
  void cloneInto(RInstructionStorage* raw) const override {             \
    new (raw->addr()) R##op(*this);                                     \
  }

#define RINSTRUCTION_HEADER_NUM_OP_MAIN(op, numOp) \
  RINSTRUCTION_HEADER_(op)                         \
  uint32_t numOperands() const override { return numOp; }

#ifdef DEBUG
#  define RINSTRUCTION_HEADER_NUM_OP_(op, numOp)                      \
    RINSTRUCTION_HEADER_NUM_OP_MAIN(op, numOp)                        \
    static_assert(                                                    \
        M##op::staticNumOperands == numOp,                            \
        "The recover instructions's numOperands should equal to the " \
        "MIR's numOperands");
#else
#  define RINSTRUCTION_HEADER_NUM_OP_(op, numOp) \
    RINSTRUCTION_HEADER_NUM_OP_MAIN(op, numOp)
#endif

class RResumePoint final : public RInstruction {
 private:
  uint32_t pcOffset_;     // Offset from script->code.
  uint32_t numOperands_;  // Number of slots.

 public:
  RINSTRUCTION_HEADER_(ResumePoint)

  uint32_t pcOffset() const { return pcOffset_; }
  uint32_t numOperands() const override { return numOperands_; }
  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBitNot final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BitNot, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBitAnd final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BitAnd, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBitOr final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BitOr, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBitXor final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BitXor, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RLsh final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Lsh, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RRsh final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Rsh, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RUrsh final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Ursh, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RSignExtendInt32 final : public RInstruction {
 private:
  uint8_t mode_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(SignExtendInt32, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RAdd final : public RInstruction {
 private:
  bool isFloatOperation_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(Add, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RSub final : public RInstruction {
 private:
  bool isFloatOperation_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(Sub, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RMul final : public RInstruction {
 private:
  bool isFloatOperation_;
  uint8_t mode_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(Mul, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RDiv final : public RInstruction {
 private:
  bool isFloatOperation_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(Div, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RMod final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Mod, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNot final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Not, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntAdd final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntAdd, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntSub final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntSub, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntMul final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntMul, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntDiv final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntDiv, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntMod final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntMod, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntPow final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntPow, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntBitAnd final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntBitAnd, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntBitOr final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntBitOr, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntBitXor final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntBitXor, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntLsh final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntLsh, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntRsh final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntRsh, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntIncrement final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntIncrement, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntDecrement final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntDecrement, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntNegate final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntNegate, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntBitNot final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntBitNot, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RConcat final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Concat, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RStringLength final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(StringLength, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RArgumentsLength final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(ArgumentsLength, 0)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RFloor final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Floor, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RCeil final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Ceil, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RRound final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Round, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RTrunc final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Trunc, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RCharCodeAt final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(CharCodeAt, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RFromCharCode final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(FromCharCode, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RPow final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Pow, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RPowHalf final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(PowHalf, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RMinMax final : public RInstruction {
 private:
  bool isMax_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(MinMax, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RAbs final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Abs, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RSqrt final : public RInstruction {
 private:
  bool isFloatOperation_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(Sqrt, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RAtan2 final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Atan2, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RHypot final : public RInstruction {
 private:
  uint32_t numOperands_;

 public:
  RINSTRUCTION_HEADER_(Hypot)

  uint32_t numOperands() const override { return numOperands_; }

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNearbyInt final : public RInstruction {
 private:
  uint8_t roundingMode_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(NearbyInt, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RSign final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Sign, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RMathFunction final : public RInstruction {
 private:
  UnaryMathFunction function_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(MathFunction, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RRandom final : public RInstruction {
  RINSTRUCTION_HEADER_NUM_OP_(Random, 0)
 public:
  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RStringSplit final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(StringSplit, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNaNToZero final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(NaNToZero, 1);

  bool recover(JSContext* cx, SnapshotIterator& iter) const override;
};

class RRegExpMatcher final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(RegExpMatcher, 3)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RRegExpSearcher final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(RegExpSearcher, 3)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RRegExpTester final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(RegExpTester, 3)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RStringReplace final : public RInstruction {
 private:
  bool isFlatReplacement_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(StringReplace, 3)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RTypeOf final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(TypeOf, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RToDouble final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(ToDouble, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RToFloat32 final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(ToFloat32, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RTruncateToInt32 final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(TruncateToInt32, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNewObject final : public RInstruction {
 private:
  MNewObject::Mode mode_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(NewObject, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNewPlainObject final : public RInstruction {
 private:
  gc::AllocKind allocKind_;
  gc::InitialHeap initialHeap_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(NewPlainObject, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNewArrayObject final : public RInstruction {
 private:
  uint32_t length_;
  gc::InitialHeap initialHeap_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(NewArrayObject, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNewTypedArray final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(NewTypedArray, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNewArray final : public RInstruction {
 private:
  uint32_t count_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(NewArray, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNewIterator final : public RInstruction {
 private:
  uint8_t type_;

 public:
  RINSTRUCTION_HEADER_NUM_OP_(NewIterator, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RCreateThisWithTemplate final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(CreateThisWithTemplate, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RLambda final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(Lambda, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RLambdaArrow final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(LambdaArrow, 3)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RFunctionWithProto final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(FunctionWithProto, 3)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RNewCallObject final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(NewCallObject, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RObjectState final : public RInstruction {
 private:
  uint32_t numSlots_;  // Number of slots.

 public:
  RINSTRUCTION_HEADER_(ObjectState)

  uint32_t numSlots() const { return numSlots_; }
  uint32_t numOperands() const override {
    // +1 for the object.
    return numSlots() + 1;
  }

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RArrayState final : public RInstruction {
 private:
  uint32_t numElements_;

 public:
  RINSTRUCTION_HEADER_(ArrayState)

  uint32_t numElements() const { return numElements_; }
  uint32_t numOperands() const override {
    // +1 for the array.
    // +1 for the initalized length.
    return numElements() + 2;
  }

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RSetArrayLength final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(SetArrayLength, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RAtomicIsLockFree final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(AtomicIsLockFree, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntAsIntN final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntAsIntN, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RBigIntAsUintN final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(BigIntAsUintN, 2)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RCreateArgumentsObject final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(CreateArgumentsObject, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RCreateInlinedArgumentsObject final : public RInstruction {
 private:
  uint32_t numActuals_;

 public:
  RINSTRUCTION_HEADER_(CreateInlinedArgumentsObject)

  uint32_t numActuals() const { return numActuals_; }
  uint32_t numOperands() const override {
    // +1 for the callObj.
    // +1 for the callee.
    return numActuals() + 2;
  }

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

class RAssertRecoveredOnBailout final : public RInstruction {
 public:
  RINSTRUCTION_HEADER_NUM_OP_(AssertRecoveredOnBailout, 1)

  [[nodiscard]] bool recover(JSContext* cx,
                             SnapshotIterator& iter) const override;
};

#undef RINSTRUCTION_HEADER_
#undef RINSTRUCTION_HEADER_NUM_OP_
#undef RINSTRUCTION_HEADER_NUM_OP_MAIN

const RResumePoint* RInstruction::toResumePoint() const {
  MOZ_ASSERT(isResumePoint());
  return static_cast<const RResumePoint*>(this);
}

}  // namespace jit
}  // namespace js

#endif /* jit_Recover_h */

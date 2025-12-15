/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_LIR_shared_h
#define jit_shared_LIR_shared_h

#include "mozilla/Maybe.h"
#include "jit/AtomicOp.h"
#include "jit/shared/Assembler-shared.h"
#include "util/Memory.h"

// This file declares LIR instructions that are common to every platform.

namespace js {
namespace jit {

LIR_OPCODE_CLASS_GENERATED

template <size_t Temps, size_t ExtraUses = 0>
class LBinaryMath : public LInstructionHelper<1, 2 + ExtraUses, Temps> {
 protected:
  explicit LBinaryMath(LNode::Opcode opcode)
      : LInstructionHelper<1, 2 + ExtraUses, Temps>(opcode) {}

 public:
  const LAllocation* lhs() { return this->getOperand(0); }
  const LAllocation* rhs() { return this->getOperand(1); }
};

// An LOsiPoint captures a snapshot after a call and ensures enough space to
// patch in a call to the invalidation mechanism.
//
// Note: LSafepoints are 1:1 with LOsiPoints, so it holds a reference to the
// corresponding LSafepoint to inform it of the LOsiPoint's masm offset when it
// gets GC'd.
class LOsiPoint : public LInstructionHelper<0, 0, 0> {
  LSafepoint* safepoint_;

 public:
  LOsiPoint(LSafepoint* safepoint, LSnapshot* snapshot)
      : LInstructionHelper(classOpcode), safepoint_(safepoint) {
    MOZ_ASSERT(safepoint && snapshot);
    assignSnapshot(snapshot);
  }

  LSafepoint* associatedSafepoint() { return safepoint_; }

  LIR_HEADER(OsiPoint)
};

class LMove {
  LAllocation from_;
  LAllocation to_;
  LDefinition::Type type_;

 public:
  LMove(LAllocation from, LAllocation to, LDefinition::Type type)
      : from_(from), to_(to), type_(type) {}

  LAllocation from() const { return from_; }
  LAllocation to() const { return to_; }
  LDefinition::Type type() const { return type_; }
};

class LMoveGroup : public LInstructionHelper<0, 0, 0> {
  js::Vector<LMove, 2, JitAllocPolicy> moves_;

#ifdef JS_CODEGEN_X86
  // Optional general register available for use when executing moves.
  LAllocation scratchRegister_;
#endif

  explicit LMoveGroup(TempAllocator& alloc)
      : LInstructionHelper(classOpcode), moves_(alloc) {}

 public:
  LIR_HEADER(MoveGroup)

  static LMoveGroup* New(TempAllocator& alloc) {
    return new (alloc) LMoveGroup(alloc);
  }

  void printOperands(GenericPrinter& out);

  // Add a move which takes place simultaneously with all others in the group.
  bool add(LAllocation from, LAllocation to, LDefinition::Type type);

  // Add a move which takes place after existing moves in the group.
  bool addAfter(LAllocation from, LAllocation to, LDefinition::Type type);

  size_t numMoves() const { return moves_.length(); }
  const LMove& getMove(size_t i) const { return moves_[i]; }

#ifdef JS_CODEGEN_X86
  void setScratchRegister(Register reg) { scratchRegister_ = LGeneralReg(reg); }
  LAllocation maybeScratchRegister() { return scratchRegister_; }
#endif

  bool uses(Register reg) {
    for (size_t i = 0; i < numMoves(); i++) {
      LMove move = getMove(i);
      if (move.from() == LGeneralReg(reg) || move.to() == LGeneralReg(reg)) {
        return true;
      }
    }
    return false;
  }
};

// A constant Value.
class LValue : public LInstructionHelper<BOX_PIECES, 0, 0> {
  Value v_;

 public:
  LIR_HEADER(Value)

  explicit LValue(const Value& v) : LInstructionHelper(classOpcode), v_(v) {}

  Value value() const { return v_; }
};

// Allocate a new arguments object for an inlined frame.
class LCreateInlinedArgumentsObject : public LVariadicInstruction<1, 2> {
 public:
  LIR_HEADER(CreateInlinedArgumentsObject)

  static const size_t CallObj = 0;
  static const size_t Callee = 1;
  static const size_t NumNonArgumentOperands = 2;
  static size_t ArgIndex(size_t i) {
    return NumNonArgumentOperands + BOX_PIECES * i;
  }

  LCreateInlinedArgumentsObject(uint32_t numOperands, const LDefinition& temp1,
                                const LDefinition& temp2)
      : LVariadicInstruction(classOpcode, numOperands) {
    setIsCall();
    setTemp(0, temp1);
    setTemp(1, temp2);
  }

  const LAllocation* getCallObject() { return getOperand(CallObj); }
  const LAllocation* getCallee() { return getOperand(Callee); }

  const LDefinition* temp1() { return getTemp(0); }
  const LDefinition* temp2() { return getTemp(1); }

  MCreateInlinedArgumentsObject* mir() const {
    return mir_->toCreateInlinedArgumentsObject();
  }
};

class LGetInlinedArgument : public LVariadicInstruction<BOX_PIECES, 0> {
 public:
  LIR_HEADER(GetInlinedArgument)

  static const size_t Index = 0;
  static const size_t NumNonArgumentOperands = 1;
  static size_t ArgIndex(size_t i) {
    return NumNonArgumentOperands + BOX_PIECES * i;
  }

  explicit LGetInlinedArgument(uint32_t numOperands)
      : LVariadicInstruction(classOpcode, numOperands) {}

  const LAllocation* getIndex() { return getOperand(Index); }

  MGetInlinedArgument* mir() const { return mir_->toGetInlinedArgument(); }
};

class LGetInlinedArgumentHole : public LVariadicInstruction<BOX_PIECES, 0> {
 public:
  LIR_HEADER(GetInlinedArgumentHole)

  static const size_t Index = 0;
  static const size_t NumNonArgumentOperands = 1;
  static size_t ArgIndex(size_t i) {
    return NumNonArgumentOperands + BOX_PIECES * i;
  }

  explicit LGetInlinedArgumentHole(uint32_t numOperands)
      : LVariadicInstruction(classOpcode, numOperands) {}

  const LAllocation* getIndex() { return getOperand(Index); }

  MGetInlinedArgumentHole* mir() const {
    return mir_->toGetInlinedArgumentHole();
  }
};

class LInlineArgumentsSlice : public LVariadicInstruction<1, 1> {
 public:
  LIR_HEADER(InlineArgumentsSlice)

  static const size_t Begin = 0;
  static const size_t Count = 1;
  static const size_t NumNonArgumentOperands = 2;
  static size_t ArgIndex(size_t i) {
    return NumNonArgumentOperands + BOX_PIECES * i;
  }

  explicit LInlineArgumentsSlice(uint32_t numOperands, const LDefinition& temp)
      : LVariadicInstruction(classOpcode, numOperands) {
    setTemp(0, temp);
  }

  const LAllocation* begin() { return getOperand(Begin); }
  const LAllocation* count() { return getOperand(Count); }
  const LDefinition* temp() { return getTemp(0); }

  MInlineArgumentsSlice* mir() const { return mir_->toInlineArgumentsSlice(); }
};

// Common code for LIR descended from MCall.
template <size_t Defs, size_t Operands, size_t Temps>
class LJSCallInstructionHelper
    : public LCallInstructionHelper<Defs, Operands, Temps> {
 protected:
  explicit LJSCallInstructionHelper(LNode::Opcode opcode)
      : LCallInstructionHelper<Defs, Operands, Temps>(opcode) {}

 public:
  MCall* mir() const { return this->mir_->toCall(); }

  bool hasSingleTarget() const { return getSingleTarget() != nullptr; }
  WrappedFunction* getSingleTarget() const { return mir()->getSingleTarget(); }

  // Does not include |this|.
  uint32_t numActualArgs() const { return mir()->numActualArgs(); }

  bool isConstructing() const { return mir()->isConstructing(); }
  bool ignoresReturnValue() const { return mir()->ignoresReturnValue(); }
};

// Generates a polymorphic callsite, wherein the function being called is
// unknown and anticipated to vary.
class LCallGeneric : public LJSCallInstructionHelper<BOX_PIECES, 1, 1> {
 public:
  LIR_HEADER(CallGeneric)

  LCallGeneric(const LAllocation& callee, const LDefinition& argc)
      : LJSCallInstructionHelper(classOpcode) {
    setOperand(0, callee);
    setTemp(0, argc);
  }

  const LAllocation* getCallee() { return getOperand(0); }
  const LDefinition* getArgc() { return getTemp(0); }
};

// Generates a hardcoded callsite for a known, non-native target.
class LCallKnown : public LJSCallInstructionHelper<BOX_PIECES, 1, 1> {
 public:
  LIR_HEADER(CallKnown)

  LCallKnown(const LAllocation& func, const LDefinition& tmpobjreg)
      : LJSCallInstructionHelper(classOpcode) {
    setOperand(0, func);
    setTemp(0, tmpobjreg);
  }

  const LAllocation* getFunction() { return getOperand(0); }
  const LDefinition* getTempObject() { return getTemp(0); }
};

// Generates a hardcoded callsite for a known, native target.
class LCallNative : public LJSCallInstructionHelper<BOX_PIECES, 0, 4> {
 public:
  LIR_HEADER(CallNative)

  LCallNative(const LDefinition& argContext, const LDefinition& argUintN,
              const LDefinition& argVp, const LDefinition& tmpreg)
      : LJSCallInstructionHelper(classOpcode) {
    // Registers used for callWithABI().
    setTemp(0, argContext);
    setTemp(1, argUintN);
    setTemp(2, argVp);

    // Temporary registers.
    setTemp(3, tmpreg);
  }

  const LDefinition* getArgContextReg() { return getTemp(0); }
  const LDefinition* getArgUintNReg() { return getTemp(1); }
  const LDefinition* getArgVpReg() { return getTemp(2); }
  const LDefinition* getTempReg() { return getTemp(3); }
};

class LCallClassHook : public LCallInstructionHelper<BOX_PIECES, 1, 4> {
 public:
  LIR_HEADER(CallClassHook)

  LCallClassHook(const LAllocation& callee, const LDefinition& argContext,
                 const LDefinition& argUintN, const LDefinition& argVp,
                 const LDefinition& tmpreg)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, callee);

    // Registers used for callWithABI().
    setTemp(0, argContext);
    setTemp(1, argUintN);
    setTemp(2, argVp);

    // Temporary registers.
    setTemp(3, tmpreg);
  }

  MCallClassHook* mir() const { return mir_->toCallClassHook(); }

  const LAllocation* getCallee() { return this->getOperand(0); }

  const LDefinition* getArgContextReg() { return getTemp(0); }
  const LDefinition* getArgUintNReg() { return getTemp(1); }
  const LDefinition* getArgVpReg() { return getTemp(2); }
  const LDefinition* getTempReg() { return getTemp(3); }
};

// Generates a hardcoded callsite for a known, DOM-native target.
class LCallDOMNative : public LJSCallInstructionHelper<BOX_PIECES, 0, 4> {
 public:
  LIR_HEADER(CallDOMNative)

  LCallDOMNative(const LDefinition& argJSContext, const LDefinition& argObj,
                 const LDefinition& argPrivate, const LDefinition& argArgs)
      : LJSCallInstructionHelper(classOpcode) {
    setTemp(0, argJSContext);
    setTemp(1, argObj);
    setTemp(2, argPrivate);
    setTemp(3, argArgs);
  }

  const LDefinition* getArgJSContext() { return getTemp(0); }
  const LDefinition* getArgObj() { return getTemp(1); }
  const LDefinition* getArgPrivate() { return getTemp(2); }
  const LDefinition* getArgArgs() { return getTemp(3); }
};

// Generates a polymorphic callsite, wherein the function being called is
// unknown and anticipated to vary.
class LApplyArgsGeneric
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 2, 2> {
 public:
  LIR_HEADER(ApplyArgsGeneric)

  LApplyArgsGeneric(const LAllocation& func, const LAllocation& argc,
                    const LBoxAllocation& thisv, const LDefinition& tmpObjReg,
                    const LDefinition& tmpCopy)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, func);
    setOperand(1, argc);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
  }

  MApplyArgs* mir() const { return mir_->toApplyArgs(); }

  bool hasSingleTarget() const { return getSingleTarget() != nullptr; }
  WrappedFunction* getSingleTarget() const { return mir()->getSingleTarget(); }

  uint32_t numExtraFormals() const { return mir()->numExtraFormals(); }

  const LAllocation* getFunction() { return getOperand(0); }
  const LAllocation* getArgc() { return getOperand(1); }
  static const size_t ThisIndex = 2;
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
};

class LApplyArgsObj
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 2, 2> {
 public:
  LIR_HEADER(ApplyArgsObj)

  LApplyArgsObj(const LAllocation& func, const LAllocation& argsObj,
                const LBoxAllocation& thisv, const LDefinition& tmpObjReg,
                const LDefinition& tmpCopy)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, func);
    setOperand(1, argsObj);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
  }

  MApplyArgsObj* mir() const { return mir_->toApplyArgsObj(); }

  bool hasSingleTarget() const { return getSingleTarget() != nullptr; }
  WrappedFunction* getSingleTarget() const { return mir()->getSingleTarget(); }

  const LAllocation* getFunction() { return getOperand(0); }
  const LAllocation* getArgsObj() { return getOperand(1); }
  // All registers are calltemps. argc is mapped to the same register as
  // ArgsObj. argc becomes live as ArgsObj is dying.
  const LAllocation* getArgc() { return getOperand(1); }
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }
  static const size_t ThisIndex = 2;

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
};

class LApplyArrayGeneric
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 2, 2> {
 public:
  LIR_HEADER(ApplyArrayGeneric)

  LApplyArrayGeneric(const LAllocation& func, const LAllocation& elements,
                     const LBoxAllocation& thisv, const LDefinition& tmpObjReg,
                     const LDefinition& tmpCopy)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, func);
    setOperand(1, elements);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
  }

  MApplyArray* mir() const { return mir_->toApplyArray(); }

  bool hasSingleTarget() const { return getSingleTarget() != nullptr; }
  WrappedFunction* getSingleTarget() const { return mir()->getSingleTarget(); }

  const LAllocation* getFunction() { return getOperand(0); }
  const LAllocation* getElements() { return getOperand(1); }
  // argc is mapped to the same register as elements: argc becomes
  // live as elements is dying, all registers are calltemps.
  const LAllocation* getArgc() { return getOperand(1); }
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }
  static const size_t ThisIndex = 2;

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
};

class LConstructArgsGeneric
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 3, 1> {
 public:
  LIR_HEADER(ConstructArgsGeneric)

  LConstructArgsGeneric(const LAllocation& func, const LAllocation& argc,
                        const LAllocation& newTarget,
                        const LBoxAllocation& thisv,
                        const LDefinition& tmpObjReg)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, func);
    setOperand(1, argc);
    setOperand(2, newTarget);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
  }

  MConstructArgs* mir() const { return mir_->toConstructArgs(); }

  bool hasSingleTarget() const { return getSingleTarget() != nullptr; }
  WrappedFunction* getSingleTarget() const { return mir()->getSingleTarget(); }

  uint32_t numExtraFormals() const { return mir()->numExtraFormals(); }

  const LAllocation* getFunction() { return getOperand(0); }
  const LAllocation* getArgc() { return getOperand(1); }
  const LAllocation* getNewTarget() { return getOperand(2); }
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }

  static const size_t ThisIndex = 3;

  const LDefinition* getTempObject() { return getTemp(0); }

  // tempForArgCopy is mapped to the same register as newTarget:
  // tempForArgCopy becomes live as newTarget is dying, all registers are
  // calltemps.
  const LAllocation* getTempForArgCopy() { return getOperand(2); }
};

class LConstructArrayGeneric
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 3, 1> {
 public:
  LIR_HEADER(ConstructArrayGeneric)

  LConstructArrayGeneric(const LAllocation& func, const LAllocation& elements,
                         const LAllocation& newTarget,
                         const LBoxAllocation& thisv,
                         const LDefinition& tmpObjReg)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, func);
    setOperand(1, elements);
    setOperand(2, newTarget);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
  }

  MConstructArray* mir() const { return mir_->toConstructArray(); }

  bool hasSingleTarget() const { return getSingleTarget() != nullptr; }
  WrappedFunction* getSingleTarget() const { return mir()->getSingleTarget(); }

  const LAllocation* getFunction() { return getOperand(0); }
  const LAllocation* getElements() { return getOperand(1); }
  const LAllocation* getNewTarget() { return getOperand(2); }
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }

  static const size_t ThisIndex = 3;

  const LDefinition* getTempObject() { return getTemp(0); }

  // argc is mapped to the same register as elements: argc becomes
  // live as elements is dying, all registers are calltemps.
  const LAllocation* getArgc() { return getOperand(1); }

  // tempForArgCopy is mapped to the same register as newTarget:
  // tempForArgCopy becomes live as newTarget is dying, all registers are
  // calltemps.
  const LAllocation* getTempForArgCopy() { return getOperand(2); }
};

class LApplyArgsNative
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 1, 3> {
 public:
  LIR_HEADER(ApplyArgsNative)

  LApplyArgsNative(const LAllocation& argc, const LBoxAllocation& thisv,
                   const LDefinition& tmpObjReg, const LDefinition& tmpCopy,
                   const LDefinition& tmpExtra)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, argc);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
    setTemp(2, tmpExtra);
  }

  static constexpr bool isConstructing() { return false; }

  MApplyArgs* mir() const { return mir_->toApplyArgs(); }

  uint32_t numExtraFormals() const { return mir()->numExtraFormals(); }

  const LAllocation* getArgc() { return getOperand(0); }
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }

  static const size_t ThisIndex = 1;

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
  const LDefinition* getTempExtra() { return getTemp(2); }
};

class LApplyArgsObjNative
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 1, 3> {
 public:
  LIR_HEADER(ApplyArgsObjNative)

  LApplyArgsObjNative(const LAllocation& argsObj, const LBoxAllocation& thisv,
                      const LDefinition& tmpObjReg, const LDefinition& tmpCopy,
                      const LDefinition& tmpExtra)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, argsObj);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
    setTemp(2, tmpExtra);
  }

  static constexpr bool isConstructing() { return false; }

  MApplyArgsObj* mir() const { return mir_->toApplyArgsObj(); }

  const LAllocation* getArgsObj() { return getOperand(0); }
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }

  static const size_t ThisIndex = 1;

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
  const LDefinition* getTempExtra() { return getTemp(2); }

  // argc is mapped to the same register as argsObj: argc becomes live as
  // argsObj is dying, all registers are calltemps.
  const LAllocation* getArgc() { return getOperand(0); }
};

class LApplyArrayNative
    : public LCallInstructionHelper<BOX_PIECES, BOX_PIECES + 1, 3> {
 public:
  LIR_HEADER(ApplyArrayNative)

  LApplyArrayNative(const LAllocation& elements, const LBoxAllocation& thisv,
                    const LDefinition& tmpObjReg, const LDefinition& tmpCopy,
                    const LDefinition& tmpExtra)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, elements);
    setBoxOperand(ThisIndex, thisv);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
    setTemp(2, tmpExtra);
  }

  static constexpr bool isConstructing() { return false; }

  MApplyArray* mir() const { return mir_->toApplyArray(); }

  const LAllocation* getElements() { return getOperand(0); }
  LBoxAllocation thisValue() const { return getBoxOperand(ThisIndex); }

  static const size_t ThisIndex = 1;

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
  const LDefinition* getTempExtra() { return getTemp(2); }

  // argc is mapped to the same register as elements: argc becomes live as
  // elements is dying, all registers are calltemps.
  const LAllocation* getArgc() { return getOperand(0); }
};

class LConstructArgsNative : public LCallInstructionHelper<BOX_PIECES, 2, 3> {
 public:
  LIR_HEADER(ConstructArgsNative)

  LConstructArgsNative(const LAllocation& argc, const LAllocation& newTarget,
                       const LDefinition& tmpObjReg, const LDefinition& tmpCopy,
                       const LDefinition& tmpExtra)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, argc);
    setOperand(1, newTarget);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
    setTemp(2, tmpExtra);
  }

  static constexpr bool isConstructing() { return true; }

  MConstructArgs* mir() const { return mir_->toConstructArgs(); }

  uint32_t numExtraFormals() const { return mir()->numExtraFormals(); }

  const LAllocation* getArgc() { return getOperand(0); }
  const LAllocation* getNewTarget() { return getOperand(1); }

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
  const LDefinition* getTempExtra() { return getTemp(2); }
};

class LConstructArrayNative : public LCallInstructionHelper<BOX_PIECES, 2, 3> {
 public:
  LIR_HEADER(ConstructArrayNative)

  LConstructArrayNative(const LAllocation& elements,
                        const LAllocation& newTarget,
                        const LDefinition& tmpObjReg,
                        const LDefinition& tmpCopy, const LDefinition& tmpExtra)
      : LCallInstructionHelper(classOpcode) {
    setOperand(0, elements);
    setOperand(1, newTarget);
    setTemp(0, tmpObjReg);
    setTemp(1, tmpCopy);
    setTemp(2, tmpExtra);
  }

  static constexpr bool isConstructing() { return true; }

  MConstructArray* mir() const { return mir_->toConstructArray(); }

  const LAllocation* getElements() { return getOperand(0); }
  const LAllocation* getNewTarget() { return getOperand(1); }

  const LDefinition* getTempObject() { return getTemp(0); }
  const LDefinition* getTempForArgCopy() { return getTemp(1); }
  const LDefinition* getTempExtra() { return getTemp(2); }

  // argc is mapped to the same register as elements: argc becomes live as
  // elements is dying, all registers are calltemps.
  const LAllocation* getArgc() { return getOperand(0); }
};

// Returns from the function being compiled (not used in inlined frames). The
// input must be a box.
class LReturn : public LInstructionHelper<0, BOX_PIECES, 0> {
  bool isGenerator_;

 public:
  LIR_HEADER(Return)

  explicit LReturn(bool isGenerator)
      : LInstructionHelper(classOpcode), isGenerator_(isGenerator) {}

  bool isGenerator() { return isGenerator_; }
};

class LHypot : public LCallInstructionHelper<1, 4, 0> {
  uint32_t numOperands_;

 public:
  LIR_HEADER(Hypot)
  LHypot(const LAllocation& x, const LAllocation& y)
      : LCallInstructionHelper(classOpcode), numOperands_(2) {
    setOperand(0, x);
    setOperand(1, y);
  }

  LHypot(const LAllocation& x, const LAllocation& y, const LAllocation& z)
      : LCallInstructionHelper(classOpcode), numOperands_(3) {
    setOperand(0, x);
    setOperand(1, y);
    setOperand(2, z);
  }

  LHypot(const LAllocation& x, const LAllocation& y, const LAllocation& z,
         const LAllocation& w)
      : LCallInstructionHelper(classOpcode), numOperands_(4) {
    setOperand(0, x);
    setOperand(1, y);
    setOperand(2, z);
    setOperand(3, w);
  }

  uint32_t numArgs() const { return numOperands_; }

  const LAllocation* x() { return getOperand(0); }

  const LAllocation* y() { return getOperand(1); }
};

// Adds two integers, returning an integer value.
class LAddI : public LBinaryMath<0> {
  bool recoversInput_;

 public:
  LIR_HEADER(AddI)

  LAddI() : LBinaryMath(classOpcode), recoversInput_(false) {}

  const char* extraName() const {
    return snapshot() ? "OverflowCheck" : nullptr;
  }

  bool recoversInput() const { return recoversInput_; }
  void setRecoversInput() { recoversInput_ = true; }

  MAdd* mir() const { return mir_->toAdd(); }
};

// Subtracts two integers, returning an integer value.
class LSubI : public LBinaryMath<0> {
  bool recoversInput_;

 public:
  LIR_HEADER(SubI)

  LSubI() : LBinaryMath(classOpcode), recoversInput_(false) {}

  const char* extraName() const {
    return snapshot() ? "OverflowCheck" : nullptr;
  }

  bool recoversInput() const { return recoversInput_; }
  void setRecoversInput() { recoversInput_ = true; }
  MSub* mir() const { return mir_->toSub(); }
};

inline bool LNode::recoversInput() const {
  switch (op()) {
    case Opcode::AddI:
      return toAddI()->recoversInput();
    case Opcode::SubI:
      return toSubI()->recoversInput();
    default:
      return false;
  }
}

// Passed the BaselineFrame address in the OsrFrameReg via the IonOsrTempData
// populated by PrepareOsrTempData.
//
// Forwards this object to the LOsrValues for Value materialization.
class LOsrEntry : public LInstructionHelper<1, 0, 1> {
 protected:
  Label label_;
  uint32_t frameDepth_;

 public:
  LIR_HEADER(OsrEntry)

  explicit LOsrEntry(const LDefinition& temp)
      : LInstructionHelper(classOpcode), frameDepth_(0) {
    setTemp(0, temp);
  }

  void setFrameDepth(uint32_t depth) { frameDepth_ = depth; }
  uint32_t getFrameDepth() { return frameDepth_; }
  Label* label() { return &label_; }
  const LDefinition* temp() { return getTemp(0); }
};

// This is used only with LWasmCall.
class LWasmCallIndirectAdjunctSafepoint : public LInstructionHelper<0, 0, 0> {
  CodeOffset offs_;
  uint32_t framePushedAtStackMapBase_;

 public:
  LIR_HEADER(WasmCallIndirectAdjunctSafepoint);

  LWasmCallIndirectAdjunctSafepoint()
      : LInstructionHelper(classOpcode),
        offs_(0),
        framePushedAtStackMapBase_(0) {
    // Ensure that the safepoint does not get live registers associated with it.
    setIsCall();
  }

  CodeOffset safepointLocation() const {
    MOZ_ASSERT(offs_.offset() != 0);
    return offs_;
  }
  uint32_t framePushedAtStackMapBase() const {
    MOZ_ASSERT(offs_.offset() != 0);
    return framePushedAtStackMapBase_;
  }
  void recordSafepointInfo(CodeOffset offs, uint32_t framePushed) {
    offs_ = offs;
    framePushedAtStackMapBase_ = framePushed;
  }
};

// LWasmCall may be generated into two function calls in the case of
// call_indirect, one for the fast path and one for the slow path.  In that
// case, the node carries a pointer to a companion node, the "adjunct
// safepoint", representing the safepoint for the second of the two calls.  The
// dual-call construction is only meaningful for wasm because wasm has no
// invalidation of code; this is not a pattern to be used generally.
class LWasmCall : public LVariadicInstruction<0, 0> {
  bool needsBoundsCheck_;
  mozilla::Maybe<uint32_t> tableSize_;
  LWasmCallIndirectAdjunctSafepoint* adjunctSafepoint_;

 public:
  LIR_HEADER(WasmCall);

  LWasmCall(uint32_t numOperands, bool needsBoundsCheck,
            mozilla::Maybe<uint32_t> tableSize = mozilla::Nothing())
      : LVariadicInstruction(classOpcode, numOperands),
        needsBoundsCheck_(needsBoundsCheck),
        tableSize_(tableSize),
        adjunctSafepoint_(nullptr) {
    this->setIsCall();
  }

  MWasmCallBase* callBase() const {
    if (isCatchable() && !isReturnCall()) {
      return static_cast<MWasmCallBase*>(mirCatchable());
    }
    if (isReturnCall()) {
      return static_cast<MWasmReturnCall*>(mirReturnCall());
    }
    return static_cast<MWasmCallBase*>(mirUncatchable());
  }
  bool isCatchable() const { return mir_->isWasmCallCatchable(); }
  bool isReturnCall() const { return mir_->isWasmReturnCall(); }
  MWasmCallCatchable* mirCatchable() const {
    return mir_->toWasmCallCatchable();
  }
  MWasmCallUncatchable* mirUncatchable() const {
    return mir_->toWasmCallUncatchable();
  }
  MWasmReturnCall* mirReturnCall() const { return mir_->toWasmReturnCall(); }

  static bool isCallPreserved(AnyRegister reg) {
    // All MWasmCalls preserve the TLS register:
    //  - internal/indirect calls do by the internal wasm ABI
    //  - import calls do by explicitly saving/restoring at the callsite
    //  - builtin calls do because the TLS reg is non-volatile
    // See also CodeGeneratorShared::emitWasmCall.
    //
    // All other registers are not preserved. This is is relied upon by
    // MWasmCallCatchable which needs all live registers to be spilled before
    // a call.
    return !reg.isFloat() && reg.gpr() == InstanceReg;
  }

  bool needsBoundsCheck() const { return needsBoundsCheck_; }
  mozilla::Maybe<uint32_t> tableSize() const { return tableSize_; }
  LWasmCallIndirectAdjunctSafepoint* adjunctSafepoint() const {
    MOZ_ASSERT(adjunctSafepoint_ != nullptr);
    return adjunctSafepoint_;
  }
  void setAdjunctSafepoint(LWasmCallIndirectAdjunctSafepoint* asp) {
    adjunctSafepoint_ = asp;
  }
};

class LWasmRegisterResult : public LInstructionHelper<1, 0, 0> {
 public:
  LIR_HEADER(WasmRegisterResult);

  LWasmRegisterResult() : LInstructionHelper(classOpcode) {}

  MWasmRegisterResult* mir() const {
    if (!mir_->isWasmRegisterResult()) {
      return nullptr;
    }
    return mir_->toWasmRegisterResult();
  }
};

class LWasmRegisterPairResult : public LInstructionHelper<2, 0, 0> {
 public:
  LIR_HEADER(WasmRegisterPairResult);

  LWasmRegisterPairResult() : LInstructionHelper(classOpcode) {}

  MDefinition* mir() const { return mirRaw(); }
};

inline uint32_t LStackArea::base() const {
  return ins()->toWasmStackResultArea()->mir()->base();
}
inline void LStackArea::setBase(uint32_t base) {
  ins()->toWasmStackResultArea()->mir()->setBase(base);
}
inline uint32_t LStackArea::size() const {
  return ins()->toWasmStackResultArea()->mir()->byteSize();
}

inline bool LStackArea::ResultIterator::done() const {
  return idx_ == alloc_.ins()->toWasmStackResultArea()->mir()->resultCount();
}
inline void LStackArea::ResultIterator::next() {
  MOZ_ASSERT(!done());
  idx_++;
}
inline LAllocation LStackArea::ResultIterator::alloc() const {
  MOZ_ASSERT(!done());
  MWasmStackResultArea* area = alloc_.ins()->toWasmStackResultArea()->mir();
  const auto& stackResult = area->result(idx_);
  MIRType type = stackResult.type();
  auto width =
#ifndef JS_PUNBOX64
      type == MIRType::Int64 ? LStackSlot::DoubleWord :
#endif
                             LStackSlot::width(LDefinition::TypeFrom(type));
  return LStackSlot(area->base() - stackResult.offset(), width);
}
inline bool LStackArea::ResultIterator::isWasmAnyRef() const {
  MOZ_ASSERT(!done());
  MWasmStackResultArea* area = alloc_.ins()->toWasmStackResultArea()->mir();
  MIRType type = area->result(idx_).type();
#ifndef JS_PUNBOX64
  // LDefinition::TypeFrom isn't defined for MIRType::Int64 values on
  // this platform, so here we have a special case.
  if (type == MIRType::Int64) {
    return false;
  }
#endif
  return LDefinition::TypeFrom(type) == LDefinition::WASM_ANYREF;
}

class LWasmStackResult : public LInstructionHelper<1, 1, 0> {
 public:
  LIR_HEADER(WasmStackResult);

  LWasmStackResult() : LInstructionHelper(classOpcode) {}

  MWasmStackResult* mir() const { return mir_->toWasmStackResult(); }
  LStackSlot result(uint32_t base) const {
    auto width = LStackSlot::width(LDefinition::TypeFrom(mir()->type()));
    return LStackSlot(base - mir()->result().offset(), width);
  }
};

class LWasmStackResult64 : public LInstructionHelper<INT64_PIECES, 1, 0> {
 public:
  LIR_HEADER(WasmStackResult64);

  LWasmStackResult64() : LInstructionHelper(classOpcode) {}

  MWasmStackResult* mir() const { return mir_->toWasmStackResult(); }
  LStackSlot result(uint32_t base, LDefinition* def) {
    uint32_t offset = base - mir()->result().offset();
#if defined(JS_NUNBOX32)
    if (def == getDef(INT64LOW_INDEX)) {
      offset -= INT64LOW_OFFSET;
    } else {
      MOZ_ASSERT(def == getDef(INT64HIGH_INDEX));
      offset -= INT64HIGH_OFFSET;
    }
#else
    MOZ_ASSERT(def == getDef(0));
#endif
    return LStackSlot(offset, LStackSlot::DoubleWord);
  }
};

inline LStackSlot LStackArea::resultAlloc(LInstruction* lir,
                                          LDefinition* def) const {
  if (lir->isWasmStackResult64()) {
    return lir->toWasmStackResult64()->result(base(), def);
  }
  MOZ_ASSERT(def == lir->getDef(0));
  return lir->toWasmStackResult()->result(base());
}

inline bool LNode::isCallPreserved(AnyRegister reg) const {
  return isWasmCall() && LWasmCall::isCallPreserved(reg);
}

template <size_t NumDefs>
class LIonToWasmCallBase : public LVariadicInstruction<NumDefs, 1> {
  using Base = LVariadicInstruction<NumDefs, 1>;

 public:
  explicit LIonToWasmCallBase(LNode::Opcode classOpcode, uint32_t numOperands,
                              const LDefinition& temp)
      : Base(classOpcode, numOperands) {
    this->setIsCall();
    this->setTemp(0, temp);
  }
  MIonToWasmCall* mir() const { return this->mir_->toIonToWasmCall(); }
  const LDefinition* temp() { return this->getTemp(0); }
};

class LIonToWasmCall : public LIonToWasmCallBase<1> {
 public:
  LIR_HEADER(IonToWasmCall);
  LIonToWasmCall(uint32_t numOperands, const LDefinition& temp)
      : LIonToWasmCallBase<1>(classOpcode, numOperands, temp) {}
};

class LIonToWasmCallV : public LIonToWasmCallBase<BOX_PIECES> {
 public:
  LIR_HEADER(IonToWasmCallV);
  LIonToWasmCallV(uint32_t numOperands, const LDefinition& temp)
      : LIonToWasmCallBase<BOX_PIECES>(classOpcode, numOperands, temp) {}
};

class LIonToWasmCallI64 : public LIonToWasmCallBase<INT64_PIECES> {
 public:
  LIR_HEADER(IonToWasmCallI64);
  LIonToWasmCallI64(uint32_t numOperands, const LDefinition& temp)
      : LIonToWasmCallBase<INT64_PIECES>(classOpcode, numOperands, temp) {}
};

// Definitions for `extraName` methods of generated LIR instructions.

#ifdef JS_JITSPEW
const char* LBox::extraName() const { return StringFromMIRType(type_); }

const char* LNewArray::extraName() const {
  return mir()->isVMCall() ? "VMCall" : nullptr;
}

const char* LNewObject::extraName() const {
  return mir()->isVMCall() ? "VMCall" : nullptr;
}

const char* LCompare::extraName() const { return CodeName(jsop_); }

const char* LCompareI64::extraName() const { return CodeName(jsop_); }

const char* LCompareI64AndBranch::extraName() const { return CodeName(jsop_); }

const char* LCompareAndBranch::extraName() const { return CodeName(jsop_); }

const char* LMathFunctionD::extraName() const {
  return MMathFunction::FunctionName(mir()->function());
}

const char* LMathFunctionF::extraName() const {
  return MMathFunction::FunctionName(mir()->function());
}

const char* LStoreElementV::extraName() const {
  return mir()->needsHoleCheck() ? "HoleCheck" : nullptr;
}

const char* LStoreElementT::extraName() const {
  return mir()->needsHoleCheck() ? "HoleCheck" : nullptr;
}

const char* LArrayPopShift::extraName() const {
  return mir()->mode() == MArrayPopShift::Pop ? "Pop" : "Shift";
}

const char* LMinMaxI::extraName() const {
  return mir()->isMax() ? "Max" : "Min";
}

const char* LMinMaxD::extraName() const {
  return mir()->isMax() ? "Max" : "Min";
}

const char* LMinMaxF::extraName() const {
  return mir()->isMax() ? "Max" : "Min";
}

const char* LMulI::extraName() const {
  return (mir()->mode() == MMul::Integer)
             ? "Integer"
             : (mir()->canBeNegativeZero() ? "CanBeNegativeZero" : nullptr);
}

const char* LDivI::extraName() const {
  if (mir()->isTruncated()) {
    if (mir()->canBeNegativeZero()) {
      return mir()->canBeNegativeOverflow()
                 ? "Truncate_NegativeZero_NegativeOverflow"
                 : "Truncate_NegativeZero";
    }
    return mir()->canBeNegativeOverflow() ? "Truncate_NegativeOverflow"
                                          : "Truncate";
  }
  if (mir()->canBeNegativeZero()) {
    return mir()->canBeNegativeOverflow() ? "NegativeZero_NegativeOverflow"
                                          : "NegativeZero";
  }
  return mir()->canBeNegativeOverflow() ? "NegativeOverflow" : nullptr;
}

const char* LModI::extraName() const {
  return mir()->isTruncated() ? "Truncated" : nullptr;
}

const char* LBitOpI::extraName() const {
  if (bitop() == JSOp::Ursh && mir_->toUrsh()->bailoutsDisabled()) {
    return "ursh:BailoutsDisabled";
  }
  return CodeName(bitop_);
}

const char* LBitOpI64::extraName() const { return CodeName(bitop_); }

const char* LShiftI::extraName() const { return CodeName(bitop_); }

const char* LShiftI64::extraName() const { return CodeName(bitop_); }

const char* LMathD::extraName() const { return CodeName(jsop_); }

const char* LMathF::extraName() const { return CodeName(jsop_); }
#endif

}  // namespace jit
}  // namespace js

#endif /* jit_shared_LIR_shared_h */

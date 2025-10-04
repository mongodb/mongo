/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrameInfo_h
#define jit_BaselineFrameInfo_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <new>

#include "jit/BaselineFrame.h"
#include "jit/BaselineJIT.h"
#include "jit/FixedList.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

struct BytecodeInfo;
class MacroAssembler;

// [SMDOC] Baseline FrameInfo overview.
//
// FrameInfo is used by BaselineCodeGen to track values stored in the frame.
// There are two implementations:
//
// InterpreterFrameInfo
// --------------------
// The InterpreterFrameInfo class is used by the interpreter generator and is
// a very simple interface on top of the MacroAssembler, because the stack is
// always synced.
//
// CompilerFrameInfo
// -----------------
// The CompilerFrameInfo class is more complicated because it maintains a
// virtual stack to optimize some common stack operations. Locals and arguments
// are always fully synced. Stack values can either be synced, stored as
// constant, stored in a Value register or refer to a local slot. Syncing a
// StackValue ensures it's stored on the stack, e.g. kind == Stack.
//
// To see how this works, consider the following statement:
//
//    var y = x + 9;
//
// Here two values are pushed: StackValue(LocalSlot(0)) and
// StackValue(Int32Value(9)). Only when we reach the ADD op, code is generated
// to load the operands directly into the right operand registers and sync all
// other stack values.
//
// For stack values, the following invariants hold (and are checked between
// ops):
//
// (1) If a value is synced (kind == Stack), all values below it must also be
//     synced. In other words, values with kind other than Stack can only appear
//     on top of the abstract stack.
//
// (2) When we call a stub or IC, all values still on the stack must be synced.

// Represents a value pushed on the stack. Note that StackValue is not used for
// locals or arguments since these are always fully synced.
class StackValue {
 public:
  enum Kind {
    Constant,
    Register,
    Stack,
    LocalSlot,
    ArgSlot,
    ThisSlot,
#ifdef DEBUG
    // In debug builds, assert Kind is initialized.
    Uninitialized,
#endif
  };

 private:
  MOZ_INIT_OUTSIDE_CTOR Kind kind_;

  MOZ_INIT_OUTSIDE_CTOR union Data {
    JS::Value constant;
    ValueOperand reg;
    uint32_t localSlot;
    uint32_t argSlot;

    // |constant| has a non-trivial constructor and therefore MUST be
    // placement-new'd into existence.
    MOZ_PUSH_DISABLE_NONTRIVIAL_UNION_WARNINGS
    Data() {}
    MOZ_POP_DISABLE_NONTRIVIAL_UNION_WARNINGS
  } data;

  MOZ_INIT_OUTSIDE_CTOR JSValueType knownType_;

 public:
  StackValue() { reset(); }

  Kind kind() const { return kind_; }
  bool hasKnownType() const { return knownType_ != JSVAL_TYPE_UNKNOWN; }
  bool hasKnownType(JSValueType type) const {
    MOZ_ASSERT(type != JSVAL_TYPE_UNKNOWN);
    return knownType_ == type;
  }
  JSValueType knownType() const {
    MOZ_ASSERT(hasKnownType());
    return knownType_;
  }
  void reset() {
#ifdef DEBUG
    kind_ = Uninitialized;
    knownType_ = JSVAL_TYPE_UNKNOWN;
#endif
  }
  Value constant() const {
    MOZ_ASSERT(kind_ == Constant);
    return data.constant;
  }
  ValueOperand reg() const {
    MOZ_ASSERT(kind_ == Register);
    return data.reg;
  }
  uint32_t localSlot() const {
    MOZ_ASSERT(kind_ == LocalSlot);
    return data.localSlot;
  }
  uint32_t argSlot() const {
    MOZ_ASSERT(kind_ == ArgSlot);
    return data.argSlot;
  }

  void setConstant(const Value& v) {
    kind_ = Constant;
    new (&data.constant) Value(v);
    knownType_ = v.isDouble() ? JSVAL_TYPE_DOUBLE : v.extractNonDoubleType();
  }
  void setRegister(const ValueOperand& val,
                   JSValueType knownType = JSVAL_TYPE_UNKNOWN) {
    kind_ = Register;
    new (&data.reg) ValueOperand(val);
    knownType_ = knownType;
  }
  void setLocalSlot(uint32_t slot) {
    kind_ = LocalSlot;
    new (&data.localSlot) uint32_t(slot);
    knownType_ = JSVAL_TYPE_UNKNOWN;
  }
  void setArgSlot(uint32_t slot) {
    kind_ = ArgSlot;
    new (&data.argSlot) uint32_t(slot);
    knownType_ = JSVAL_TYPE_UNKNOWN;
  }
  void setThis() {
    kind_ = ThisSlot;
    knownType_ = JSVAL_TYPE_UNKNOWN;
  }
  void setStack() {
    kind_ = Stack;
    knownType_ = JSVAL_TYPE_UNKNOWN;
  }
};

enum StackAdjustment { AdjustStack, DontAdjustStack };

class FrameInfo {
 protected:
  MacroAssembler& masm;

 public:
  explicit FrameInfo(MacroAssembler& masm) : masm(masm) {}

  Address addressOfLocal(size_t local) const {
    return Address(FramePointer, BaselineFrame::reverseOffsetOfLocal(local));
  }
  Address addressOfArg(size_t arg) const {
    return Address(FramePointer, JitFrameLayout::offsetOfActualArg(arg));
  }
  Address addressOfThis() const {
    return Address(FramePointer, JitFrameLayout::offsetOfThis());
  }
  Address addressOfCalleeToken() const {
    return Address(FramePointer, JitFrameLayout::offsetOfCalleeToken());
  }
  Address addressOfEnvironmentChain() const {
    return Address(FramePointer,
                   BaselineFrame::reverseOffsetOfEnvironmentChain());
  }
  Address addressOfICScript() const {
    return Address(FramePointer, BaselineFrame::reverseOffsetOfICScript());
  }
  Address addressOfFlags() const {
    return Address(FramePointer, BaselineFrame::reverseOffsetOfFlags());
  }
  Address addressOfReturnValue() const {
    return Address(FramePointer, BaselineFrame::reverseOffsetOfReturnValue());
  }
  Address addressOfArgsObj() const {
    return Address(FramePointer, BaselineFrame::reverseOffsetOfArgsObj());
  }
  Address addressOfScratchValue() const {
    return Address(FramePointer, BaselineFrame::reverseOffsetOfScratchValue());
  }
  Address addressOfScratchValueLow32() const {
    return Address(FramePointer,
                   BaselineFrame::reverseOffsetOfScratchValueLow32());
  }
  Address addressOfScratchValueHigh32() const {
    return Address(FramePointer,
                   BaselineFrame::reverseOffsetOfScratchValueHigh32());
  }
#ifdef DEBUG
  Address addressOfDebugFrameSize() const {
    return Address(FramePointer,
                   BaselineFrame::reverseOffsetOfDebugFrameSize());
  }
#endif
};

class CompilerFrameInfo : public FrameInfo {
  friend class BaselinePerfSpewer;
  JSScript* script;
  FixedList<StackValue> stack;
  size_t spIndex;

 public:
  CompilerFrameInfo(JSScript* script, MacroAssembler& masm)
      : FrameInfo(masm), script(script), spIndex(0) {}
  [[nodiscard]] bool init(TempAllocator& alloc);

  size_t nlocals() const { return script->nfixed(); }
  size_t nargs() const { return script->function()->nargs(); }

 private:
  inline StackValue* rawPush() {
    StackValue* val = &stack[spIndex++];
    val->reset();
    return val;
  }

  inline StackValue* peek(int32_t index) const {
    MOZ_ASSERT(index < 0);
    return const_cast<StackValue*>(&stack[spIndex + index]);
  }

 public:
  inline size_t stackDepth() const { return spIndex; }
  inline void setStackDepth(uint32_t newDepth) {
    if (newDepth <= stackDepth()) {
      spIndex = newDepth;
    } else {
      uint32_t diff = newDepth - stackDepth();
      for (uint32_t i = 0; i < diff; i++) {
        StackValue* val = rawPush();
        val->setStack();
      }

      MOZ_ASSERT(spIndex == newDepth);
    }
  }

  void assertStackDepth(uint32_t depth) { MOZ_ASSERT(stackDepth() == depth); }
  void incStackDepth(int32_t diff) { setStackDepth(stackDepth() + diff); }
  bool hasKnownStackDepth(uint32_t depth) { return stackDepth() == depth; }

  inline void pop(StackAdjustment adjust = AdjustStack);
  inline void popn(uint32_t n, StackAdjustment adjust = AdjustStack);
  inline void push(const Value& val) {
    StackValue* sv = rawPush();
    sv->setConstant(val);
  }
  inline void push(const ValueOperand& val,
                   JSValueType knownType = JSVAL_TYPE_UNKNOWN) {
    StackValue* sv = rawPush();
    sv->setRegister(val, knownType);
  }
  inline void pushLocal(uint32_t local) {
    MOZ_ASSERT(local < nlocals());
    StackValue* sv = rawPush();
    sv->setLocalSlot(local);
  }
  inline void pushArg(uint32_t arg) {
    StackValue* sv = rawPush();
    sv->setArgSlot(arg);
  }
  inline void pushThis() {
    StackValue* sv = rawPush();
    sv->setThis();
  }

  inline void pushScratchValue() {
    masm.pushValue(addressOfScratchValue());
    StackValue* sv = rawPush();
    sv->setStack();
  }

  Address addressOfLocal(size_t local) const {
    MOZ_ASSERT(local < nlocals());
    return FrameInfo::addressOfLocal(local);
  }
  Address addressOfArg(size_t arg) const {
    MOZ_ASSERT(arg < nargs());
    return FrameInfo::addressOfArg(arg);
  }

  Address addressOfStackValue(int32_t depth) const {
    const StackValue* value = peek(depth);
    MOZ_ASSERT(value->kind() == StackValue::Stack);
    size_t slot = value - &stack[0];
    MOZ_ASSERT(slot < stackDepth());
    return Address(FramePointer,
                   BaselineFrame::reverseOffsetOfLocal(nlocals() + slot));
  }

  void popValue(ValueOperand dest);

  void sync(StackValue* val);
  void syncStack(uint32_t uses);
  uint32_t numUnsyncedSlots();
  void popRegsAndSync(uint32_t uses);

  void assertSyncedStack() const {
    MOZ_ASSERT_IF(stackDepth() > 0, peek(-1)->kind() == StackValue::Stack);
  }

  bool stackValueHasKnownType(int32_t depth, JSValueType type) const {
    return peek(depth)->hasKnownType(type);
  }

  mozilla::Maybe<Value> knownStackValue(int32_t depth) const {
    StackValue* val = peek(depth);
    if (val->kind() == StackValue::Constant) {
      return mozilla::Some(val->constant());
    }
    return mozilla::Nothing();
  }

  void storeStackValue(int32_t depth, const Address& dest,
                       const ValueOperand& scratch);

  uint32_t frameSize() const {
    return BaselineFrame::frameSizeForNumValueSlots(nlocals() + stackDepth());
  }

#ifdef DEBUG
  // Assert the state is valid before excuting "pc".
  void assertValidState(const BytecodeInfo& info);
#else
  inline void assertValidState(const BytecodeInfo& info) {}
#endif
};

class InterpreterFrameInfo : public FrameInfo {
 public:
  explicit InterpreterFrameInfo(MacroAssembler& masm) : FrameInfo(masm) {}

  // These methods are no-ops in the interpreter, because we don't have a
  // virtual stack there.
  void syncStack(uint32_t uses) {}
  void assertSyncedStack() const {}
  void assertStackDepth(uint32_t depth) {}
  void incStackDepth(int32_t diff) {}
  bool hasKnownStackDepth(uint32_t depth) { return false; }
  uint32_t numUnsyncedSlots() { return 0; }

  bool stackValueHasKnownType(int32_t depth, JSValueType type) const {
    return false;
  }

  mozilla::Maybe<Value> knownStackValue(int32_t depth) const {
    return mozilla::Nothing();
  }

  Address addressOfStackValue(int depth) const {
    MOZ_ASSERT(depth < 0);
    return Address(masm.getStackPointer(),
                   masm.framePushed() + size_t(-(depth + 1)) * sizeof(Value));
  }

  BaseIndex addressOfStackValue(Register index, int32_t offset = 0) const {
    return BaseIndex(masm.getStackPointer(), index, ValueScale, offset);
  }

  void popRegsAndSync(uint32_t uses);

  inline void pop();

  inline void popn(uint32_t n);

  void popn(Register reg) {
    // sp := sp + reg * sizeof(Value)
    Register spReg = AsRegister(masm.getStackPointer());
    masm.computeEffectiveAddress(BaseValueIndex(spReg, reg), spReg);
    // On arm64, SP may be < PSP now (that's OK).
    // eg testcase: tests/arguments/strict-args-generator-flushstack.js
  }

  void popValue(ValueOperand dest) { masm.popValue(dest); }

  void push(const ValueOperand& val,
            JSValueType knownType = JSVAL_TYPE_UNKNOWN) {
    masm.pushValue(val);
  }
  void push(const Value& val) { masm.pushValue(val); }

  void pushThis() { masm.pushValue(addressOfThis()); }
  void pushScratchValue() { masm.pushValue(addressOfScratchValue()); }

  void storeStackValue(int32_t depth, const Address& dest,
                       const ValueOperand& scratch) {
    masm.loadValue(addressOfStackValue(depth), scratch);
    masm.storeValue(scratch, dest);
  }

  void bumpInterpreterICEntry();

  Address addressOfInterpreterScript() const {
    return Address(FramePointer,
                   BaselineFrame::reverseOffsetOfInterpreterScript());
  }
  Address addressOfInterpreterPC() const {
    return Address(FramePointer, BaselineFrame::reverseOffsetOfInterpreterPC());
  }
  Address addressOfInterpreterICEntry() const {
    return Address(FramePointer,
                   BaselineFrame::reverseOffsetOfInterpreterICEntry());
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineFrameInfo_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrameInfo_h
#define jit_BaselineFrameInfo_h

#include "mozilla/Alignment.h"

#include "jit/BaselineFrame.h"
#include "jit/FixedList.h"
#include "jit/MacroAssembler.h"
#include "jit/SharedICRegisters.h"

namespace js {
namespace jit {

struct BytecodeInfo;

// FrameInfo overview.
//
// FrameInfo is used by the compiler to track values stored in the frame. This
// includes locals, arguments and stack values. Locals and arguments are always
// fully synced. Stack values can either be synced, stored as constant, stored in
// a Value register or refer to a local slot. Syncing a StackValue ensures it's
// stored on the stack, e.g. kind == Stack.
//
// To see how this works, consider the following statement:
//
//    var y = x + 9;
//
// Here two values are pushed: StackValue(LocalSlot(0)) and StackValue(Int32Value(9)).
// Only when we reach the ADD op, code is generated to load the operands directly
// into the right operand registers and sync all other stack values.
//
// For stack values, the following invariants hold (and are checked between ops):
//
// (1) If a value is synced (kind == Stack), all values below it must also be synced.
//     In other words, values with kind other than Stack can only appear on top of the
//     abstract stack.
//
// (2) When we call a stub or IC, all values still on the stack must be synced.

// Represents a value pushed on the stack. Note that StackValue is not used for
// locals or arguments since these are always fully synced.
class StackValue
{
  public:
    enum Kind {
        Constant,
        Register,
        Stack,
        LocalSlot,
        ArgSlot,
        ThisSlot,
        EvalNewTargetSlot
#ifdef DEBUG
        // In debug builds, assert Kind is initialized.
        , Uninitialized
#endif
    };

  private:
    Kind kind_;

    union {
        struct {
            JS::UninitializedValue v;
        } constant;
        struct {
            mozilla::AlignedStorage2<ValueOperand> reg;
        } reg;
        struct {
            uint32_t slot;
        } local;
        struct {
            uint32_t slot;
        } arg;
    } data;

    JSValueType knownType_;

  public:
    StackValue() {
        reset();
    }

    Kind kind() const {
        return kind_;
    }
    bool hasKnownType() const {
        return knownType_ != JSVAL_TYPE_UNKNOWN;
    }
    bool hasKnownType(JSValueType type) const {
        MOZ_ASSERT(type != JSVAL_TYPE_UNKNOWN);
        return knownType_ == type;
    }
    bool isKnownBoolean() const {
        return hasKnownType(JSVAL_TYPE_BOOLEAN);
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
        return data.constant.v.asValueRef();
    }
    ValueOperand reg() const {
        MOZ_ASSERT(kind_ == Register);
        return *data.reg.reg.addr();
    }
    uint32_t localSlot() const {
        MOZ_ASSERT(kind_ == LocalSlot);
        return data.local.slot;
    }
    uint32_t argSlot() const {
        MOZ_ASSERT(kind_ == ArgSlot);
        return data.arg.slot;
    }

    void setConstant(const Value& v) {
        kind_ = Constant;
        data.constant.v = v;
        knownType_ = v.isDouble() ? JSVAL_TYPE_DOUBLE : v.extractNonDoubleType();
    }
    void setRegister(const ValueOperand& val, JSValueType knownType = JSVAL_TYPE_UNKNOWN) {
        kind_ = Register;
        *data.reg.reg.addr() = val;
        knownType_ = knownType;
    }
    void setLocalSlot(uint32_t slot) {
        kind_ = LocalSlot;
        data.local.slot = slot;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
    void setArgSlot(uint32_t slot) {
        kind_ = ArgSlot;
        data.arg.slot = slot;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
    void setThis() {
        kind_ = ThisSlot;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
    void setEvalNewTarget() {
        kind_ = EvalNewTargetSlot;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
    void setStack() {
        kind_ = Stack;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
};

enum StackAdjustment { AdjustStack, DontAdjustStack };

class FrameInfo
{
    JSScript* script;
    MacroAssembler& masm;

    FixedList<StackValue> stack;
    size_t spIndex;

  public:
    FrameInfo(JSScript* script, MacroAssembler& masm)
      : script(script),
        masm(masm),
        stack(),
        spIndex(0)
    { }

    MOZ_MUST_USE bool init(TempAllocator& alloc);

    size_t nlocals() const {
        return script->nfixed();
    }
    size_t nargs() const {
        return script->functionNonDelazifying()->nargs();
    }

  private:
    inline StackValue* rawPush() {
        StackValue* val = &stack[spIndex++];
        val->reset();
        return val;
    }

  public:
    inline size_t stackDepth() const {
        return spIndex;
    }
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
    inline StackValue* peek(int32_t index) const {
        MOZ_ASSERT(index < 0);
        return const_cast<StackValue*>(&stack[spIndex + index]);
    }

    inline void pop(StackAdjustment adjust = AdjustStack);
    inline void popn(uint32_t n, StackAdjustment adjust = AdjustStack);
    inline void push(const Value& val) {
        StackValue* sv = rawPush();
        sv->setConstant(val);
    }
    inline void push(const ValueOperand& val, JSValueType knownType=JSVAL_TYPE_UNKNOWN) {
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
    inline void pushEvalNewTarget() {
        MOZ_ASSERT(script->isForEval());
        StackValue* sv = rawPush();
        sv->setEvalNewTarget();
    }

    inline void pushScratchValue() {
        masm.pushValue(addressOfScratchValue());
        StackValue* sv = rawPush();
        sv->setStack();
    }
    inline Address addressOfLocal(size_t local) const {
        MOZ_ASSERT(local < nlocals());
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfLocal(local));
    }
    Address addressOfArg(size_t arg) const {
        MOZ_ASSERT(arg < nargs());
        return Address(BaselineFrameReg, BaselineFrame::offsetOfArg(arg));
    }
    Address addressOfThis() const {
        return Address(BaselineFrameReg, BaselineFrame::offsetOfThis());
    }
    Address addressOfEvalNewTarget() const {
        return Address(BaselineFrameReg, BaselineFrame::offsetOfEvalNewTarget());
    }
    Address addressOfCalleeToken() const {
        return Address(BaselineFrameReg, BaselineFrame::offsetOfCalleeToken());
    }
    Address addressOfEnvironmentChain() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfEnvironmentChain());
    }
    Address addressOfFlags() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags());
    }
    Address addressOfReturnValue() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue());
    }
    Address addressOfArgsObj() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfArgsObj());
    }
    Address addressOfStackValue(const StackValue* value) const {
        MOZ_ASSERT(value->kind() == StackValue::Stack);
        size_t slot = value - &stack[0];
        MOZ_ASSERT(slot < stackDepth());
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfLocal(nlocals() + slot));
    }
    Address addressOfScratchValue() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfScratchValue());
    }

    void popValue(ValueOperand dest);

    void sync(StackValue* val);
    void syncStack(uint32_t uses);
    uint32_t numUnsyncedSlots();
    void popRegsAndSync(uint32_t uses);

    inline void assertSyncedStack() const {
        MOZ_ASSERT_IF(stackDepth() > 0, peek(-1)->kind() == StackValue::Stack);
    }

#ifdef DEBUG
    // Assert the state is valid before excuting "pc".
    void assertValidState(const BytecodeInfo& info);
#else
    inline void assertValidState(const BytecodeInfo& info) {}
#endif
};

} // namespace jit
} // namespace js

#endif /* jit_BaselineFrameInfo_h */

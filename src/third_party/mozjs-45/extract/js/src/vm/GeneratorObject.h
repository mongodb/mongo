/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GeneratorObject_h
#define vm_GeneratorObject_h

#include "jscntxt.h"
#include "jsobj.h"

#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/Stack.h"

namespace js {

class GeneratorObject : public NativeObject
{
  public:
    // Magic values stored in the yield index slot when the generator is
    // running or closing. See the yield index comment below.
    static const int32_t YIELD_INDEX_RUNNING = INT32_MAX;
    static const int32_t YIELD_INDEX_CLOSING = INT32_MAX - 1;

    enum {
        CALLEE_SLOT = 0,
        SCOPE_CHAIN_SLOT,
        ARGS_OBJ_SLOT,
        EXPRESSION_STACK_SLOT,
        YIELD_INDEX_SLOT,
        NEWTARGET_SLOT,
        RESERVED_SLOTS
    };

    enum ResumeKind { NEXT, THROW, CLOSE };

  private:
    static bool suspend(JSContext* cx, HandleObject obj, AbstractFramePtr frame, jsbytecode* pc,
                        Value* vp, unsigned nvalues);

  public:
    static inline ResumeKind getResumeKind(jsbytecode* pc) {
        MOZ_ASSERT(*pc == JSOP_RESUME);
        unsigned arg = GET_UINT16(pc);
        MOZ_ASSERT(arg <= CLOSE);
        return static_cast<ResumeKind>(arg);
    }

    static inline ResumeKind getResumeKind(ExclusiveContext* cx, JSAtom* atom) {
        if (atom == cx->names().next)
            return NEXT;
        if (atom == cx->names().throw_)
            return THROW;
        MOZ_ASSERT(atom == cx->names().close);
        return CLOSE;
    }

    static JSObject* create(JSContext* cx, AbstractFramePtr frame);

    static bool resume(JSContext* cx, InterpreterActivation& activation,
                       HandleObject obj, HandleValue arg, ResumeKind resumeKind);

    static bool initialSuspend(JSContext* cx, HandleObject obj, AbstractFramePtr frame, jsbytecode* pc) {
        return suspend(cx, obj, frame, pc, nullptr, 0);
    }

    static bool normalSuspend(JSContext* cx, HandleObject obj, AbstractFramePtr frame, jsbytecode* pc,
                              Value* vp, unsigned nvalues) {
        return suspend(cx, obj, frame, pc, vp, nvalues);
    }

    static bool finalSuspend(JSContext* cx, HandleObject obj);

    JSFunction& callee() const {
        return getFixedSlot(CALLEE_SLOT).toObject().as<JSFunction>();
    }
    void setCallee(JSFunction& callee) {
        setFixedSlot(CALLEE_SLOT, ObjectValue(callee));
    }

    JSObject& scopeChain() const {
        return getFixedSlot(SCOPE_CHAIN_SLOT).toObject();
    }
    void setScopeChain(JSObject& scopeChain) {
        setFixedSlot(SCOPE_CHAIN_SLOT, ObjectValue(scopeChain));
    }

    bool hasArgsObj() const {
        return getFixedSlot(ARGS_OBJ_SLOT).isObject();
    }
    ArgumentsObject& argsObj() const {
        return getFixedSlot(ARGS_OBJ_SLOT).toObject().as<ArgumentsObject>();
    }
    void setArgsObj(ArgumentsObject& argsObj) {
        setFixedSlot(ARGS_OBJ_SLOT, ObjectValue(argsObj));
    }

    bool hasExpressionStack() const {
        return getFixedSlot(EXPRESSION_STACK_SLOT).isObject();
    }
    ArrayObject& expressionStack() const {
        return getFixedSlot(EXPRESSION_STACK_SLOT).toObject().as<ArrayObject>();
    }
    void setExpressionStack(ArrayObject& expressionStack) {
        setFixedSlot(EXPRESSION_STACK_SLOT, ObjectValue(expressionStack));
    }
    void clearExpressionStack() {
        setFixedSlot(EXPRESSION_STACK_SLOT, NullValue());
    }

    bool isConstructing() const {
        return getFixedSlot(NEWTARGET_SLOT).isObject();
    }
    const Value& newTarget() const {
        return getFixedSlot(NEWTARGET_SLOT);
    }
    void setNewTarget(Value newTarget) {
        setFixedSlot(NEWTARGET_SLOT, newTarget);
    }


    // The yield index slot is abused for a few purposes.  It's undefined if
    // it hasn't been set yet (before the initial yield), and null if the
    // generator is closed. If the generator is running, the yield index is
    // YIELD_INDEX_RUNNING. If the generator is in that bizarre "closing"
    // state, the yield index is YIELD_INDEX_CLOSING.
    //
    // If the generator is suspended, it's the yield index (stored as
    // JSOP_INITIALYIELD/JSOP_YIELD operand) of the yield instruction that
    // suspended the generator. The yield index can be mapped to the bytecode
    // offset (interpreter) or to the native code offset (JIT).

    bool isRunning() const {
        MOZ_ASSERT(!isClosed());
        return getFixedSlot(YIELD_INDEX_SLOT).toInt32() == YIELD_INDEX_RUNNING;
    }
    bool isClosing() const {
        return getFixedSlot(YIELD_INDEX_SLOT).toInt32() == YIELD_INDEX_CLOSING;
    }
    bool isSuspended() const {
        // Note: also update Baseline's IsSuspendedStarGenerator code if this
        // changes.
        MOZ_ASSERT(!isClosed());
        static_assert(YIELD_INDEX_CLOSING < YIELD_INDEX_RUNNING,
                      "test below should return false for YIELD_INDEX_RUNNING");
        return getFixedSlot(YIELD_INDEX_SLOT).toInt32() < YIELD_INDEX_CLOSING;
    }
    void setRunning() {
        MOZ_ASSERT(isSuspended());
        setFixedSlot(YIELD_INDEX_SLOT, Int32Value(YIELD_INDEX_RUNNING));
    }
    void setClosing() {
        MOZ_ASSERT(isSuspended());
        setFixedSlot(YIELD_INDEX_SLOT, Int32Value(YIELD_INDEX_CLOSING));
    }
    void setYieldIndex(uint32_t yieldIndex) {
        MOZ_ASSERT_IF(yieldIndex == 0, getFixedSlot(YIELD_INDEX_SLOT).isUndefined());
        MOZ_ASSERT_IF(yieldIndex != 0, isRunning() || isClosing());
        MOZ_ASSERT(yieldIndex < uint32_t(YIELD_INDEX_CLOSING));
        setFixedSlot(YIELD_INDEX_SLOT, Int32Value(yieldIndex));
        MOZ_ASSERT(isSuspended());
    }
    uint32_t yieldIndex() const {
        MOZ_ASSERT(isSuspended());
        return getFixedSlot(YIELD_INDEX_SLOT).toInt32();
    }
    bool isClosed() const {
        return getFixedSlot(CALLEE_SLOT).isNull();
    }
    void setClosed() {
        setFixedSlot(CALLEE_SLOT, NullValue());
        setFixedSlot(SCOPE_CHAIN_SLOT, NullValue());
        setFixedSlot(ARGS_OBJ_SLOT, NullValue());
        setFixedSlot(EXPRESSION_STACK_SLOT, NullValue());
        setFixedSlot(YIELD_INDEX_SLOT, NullValue());
        setFixedSlot(NEWTARGET_SLOT, NullValue());
    }

    static size_t offsetOfCalleeSlot() {
        return getFixedSlotOffset(CALLEE_SLOT);
    }
    static size_t offsetOfScopeChainSlot() {
        return getFixedSlotOffset(SCOPE_CHAIN_SLOT);
    }
    static size_t offsetOfArgsObjSlot() {
        return getFixedSlotOffset(ARGS_OBJ_SLOT);
    }
    static size_t offsetOfYieldIndexSlot() {
        return getFixedSlotOffset(YIELD_INDEX_SLOT);
    }
    static size_t offsetOfExpressionStackSlot() {
        return getFixedSlotOffset(EXPRESSION_STACK_SLOT);
    }
    static size_t offsetOfNewTargetSlot() {
        return getFixedSlotOffset(NEWTARGET_SLOT);
    }
};

class LegacyGeneratorObject : public GeneratorObject
{
  public:
    static const Class class_;

    static bool close(JSContext* cx, HandleObject obj);
};

class StarGeneratorObject : public GeneratorObject
{
  public:
    static const Class class_;
};

bool GeneratorThrowOrClose(JSContext* cx, AbstractFramePtr frame, Handle<GeneratorObject*> obj,
                           HandleValue val, uint32_t resumeKind);
void SetReturnValueForClosingGenerator(JSContext* cx, AbstractFramePtr frame);

} // namespace js

template<>
inline bool
JSObject::is<js::GeneratorObject>() const
{
    return is<js::LegacyGeneratorObject>() || is<js::StarGeneratorObject>();
}

#endif /* vm_GeneratorObject_h */

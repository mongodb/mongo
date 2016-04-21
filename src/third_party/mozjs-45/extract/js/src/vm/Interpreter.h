/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Interpreter_h
#define vm_Interpreter_h

/*
 * JS interpreter interface.
 */

#include "jsiter.h"
#include "jspubtd.h"

#include "frontend/ParseNode.h"

#include "vm/Stack.h"

namespace js {

class ScopeIter;

/*
 * For a given |call|, convert null/undefined |this| into the global object for
 * the callee and replace other primitives with boxed versions. This assumes
 * that call.callee() is not strict mode code. This is the special/slow case of
 * ComputeThis.
 */
extern bool
BoxNonStrictThis(JSContext* cx, const CallReceiver& call);

extern bool
BoxNonStrictThis(JSContext* cx, HandleValue thisv, MutableHandleValue vp);

extern bool
GetFunctionThis(JSContext* cx, AbstractFramePtr frame, MutableHandleValue res);

extern bool
GetNonSyntacticGlobalThis(JSContext* cx, HandleObject scopeChain, MutableHandleValue res);

enum MaybeConstruct {
    NO_CONSTRUCT = INITIAL_NONE,
    CONSTRUCT = INITIAL_CONSTRUCT
};

/*
 * numToSkip is the number of stack values the expression decompiler should skip
 * before it reaches |v|. If it's -1, the decompiler will search the stack.
 */
extern bool
ReportIsNotFunction(JSContext* cx, HandleValue v, int numToSkip,
                    MaybeConstruct construct = NO_CONSTRUCT);

/* See ReportIsNotFunction comment for the meaning of numToSkip. */
extern JSObject*
ValueToCallable(JSContext* cx, HandleValue v, int numToSkip = -1,
                MaybeConstruct construct = NO_CONSTRUCT);

/*
 * Invoke assumes that the given args have been pushed on the top of the
 * VM stack.
 */
extern bool
Invoke(JSContext* cx, const CallArgs& args, MaybeConstruct construct = NO_CONSTRUCT);

/*
 * This Invoke overload places the least requirements on the caller: it may be
 * called at any time and it takes care of copying the given callee, this, and
 * arguments onto the stack.
 */
extern bool
Invoke(JSContext* cx, const Value& thisv, const Value& fval, unsigned argc, const Value* argv,
       MutableHandleValue rval);

/*
 * These helpers take care of the infinite-recursion check necessary for
 * getter/setter calls.
 */
extern bool
InvokeGetter(JSContext* cx, const Value& thisv, Value fval, MutableHandleValue rval);

extern bool
InvokeSetter(JSContext* cx, const Value& thisv, Value fval, HandleValue v);

// ES6 7.3.13 Construct(F, argumentsList, newTarget).  All parameters are
// required, hopefully forcing callers to be careful not to (say) blindly pass
// callee as |newTarget| when a different value should have been passed.
//
// NOTE: As with the ES6 spec operation, it's the caller's responsibility to
//       ensure |fval| and |newTarget| are both |IsConstructor|.
extern bool
Construct(JSContext* cx, HandleValue fval, const ConstructArgs& args, HandleValue newTarget,
          MutableHandleValue rval);

// Call Construct(fval, args, newTarget), but use the given |thisv| as |this|
// during construction of |fval|.
//
// This method exists only for very rare cases where a |this| was created
// caller-side for construction of |fval|: basically only for JITs using
// |CreateThis|.  If that's not you, use Construct()!
extern bool
InternalConstructWithProvidedThis(JSContext* cx, HandleValue fval, HandleValue thisv,
                                  const ConstructArgs& args, HandleValue newTarget,
                                  MutableHandleValue rval);

/*
 * Executes a script with the given scopeChain/this. The 'type' indicates
 * whether this is eval code or global code. To support debugging, the
 * evalFrame parameter can point to an arbitrary frame in the context's call
 * stack to simulate executing an eval in that frame.
 */
extern bool
ExecuteKernel(JSContext* cx, HandleScript script, JSObject& scopeChain,
              const Value& newTargetVal, ExecuteType type, AbstractFramePtr evalInFrame,
              Value* result);

/* Execute a script with the given scopeChain as global code. */
extern bool
Execute(JSContext* cx, HandleScript script, JSObject& scopeChain, Value* rval);

class ExecuteState;
class InvokeState;

// RunState is passed to RunScript and RunScript then eiter passes it to the
// interpreter or to the JITs. RunState contains all information we need to
// construct an interpreter or JIT frame.
class RunState
{
  protected:
    enum Kind { Execute, Invoke };
    Kind kind_;

    RootedScript script_;

    explicit RunState(JSContext* cx, Kind kind, JSScript* script)
      : kind_(kind),
        script_(cx, script)
    { }

  public:
    bool isExecute() const { return kind_ == Execute; }
    bool isInvoke() const { return kind_ == Invoke; }

    ExecuteState* asExecute() const {
        MOZ_ASSERT(isExecute());
        return (ExecuteState*)this;
    }
    InvokeState* asInvoke() const {
        MOZ_ASSERT(isInvoke());
        return (InvokeState*)this;
    }

    JS::HandleScript script() const { return script_; }

    virtual InterpreterFrame* pushInterpreterFrame(JSContext* cx) = 0;
    virtual void setReturnValue(Value v) = 0;

    bool maybeCreateThisForConstructor(JSContext* cx);

  private:
    RunState(const RunState& other) = delete;
    RunState(const ExecuteState& other) = delete;
    RunState(const InvokeState& other) = delete;
    void operator=(const RunState& other) = delete;
};

// Eval or global script.
class ExecuteState : public RunState
{
    ExecuteType type_;

    RootedValue newTargetValue_;
    RootedObject scopeChain_;

    AbstractFramePtr evalInFrame_;
    Value* result_;

  public:
    ExecuteState(JSContext* cx, JSScript* script, const Value& newTargetValue,
                 JSObject& scopeChain, ExecuteType type, AbstractFramePtr evalInFrame,
                 Value* result)
      : RunState(cx, Execute, script),
        type_(type),
        newTargetValue_(cx, newTargetValue),
        scopeChain_(cx, &scopeChain),
        evalInFrame_(evalInFrame),
        result_(result)
    { }

    Value newTarget() { return newTargetValue_; }
    JSObject* scopeChain() const { return scopeChain_; }
    ExecuteType type() const { return type_; }

    virtual InterpreterFrame* pushInterpreterFrame(JSContext* cx);

    virtual void setReturnValue(Value v) {
        if (result_)
            *result_ = v;
    }
};

// Data to invoke a function.
class InvokeState : public RunState
{
    const CallArgs& args_;
    InitialFrameFlags initial_;
    bool createSingleton_;

  public:
    InvokeState(JSContext* cx, const CallArgs& args, InitialFrameFlags initial)
      : RunState(cx, Invoke, args.callee().as<JSFunction>().nonLazyScript()),
        args_(args),
        initial_(initial),
        createSingleton_(false)
    { }

    bool createSingleton() const { return createSingleton_; }
    void setCreateSingleton() { createSingleton_ = true; }

    bool constructing() const { return InitialFrameFlagsAreConstructing(initial_); }
    const CallArgs& args() const { return args_; }

    virtual InterpreterFrame* pushInterpreterFrame(JSContext* cx);

    virtual void setReturnValue(Value v) {
        args_.rval().set(v);
    }
};

extern bool
RunScript(JSContext* cx, RunState& state);

extern bool
StrictlyEqual(JSContext* cx, HandleValue lval, HandleValue rval, bool* equal);

extern bool
LooselyEqual(JSContext* cx, HandleValue lval, HandleValue rval, bool* equal);

/* === except that NaN is the same as NaN and -0 is not the same as +0. */
extern bool
SameValue(JSContext* cx, HandleValue v1, HandleValue v2, bool* same);

extern JSType
TypeOfObject(JSObject* obj);

extern JSType
TypeOfValue(const Value& v);

extern bool
HasInstance(JSContext* cx, HandleObject obj, HandleValue v, bool* bp);

// Unwind scope chain and iterator to match the static scope corresponding to
// the given bytecode position.
extern void
UnwindScope(JSContext* cx, ScopeIter& si, jsbytecode* pc);

// Unwind all scopes.
extern void
UnwindAllScopesInFrame(JSContext* cx, ScopeIter& si);

// Compute the pc needed to unwind the scope to the beginning of the block
// pointed to by the try note.
extern jsbytecode*
UnwindScopeToTryPc(JSScript* script, JSTryNote* tn);

template <class StackDepthOp>
class MOZ_STACK_CLASS TryNoteIter
{
    RootedScript script_;
    uint32_t pcOffset_;
    JSTryNote* tn_;
    JSTryNote* tnEnd_;
    StackDepthOp getStackDepth_;

    void settle() {
        for (; tn_ != tnEnd_; ++tn_) {
            /* If pc is out of range, try the next one. */
            if (pcOffset_ - tn_->start >= tn_->length)
                continue;

            /*
             * We have a note that covers the exception pc but we must check
             * whether the interpreter has already executed the corresponding
             * handler. This is possible when the executed bytecode implements
             * break or return from inside a for-in loop.
             *
             * In this case the emitter generates additional [enditer] and [gosub]
             * opcodes to close all outstanding iterators and execute the finally
             * blocks. If such an [enditer] throws an exception, its pc can still
             * be inside several nested for-in loops and try-finally statements
             * even if we have already closed the corresponding iterators and
             * invoked the finally blocks.
             *
             * To address this, we make [enditer] always decrease the stack even
             * when its implementation throws an exception. Thus already executed
             * [enditer] and [gosub] opcodes will have try notes with the stack
             * depth exceeding the current one and this condition is what we use to
             * filter them out.
             */
            if (tn_->stackDepth <= getStackDepth_())
                break;
        }
    }

  public:
    TryNoteIter(JSContext* cx, JSScript* script, jsbytecode* pc,
                StackDepthOp getStackDepth)
      : script_(cx, script),
        pcOffset_(pc - script->main()),
        getStackDepth_(getStackDepth)
    {
        if (script->hasTrynotes()) {
            tn_ = script->trynotes()->vector;
            tnEnd_ = tn_ + script->trynotes()->length;
        } else {
            tn_ = tnEnd_ = nullptr;
        }
        settle();
    }

    void operator++() {
        ++tn_;
        settle();
    }

    bool done() const { return tn_ == tnEnd_; }
    JSTryNote* operator*() const { return tn_; }
};

bool
HandleClosingGeneratorReturn(JSContext* cx, AbstractFramePtr frame, bool ok);

/************************************************************************/

bool
Throw(JSContext* cx, HandleValue v);

bool
ThrowingOperation(JSContext* cx, HandleValue v);

bool
GetProperty(JSContext* cx, HandleValue value, HandlePropertyName name, MutableHandleValue vp);

bool
GetScopeName(JSContext* cx, HandleObject obj, HandlePropertyName name, MutableHandleValue vp);

bool
GetScopeNameForTypeOf(JSContext* cx, HandleObject obj, HandlePropertyName name,
                      MutableHandleValue vp);

JSObject*
Lambda(JSContext* cx, HandleFunction fun, HandleObject parent);

JSObject*
LambdaArrow(JSContext* cx, HandleFunction fun, HandleObject parent, HandleValue newTargetv);

bool
GetElement(JSContext* cx, MutableHandleValue lref, HandleValue rref, MutableHandleValue res);

bool
CallElement(JSContext* cx, MutableHandleValue lref, HandleValue rref, MutableHandleValue res);

bool
SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                 bool strict);
bool
SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                 bool strict, HandleScript script, jsbytecode* pc);

bool
InitElementArray(JSContext* cx, jsbytecode* pc,
                 HandleObject obj, uint32_t index, HandleValue value);

bool
AddValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res);

bool
SubValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res);

bool
MulValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res);

bool
DivValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res);

bool
ModValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res);

bool
UrshValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs, MutableHandleValue res);

bool
AtomicIsLockFree(JSContext* cx, HandleValue in, int* out);

template <bool strict>
bool
DeletePropertyJit(JSContext* ctx, HandleValue val, HandlePropertyName name, bool* bv);

template <bool strict>
bool
DeleteElementJit(JSContext* cx, HandleValue val, HandleValue index, bool* bv);

bool
DefFunOperation(JSContext* cx, HandleScript script, HandleObject scopeChain, HandleFunction funArg);

bool
ThrowMsgOperation(JSContext* cx, const unsigned errorNum);

bool
GetAndClearException(JSContext* cx, MutableHandleValue res);

bool
DeleteNameOperation(JSContext* cx, HandlePropertyName name, HandleObject scopeObj,
                    MutableHandleValue res);

bool
ImplicitThisOperation(JSContext* cx, HandleObject scopeObj, HandlePropertyName name,
                      MutableHandleValue res);

bool
RunOnceScriptPrologue(JSContext* cx, HandleScript script);

bool
InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleId id,
                          HandleObject val);

bool
InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandlePropertyName name,
                          HandleObject val);

unsigned
GetInitDataPropAttrs(JSOp op);

bool
EnterWithOperation(JSContext* cx, AbstractFramePtr frame, HandleValue val, HandleObject staticWith);


bool
InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleValue idval,
                          HandleObject val);

bool
SpreadCallOperation(JSContext* cx, HandleScript script, jsbytecode* pc, HandleValue thisv,
                    HandleValue callee, HandleValue arr, HandleValue newTarget, MutableHandleValue res);

JSObject*
NewObjectOperation(JSContext* cx, HandleScript script, jsbytecode* pc,
                   NewObjectKind newKind = GenericObject);

JSObject*
NewObjectOperationWithTemplate(JSContext* cx, HandleObject templateObject);

JSObject*
NewArrayOperation(JSContext* cx, HandleScript script, jsbytecode* pc, uint32_t length,
                  NewObjectKind newKind = GenericObject);

JSObject*
NewArrayOperationWithTemplate(JSContext* cx, HandleObject templateObject);

void
ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber, HandleId id);

void
ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber, HandlePropertyName name);

void
ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber, HandleScript script, jsbytecode* pc);

// The parser only reports redeclarations that occurs within a single
// script. Due to the extensibility of the global lexical scope, we also check
// for redeclarations during runtime in JSOP_DEF{VAR,LET,CONST}.
void
ReportRuntimeRedeclaration(JSContext* cx, HandlePropertyName name,
                           frontend::Definition::Kind declKind);

bool
ThrowUninitializedThis(JSContext* cx, AbstractFramePtr frame);

bool
DefaultClassConstructor(JSContext* cx, unsigned argc, Value* vp);

bool
DefaultDerivedClassConstructor(JSContext* cx, unsigned argc, Value* vp);

}  /* namespace js */

#endif /* vm_Interpreter_h */

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

#include "jspubtd.h"

#include "vm/Iteration.h"
#include "vm/Stack.h"

namespace js {

class EnvironmentIter;

/*
 * Convert null/undefined |thisv| into the current global object for the
 * compartment, and replace other primitives with boxed versions.
 */
extern bool
BoxNonStrictThis(JSContext* cx, HandleValue thisv, MutableHandleValue vp);

extern bool
GetFunctionThis(JSContext* cx, AbstractFramePtr frame, MutableHandleValue res);

extern void
GetNonSyntacticGlobalThis(JSContext* cx, HandleObject envChain, MutableHandleValue res);

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
 * Call or construct arguments that are stored in rooted memory.
 *
 * NOTE: Any necessary |GetThisValue| computation must have been performed on
 *       |args.thisv()|, likely by the interpreter when pushing |this| onto the
 *       stack.  If you're not sure whether |GetThisValue| processing has been
 *       performed, use |Invoke|.
 */
extern bool
InternalCallOrConstruct(JSContext* cx, const CallArgs& args,
                        MaybeConstruct construct);

/*
 * These helpers take care of the infinite-recursion check necessary for
 * getter/setter calls.
 */
extern bool
CallGetter(JSContext* cx, HandleValue thisv, HandleValue getter, MutableHandleValue rval);

extern bool
CallSetter(JSContext* cx, HandleValue thisv, HandleValue setter, HandleValue rval);

// ES7 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93 7.3.12 Call(F, V, argumentsList).
// All parameters are required, hopefully forcing callers to be careful not to
// (say) blindly pass callee as |newTarget| when a different value should have
// been passed.  Behavior is unspecified if any element of |args| isn't initialized.
//
// |rval| is written to *only* after |fval| and |thisv| have been consumed, so
// |rval| *may* alias either argument.
extern bool
Call(JSContext* cx, HandleValue fval, HandleValue thisv, const AnyInvokeArgs& args,
     MutableHandleValue rval);

inline bool
Call(JSContext* cx, HandleValue fval, HandleValue thisv, MutableHandleValue rval)
{
    FixedInvokeArgs<0> args(cx);
    return Call(cx, fval, thisv, args, rval);
}

inline bool
Call(JSContext* cx, HandleValue fval, JSObject* thisObj, MutableHandleValue rval)
{
    RootedValue thisv(cx, ObjectOrNullValue(thisObj));
    FixedInvokeArgs<0> args(cx);
    return Call(cx, fval, thisv, args, rval);
}

inline bool
Call(JSContext* cx, HandleValue fval, HandleValue thisv, HandleValue arg0, MutableHandleValue rval)
{
    FixedInvokeArgs<1> args(cx);
    args[0].set(arg0);
    return Call(cx, fval, thisv, args, rval);
}

inline bool
Call(JSContext* cx, HandleValue fval, JSObject* thisObj, HandleValue arg0,
     MutableHandleValue rval)
{
    RootedValue thisv(cx, ObjectOrNullValue(thisObj));
    FixedInvokeArgs<1> args(cx);
    args[0].set(arg0);
    return Call(cx, fval, thisv, args, rval);
}

inline bool
Call(JSContext* cx, HandleValue fval, HandleValue thisv,
     HandleValue arg0, HandleValue arg1, MutableHandleValue rval)
{
    FixedInvokeArgs<2> args(cx);
    args[0].set(arg0);
    args[1].set(arg1);
    return Call(cx, fval, thisv, args, rval);
}

inline bool
Call(JSContext* cx, HandleValue fval, JSObject* thisObj,
     HandleValue arg0, HandleValue arg1, MutableHandleValue rval)
{
    RootedValue thisv(cx, ObjectOrNullValue(thisObj));
    FixedInvokeArgs<2> args(cx);
    args[0].set(arg0);
    args[1].set(arg1);
    return Call(cx, fval, thisv, args, rval);
}

// Perform the above Call() operation using the given arguments.  Similar to
// ConstructFromStack() below, this handles |!IsCallable(args.calleev())|.
//
// This internal operation is intended only for use with arguments known to be
// on the JS stack, or at least in carefully-rooted memory. The vast majority of
// potential users should instead use InvokeArgs in concert with Call().
extern bool
CallFromStack(JSContext* cx, const CallArgs& args);

// ES6 7.3.13 Construct(F, argumentsList, newTarget).  All parameters are
// required, hopefully forcing callers to be careful not to (say) blindly pass
// callee as |newTarget| when a different value should have been passed.
// Behavior is unspecified if any element of |args| isn't initialized.
//
// |rval| is written to *only* after |fval| and |newTarget| have been consumed,
// so |rval| *may* alias either argument.
//
// NOTE: As with the ES6 spec operation, it's the caller's responsibility to
//       ensure |fval| and |newTarget| are both |IsConstructor|.
extern bool
Construct(JSContext* cx, HandleValue fval, const AnyConstructArgs& args, HandleValue newTarget,
          MutableHandleObject objp);

// Check that in the given |args|, which must be |args.isConstructing()|, that
// |IsConstructor(args.callee())|. If this is not the case, throw a TypeError.
// Otherwise, the user must ensure that, additionally, |IsConstructor(args.newTarget())|.
// (If |args| comes directly from the interpreter stack, as set up by JSOP_NEW,
// this comes for free.) Then perform a Construct() operation using |args|.
//
// This internal operation is intended only for use with arguments known to be
// on the JS stack, or at least in carefully-rooted memory. The vast majority of
// potential users should instead use ConstructArgs in concert with Construct().
extern bool
ConstructFromStack(JSContext* cx, const CallArgs& args);

// Call Construct(fval, args, newTarget), but use the given |thisv| as |this|
// during construction of |fval|.
//
// |rval| is written to *only* after |fval|, |thisv|, and |newTarget| have been
// consumed, so |rval| *may* alias any of these arguments.
//
// This method exists only for very rare cases where a |this| was created
// caller-side for construction of |fval|: basically only for JITs using
// |CreateThis|.  If that's not you, use Construct()!
extern bool
InternalConstructWithProvidedThis(JSContext* cx, HandleValue fval, HandleValue thisv,
                                  const AnyConstructArgs& args, HandleValue newTarget,
                                  MutableHandleValue rval);

/*
 * Executes a script with the given scopeChain/this. The 'type' indicates
 * whether this is eval code or global code. To support debugging, the
 * evalFrame parameter can point to an arbitrary frame in the context's call
 * stack to simulate executing an eval in that frame.
 */
extern bool
ExecuteKernel(JSContext* cx, HandleScript script, JSObject& scopeChain,
              const Value& newTargetVal, AbstractFramePtr evalInFrame, Value* result);

/* Execute a script with the given scopeChain as global code. */
extern bool
Execute(JSContext* cx, HandleScript script, JSObject& scopeChain, Value* rval);

class ExecuteState;
class InvokeState;

// RunState is passed to RunScript and RunScript then either passes it to the
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

    InterpreterFrame* pushInterpreterFrame(JSContext* cx);
    inline void setReturnValue(const Value& v);

  private:
    RunState(const RunState& other) = delete;
    RunState(const ExecuteState& other) = delete;
    RunState(const InvokeState& other) = delete;
    void operator=(const RunState& other) = delete;
};

// Eval or global script.
class ExecuteState : public RunState
{
    RootedValue newTargetValue_;
    RootedObject envChain_;

    AbstractFramePtr evalInFrame_;
    Value* result_;

  public:
    ExecuteState(JSContext* cx, JSScript* script, const Value& newTargetValue,
                 JSObject& envChain, AbstractFramePtr evalInFrame, Value* result)
      : RunState(cx, Execute, script),
        newTargetValue_(cx, newTargetValue),
        envChain_(cx, &envChain),
        evalInFrame_(evalInFrame),
        result_(result)
    { }

    Value newTarget() const { return newTargetValue_; }
    void setNewTarget(const Value& v) { newTargetValue_ = v; }
    Value* addressOfNewTarget() { return newTargetValue_.address(); }

    JSObject* environmentChain() const { return envChain_; }
    bool isDebuggerEval() const { return !!evalInFrame_; }

    InterpreterFrame* pushInterpreterFrame(JSContext* cx);

    void setReturnValue(const Value& v) {
        if (result_)
            *result_ = v;
    }
};

// Data to invoke a function.
class InvokeState final : public RunState
{
    const CallArgs& args_;
    MaybeConstruct construct_;

  public:
    InvokeState(JSContext* cx, const CallArgs& args, MaybeConstruct construct)
      : RunState(cx, Invoke, args.callee().as<JSFunction>().nonLazyScript()),
        args_(args),
        construct_(construct)
    { }

    bool constructing() const { return construct_; }
    const CallArgs& args() const { return args_; }

    InterpreterFrame* pushInterpreterFrame(JSContext* cx);

    void setReturnValue(const Value& v) {
        args_.rval().set(v);
    }
};

inline void
RunState::setReturnValue(const Value& v)
{
    if (isInvoke())
        asInvoke()->setReturnValue(v);
    else
        asExecute()->setReturnValue(v);
}

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
InstanceOfOperator(JSContext* cx, HandleObject obj, HandleValue v, bool* bp);

extern bool
HasInstance(JSContext* cx, HandleObject obj, HandleValue v, bool* bp);

// Unwind environment chain and iterator to match the scope corresponding to
// the given bytecode position.
extern void
UnwindEnvironment(JSContext* cx, EnvironmentIter& ei, jsbytecode* pc);

// Unwind all environments.
extern void
UnwindAllEnvironmentsInFrame(JSContext* cx, EnvironmentIter& ei);

// Compute the pc needed to unwind the scope to the beginning of the block
// pointed to by the try note.
extern jsbytecode*
UnwindEnvironmentToTryPc(JSScript* script, JSTryNote* tn);

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
SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                 HandleValue receiver, bool strict);
bool
SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index, HandleValue value,
                 HandleValue receiver, bool strict, HandleScript script, jsbytecode* pc);

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
DefFunOperation(JSContext* cx, HandleScript script, HandleObject envChain, HandleFunction funArg);

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
EnterWithOperation(JSContext* cx, AbstractFramePtr frame, HandleValue val,
                   Handle<WithScope*> scope);


bool
InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleValue idval,
                          HandleObject val);

bool
SpreadCallOperation(JSContext* cx, HandleScript script, jsbytecode* pc, HandleValue thisv,
                    HandleValue callee, HandleValue arr, HandleValue newTarget, MutableHandleValue res);

bool
OptimizeSpreadCall(JSContext* cx, HandleValue arg, bool* optimized);

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

void
ReportInNotObjectError(JSContext* cx, HandleValue lref, int lindex, HandleValue rref, int rindex);

// The parser only reports redeclarations that occurs within a single
// script. Due to the extensibility of the global lexical scope, we also check
// for redeclarations during runtime in JSOP_DEF{VAR,LET,CONST}.
void
ReportRuntimeRedeclaration(JSContext* cx, HandlePropertyName name, const char* redeclKind);

enum class CheckIsObjectKind : uint8_t {
    IteratorNext,
    IteratorReturn,
    IteratorThrow,
    GetIterator,
    GetAsyncIterator
};

bool
ThrowCheckIsObject(JSContext* cx, CheckIsObjectKind kind);

enum class CheckIsCallableKind : uint8_t {
    IteratorReturn
};

bool
ThrowCheckIsCallable(JSContext* cx, CheckIsCallableKind kind);

bool
ThrowUninitializedThis(JSContext* cx, AbstractFramePtr frame);

bool
ThrowInitializedThis(JSContext* cx);

bool
DefaultClassConstructor(JSContext* cx, unsigned argc, Value* vp);

bool
Debug_CheckSelfHosted(JSContext* cx, HandleValue v);

bool
CheckClassHeritageOperation(JSContext* cx, HandleValue heritage);

JSObject*
ObjectWithProtoOperation(JSContext* cx, HandleValue proto);

JSObject*
FunWithProtoOperation(JSContext* cx, HandleFunction fun, HandleObject parent, HandleObject proto);

JSFunction*
MakeDefaultConstructor(JSContext* cx, HandleScript script, jsbytecode* pc, HandleObject proto);

JSObject*
HomeObjectSuperBase(JSContext* cx, HandleObject homeObj);

JSObject*
SuperFunOperation(JSContext* cx, HandleObject callee);

bool
SetPropertySuper(JSContext* cx, HandleObject obj, HandleValue receiver,
                 HandlePropertyName id, HandleValue rval, bool strict);

}  /* namespace js */

#endif /* vm_Interpreter_h */

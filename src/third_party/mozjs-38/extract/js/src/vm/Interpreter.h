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

extern JSObject*
BoxNonStrictThis(JSContext* cx, HandleValue thisv);

/*
 * Ensure that fp->thisValue() is the correct value of |this| for the scripted
 * call represented by |fp|. ComputeThis is necessary because fp->thisValue()
 * may be set to 'undefined' when 'this' should really be the global object (as
 * an optimization to avoid global-this computation).
 */
inline bool
ComputeThis(JSContext* cx, AbstractFramePtr frame);

enum MaybeConstruct {
    NO_CONSTRUCT = INITIAL_NONE,
    CONSTRUCT = INITIAL_CONSTRUCT
};

/*
 * numToSkip is the number of stack values the expression decompiler should skip
 * before it reaches |v|. If it's -1, the decompiler will search the stack.
 */
extern bool
ReportIsNotFunction(JSContext* cx, HandleValue v, int numToSkip = -1,
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
Invoke(JSContext* cx, CallArgs args, MaybeConstruct construct = NO_CONSTRUCT);

/*
 * This Invoke overload places the least requirements on the caller: it may be
 * called at any time and it takes care of copying the given callee, this, and
 * arguments onto the stack.
 */
extern bool
Invoke(JSContext* cx, const Value& thisv, const Value& fval, unsigned argc, const Value* argv,
       MutableHandleValue rval);

/*
 * This helper takes care of the infinite-recursion check necessary for
 * getter/setter calls.
 */
extern bool
InvokeGetterOrSetter(JSContext* cx, JSObject* obj, Value fval, unsigned argc, Value* argv,
                     MutableHandleValue rval);

/*
 * InvokeConstructor implement a function call from a constructor context
 * (e.g. 'new') handling the the creation of the new 'this' object.
 */
extern bool
InvokeConstructor(JSContext* cx, CallArgs args);

/* See the fval overload of Invoke. */
extern bool
InvokeConstructor(JSContext* cx, Value fval, unsigned argc, const Value* argv,
                  MutableHandleValue rval);

/*
 * Executes a script with the given scopeChain/this. The 'type' indicates
 * whether this is eval code or global code. To support debugging, the
 * evalFrame parameter can point to an arbitrary frame in the context's call
 * stack to simulate executing an eval in that frame.
 */
extern bool
ExecuteKernel(JSContext* cx, HandleScript script, JSObject& scopeChain, const Value& thisv,
              ExecuteType type, AbstractFramePtr evalInFrame, Value* result);

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

    RootedValue thisv_;
    RootedObject scopeChain_;

    AbstractFramePtr evalInFrame_;
    Value* result_;

  public:
    ExecuteState(JSContext* cx, JSScript* script, const Value& thisv, JSObject& scopeChain,
                 ExecuteType type, AbstractFramePtr evalInFrame, Value* result)
      : RunState(cx, Execute, script),
        type_(type),
        thisv_(cx, thisv),
        scopeChain_(cx, &scopeChain),
        evalInFrame_(evalInFrame),
        result_(result)
    { }

    Value* addressOfThisv() { return thisv_.address(); }
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
    CallArgs& args_;
    InitialFrameFlags initial_;
    bool createSingleton_;

  public:
    InvokeState(JSContext* cx, CallArgs& args, InitialFrameFlags initial)
      : RunState(cx, Invoke, args.callee().as<JSFunction>().nonLazyScript()),
        args_(args),
        initial_(initial),
        createSingleton_(false)
    { }

    bool createSingleton() const { return createSingleton_; }
    void setCreateSingleton() { createSingleton_ = true; }

    bool constructing() const { return InitialFrameFlagsAreConstructing(initial_); }
    CallArgs& args() const { return args_; }

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

/*
 * Unwind for an uncatchable exception. This means not running finalizers, etc;
 * just preserving the basic engine stack invariants.
 */
extern void
UnwindForUncatchableException(JSContext* cx, const InterpreterRegs& regs);

extern bool
OnUnknownMethod(JSContext* cx, HandleObject obj, Value idval, MutableHandleValue vp);

class TryNoteIter
{
    const InterpreterRegs& regs;
    RootedScript script; /* TryNotIter is always stack allocated. */
    uint32_t pcOffset;
    JSTryNote* tn, *tnEnd;

    void settle();

  public:
    explicit TryNoteIter(JSContext* cx, const InterpreterRegs& regs);
    bool done() const;
    void operator++();
    JSTryNote* operator*() const { return tn; }
};

/************************************************************************/

bool
Throw(JSContext* cx, HandleValue v);

bool
ThrowingOperation(JSContext* cx, HandleValue v);

bool
GetProperty(JSContext* cx, HandleValue value, HandlePropertyName name, MutableHandleValue vp);

bool
CallProperty(JSContext* cx, HandleValue value, HandlePropertyName name, MutableHandleValue vp);

bool
GetScopeName(JSContext* cx, HandleObject obj, HandlePropertyName name, MutableHandleValue vp);

bool
GetScopeNameForTypeOf(JSContext* cx, HandleObject obj, HandlePropertyName name,
                      MutableHandleValue vp);

JSObject*
Lambda(JSContext* cx, HandleFunction fun, HandleObject parent);

JSObject*
LambdaArrow(JSContext* cx, HandleFunction fun, HandleObject parent, HandleValue thisv);

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

template <bool strict>
bool
DeleteProperty(JSContext* ctx, HandleValue val, HandlePropertyName name, bool* bv);

template <bool strict>
bool
DeleteElement(JSContext* cx, HandleValue val, HandleValue index, bool* bv);

bool
DefFunOperation(JSContext* cx, HandleScript script, HandleObject scopeChain, HandleFunction funArg);

bool
SetCallOperation(JSContext* cx);

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

bool
EnterWithOperation(JSContext* cx, AbstractFramePtr frame, HandleValue val, HandleObject staticWith);


bool
InitGetterSetterOperation(JSContext* cx, jsbytecode* pc, HandleObject obj, HandleValue idval,
                          HandleObject val);

bool
SpreadCallOperation(JSContext* cx, HandleScript script, jsbytecode* pc, HandleValue thisv,
                    HandleValue callee, HandleValue arr, MutableHandleValue res);

inline bool
SetConstOperation(JSContext* cx, HandleObject varobj, HandlePropertyName name, HandleValue rval)
{
    return DefineProperty(cx, varobj, name, rval, nullptr, nullptr,
                          JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_READONLY);
}

void
ReportUninitializedLexical(JSContext* cx, HandlePropertyName name);

void
ReportUninitializedLexical(JSContext* cx, HandleScript script, jsbytecode* pc);

void
ReportUninitializedLexical(JSContext* cx, HandleScript script, jsbytecode* pc, ScopeCoordinate sc);

}  /* namespace js */

#endif /* vm_Interpreter_h */

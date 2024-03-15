/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Interpreter_h
#define vm_Interpreter_h

/*
 * JS interpreter interface.
 */

#include "jspubtd.h"

#include "vm/BuiltinObjectKind.h"
#include "vm/CheckIsObjectKind.h"  // CheckIsObjectKind
#include "vm/Stack.h"

namespace js {

class WithScope;
class EnvironmentIter;
class PlainObject;

/*
 * Convert null/undefined |thisv| into the global lexical's |this| object, and
 * replace other primitives with boxed versions.
 */
extern JSObject* BoxNonStrictThis(JSContext* cx, HandleValue thisv);

extern bool GetFunctionThis(JSContext* cx, AbstractFramePtr frame,
                            MutableHandleValue res);

extern void GetNonSyntacticGlobalThis(JSContext* cx, HandleObject envChain,
                                      MutableHandleValue res);

/*
 * numToSkip is the number of stack values the expression decompiler should skip
 * before it reaches |v|. If it's -1, the decompiler will search the stack.
 */
extern bool ReportIsNotFunction(JSContext* cx, HandleValue v, int numToSkip,
                                MaybeConstruct construct = NO_CONSTRUCT);

/* See ReportIsNotFunction comment for the meaning of numToSkip. */
extern JSObject* ValueToCallable(JSContext* cx, HandleValue v,
                                 int numToSkip = -1,
                                 MaybeConstruct construct = NO_CONSTRUCT);

// Reasons why a call could be performed, for passing onto the debugger's
// `onNativeCall` hook.
// `onNativeCall` hook disabled all JITs, and this needs to be handled only in
// the interpreter.
enum class CallReason {
  Call,
  // callContentFunction or constructContentFunction in self-hosted JS.
  CallContent,
  // Function.prototype.call or Function.prototype.apply.
  FunCall,
  Getter,
  Setter,
};

/*
 * Call or construct arguments that are stored in rooted memory.
 *
 * NOTE: Any necessary |GetThisValue| computation must have been performed on
 *       |args.thisv()|, likely by the interpreter when pushing |this| onto the
 *       stack.  If you're not sure whether |GetThisValue| processing has been
 *       performed, use |Invoke|.
 */
extern bool InternalCallOrConstruct(JSContext* cx, const CallArgs& args,
                                    MaybeConstruct construct,
                                    CallReason reason = CallReason::Call);

/*
 * These helpers take care of the infinite-recursion check necessary for
 * getter/setter calls.
 */
extern bool CallGetter(JSContext* cx, HandleValue thisv, HandleValue getter,
                       MutableHandleValue rval);

extern bool CallSetter(JSContext* cx, HandleValue thisv, HandleValue setter,
                       HandleValue rval);

// ES7 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 7.3.12 Call(F, V, argumentsList).
// All parameters are required, hopefully forcing callers to be careful not to
// (say) blindly pass callee as |newTarget| when a different value should have
// been passed.  Behavior is unspecified if any element of |args| isn't
// initialized.
//
// |rval| is written to *only* after |fval| and |thisv| have been consumed, so
// |rval| *may* alias either argument.
extern bool Call(JSContext* cx, HandleValue fval, HandleValue thisv,
                 const AnyInvokeArgs& args, MutableHandleValue rval,
                 CallReason reason = CallReason::Call);

inline bool Call(JSContext* cx, HandleValue fval, HandleValue thisv,
                 MutableHandleValue rval) {
  FixedInvokeArgs<0> args(cx);
  return Call(cx, fval, thisv, args, rval);
}

inline bool Call(JSContext* cx, HandleValue fval, JSObject* thisObj,
                 MutableHandleValue rval) {
  RootedValue thisv(cx, ObjectOrNullValue(thisObj));
  FixedInvokeArgs<0> args(cx);
  return Call(cx, fval, thisv, args, rval);
}

inline bool Call(JSContext* cx, HandleValue fval, HandleValue thisv,
                 HandleValue arg0, MutableHandleValue rval) {
  FixedInvokeArgs<1> args(cx);
  args[0].set(arg0);
  return Call(cx, fval, thisv, args, rval);
}

inline bool Call(JSContext* cx, HandleValue fval, JSObject* thisObj,
                 HandleValue arg0, MutableHandleValue rval) {
  RootedValue thisv(cx, ObjectOrNullValue(thisObj));
  FixedInvokeArgs<1> args(cx);
  args[0].set(arg0);
  return Call(cx, fval, thisv, args, rval);
}

inline bool Call(JSContext* cx, HandleValue fval, HandleValue thisv,
                 HandleValue arg0, HandleValue arg1, MutableHandleValue rval) {
  FixedInvokeArgs<2> args(cx);
  args[0].set(arg0);
  args[1].set(arg1);
  return Call(cx, fval, thisv, args, rval);
}

inline bool Call(JSContext* cx, HandleValue fval, JSObject* thisObj,
                 HandleValue arg0, HandleValue arg1, MutableHandleValue rval) {
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
extern bool CallFromStack(JSContext* cx, const CallArgs& args,
                          CallReason reason = CallReason::Call);

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
extern bool Construct(JSContext* cx, HandleValue fval,
                      const AnyConstructArgs& args, HandleValue newTarget,
                      MutableHandleObject objp);

// Check that in the given |args|, which must be |args.isConstructing()|, that
// |IsConstructor(args.callee())|. If this is not the case, throw a TypeError.
// Otherwise, the user must ensure that, additionally,
// |IsConstructor(args.newTarget())|. (If |args| comes directly from the
// interpreter stack, as set up by JSOp::New, this comes for free.) Then perform
// a Construct() operation using |args|.
//
// This internal operation is intended only for use with arguments known to be
// on the JS stack, or at least in carefully-rooted memory. The vast majority of
// potential users should instead use ConstructArgs in concert with Construct().
extern bool ConstructFromStack(JSContext* cx, const CallArgs& args,
                               CallReason reason = CallReason::Call);

// Call Construct(fval, args, newTarget), but use the given |thisv| as |this|
// during construction of |fval|.
//
// |rval| is written to *only* after |fval|, |thisv|, and |newTarget| have been
// consumed, so |rval| *may* alias any of these arguments.
//
// This method exists only for very rare cases where a |this| was created
// caller-side for construction of |fval|: basically only for JITs using
// |CreateThis|.  If that's not you, use Construct()!
extern bool InternalConstructWithProvidedThis(JSContext* cx, HandleValue fval,
                                              HandleValue thisv,
                                              const AnyConstructArgs& args,
                                              HandleValue newTarget,
                                              MutableHandleValue rval);

/*
 * Executes a script with the given envChain. To support debugging, the
 * evalInFrame parameter can point to an arbitrary frame in the context's call
 * stack to simulate executing an eval in that frame.
 */
extern bool ExecuteKernel(JSContext* cx, HandleScript script,
                          HandleObject envChainArg,
                          AbstractFramePtr evalInFrame,
                          MutableHandleValue result);

/* Execute a script with the given envChain as global code. */
extern bool Execute(JSContext* cx, HandleScript script, HandleObject envChain,
                    MutableHandleValue rval);

class ExecuteState;
class InvokeState;

// RunState is passed to RunScript and RunScript then either passes it to the
// interpreter or to the JITs. RunState contains all information we need to
// construct an interpreter or JIT frame.
class MOZ_RAII RunState {
 protected:
  enum Kind { Execute, Invoke };
  Kind kind_;

  RootedScript script_;

  explicit RunState(JSContext* cx, Kind kind, JSScript* script)
      : kind_(kind), script_(cx, script) {}

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
class MOZ_RAII ExecuteState : public RunState {
  HandleObject envChain_;

  AbstractFramePtr evalInFrame_;
  MutableHandleValue result_;

 public:
  ExecuteState(JSContext* cx, JSScript* script, HandleObject envChain,
               AbstractFramePtr evalInFrame, MutableHandleValue result)
      : RunState(cx, Execute, script),
        envChain_(envChain),
        evalInFrame_(evalInFrame),
        result_(result) {}

  JSObject* environmentChain() const { return envChain_; }
  bool isDebuggerEval() const { return !!evalInFrame_; }

  InterpreterFrame* pushInterpreterFrame(JSContext* cx);

  void setReturnValue(const Value& v) { result_.set(v); }
};

// Data to invoke a function.
class MOZ_RAII InvokeState final : public RunState {
  const CallArgs& args_;
  MaybeConstruct construct_;

 public:
  InvokeState(JSContext* cx, const CallArgs& args, MaybeConstruct construct)
      : RunState(cx, Invoke, args.callee().as<JSFunction>().nonLazyScript()),
        args_(args),
        construct_(construct) {}

  bool constructing() const { return construct_; }
  const CallArgs& args() const { return args_; }

  InterpreterFrame* pushInterpreterFrame(JSContext* cx);

  void setReturnValue(const Value& v) { args_.rval().set(v); }
};

inline void RunState::setReturnValue(const Value& v) {
  if (isInvoke()) {
    asInvoke()->setReturnValue(v);
  } else {
    asExecute()->setReturnValue(v);
  }
}

extern bool RunScript(JSContext* cx, RunState& state);
extern bool Interpret(JSContext* cx, RunState& state);

extern JSType TypeOfObject(JSObject* obj);

extern JSType TypeOfValue(const Value& v);

// Implementation of
// https://www.ecma-international.org/ecma-262/6.0/#sec-instanceofoperator
extern bool InstanceofOperator(JSContext* cx, HandleObject obj, HandleValue v,
                               bool* bp);

// Unwind environment chain and iterator to match the scope corresponding to
// the given bytecode position.
extern void UnwindEnvironment(JSContext* cx, EnvironmentIter& ei,
                              jsbytecode* pc);

// Unwind all environments.
extern void UnwindAllEnvironmentsInFrame(JSContext* cx, EnvironmentIter& ei);

// Compute the pc needed to unwind the scope to the beginning of the block
// pointed to by the try note.
extern jsbytecode* UnwindEnvironmentToTryPc(JSScript* script,
                                            const TryNote* tn);

namespace detail {

template <class TryNoteFilter>
class MOZ_STACK_CLASS BaseTryNoteIter {
  uint32_t pcOffset_;
  TryNoteFilter isTryNoteValid_;

  const TryNote* tn_;
  const TryNote* tnEnd_;

  void settle() {
    for (; tn_ != tnEnd_; ++tn_) {
      if (!pcInRange()) {
        continue;
      }

      /*  Try notes cannot be disjoint. That is, we can't have
       *  multiple notes with disjoint pc ranges jumping to the same
       *  catch block. This interacts awkwardly with for-of loops, in
       *  which calls to IteratorClose emitted due to abnormal
       *  completion (break, throw, return) are emitted inline, at the
       *  source location of the break, throw, or return
       *  statement. For example:
       *
       *      for (x of iter) {
       *        try { return; } catch (e) { }
       *      }
       *
       *  From the try-note nesting's perspective, the IteratorClose
       *  resulting from |return| is covered by the inner try, when it
       *  should not be. If IteratorClose throws, we don't want to
       *  catch it here.
       *
       *  To make this work, we use TryNoteKind::ForOfIterClose try-notes,
       *  which cover the range of the abnormal completion. When
       *  looking up trynotes, a for-of iterclose note indicates that
       *  the enclosing for-of has just been terminated. As a result,
       *  trynotes within that for-of are no longer active. When we
       *  see a for-of-iterclose, we skip ahead in the trynotes list
       *  until we see the matching for-of.
       *
       *  Breaking out of multiple levels of for-of at once is handled
       *  using nested FOR_OF_ITERCLOSE try-notes. Consider this code:
       *
       *  try {
       *    loop: for (i of first) {
       *      <A>
       *      for (j of second) {
       *        <B>
       *        break loop; // <C1/2>
       *      }
       *    }
       *  } catch {...}
       *
       *  Here is the mapping from various PCs to try-notes that we
       *  want to return:
       *
       *        A     B     C1     C2
       *        |     |     |      |
       *        |     |     |  [---|---]     ForOfIterClose (outer)
       *        |     | [---|------|---]     ForOfIterClose (inner)
       *        |  [--X-----|------|----]    ForOf (inner)
       *    [---X-----------X------|-----]   ForOf (outer)
       *  [------------------------X------]  TryCatch
       *
       *  - At A, we find the outer for-of.
       *  - At B, we find the inner for-of.
       *  - At C1, we find one FOR_OF_ITERCLOSE, skip past one FOR_OF, and find
       *    the outer for-of. (This occurs if an exception is thrown while
       *    closing the inner iterator.)
       *  - At C2, we find two FOR_OF_ITERCLOSE, skip past two FOR_OF, and reach
       *    the outer try-catch. (This occurs if an exception is thrown while
       *    closing the outer iterator.)
       */
      if (tn_->kind() == TryNoteKind::ForOfIterClose) {
        uint32_t iterCloseDepth = 1;
        do {
          ++tn_;
          MOZ_ASSERT(tn_ != tnEnd_);
          if (pcInRange()) {
            if (tn_->kind() == TryNoteKind::ForOfIterClose) {
              iterCloseDepth++;
            } else if (tn_->kind() == TryNoteKind::ForOf) {
              iterCloseDepth--;
            }
          }
        } while (iterCloseDepth > 0);

        // Advance to trynote following the enclosing for-of.
        continue;
      }

      /*
       * We have a note that covers the exception pc but we must check
       * whether the interpreter has already executed the corresponding
       * handler. This is possible when the executed bytecode implements
       * break or return from inside a for-in loop.
       *
       * In this case the emitter generates additional [enditer] and [goto]
       * opcodes to close all outstanding iterators and execute the finally
       * blocks. If such an [enditer] throws an exception, its pc can still
       * be inside several nested for-in loops and try-finally statements
       * even if we have already closed the corresponding iterators and
       * invoked the finally blocks.
       *
       * To address this, we make [enditer] always decrease the stack even
       * when its implementation throws an exception. Thus already executed
       * [enditer] and [goto] opcodes will have try notes with the stack
       * depth exceeding the current one and this condition is what we use to
       * filter them out.
       */
      if (tn_ == tnEnd_ || isTryNoteValid_(tn_)) {
        return;
      }
    }
  }

 public:
  BaseTryNoteIter(JSScript* script, jsbytecode* pc,
                  TryNoteFilter isTryNoteValid)
      : pcOffset_(script->pcToOffset(pc)), isTryNoteValid_(isTryNoteValid) {
    // NOTE: The Span is a temporary so we can't use begin()/end()
    // here or the iterator will outlive the span.
    auto trynotes = script->trynotes();
    tn_ = trynotes.data();
    tnEnd_ = tn_ + trynotes.size();

    settle();
  }

  void operator++() {
    ++tn_;
    settle();
  }

  bool pcInRange() const {
    // This checks both ends of the range at once
    // because unsigned integers wrap on underflow.
    uint32_t offset = pcOffset_;
    uint32_t start = tn_->start;
    uint32_t length = tn_->length;
    return offset - start < length;
  }
  bool done() const { return tn_ == tnEnd_; }
  const TryNote* operator*() const { return tn_; }
};

}  // namespace detail

template <class TryNoteFilter>
class MOZ_STACK_CLASS TryNoteIter
    : public detail::BaseTryNoteIter<TryNoteFilter> {
  using Base = detail::BaseTryNoteIter<TryNoteFilter>;

  // Keep the script alive as long as the iterator is live.
  RootedScript script_;

 public:
  TryNoteIter(JSContext* cx, JSScript* script, jsbytecode* pc,
              TryNoteFilter isTryNoteValid)
      : Base(script, pc, isTryNoteValid), script_(cx, script) {}
};

class NoOpTryNoteFilter {
 public:
  explicit NoOpTryNoteFilter() = default;
  bool operator()(const TryNote*) { return true; }
};

// Iterator over all try notes. Code using this iterator is not allowed to
// trigger GC to make sure the script stays alive. See TryNoteIter above for the
// can-GC version.
class MOZ_STACK_CLASS TryNoteIterAllNoGC
    : public detail::BaseTryNoteIter<NoOpTryNoteFilter> {
  using Base = detail::BaseTryNoteIter<NoOpTryNoteFilter>;
  JS::AutoCheckCannotGC nogc;

 public:
  TryNoteIterAllNoGC(JSScript* script, jsbytecode* pc)
      : Base(script, pc, NoOpTryNoteFilter()) {}
};

bool HandleClosingGeneratorReturn(JSContext* cx, AbstractFramePtr frame,
                                  bool ok);

/************************************************************************/

bool ThrowOperation(JSContext* cx, HandleValue v);

bool GetProperty(JSContext* cx, HandleValue value, Handle<PropertyName*> name,
                 MutableHandleValue vp);

JSObject* Lambda(JSContext* cx, HandleFunction fun, HandleObject parent);

bool SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index,
                      HandleValue value, bool strict);

bool SetObjectElementWithReceiver(JSContext* cx, HandleObject obj,
                                  HandleValue index, HandleValue value,
                                  HandleValue receiver, bool strict);

bool AddValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
               MutableHandleValue res);

bool SubValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
               MutableHandleValue res);

bool MulValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
               MutableHandleValue res);

bool DivValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
               MutableHandleValue res);

bool ModValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
               MutableHandleValue res);

bool PowValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
               MutableHandleValue res);

bool BitNot(JSContext* cx, MutableHandleValue in, MutableHandleValue res);

bool BitXor(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
            MutableHandleValue res);

bool BitOr(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
           MutableHandleValue res);

bool BitAnd(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
            MutableHandleValue res);

bool BitLsh(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
            MutableHandleValue res);

bool BitRsh(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
            MutableHandleValue res);

bool UrshValues(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
                MutableHandleValue res);

bool LessThan(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
              bool* res);

bool LessThanOrEqual(JSContext* cx, MutableHandleValue lhs,
                     MutableHandleValue rhs, bool* res);

bool GreaterThan(JSContext* cx, MutableHandleValue lhs, MutableHandleValue rhs,
                 bool* res);

bool GreaterThanOrEqual(JSContext* cx, MutableHandleValue lhs,
                        MutableHandleValue rhs, bool* res);

bool AtomicIsLockFree(JSContext* cx, HandleValue in, int* out);

template <bool strict>
bool DelPropOperation(JSContext* cx, HandleValue val,
                      Handle<PropertyName*> name, bool* res);

template <bool strict>
bool DelElemOperation(JSContext* cx, HandleValue val, HandleValue index,
                      bool* res);

JSObject* BindVarOperation(JSContext* cx, JSObject* envChain);

JSObject* ImportMetaOperation(JSContext* cx, HandleScript script);

JSObject* BuiltinObjectOperation(JSContext* cx, BuiltinObjectKind kind);

bool ThrowMsgOperation(JSContext* cx, const unsigned throwMsgKind);

bool GetAndClearException(JSContext* cx, MutableHandleValue res);

bool GetAndClearExceptionAndStack(JSContext* cx, MutableHandleValue res,
                                  MutableHandle<SavedFrame*> stack);

bool DeleteNameOperation(JSContext* cx, Handle<PropertyName*> name,
                         HandleObject scopeObj, MutableHandleValue res);

bool ImplicitThisOperation(JSContext* cx, HandleObject scopeObj,
                           Handle<PropertyName*> name, MutableHandleValue res);

bool InitPropGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                   HandleObject obj, Handle<PropertyName*> name,
                                   HandleObject val);

unsigned GetInitDataPropAttrs(JSOp op);

bool EnterWithOperation(JSContext* cx, AbstractFramePtr frame, HandleValue val,
                        Handle<WithScope*> scope);

bool InitElemGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                   HandleObject obj, HandleValue idval,
                                   HandleObject val);

bool SpreadCallOperation(JSContext* cx, HandleScript script, jsbytecode* pc,
                         HandleValue thisv, HandleValue callee, HandleValue arr,
                         HandleValue newTarget, MutableHandleValue res);

bool OptimizeSpreadCall(JSContext* cx, HandleValue arg,
                        MutableHandleValue result);

ArrayObject* ArrayFromArgumentsObject(JSContext* cx,
                                      Handle<ArgumentsObject*> args);

JSObject* NewObjectOperation(JSContext* cx, HandleScript script,
                             const jsbytecode* pc);

JSObject* NewPlainObjectBaselineFallback(JSContext* cx,
                                         Handle<SharedShape*> shape,
                                         gc::AllocKind allocKind,
                                         gc::AllocSite* site);

JSObject* NewPlainObjectOptimizedFallback(JSContext* cx,
                                          Handle<SharedShape*> shape,
                                          gc::AllocKind allocKind,
                                          gc::Heap initialHeap);

ArrayObject* NewArrayOperation(JSContext* cx, uint32_t length,
                               NewObjectKind newKind = GenericObject);

// Called from JIT code when inline array allocation fails.
ArrayObject* NewArrayObjectBaselineFallback(JSContext* cx, uint32_t length,
                                            gc::AllocKind allocKind,
                                            gc::AllocSite* site);
ArrayObject* NewArrayObjectOptimizedFallback(JSContext* cx, uint32_t length,
                                             gc::AllocKind allocKind,
                                             NewObjectKind newKind);

[[nodiscard]] bool GetImportOperation(JSContext* cx, HandleObject envChain,
                                      HandleScript script, jsbytecode* pc,
                                      MutableHandleValue vp);

void ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                               HandleId id);

void ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                               Handle<PropertyName*> name);

void ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                               HandleScript script, jsbytecode* pc);

void ReportInNotObjectError(JSContext* cx, HandleValue lref, HandleValue rref);

// The parser only reports redeclarations that occurs within a single
// script. Due to the extensibility of the global lexical scope, we also check
// for redeclarations during runtime in JSOp::GlobalOrEvalDeclInstantation.
void ReportRuntimeRedeclaration(JSContext* cx, Handle<PropertyName*> name,
                                const char* redeclKind);

bool ThrowCheckIsObject(JSContext* cx, CheckIsObjectKind kind);

bool ThrowUninitializedThis(JSContext* cx);

bool ThrowInitializedThis(JSContext* cx);

bool ThrowObjectCoercible(JSContext* cx, HandleValue value);

bool DefaultClassConstructor(JSContext* cx, unsigned argc, Value* vp);

bool Debug_CheckSelfHosted(JSContext* cx, HandleValue funVal);

bool CheckClassHeritageOperation(JSContext* cx, HandleValue heritage);

PlainObject* ObjectWithProtoOperation(JSContext* cx, HandleValue proto);

JSObject* FunWithProtoOperation(JSContext* cx, HandleFunction fun,
                                HandleObject parent, HandleObject proto);

bool SetPropertySuper(JSContext* cx, HandleValue lval, HandleValue receiver,
                      Handle<PropertyName*> name, HandleValue rval,
                      bool strict);

bool SetElementSuper(JSContext* cx, HandleValue lval, HandleValue receiver,
                     HandleValue index, HandleValue rval, bool strict);

bool LoadAliasedDebugVar(JSContext* cx, JSObject* env, jsbytecode* pc,
                         MutableHandleValue result);

bool CloseIterOperation(JSContext* cx, HandleObject iter, CompletionKind kind);
} /* namespace js */

#endif /* vm_Interpreter_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_FrameIter_h
#define vm_FrameIter_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_IMPLICIT, MOZ_RAII
#include "mozilla/MaybeOneOf.h"  // mozilla::MaybeOneOf

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t, uintptr_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "jit/JSJitFrameIter.h"  // js::jit::{InlineFrameIterator,JSJitFrameIter}
#include "js/ColumnNumber.h"     // JS::TaggedColumnNumberOneOrigin
#include "js/RootingAPI.h"       // JS::Handle, JS::Rooted
#include "js/TypeDecls.h"  // jsbytecode, JSContext, JSAtom, JSFunction, JSObject, JSScript
#include "js/Value.h"       // JS::Value
#include "vm/Activation.h"  // js::InterpreterActivation
#include "vm/Stack.h"       // js::{AbstractFramePtr,MaybeCheckAliasing}
#include "wasm/WasmFrameIter.h"  // js::wasm::{ExitReason,RegisterState,WasmFrameIter}

struct JSPrincipals;

namespace JS {

class JS_PUBLIC_API Compartment;
class JS_PUBLIC_API Realm;

}  // namespace JS

namespace js {

class ArgumentsObject;
class CallObject;

namespace jit {
class CommonFrameLayout;
class JitActivation;
}  // namespace jit

namespace wasm {
class Instance;
}  // namespace wasm

// Iterates over the frames of a single InterpreterActivation.
class InterpreterFrameIterator {
  InterpreterActivation* activation_;
  InterpreterFrame* fp_;
  jsbytecode* pc_;
  JS::Value* sp_;

 public:
  explicit InterpreterFrameIterator(InterpreterActivation* activation)
      : activation_(activation), fp_(nullptr), pc_(nullptr), sp_(nullptr) {
    if (activation) {
      fp_ = activation->current();
      pc_ = activation->regs().pc;
      sp_ = activation->regs().sp;
    }
  }

  InterpreterFrame* frame() const {
    MOZ_ASSERT(!done());
    return fp_;
  }
  jsbytecode* pc() const {
    MOZ_ASSERT(!done());
    return pc_;
  }
  JS::Value* sp() const {
    MOZ_ASSERT(!done());
    return sp_;
  }

  InterpreterFrameIterator& operator++();

  bool done() const { return fp_ == nullptr; }
};

// A JitFrameIter can iterate over all kind of frames emitted by our code
// generators, be they composed of JS jit frames or wasm frames, interleaved or
// not, in any order.
//
// In the following class:
// - code generated for JS is referred to as JSJit.
// - code generated for wasm is referred to as Wasm.
// Also, Jit refers to any one of them.
//
// JitFrameIter uses JSJitFrameIter to iterate over JSJit code or a
// WasmFrameIter to iterate over wasm code; only one of them is active at the
// time. When a sub-iterator is done, the JitFrameIter knows how to stop, move
// onto the next activation or move onto another kind of Jit code.
//
// For ease of use, there is also OnlyJSJitFrameIter, which skips all the
// non-JSJit frames.
//
// Note it is allowed to get a handle to the internal frame iterator via
// asJSJit() and asWasm(), but the user has to be careful not to have those be
// used after JitFrameIter leaves the scope or the operator++ is called.
//
// In particular, this can handle the transition from wasm to jit and from jit
// to wasm, since these can be interleaved in the same JitActivation.
class JitFrameIter {
 protected:
  jit::JitActivation* act_ = nullptr;
  mozilla::MaybeOneOf<jit::JSJitFrameIter, wasm::WasmFrameIter> iter_ = {};
  bool mustUnwindActivation_ = false;

  void settle();

 public:
  JitFrameIter() = default;

  explicit JitFrameIter(jit::JitActivation* activation,
                        bool mustUnwindActivation = false);

  explicit JitFrameIter(const JitFrameIter& another);
  JitFrameIter& operator=(const JitFrameIter& another);

  bool isSome() const { return !iter_.empty(); }
  void reset() {
    MOZ_ASSERT(isSome());
    iter_.destroy();
  }

  bool isJSJit() const {
    return isSome() && iter_.constructed<jit::JSJitFrameIter>();
  }
  jit::JSJitFrameIter& asJSJit() { return iter_.ref<jit::JSJitFrameIter>(); }
  const jit::JSJitFrameIter& asJSJit() const {
    return iter_.ref<jit::JSJitFrameIter>();
  }

  bool isWasm() const {
    return isSome() && iter_.constructed<wasm::WasmFrameIter>();
  }
  wasm::WasmFrameIter& asWasm() { return iter_.ref<wasm::WasmFrameIter>(); }
  const wasm::WasmFrameIter& asWasm() const {
    return iter_.ref<wasm::WasmFrameIter>();
  }

  // Operations common to all frame iterators.
  const jit::JitActivation* activation() const { return act_; }
  bool done() const;
  void operator++();

  JS::Realm* realm() const;

  // Returns the address of the next instruction that will execute in this
  // frame, once control returns to this frame.
  uint8_t* resumePCinCurrentFrame() const;

  // Operations which have an effect only on JIT frames.
  void skipNonScriptedJSFrames();

  // Returns true iff this is a JIT frame with a self-hosted script. Note: be
  // careful, JitFrameIter does not consider functions inlined by Ion.
  bool isSelfHostedIgnoringInlining() const;
};

// A JitFrameIter that skips all the non-JSJit frames, skipping interleaved
// frames of any another kind.

class OnlyJSJitFrameIter : public JitFrameIter {
  void settle() {
    while (!done() && !isJSJit()) {
      JitFrameIter::operator++();
    }
  }

 public:
  explicit OnlyJSJitFrameIter(jit::JitActivation* act);
  explicit OnlyJSJitFrameIter(const ActivationIterator& cx);

  void operator++() {
    JitFrameIter::operator++();
    settle();
  }

  const jit::JSJitFrameIter& frame() const { return asJSJit(); }
};

class ScriptSource;

// A FrameIter walks over a context's stack of JS script activations,
// abstracting over whether the JS scripts were running in the interpreter or
// different modes of compiled code.
//
// FrameIter is parameterized by what it includes in the stack iteration:
//  - When provided, the optional JSPrincipal argument will cause FrameIter to
//    only show frames in globals whose JSPrincipals are subsumed (via
//    JSSecurityCallbacks::subsume) by the given JSPrincipal.
//
// Additionally, there are derived FrameIter types that automatically skip
// certain frames:
//  - ScriptFrameIter only shows frames that have an associated JSScript
//    (currently everything other than wasm stack frames). When !hasScript(),
//    clients must stick to the portion of the
//    interface marked below.
//  - NonBuiltinScriptFrameIter additionally filters out builtin (self-hosted)
//    scripts.
class FrameIter {
 public:
  enum DebuggerEvalOption {
    FOLLOW_DEBUGGER_EVAL_PREV_LINK,
    IGNORE_DEBUGGER_EVAL_PREV_LINK
  };

  enum State {
    DONE,    // when there are no more frames nor activations to unwind.
    INTERP,  // interpreter activation on the stack
    JIT      // jit or wasm activations on the stack
  };

  // Unlike ScriptFrameIter itself, ScriptFrameIter::Data can be allocated on
  // the heap, so this structure should not contain any GC things.
  struct Data {
    JSContext* cx_;
    DebuggerEvalOption debuggerEvalOption_;
    JSPrincipals* principals_;

    State state_;

    jsbytecode* pc_;

    InterpreterFrameIterator interpFrames_;
    ActivationIterator activations_;

    JitFrameIter jitFrames_;
    unsigned ionInlineFrameNo_;

    Data(JSContext* cx, DebuggerEvalOption debuggerEvalOption,
         JSPrincipals* principals);
    Data(const Data& other);
  };

  explicit FrameIter(JSContext* cx,
                     DebuggerEvalOption = FOLLOW_DEBUGGER_EVAL_PREV_LINK);
  FrameIter(JSContext* cx, DebuggerEvalOption, JSPrincipals*);
  FrameIter(const FrameIter& iter);
  MOZ_IMPLICIT FrameIter(const Data& data);

  bool done() const { return data_.state_ == DONE; }

  // -------------------------------------------------------
  // The following functions can only be called when !done()
  // -------------------------------------------------------

  FrameIter& operator++();

  JS::Realm* realm() const;
  JS::Compartment* compartment() const;
  Activation* activation() const { return data_.activations_.activation(); }

  bool isInterp() const {
    MOZ_ASSERT(!done());
    return data_.state_ == INTERP;
  }
  bool isJSJit() const {
    MOZ_ASSERT(!done());
    return data_.state_ == JIT && data_.jitFrames_.isJSJit();
  }
  bool isWasm() const {
    MOZ_ASSERT(!done());
    return data_.state_ == JIT && data_.jitFrames_.isWasm();
  }

  inline bool isIon() const;
  inline bool isBaseline() const;
  inline bool isPhysicalJitFrame() const;

  bool isEvalFrame() const;
  bool isModuleFrame() const;
  bool isFunctionFrame() const;
  bool hasArgs() const { return isFunctionFrame(); }

  ScriptSource* scriptSource() const;
  const char* filename() const;
  const char16_t* displayURL() const;
  unsigned computeLine(JS::TaggedColumnNumberOneOrigin* column = nullptr) const;
  JSAtom* maybeFunctionDisplayAtom() const;
  bool mutedErrors() const;

  bool hasScript() const { return !isWasm(); }

  // -----------------------------------------------------------
  //  The following functions can only be called when isWasm()
  // -----------------------------------------------------------

  inline bool wasmDebugEnabled() const;
  inline wasm::Instance* wasmInstance() const;
  inline uint32_t wasmFuncIndex() const;
  inline unsigned wasmBytecodeOffset() const;
  void wasmUpdateBytecodeOffset();

  // -----------------------------------------------------------
  // The following functions can only be called when hasScript()
  // -----------------------------------------------------------

  inline JSScript* script() const;

  bool isConstructing() const;
  jsbytecode* pc() const {
    MOZ_ASSERT(!done());
    return data_.pc_;
  }
  void updatePcQuadratic();

  // The function |calleeTemplate()| returns either the function from which
  // the current |callee| was cloned or the |callee| if it can be read. As
  // long as we do not have to investigate the environment chain or build a
  // new frame, we should prefer to use |calleeTemplate| instead of
  // |callee|, as requesting the |callee| might cause the invalidation of
  // the frame. (see js::Lambda)
  JSFunction* calleeTemplate() const;
  JSFunction* callee(JSContext* cx) const;

  JSFunction* maybeCallee(JSContext* cx) const {
    return isFunctionFrame() ? callee(cx) : nullptr;
  }

  bool matchCallee(JSContext* cx, JS::Handle<JSFunction*> fun) const;

  unsigned numActualArgs() const;
  unsigned numFormalArgs() const;
  JS::Value unaliasedActual(unsigned i,
                            MaybeCheckAliasing = CHECK_ALIASING) const;
  template <class Op>
  inline void unaliasedForEachActual(JSContext* cx, Op op);

  JSObject* environmentChain(JSContext* cx) const;
  bool hasInitialEnvironment(JSContext* cx) const;
  CallObject& callObj(JSContext* cx) const;

  bool hasArgsObj() const;
  ArgumentsObject& argsObj() const;

  // Get the original |this| value passed to this function. May not be the
  // actual this-binding (for instance, derived class constructors will
  // change their this-value later and non-strict functions will box
  // primitives).
  JS::Value thisArgument(JSContext* cx) const;

  JS::Value returnValue() const;
  void setReturnValue(const JS::Value& v);

  // These are only valid for the top frame.
  size_t numFrameSlots() const;
  JS::Value frameSlotValue(size_t index) const;

  // Ensures that we have rematerialized the top frame and its associated
  // inline frames. Can only be called when isIon().
  bool ensureHasRematerializedFrame(JSContext* cx);

  // True when isInterp() or isBaseline(). True when isIon() if it
  // has a rematerialized frame. False otherwise.
  bool hasUsableAbstractFramePtr() const;

  // -----------------------------------------------------------
  // The following functions can only be called when isInterp(),
  // isBaseline(), isWasm() or isIon(). Further, abstractFramePtr() can
  // only be called when hasUsableAbstractFramePtr().
  // -----------------------------------------------------------

  AbstractFramePtr abstractFramePtr() const;
  Data* copyData() const;

  // This can only be called when isInterp():
  inline InterpreterFrame* interpFrame() const;

  // This can only be called when isPhysicalJitFrame():
  inline jit::CommonFrameLayout* physicalJitFrame() const;

  // This is used to provide a raw interface for debugging.
  void* rawFramePtr() const;

  bool inPrologue() const;

  const wasm::WasmFrameIter& wasmFrame() const {
    return data_.jitFrames_.asWasm();
  }
  wasm::WasmFrameIter& wasmFrame() { return data_.jitFrames_.asWasm(); }

 private:
  Data data_;
  jit::InlineFrameIterator ionInlineFrames_;

  const jit::JSJitFrameIter& jsJitFrame() const {
    return data_.jitFrames_.asJSJit();
  }

  jit::JSJitFrameIter& jsJitFrame() { return data_.jitFrames_.asJSJit(); }

  bool isIonScripted() const {
    return isJSJit() && jsJitFrame().isIonScripted();
  }

  bool principalsSubsumeFrame() const;

  void popActivation();
  void popInterpreterFrame();
  void nextJitFrame();
  void popJitFrame();
  void settleOnActivation();
};

class ScriptFrameIter : public FrameIter {
  void settle() {
    while (!done() && !hasScript()) {
      FrameIter::operator++();
    }
  }

 public:
  explicit ScriptFrameIter(
      JSContext* cx,
      DebuggerEvalOption debuggerEvalOption = FOLLOW_DEBUGGER_EVAL_PREV_LINK)
      : FrameIter(cx, debuggerEvalOption) {
    settle();
  }

  ScriptFrameIter& operator++() {
    FrameIter::operator++();
    settle();
    return *this;
  }
};

#ifdef DEBUG
bool SelfHostedFramesVisible();
#else
static inline bool SelfHostedFramesVisible() { return false; }
#endif

/* A filtering of the FrameIter to only stop at non-self-hosted scripts. */
class NonBuiltinFrameIter : public FrameIter {
  void settle();

 public:
  explicit NonBuiltinFrameIter(
      JSContext* cx, FrameIter::DebuggerEvalOption debuggerEvalOption =
                         FrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK)
      : FrameIter(cx, debuggerEvalOption) {
    settle();
  }

  NonBuiltinFrameIter(JSContext* cx,
                      FrameIter::DebuggerEvalOption debuggerEvalOption,
                      JSPrincipals* principals)
      : FrameIter(cx, debuggerEvalOption, principals) {
    settle();
  }

  NonBuiltinFrameIter(JSContext* cx, JSPrincipals* principals)
      : FrameIter(cx, FrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK, principals) {
    settle();
  }

  NonBuiltinFrameIter& operator++() {
    FrameIter::operator++();
    settle();
    return *this;
  }
};

// A filtering of the ScriptFrameIter to only stop at non-self-hosted scripts.
class NonBuiltinScriptFrameIter : public ScriptFrameIter {
  void settle();

 public:
  explicit NonBuiltinScriptFrameIter(
      JSContext* cx, ScriptFrameIter::DebuggerEvalOption debuggerEvalOption =
                         ScriptFrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK)
      : ScriptFrameIter(cx, debuggerEvalOption) {
    settle();
  }

  NonBuiltinScriptFrameIter& operator++() {
    ScriptFrameIter::operator++();
    settle();
    return *this;
  }
};

/*
 * Blindly iterate over all frames in the current thread's stack. These frames
 * can be from different contexts and compartments, so beware.
 */
class AllFramesIter : public FrameIter {
 public:
  explicit AllFramesIter(JSContext* cx)
      : FrameIter(cx, ScriptFrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK) {}
};

/* Iterates over all script frame in the current thread's stack.
 * See also AllFramesIter and ScriptFrameIter.
 */
class AllScriptFramesIter : public ScriptFrameIter {
 public:
  explicit AllScriptFramesIter(JSContext* cx)
      : ScriptFrameIter(cx, ScriptFrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK) {}
};

/* Popular inline definitions. */

inline JSScript* FrameIter::script() const {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(hasScript());
  if (data_.state_ == INTERP) {
    return interpFrame()->script();
  }
  if (jsJitFrame().isIonJS()) {
    return ionInlineFrames_.script();
  }
  return jsJitFrame().script();
}

inline bool FrameIter::wasmDebugEnabled() const {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(isWasm());
  return wasmFrame().debugEnabled();
}

inline wasm::Instance* FrameIter::wasmInstance() const {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(isWasm());
  return wasmFrame().instance();
}

inline unsigned FrameIter::wasmBytecodeOffset() const {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(isWasm());
  return wasmFrame().lineOrBytecode();
}

inline uint32_t FrameIter::wasmFuncIndex() const {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(isWasm());
  return wasmFrame().funcIndex();
}

inline bool FrameIter::isIon() const {
  return isJSJit() && jsJitFrame().isIonJS();
}

inline bool FrameIter::isBaseline() const {
  return isJSJit() && jsJitFrame().isBaselineJS();
}

inline InterpreterFrame* FrameIter::interpFrame() const {
  MOZ_ASSERT(data_.state_ == INTERP);
  return data_.interpFrames_.frame();
}

inline bool FrameIter::isPhysicalJitFrame() const {
  if (!isJSJit()) {
    return false;
  }

  auto& jitFrame = jsJitFrame();

  if (jitFrame.isBaselineJS()) {
    return true;
  }

  if (jitFrame.isIonScripted()) {
    // Only the bottom of a group of inlined Ion frames is a physical frame.
    return ionInlineFrames_.frameNo() == 0;
  }

  return false;
}

inline jit::CommonFrameLayout* FrameIter::physicalJitFrame() const {
  MOZ_ASSERT(isPhysicalJitFrame());
  return jsJitFrame().current();
}

}  // namespace js

#endif  // vm_FrameIter_h

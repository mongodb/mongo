/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/FrameIter-inl.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT, MOZ_CRASH
#include "mozilla/MaybeOneOf.h"  // mozilla::MaybeOneOf

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t
#include <stdlib.h>  // getenv

#include "jit/BaselineFrame.h"   // js::jit::BaselineFrame
#include "jit/JitFrames.h"       // js::jit::EnsureBareExitFrame
#include "jit/JSJitFrameIter.h"  // js::jit::{FrameType,InlineFrameIterator,JSJitFrameIter,MaybeReadFallback,SnapshotIterator}
#include "js/GCAPI.h"            // JS::AutoSuppressGCAnalysis
#include "js/Principals.h"       // JSSubsumesOp
#include "js/RootingAPI.h"       // JS::Rooted
#include "vm/Activation.h"       // js::Activation{,Iterator}
#include "vm/EnvironmentObject.h"  // js::CallObject
#include "vm/JitActivation.h"      // js::jit::JitActivation
#include "vm/JSContext.h"          // JSContext
#include "vm/JSFunction.h"         // JSFunction
#include "vm/JSScript.h"  // js::PCToLineNumber, JSScript, js::ScriptSource
#include "vm/Runtime.h"   // JSRuntime
#include "vm/Stack.h"  // js::{AbstractFramePtr,InterpreterFrame,MaybeCheckAliasing}
#include "wasm/WasmFrameIter.h"  // js::wasm::WasmFrameIter
#include "wasm/WasmInstance.h"   // js::wasm::Instance

#include "jit/JSJitFrameIter-inl.h"  // js::jit::JSJitFrameIter::baselineFrame{,NumValueSlots}
#include "vm/Stack-inl.h"  // js::AbstractFramePtr::*

namespace JS {
class JS_PUBLIC_API Realm;
}  // namespace JS

namespace js {
class ArgumentsObject;
}  // namespace js

using JS::Realm;
using JS::Rooted;
using JS::Value;

using js::AbstractFramePtr;
using js::ArgumentsObject;
using js::CallObject;
using js::FrameIter;
using js::JitFrameIter;
using js::NonBuiltinFrameIter;
using js::NonBuiltinScriptFrameIter;
using js::OnlyJSJitFrameIter;
using js::ScriptSource;
using js::jit::JSJitFrameIter;

JitFrameIter::JitFrameIter(const JitFrameIter& another) { *this = another; }

JitFrameIter& JitFrameIter::operator=(const JitFrameIter& another) {
  MOZ_ASSERT(this != &another);

  act_ = another.act_;
  mustUnwindActivation_ = another.mustUnwindActivation_;

  if (isSome()) {
    iter_.destroy();
  }
  if (!another.isSome()) {
    return *this;
  }

  if (another.isJSJit()) {
    iter_.construct<jit::JSJitFrameIter>(another.asJSJit());
  } else {
    MOZ_ASSERT(another.isWasm());
    iter_.construct<wasm::WasmFrameIter>(another.asWasm());
  }

  return *this;
}

JitFrameIter::JitFrameIter(jit::JitActivation* act, bool mustUnwindActivation) {
  act_ = act;
  mustUnwindActivation_ = mustUnwindActivation;
  MOZ_ASSERT(act->hasExitFP(),
             "packedExitFP is used to determine if JSJit or wasm");
  if (act->hasJSExitFP()) {
    iter_.construct<jit::JSJitFrameIter>(act);
  } else {
    MOZ_ASSERT(act->hasWasmExitFP());
    iter_.construct<wasm::WasmFrameIter>(act);
  }
  settle();
}

void JitFrameIter::skipNonScriptedJSFrames() {
  if (isJSJit()) {
    // Stop at the first scripted frame.
    jit::JSJitFrameIter& frames = asJSJit();
    while (!frames.isScripted() && !frames.done()) {
      ++frames;
    }
    settle();
  }
}

bool JitFrameIter::isSelfHostedIgnoringInlining() const {
  MOZ_ASSERT(!done());

  if (isWasm()) {
    return false;
  }

  return asJSJit().script()->selfHosted();
}

JS::Realm* JitFrameIter::realm() const {
  MOZ_ASSERT(!done());

  if (isWasm()) {
    return asWasm().instance()->realm();
  }

  return asJSJit().script()->realm();
}

uint8_t* JitFrameIter::resumePCinCurrentFrame() const {
  if (isWasm()) {
    return asWasm().resumePCinCurrentFrame();
  }
  return asJSJit().resumePCinCurrentFrame();
}

bool JitFrameIter::done() const {
  if (!isSome()) {
    return true;
  }
  if (isJSJit()) {
    return asJSJit().done();
  }
  if (isWasm()) {
    return asWasm().done();
  }
  MOZ_CRASH("unhandled case");
}

void JitFrameIter::settle() {
  if (isJSJit()) {
    const jit::JSJitFrameIter& jitFrame = asJSJit();
    if (jitFrame.type() != jit::FrameType::WasmToJSJit) {
      return;
    }

    // Transition from js jit frames to wasm frames: we're on the
    // wasm-to-jit fast path. The current stack layout is as follows:
    // (stack grows downward)
    //
    // [--------------------]
    // [WASM FUNC           ]
    // [WASM JIT EXIT FRAME ]
    // [JIT WASM ENTRY FRAME] <-- we're here.
    //
    // So prevFP points to the wasm jit exit FP, maintaing the invariant in
    // WasmFrameIter that the first frame is an exit frame and can be
    // popped.

    wasm::Frame* prevFP = (wasm::Frame*)jitFrame.prevFp();

    if (mustUnwindActivation_) {
      act_->setWasmExitFP(prevFP);
    }

    iter_.destroy();
    iter_.construct<wasm::WasmFrameIter>(act_, prevFP);
    MOZ_ASSERT(!asWasm().done());
    return;
  }

  if (isWasm()) {
    const wasm::WasmFrameIter& wasmFrame = asWasm();
    if (!wasmFrame.unwoundIonCallerFP()) {
      return;
    }

    // Transition from wasm frames to jit frames: we're on the
    // jit-to-wasm fast path. The current stack layout is as follows:
    // (stack grows downward)
    //
    // [--------------------]
    // [JIT FRAME           ]
    // [WASM JIT ENTRY FRAME] <-- we're here
    //
    // The wasm iterator has saved the previous jit frame pointer for us.

    MOZ_ASSERT(wasmFrame.done());
    uint8_t* prevFP = wasmFrame.unwoundIonCallerFP();
    jit::FrameType prevFrameType = wasmFrame.unwoundIonFrameType();

    if (mustUnwindActivation_) {
      act_->setJSExitFP(prevFP);
    }

    iter_.destroy();
    iter_.construct<jit::JSJitFrameIter>(act_, prevFrameType, prevFP);
    MOZ_ASSERT(!asJSJit().done());
    return;
  }
}

void JitFrameIter::operator++() {
  MOZ_ASSERT(isSome());
  if (isJSJit()) {
    const jit::JSJitFrameIter& jitFrame = asJSJit();

    jit::JitFrameLayout* prevFrame = nullptr;
    if (mustUnwindActivation_ && jitFrame.isScripted()) {
      prevFrame = jitFrame.jsFrame();
    }

    ++asJSJit();

    if (prevFrame) {
      // Unwind the frame by updating packedExitFP. This is necessary
      // so that (1) debugger exception unwind and leave frame hooks
      // don't see this frame when they use ScriptFrameIter, and (2)
      // ScriptFrameIter does not crash when accessing an IonScript
      // that's destroyed by the ionScript->decref call.
      EnsureBareExitFrame(act_, prevFrame);
    }
  } else if (isWasm()) {
    ++asWasm();
  } else {
    MOZ_CRASH("unhandled case");
  }
  settle();
}

OnlyJSJitFrameIter::OnlyJSJitFrameIter(jit::JitActivation* act)
    : JitFrameIter(act) {
  settle();
}

OnlyJSJitFrameIter::OnlyJSJitFrameIter(const ActivationIterator& iter)
    : OnlyJSJitFrameIter(iter->asJit()) {}

/*****************************************************************************/

void FrameIter::popActivation() {
  ++data_.activations_;
  settleOnActivation();
}

bool FrameIter::principalsSubsumeFrame() const {
  // If the caller supplied principals, only show frames which are
  // subsumed (of the same origin or of an origin accessible) by these
  // principals.

  MOZ_ASSERT(!done());

  if (!data_.principals_) {
    return true;
  }

  JSSubsumesOp subsumes = data_.cx_->runtime()->securityCallbacks->subsumes;
  if (!subsumes) {
    return true;
  }

  JS::AutoSuppressGCAnalysis nogc;
  return subsumes(data_.principals_, realm()->principals());
}

void FrameIter::popInterpreterFrame() {
  MOZ_ASSERT(data_.state_ == INTERP);

  ++data_.interpFrames_;

  if (data_.interpFrames_.done()) {
    popActivation();
  } else {
    data_.pc_ = data_.interpFrames_.pc();
  }
}

void FrameIter::settleOnActivation() {
  MOZ_ASSERT(!data_.cx_->inUnsafeCallWithABI);

  while (true) {
    if (data_.activations_.done()) {
      data_.state_ = DONE;
      return;
    }

    Activation* activation = data_.activations_.activation();

    if (activation->isJit()) {
      data_.jitFrames_ = JitFrameIter(activation->asJit());
      data_.jitFrames_.skipNonScriptedJSFrames();
      if (data_.jitFrames_.done()) {
        // It's possible to have an JitActivation with no scripted
        // frames, for instance if we hit an over-recursion during
        // bailout.
        ++data_.activations_;
        continue;
      }
      data_.state_ = JIT;
      nextJitFrame();
      return;
    }

    MOZ_ASSERT(activation->isInterpreter());

    InterpreterActivation* interpAct = activation->asInterpreter();
    data_.interpFrames_ = InterpreterFrameIterator(interpAct);

    // If we OSR'ed into JIT code, skip the interpreter frame so that
    // the same frame is not reported twice.
    if (data_.interpFrames_.frame()->runningInJit()) {
      ++data_.interpFrames_;
      if (data_.interpFrames_.done()) {
        ++data_.activations_;
        continue;
      }
    }

    MOZ_ASSERT(!data_.interpFrames_.frame()->runningInJit());
    data_.pc_ = data_.interpFrames_.pc();
    data_.state_ = INTERP;
    return;
  }
}

FrameIter::Data::Data(JSContext* cx, DebuggerEvalOption debuggerEvalOption,
                      JSPrincipals* principals)
    : cx_(cx),
      debuggerEvalOption_(debuggerEvalOption),
      principals_(principals),
      state_(DONE),
      pc_(nullptr),
      interpFrames_(nullptr),
      activations_(cx),
      ionInlineFrameNo_(0) {}

FrameIter::Data::Data(const FrameIter::Data& other) = default;

FrameIter::FrameIter(JSContext* cx, DebuggerEvalOption debuggerEvalOption)
    : data_(cx, debuggerEvalOption, nullptr),
      ionInlineFrames_(cx, (js::jit::JSJitFrameIter*)nullptr) {
  settleOnActivation();

  // No principals so we can see all frames.
  MOZ_ASSERT_IF(!done(), principalsSubsumeFrame());
}

FrameIter::FrameIter(JSContext* cx, DebuggerEvalOption debuggerEvalOption,
                     JSPrincipals* principals)
    : data_(cx, debuggerEvalOption, principals),
      ionInlineFrames_(cx, (js::jit::JSJitFrameIter*)nullptr) {
  settleOnActivation();

  // If we're not allowed to see this frame, call operator++ to skip this (and
  // other) cross-origin frames.
  if (!done() && !principalsSubsumeFrame()) {
    ++*this;
  }
}

FrameIter::FrameIter(const FrameIter& other)
    : data_(other.data_),
      ionInlineFrames_(other.data_.cx_,
                       isIonScripted() ? &other.ionInlineFrames_ : nullptr) {}

FrameIter::FrameIter(const Data& data)
    : data_(data),
      ionInlineFrames_(data.cx_, isIonScripted() ? &jsJitFrame() : nullptr) {
  MOZ_ASSERT(data.cx_);
  if (isIonScripted()) {
    while (ionInlineFrames_.frameNo() != data.ionInlineFrameNo_) {
      ++ionInlineFrames_;
    }
  }
}

void FrameIter::nextJitFrame() {
  MOZ_ASSERT(data_.jitFrames_.isSome());

  if (isJSJit()) {
    if (jsJitFrame().isIonScripted()) {
      ionInlineFrames_.resetOn(&jsJitFrame());
      data_.pc_ = ionInlineFrames_.pc();
    } else {
      MOZ_ASSERT(jsJitFrame().isBaselineJS());
      jsJitFrame().baselineScriptAndPc(nullptr, &data_.pc_);
    }
    return;
  }

  MOZ_ASSERT(isWasm());
  data_.pc_ = nullptr;
}

void FrameIter::popJitFrame() {
  MOZ_ASSERT(data_.state_ == JIT);
  MOZ_ASSERT(data_.jitFrames_.isSome());

  if (isJSJit() && jsJitFrame().isIonScripted() && ionInlineFrames_.more()) {
    ++ionInlineFrames_;
    data_.pc_ = ionInlineFrames_.pc();
    return;
  }

  ++data_.jitFrames_;
  data_.jitFrames_.skipNonScriptedJSFrames();

  if (!data_.jitFrames_.done()) {
    nextJitFrame();
  } else {
    data_.jitFrames_.reset();
    popActivation();
  }
}

FrameIter& FrameIter::operator++() {
  while (true) {
    switch (data_.state_) {
      case DONE:
        MOZ_CRASH("Unexpected state");
      case INTERP:
        if (interpFrame()->isDebuggerEvalFrame() &&
            data_.debuggerEvalOption_ == FOLLOW_DEBUGGER_EVAL_PREV_LINK) {
          AbstractFramePtr eifPrev = interpFrame()->evalInFramePrev();

          popInterpreterFrame();

          while (!hasUsableAbstractFramePtr() ||
                 abstractFramePtr() != eifPrev) {
            if (data_.state_ == JIT) {
              popJitFrame();
            } else {
              popInterpreterFrame();
            }
          }

          break;
        }
        popInterpreterFrame();
        break;
      case JIT:
        popJitFrame();
        break;
    }

    if (done() || principalsSubsumeFrame()) {
      break;
    }
  }

  return *this;
}

FrameIter::Data* FrameIter::copyData() const {
  Data* data = data_.cx_->new_<Data>(data_);
  if (!data) {
    return nullptr;
  }

  if (data && isIonScripted()) {
    data->ionInlineFrameNo_ = ionInlineFrames_.frameNo();
  }
  return data;
}

void* FrameIter::rawFramePtr() const {
  switch (data_.state_) {
    case DONE:
      return nullptr;
    case INTERP:
      return interpFrame();
    case JIT:
      if (isJSJit()) {
        return jsJitFrame().fp();
      }
      MOZ_ASSERT(isWasm());
      return nullptr;
  }
  MOZ_CRASH("Unexpected state");
}

JS::Compartment* FrameIter::compartment() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
    case JIT:
      return data_.activations_->compartment();
  }
  MOZ_CRASH("Unexpected state");
}

Realm* FrameIter::realm() const {
  MOZ_ASSERT(!done());

  if (hasScript()) {
    return script()->realm();
  }

  return wasmInstance()->realm();
}

bool FrameIter::isEvalFrame() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
      return interpFrame()->isEvalFrame();
    case JIT:
      if (isJSJit()) {
        if (jsJitFrame().isBaselineJS()) {
          return jsJitFrame().baselineFrame()->isEvalFrame();
        }
        MOZ_ASSERT(!script()->isForEval());
        return false;
      }
      MOZ_ASSERT(isWasm());
      return false;
  }
  MOZ_CRASH("Unexpected state");
}

bool FrameIter::isModuleFrame() const {
  MOZ_ASSERT(!done());

  if (hasScript()) {
    return script()->isModule();
  }
  MOZ_CRASH("Unexpected state");
}

bool FrameIter::isFunctionFrame() const {
  MOZ_ASSERT(!done());
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
      return interpFrame()->isFunctionFrame();
    case JIT:
      if (isJSJit()) {
        if (jsJitFrame().isBaselineJS()) {
          return jsJitFrame().baselineFrame()->isFunctionFrame();
        }
        return script()->isFunction();
      }
      MOZ_ASSERT(isWasm());
      return false;
  }
  MOZ_CRASH("Unexpected state");
}

JSAtom* FrameIter::maybeFunctionDisplayAtom() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
    case JIT:
      if (isWasm()) {
        return wasmFrame().functionDisplayAtom();
      }
      if (isFunctionFrame()) {
        return calleeTemplate()->displayAtom();
      }
      return nullptr;
  }

  MOZ_CRASH("Unexpected state");
}

ScriptSource* FrameIter::scriptSource() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
    case JIT:
      return script()->scriptSource();
  }

  MOZ_CRASH("Unexpected state");
}

const char* FrameIter::filename() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
    case JIT:
      if (isWasm()) {
        return wasmFrame().filename();
      }
      return script()->filename();
  }

  MOZ_CRASH("Unexpected state");
}

const char16_t* FrameIter::displayURL() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
    case JIT:
      if (isWasm()) {
        return wasmFrame().displayURL();
      }
      ScriptSource* ss = script()->scriptSource();
      return ss->hasDisplayURL() ? ss->displayURL() : nullptr;
  }
  MOZ_CRASH("Unexpected state");
}

unsigned FrameIter::computeLine(uint32_t* column) const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
    case JIT:
      if (isWasm()) {
        return wasmFrame().computeLine(column);
      }
      return PCToLineNumber(script(), pc(), column);
  }

  MOZ_CRASH("Unexpected state");
}

bool FrameIter::mutedErrors() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
    case JIT:
      if (isWasm()) {
        return wasmFrame().mutedErrors();
      }
      return script()->mutedErrors();
  }
  MOZ_CRASH("Unexpected state");
}

bool FrameIter::isConstructing() const {
  switch (data_.state_) {
    case DONE:
      break;
    case JIT:
      MOZ_ASSERT(isJSJit());
      if (jsJitFrame().isIonScripted()) {
        return ionInlineFrames_.isConstructing();
      }
      MOZ_ASSERT(jsJitFrame().isBaselineJS());
      return jsJitFrame().isConstructing();
    case INTERP:
      return interpFrame()->isConstructing();
  }

  MOZ_CRASH("Unexpected state");
}

bool FrameIter::ensureHasRematerializedFrame(JSContext* cx) {
  MOZ_ASSERT(isIon());
  return !!activation()->asJit()->getRematerializedFrame(cx, jsJitFrame());
}

bool FrameIter::hasUsableAbstractFramePtr() const {
  switch (data_.state_) {
    case DONE:
      return false;
    case JIT:
      if (isJSJit()) {
        if (jsJitFrame().isBaselineJS()) {
          return true;
        }

        MOZ_ASSERT(jsJitFrame().isIonScripted());
        return !!activation()->asJit()->lookupRematerializedFrame(
            jsJitFrame().fp(), ionInlineFrames_.frameNo());
      }
      MOZ_ASSERT(isWasm());
      return wasmFrame().debugEnabled();
    case INTERP:
      return true;
  }
  MOZ_CRASH("Unexpected state");
}

AbstractFramePtr FrameIter::abstractFramePtr() const {
  MOZ_ASSERT(hasUsableAbstractFramePtr());
  switch (data_.state_) {
    case DONE:
      break;
    case JIT: {
      if (isJSJit()) {
        if (jsJitFrame().isBaselineJS()) {
          return jsJitFrame().baselineFrame();
        }
        MOZ_ASSERT(isIonScripted());
        return activation()->asJit()->lookupRematerializedFrame(
            jsJitFrame().fp(), ionInlineFrames_.frameNo());
      }
      MOZ_ASSERT(isWasm());
      MOZ_ASSERT(wasmFrame().debugEnabled());
      return wasmFrame().debugFrame();
    }
    case INTERP:
      MOZ_ASSERT(interpFrame());
      return AbstractFramePtr(interpFrame());
  }
  MOZ_CRASH("Unexpected state");
}

void FrameIter::updatePcQuadratic() {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP: {
      InterpreterFrame* frame = interpFrame();
      InterpreterActivation* activation = data_.activations_->asInterpreter();

      // Look for the current frame.
      data_.interpFrames_ = InterpreterFrameIterator(activation);
      while (data_.interpFrames_.frame() != frame) {
        ++data_.interpFrames_;
      }

      // Update the pc.
      MOZ_ASSERT(data_.interpFrames_.frame() == frame);
      data_.pc_ = data_.interpFrames_.pc();
      return;
    }
    case JIT:
      if (jsJitFrame().isBaselineJS()) {
        jit::BaselineFrame* frame = jsJitFrame().baselineFrame();
        jit::JitActivation* activation = data_.activations_->asJit();

        // activation's exitFP may be invalid, so create a new
        // activation iterator.
        data_.activations_ = ActivationIterator(data_.cx_);
        while (data_.activations_.activation() != activation) {
          ++data_.activations_;
        }

        // Look for the current frame.
        data_.jitFrames_ = JitFrameIter(data_.activations_->asJit());
        while (!isJSJit() || !jsJitFrame().isBaselineJS() ||
               jsJitFrame().baselineFrame() != frame) {
          ++data_.jitFrames_;
        }

        // Update the pc.
        MOZ_ASSERT(jsJitFrame().baselineFrame() == frame);
        jsJitFrame().baselineScriptAndPc(nullptr, &data_.pc_);
        return;
      }
      break;
  }
  MOZ_CRASH("Unexpected state");
}

void FrameIter::wasmUpdateBytecodeOffset() {
  MOZ_RELEASE_ASSERT(isWasm(), "Unexpected state");

  wasm::DebugFrame* frame = wasmFrame().debugFrame();

  // Relookup the current frame, updating the bytecode offset in the process.
  data_.jitFrames_ = JitFrameIter(data_.activations_->asJit());
  while (wasmFrame().debugFrame() != frame) {
    ++data_.jitFrames_;
  }

  MOZ_ASSERT(wasmFrame().debugFrame() == frame);
}

JSFunction* FrameIter::calleeTemplate() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
      MOZ_ASSERT(isFunctionFrame());
      return &interpFrame()->callee();
    case JIT:
      if (jsJitFrame().isBaselineJS()) {
        return jsJitFrame().callee();
      }
      MOZ_ASSERT(jsJitFrame().isIonScripted());
      return ionInlineFrames_.calleeTemplate();
  }
  MOZ_CRASH("Unexpected state");
}

JSFunction* FrameIter::callee(JSContext* cx) const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
      return calleeTemplate();
    case JIT:
      if (isIonScripted()) {
        jit::MaybeReadFallback recover(cx, activation()->asJit(),
                                       &jsJitFrame());
        return ionInlineFrames_.callee(recover);
      }
      MOZ_ASSERT(jsJitFrame().isBaselineJS());
      return calleeTemplate();
  }
  MOZ_CRASH("Unexpected state");
}

bool FrameIter::matchCallee(JSContext* cx, JS::Handle<JSFunction*> fun) const {
  // Use the calleeTemplate to rule out a match without needing to invalidate to
  // find the actual callee. The real callee my be a clone of the template which
  // should *not* be considered a match.
  Rooted<JSFunction*> currentCallee(cx, calleeTemplate());

  if (currentCallee->nargs() != fun->nargs()) {
    return false;
  }

  if (currentCallee->flags().stableAcrossClones() !=
      fun->flags().stableAcrossClones()) {
    return false;
  }

  // The calleeTemplate for a callee will always have the same BaseScript. If
  // the script clones do not use the same script, they also have a different
  // group and Ion will not inline them interchangeably.
  //
  // See: js::jit::InlineFrameIterator::findNextFrame(),
  //      js::CloneFunctionAndScript()
  if (currentCallee->hasBaseScript()) {
    if (currentCallee->baseScript() != fun->baseScript()) {
      return false;
    }
  }

  return callee(cx) == fun;
}

unsigned FrameIter::numActualArgs() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
      MOZ_ASSERT(isFunctionFrame());
      return interpFrame()->numActualArgs();
    case JIT:
      if (isIonScripted()) {
        return ionInlineFrames_.numActualArgs();
      }
      MOZ_ASSERT(jsJitFrame().isBaselineJS());
      return jsJitFrame().numActualArgs();
  }
  MOZ_CRASH("Unexpected state");
}

unsigned FrameIter::numFormalArgs() const {
  return script()->function()->nargs();
}

Value FrameIter::unaliasedActual(unsigned i,
                                 MaybeCheckAliasing checkAliasing) const {
  return abstractFramePtr().unaliasedActual(i, checkAliasing);
}

JSObject* FrameIter::environmentChain(JSContext* cx) const {
  switch (data_.state_) {
    case DONE:
      break;
    case JIT:
      if (isJSJit()) {
        if (isIonScripted()) {
          jit::MaybeReadFallback recover(cx, activation()->asJit(),
                                         &jsJitFrame());
          return ionInlineFrames_.environmentChain(recover);
        }
        return jsJitFrame().baselineFrame()->environmentChain();
      }
      MOZ_ASSERT(isWasm());
      return wasmFrame().debugFrame()->environmentChain();
    case INTERP:
      return interpFrame()->environmentChain();
  }
  MOZ_CRASH("Unexpected state");
}

bool FrameIter::hasInitialEnvironment(JSContext* cx) const {
  if (hasUsableAbstractFramePtr()) {
    return abstractFramePtr().hasInitialEnvironment();
  }

  if (isWasm()) {
    // See JSFunction::needsFunctionEnvironmentObjects().
    return false;
  }

  MOZ_ASSERT(isJSJit());
  MOZ_ASSERT(isIonScripted());

  bool hasInitialEnv = false;
  jit::MaybeReadFallback recover(cx, activation()->asJit(), &jsJitFrame());
  ionInlineFrames_.environmentChain(recover, &hasInitialEnv);

  return hasInitialEnv;
}

CallObject& FrameIter::callObj(JSContext* cx) const {
  MOZ_ASSERT(calleeTemplate()->needsCallObject());
  MOZ_ASSERT(hasInitialEnvironment(cx));

  JSObject* pobj = environmentChain(cx);
  while (!pobj->is<CallObject>()) {
    pobj = pobj->enclosingEnvironment();
  }
  return pobj->as<CallObject>();
}

bool FrameIter::hasArgsObj() const { return abstractFramePtr().hasArgsObj(); }

ArgumentsObject& FrameIter::argsObj() const {
  MOZ_ASSERT(hasArgsObj());
  return abstractFramePtr().argsObj();
}

Value FrameIter::thisArgument(JSContext* cx) const {
  MOZ_ASSERT(isFunctionFrame());

  switch (data_.state_) {
    case DONE:
      break;
    case JIT:
      if (isIonScripted()) {
        jit::MaybeReadFallback recover(cx, activation()->asJit(),
                                       &jsJitFrame());
        return ionInlineFrames_.thisArgument(recover);
      }
      return jsJitFrame().baselineFrame()->thisArgument();
    case INTERP:
      return interpFrame()->thisArgument();
  }
  MOZ_CRASH("Unexpected state");
}

Value FrameIter::newTarget() const {
  switch (data_.state_) {
    case DONE:
      break;
    case INTERP:
      return interpFrame()->newTarget();
    case JIT:
      MOZ_ASSERT(jsJitFrame().isBaselineJS());
      return jsJitFrame().baselineFrame()->newTarget();
  }
  MOZ_CRASH("Unexpected state");
}

Value FrameIter::returnValue() const {
  switch (data_.state_) {
    case DONE:
      break;
    case JIT:
      if (jsJitFrame().isBaselineJS()) {
        return jsJitFrame().baselineFrame()->returnValue();
      }
      break;
    case INTERP:
      return interpFrame()->returnValue();
  }
  MOZ_CRASH("Unexpected state");
}

void FrameIter::setReturnValue(const Value& v) {
  switch (data_.state_) {
    case DONE:
      break;
    case JIT:
      if (jsJitFrame().isBaselineJS()) {
        jsJitFrame().baselineFrame()->setReturnValue(v);
        return;
      }
      break;
    case INTERP:
      interpFrame()->setReturnValue(v);
      return;
  }
  MOZ_CRASH("Unexpected state");
}

size_t FrameIter::numFrameSlots() const {
  switch (data_.state_) {
    case DONE:
      break;
    case JIT: {
      if (isIonScripted()) {
        return ionInlineFrames_.snapshotIterator().numAllocations() -
               ionInlineFrames_.script()->nfixed();
      }
      uint32_t numValueSlots = jsJitFrame().baselineFrameNumValueSlots();
      return numValueSlots - jsJitFrame().script()->nfixed();
    }
    case INTERP:
      MOZ_ASSERT(data_.interpFrames_.sp() >= interpFrame()->base());
      return data_.interpFrames_.sp() - interpFrame()->base();
  }
  MOZ_CRASH("Unexpected state");
}

Value FrameIter::frameSlotValue(size_t index) const {
  switch (data_.state_) {
    case DONE:
      break;
    case JIT:
      if (isIonScripted()) {
        jit::SnapshotIterator si(ionInlineFrames_.snapshotIterator());
        index += ionInlineFrames_.script()->nfixed();
        return si.maybeReadAllocByIndex(index);
      }
      index += jsJitFrame().script()->nfixed();
      return *jsJitFrame().baselineFrame()->valueSlot(index);
    case INTERP:
      return interpFrame()->base()[index];
  }
  MOZ_CRASH("Unexpected state");
}

#ifdef DEBUG
bool js::SelfHostedFramesVisible() {
  static bool checked = false;
  static bool visible = false;
  if (!checked) {
    checked = true;
    char* env = getenv("MOZ_SHOW_ALL_JS_FRAMES");
    visible = !!env;
  }
  return visible;
}
#endif

void NonBuiltinFrameIter::settle() {
  if (!SelfHostedFramesVisible()) {
    while (!done() && hasScript() && script()->selfHosted()) {
      FrameIter::operator++();
    }
  }
}

void NonBuiltinScriptFrameIter::settle() {
  if (!SelfHostedFramesVisible()) {
    while (!done() && script()->selfHosted()) {
      ScriptFrameIter::operator++();
    }
  }
}

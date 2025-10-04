/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RematerializedFrame_h
#define jit_RematerializedFrame_h

#include "mozilla/Assertions.h"

#include <algorithm>
#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "jit/JitFrames.h"
#include "jit/ScriptFromCalleeToken.h"
#include "js/GCVector.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Value.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/Stack.h"

class JS_PUBLIC_API JSTracer;

namespace js {

class ArgumentsObject;
class CallObject;

namespace jit {

class InlineFrameIterator;
struct MaybeReadFallback;

// RematerializedFrame: An optimized frame that has been rematerialized with
// values read out of Snapshots.
//
// If the Debugger API tries to inspect or modify an IonMonkey frame, much of
// the information it expects to find in a frame is missing: function calls may
// have been inlined, variables may have been optimized out, and so on. So when
// this happens, SpiderMonkey builds one or more Rematerialized frames from the
// IonMonkey frame, using the snapshot metadata built by Ion to reconstruct the
// missing parts. The Rematerialized frames are now the authority on the state
// of those frames, and the Ion frame is ignored: stack iterators ignore the Ion
// frame, producing the Rematerialized frames in their stead; and when control
// returns to the Ion frame, we pop it, rebuild Baseline frames from the
// Rematerialized frames, and resume execution in Baseline.
class RematerializedFrame {
  // See DebugScopes::updateLiveScopes.
  bool prevUpToDate_;

  // Propagated to the Baseline frame once this is popped.
  bool isDebuggee_;

  // Has an initial environment has been pushed on the environment chain for
  // function frames that need a CallObject or eval frames that need a
  // VarEnvironmentObject?
  bool hasInitialEnv_;

  // Is this frame constructing?
  bool isConstructing_;

  // If true, this frame has been on the stack when
  // |js::SavedStacks::saveCurrentStack| was called, and so there is a
  // |js::SavedFrame| object cached for this frame.
  bool hasCachedSavedFrame_;

  // The fp of the top frame associated with this possibly inlined frame.
  uint8_t* top_;

  // The bytecode at the time of rematerialization.
  jsbytecode* pc_;

  size_t frameNo_;
  unsigned numActualArgs_;

  JSScript* script_;
  JSObject* envChain_;
  JSFunction* callee_;
  ArgumentsObject* argsObj_;

  Value returnValue_;
  Value thisArgument_;
  Value slots_[1];

  RematerializedFrame(JSContext* cx, uint8_t* top, unsigned numActualArgs,
                      InlineFrameIterator& iter, MaybeReadFallback& fallback);

 public:
  static RematerializedFrame* New(JSContext* cx, uint8_t* top,
                                  InlineFrameIterator& iter,
                                  MaybeReadFallback& fallback);

  // RematerializedFrame are allocated on non-GC heap, so use GCVector and
  // UniquePtr to ensure they are traced and cleaned up correctly.
  using RematerializedFrameVector = GCVector<UniquePtr<RematerializedFrame>>;

  // Rematerialize all remaining frames pointed to by |iter| into |frames|
  // in older-to-younger order, e.g., frames[0] is the oldest frame.
  [[nodiscard]] static bool RematerializeInlineFrames(
      JSContext* cx, uint8_t* top, InlineFrameIterator& iter,
      MaybeReadFallback& fallback, RematerializedFrameVector& frames);

  bool prevUpToDate() const { return prevUpToDate_; }
  void setPrevUpToDate() { prevUpToDate_ = true; }
  void unsetPrevUpToDate() { prevUpToDate_ = false; }

  bool isDebuggee() const { return isDebuggee_; }
  void setIsDebuggee() { isDebuggee_ = true; }
  inline void unsetIsDebuggee();

  uint8_t* top() const { return top_; }
  JSScript* outerScript() const {
    JitFrameLayout* jsFrame = (JitFrameLayout*)top_;
    return ScriptFromCalleeToken(jsFrame->calleeToken());
  }
  jsbytecode* pc() const { return pc_; }
  size_t frameNo() const { return frameNo_; }
  bool inlined() const { return frameNo_ > 0; }

  JSObject* environmentChain() const { return envChain_; }

  template <typename SpecificEnvironment>
  void pushOnEnvironmentChain(SpecificEnvironment& env) {
    MOZ_ASSERT(*environmentChain() == env.enclosingEnvironment());
    envChain_ = &env;
    if (IsFrameInitialEnvironment(this, env)) {
      hasInitialEnv_ = true;
    }
  }

  template <typename SpecificEnvironment>
  void popOffEnvironmentChain() {
    MOZ_ASSERT(envChain_->is<SpecificEnvironment>());
    envChain_ = &envChain_->as<SpecificEnvironment>().enclosingEnvironment();
  }

  [[nodiscard]] bool initFunctionEnvironmentObjects(JSContext* cx);
  [[nodiscard]] bool pushVarEnvironment(JSContext* cx, Handle<Scope*> scope);

  bool hasInitialEnvironment() const { return hasInitialEnv_; }
  CallObject& callObj() const;

  bool hasArgsObj() const { return !!argsObj_; }
  ArgumentsObject& argsObj() const {
    MOZ_ASSERT(hasArgsObj());
    MOZ_ASSERT(script()->needsArgsObj());
    return *argsObj_;
  }

  bool isFunctionFrame() const { return script_->isFunction(); }
  bool isGlobalFrame() const { return script_->isGlobalCode(); }
  bool isModuleFrame() const { return script_->isModule(); }

  JSScript* script() const { return script_; }
  JSFunction* callee() const {
    MOZ_ASSERT(isFunctionFrame());
    MOZ_ASSERT(callee_);
    return callee_;
  }
  Value calleev() const { return ObjectValue(*callee()); }
  Value& thisArgument() { return thisArgument_; }

  bool isConstructing() const { return isConstructing_; }

  bool hasCachedSavedFrame() const { return hasCachedSavedFrame_; }

  void setHasCachedSavedFrame() { hasCachedSavedFrame_ = true; }

  void clearHasCachedSavedFrame() { hasCachedSavedFrame_ = false; }

  unsigned numFormalArgs() const {
    return isFunctionFrame() ? callee()->nargs() : 0;
  }
  unsigned numActualArgs() const { return numActualArgs_; }
  unsigned numArgSlots() const {
    return (std::max)(numFormalArgs(), numActualArgs());
  }

  Value* argv() { return slots_; }
  Value* locals() { return slots_ + numArgSlots(); }

  Value& unaliasedLocal(unsigned i) {
    MOZ_ASSERT(i < script()->nfixed());
    return locals()[i];
  }
  Value& unaliasedFormal(unsigned i,
                         MaybeCheckAliasing checkAliasing = CHECK_ALIASING) {
    MOZ_ASSERT(i < numFormalArgs());
    MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals() &&
                                     !script()->formalIsAliased(i));
    return argv()[i];
  }
  Value& unaliasedActual(unsigned i,
                         MaybeCheckAliasing checkAliasing = CHECK_ALIASING) {
    MOZ_ASSERT(i < numActualArgs());
    MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
    MOZ_ASSERT_IF(checkAliasing && i < numFormalArgs(),
                  !script()->formalIsAliased(i));
    return argv()[i];
  }

  void setReturnValue(const Value& value) { returnValue_ = value; }

  Value& returnValue() {
    MOZ_ASSERT(!script()->noScriptRval());
    return returnValue_;
  }

  void trace(JSTracer* trc);
  void dump();
};

}  // namespace jit
}  // namespace js

#endif  // jit_RematerializedFrame_h

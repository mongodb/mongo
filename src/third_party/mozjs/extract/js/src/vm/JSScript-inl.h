/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSScript_inl_h
#define vm_JSScript_inl_h

#include "vm/JSScript.h"

#include <utility>

#include "jit/IonScript.h"
#include "jit/JitScript.h"
#include "vm/RegExpObject.h"
#include "wasm/AsmJS.h"

namespace js {

ScriptCounts::ScriptCounts() : ionCounts_(nullptr) {}

ScriptCounts::ScriptCounts(PCCountsVector&& jumpTargets)
    : pcCounts_(std::move(jumpTargets)), ionCounts_(nullptr) {}

ScriptCounts::ScriptCounts(ScriptCounts&& src)
    : pcCounts_(std::move(src.pcCounts_)),
      throwCounts_(std::move(src.throwCounts_)),
      ionCounts_(std::move(src.ionCounts_)) {
  src.ionCounts_ = nullptr;
}

ScriptCounts& ScriptCounts::operator=(ScriptCounts&& src) {
  pcCounts_ = std::move(src.pcCounts_);
  throwCounts_ = std::move(src.throwCounts_);
  ionCounts_ = std::move(src.ionCounts_);
  src.ionCounts_ = nullptr;
  return *this;
}

ScriptCounts::~ScriptCounts() { js_delete(ionCounts_); }

ScriptAndCounts::ScriptAndCounts(JSScript* script) : script(script) {
  script->releaseScriptCounts(&scriptCounts);
}

ScriptAndCounts::ScriptAndCounts(ScriptAndCounts&& sac)
    : script(std::move(sac.script)),
      scriptCounts(std::move(sac.scriptCounts)) {}

void SetFrameArgumentsObject(JSContext* cx, AbstractFramePtr frame,
                             HandleScript script, JSObject* argsobj);

inline void ScriptWarmUpData::initEnclosingScript(BaseScript* enclosingScript) {
  MOZ_ASSERT(data_ == ResetState());
  setTaggedPtr<EnclosingScriptTag>(enclosingScript);
  static_assert(std::is_base_of_v<gc::TenuredCell, BaseScript>,
                "BaseScript must be TenuredCell to avoid post-barriers");
}
inline void ScriptWarmUpData::clearEnclosingScript() {
  gc::PreWriteBarrier(toEnclosingScript());
  data_ = ResetState();
}

inline void ScriptWarmUpData::initEnclosingScope(Scope* enclosingScope) {
  MOZ_ASSERT(data_ == ResetState());
  setTaggedPtr<EnclosingScopeTag>(enclosingScope);
  static_assert(std::is_base_of_v<gc::TenuredCell, Scope>,
                "Scope must be TenuredCell to avoid post-barriers");
}
inline void ScriptWarmUpData::clearEnclosingScope() {
  gc::PreWriteBarrier(toEnclosingScope());
  data_ = ResetState();
}

inline JSPrincipals* BaseScript::principals() const {
  return realm()->principals();
}

inline JSScript* BaseScript::asJSScript() {
  MOZ_ASSERT(hasBytecode());
  return static_cast<JSScript*>(this);
}

}  // namespace js

inline JSFunction* JSScript::getFunction(js::GCThingIndex index) const {
  JSObject* obj = getObject(index);
  MOZ_RELEASE_ASSERT(obj->is<JSFunction>(), "Script object is not JSFunction");
  JSFunction* fun = &obj->as<JSFunction>();
  MOZ_ASSERT_IF(fun->isNativeFun(), IsAsmJSModuleNative(fun->native()));
  return fun;
}

inline JSFunction* JSScript::getFunction(jsbytecode* pc) const {
  return getFunction(GET_GCTHING_INDEX(pc));
}

inline js::RegExpObject* JSScript::getRegExp(js::GCThingIndex index) const {
  JSObject* obj = getObject(index);
  MOZ_RELEASE_ASSERT(obj->is<js::RegExpObject>(),
                     "Script object is not RegExpObject");
  return &obj->as<js::RegExpObject>();
}

inline js::RegExpObject* JSScript::getRegExp(jsbytecode* pc) const {
  JSObject* obj = getObject(pc);
  MOZ_RELEASE_ASSERT(obj->is<js::RegExpObject>(),
                     "Script object is not RegExpObject");
  return &obj->as<js::RegExpObject>();
}

inline js::GlobalObject& JSScript::global() const {
  /*
   * A JSScript always marks its realm's global so we can assert it's non-null
   * here. We don't need a read barrier here for the same reason
   * JSObject::nonCCWGlobal doesn't need one.
   */
  return *realm()->unsafeUnbarrieredMaybeGlobal();
}

inline bool JSScript::hasGlobal(const js::GlobalObject* global) const {
  return global == realm()->unsafeUnbarrieredMaybeGlobal();
}

inline js::LexicalScope* JSScript::maybeNamedLambdaScope() const {
  // Dynamically created Functions via the 'new Function' are considered
  // named lambdas but they do not have the named lambda scope of
  // textually-created named lambdas.
  js::Scope* scope = outermostScope();
  if (scope->kind() == js::ScopeKind::NamedLambda ||
      scope->kind() == js::ScopeKind::StrictNamedLambda) {
    MOZ_ASSERT_IF(!strict(), scope->kind() == js::ScopeKind::NamedLambda);
    MOZ_ASSERT_IF(strict(), scope->kind() == js::ScopeKind::StrictNamedLambda);
    return &scope->as<js::LexicalScope>();
  }
  return nullptr;
}

inline js::Shape* JSScript::initialEnvironmentShape() const {
  js::Scope* scope = bodyScope();
  if (scope->is<js::FunctionScope>()) {
    if (js::Shape* envShape = scope->environmentShape()) {
      return envShape;
    }
    if (js::Scope* namedLambdaScope = maybeNamedLambdaScope()) {
      return namedLambdaScope->environmentShape();
    }
  } else if (scope->is<js::EvalScope>()) {
    return scope->environmentShape();
  }
  return nullptr;
}

inline bool JSScript::isDebuggee() const {
  return realm()->debuggerObservesAllExecution() || hasDebugScript();
}

inline bool js::BaseScript::hasBaselineScript() const {
  return hasJitScript() && jitScript()->hasBaselineScript();
}

inline bool JSScript::isBaselineCompilingOffThread() const {
  return hasJitScript() && jitScript()->isBaselineCompiling();
}

inline bool js::BaseScript::hasIonScript() const {
  return hasJitScript() && jitScript()->hasIonScript();
}

inline bool JSScript::isIonCompilingOffThread() const {
  return hasJitScript() && jitScript()->isIonCompilingOffThread();
}

inline bool JSScript::canBaselineCompile() const {
  bool disabled = baselineDisabled();
#ifdef DEBUG
  if (hasJitScript()) {
    bool jitScriptDisabled =
        jitScript()->baselineScript_ == js::jit::BaselineDisabledScriptPtr;
    MOZ_ASSERT(disabled == jitScriptDisabled);
  }
#endif
  return !disabled;
}

inline bool JSScript::canIonCompile() const {
  bool disabled = ionDisabled();
#ifdef DEBUG
  if (hasJitScript()) {
    bool jitScriptDisabled =
        jitScript()->ionScript_ == js::jit::IonDisabledScriptPtr;
    MOZ_ASSERT(disabled == jitScriptDisabled);
  }
#endif
  return !disabled;
}

inline void JSScript::disableBaselineCompile() {
  MOZ_ASSERT(!hasBaselineScript());
  setFlag(MutableFlags::BaselineDisabled);
  if (hasJitScript()) {
    jitScript()->setBaselineScriptImpl(this,
                                       js::jit::BaselineDisabledScriptPtr);
  }
}

inline void JSScript::disableIon() {
  setFlag(MutableFlags::IonDisabled);
  if (hasJitScript()) {
    jitScript()->setIonScriptImpl(this, js::jit::IonDisabledScriptPtr);
  }
}

inline js::jit::BaselineScript* JSScript::baselineScript() const {
  return jitScript()->baselineScript();
}

inline js::jit::IonScript* JSScript::ionScript() const {
  return jitScript()->ionScript();
}

inline uint32_t JSScript::getWarmUpCount() const {
  if (warmUpData_.isWarmUpCount()) {
    return warmUpData_.toWarmUpCount();
  }
  return warmUpData_.toJitScript()->warmUpCount();
}

inline void JSScript::updateLastICStubCounter() {
  if (!hasJitScript()) {
    return;
  }
  jitScript()->updateLastICStubCounter();
}

inline uint32_t JSScript::warmUpCountAtLastICStub() const {
  MOZ_ASSERT(hasJitScript());
  return jitScript()->warmUpCountAtLastICStub();
}

inline void JSScript::incWarmUpCounter() {
  if (warmUpData_.isWarmUpCount()) {
    warmUpData_.incWarmUpCount();
  } else {
    warmUpData_.toJitScript()->incWarmUpCount();
  }
}

inline void JSScript::resetWarmUpCounterForGC() {
  incWarmUpResetCounter();
  if (warmUpData_.isWarmUpCount()) {
    warmUpData_.resetWarmUpCount(0);
  } else {
    warmUpData_.toJitScript()->resetWarmUpCount(0);
  }
}

#endif /* vm_JSScript_inl_h */

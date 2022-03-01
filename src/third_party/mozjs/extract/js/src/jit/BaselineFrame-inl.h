/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrame_inl_h
#define jit_BaselineFrame_inl_h

#include "jit/BaselineFrame.h"

#include "vm/JSContext.h"
#include "vm/Realm.h"

#include "vm/EnvironmentObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"  // js::NativeObject::initDenseElementsFromRange

namespace js {
namespace jit {

template <typename SpecificEnvironment>
inline void BaselineFrame::pushOnEnvironmentChain(SpecificEnvironment& env) {
  MOZ_ASSERT(*environmentChain() == env.enclosingEnvironment());
  envChain_ = &env;
  if (IsFrameInitialEnvironment(this, env)) {
    flags_ |= HAS_INITIAL_ENV;
  }
}

template <typename SpecificEnvironment>
inline void BaselineFrame::popOffEnvironmentChain() {
  MOZ_ASSERT(envChain_->is<SpecificEnvironment>());
  envChain_ = &envChain_->as<SpecificEnvironment>().enclosingEnvironment();
}

inline void BaselineFrame::replaceInnermostEnvironment(EnvironmentObject& env) {
  MOZ_ASSERT(env.enclosingEnvironment() ==
             envChain_->as<EnvironmentObject>().enclosingEnvironment());
  envChain_ = &env;
}

inline bool BaselineFrame::saveGeneratorSlots(JSContext* cx, unsigned nslots,
                                              ArrayObject* dest) const {
  // By convention, generator slots are stored in interpreter order,
  // which is the reverse of BaselineFrame order.

  MOZ_ASSERT(nslots == numValueSlots(debugFrameSize()) - 1);
  const Value* end = reinterpret_cast<const Value*>(this);
  mozilla::Span<const Value> span{end - nslots, end};
  return dest->initDenseElementsFromRange(cx, span.rbegin(), span.rend());
}

inline bool BaselineFrame::pushLexicalEnvironment(JSContext* cx,
                                                  Handle<LexicalScope*> scope) {
  BlockLexicalEnvironmentObject* env =
      BlockLexicalEnvironmentObject::createForFrame(cx, scope, this);
  if (!env) {
    return false;
  }
  pushOnEnvironmentChain(*env);

  return true;
}

inline bool BaselineFrame::pushClassBodyEnvironment(
    JSContext* cx, Handle<ClassBodyScope*> scope) {
  ClassBodyLexicalEnvironmentObject* env =
      ClassBodyLexicalEnvironmentObject::createForFrame(cx, scope, this);
  if (!env) {
    return false;
  }
  pushOnEnvironmentChain(*env);

  return true;
}

inline bool BaselineFrame::freshenLexicalEnvironment(JSContext* cx) {
  Rooted<BlockLexicalEnvironmentObject*> current(
      cx, &envChain_->as<BlockLexicalEnvironmentObject>());
  BlockLexicalEnvironmentObject* clone =
      BlockLexicalEnvironmentObject::clone(cx, current);
  if (!clone) {
    return false;
  }

  replaceInnermostEnvironment(*clone);
  return true;
}

inline bool BaselineFrame::recreateLexicalEnvironment(JSContext* cx) {
  Rooted<BlockLexicalEnvironmentObject*> current(
      cx, &envChain_->as<BlockLexicalEnvironmentObject>());
  BlockLexicalEnvironmentObject* clone =
      BlockLexicalEnvironmentObject::recreate(cx, current);
  if (!clone) {
    return false;
  }

  replaceInnermostEnvironment(*clone);
  return true;
}

inline CallObject& BaselineFrame::callObj() const {
  MOZ_ASSERT(hasInitialEnvironment());
  MOZ_ASSERT(callee()->needsCallObject());

  JSObject* obj = environmentChain();
  while (!obj->is<CallObject>()) {
    obj = obj->enclosingEnvironment();
  }
  return obj->as<CallObject>();
}

inline JSScript* BaselineFrame::outerScript() const {
  if (!icScript()->isInlined()) {
    return script();
  }
  return icScript()->inliningRoot()->owningScript();
}

inline void BaselineFrame::unsetIsDebuggee() {
  MOZ_ASSERT(!script()->isDebuggee());
  flags_ &= ~DEBUGGEE;
}

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineFrame_inl_h */

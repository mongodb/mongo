/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_EnvironmentObject_inl_h
#define vm_EnvironmentObject_inl_h

#include "vm/EnvironmentObject.h"

#include "vm/JSObject-inl.h"

namespace js {

inline ExtensibleLexicalEnvironmentObject&
NearestEnclosingExtensibleLexicalEnvironment(JSObject* env) {
  MOZ_ASSERT(env);
  while (!env->is<ExtensibleLexicalEnvironmentObject>()) {
    env = env->enclosingEnvironment();
    MOZ_ASSERT(env);
  }
  return env->as<ExtensibleLexicalEnvironmentObject>();
}

// Returns the innermost "qualified var object" on the environment chain.
// See the JSObject::isQualifiedVarObj comment for more info.
inline JSObject& GetVariablesObject(JSObject* envChain) {
  while (!envChain->isQualifiedVarObj()) {
    envChain = envChain->enclosingEnvironment();
  }
  MOZ_ASSERT(envChain);
  return *envChain;
}

inline const Value& EnvironmentObject::aliasedBinding(
    EnvironmentCoordinate ec) {
  MOZ_ASSERT(!is<ExtensibleLexicalEnvironmentObject>());
  MOZ_ASSERT(nonExtensibleIsFixedSlot(ec) ==
             NativeObject::isFixedSlot(ec.slot()));
  return getSlot(ec.slot());
}

inline void EnvironmentObject::setAliasedBinding(uint32_t slot,
                                                 const Value& v) {
  setSlot(slot, v);
}

inline void EnvironmentObject::setAliasedBinding(EnvironmentCoordinate ec,
                                                 const Value& v) {
  MOZ_ASSERT(!is<ExtensibleLexicalEnvironmentObject>());
  MOZ_ASSERT(nonExtensibleIsFixedSlot(ec) ==
             NativeObject::isFixedSlot(ec.slot()));
  setAliasedBinding(ec.slot(), v);
}

inline void EnvironmentObject::setAliasedBinding(const BindingIter& bi,
                                                 const Value& v) {
  MOZ_ASSERT(bi.location().kind() == BindingLocation::Kind::Environment);
  setAliasedBinding(bi.location().slot(), v);
}

inline void CallObject::setAliasedFormalFromArguments(const Value& argsValue,
                                                      const Value& v) {
  setSlot(ArgumentsObject::SlotFromMagicScopeSlotValue(argsValue), v);
}

} /* namespace js */

inline JSObject* JSObject::enclosingEnvironment() const {
  if (is<js::EnvironmentObject>()) {
    return &as<js::EnvironmentObject>().enclosingEnvironment();
  }

  if (is<js::DebugEnvironmentProxy>()) {
    return &as<js::DebugEnvironmentProxy>().enclosingEnvironment();
  }

  if (is<js::GlobalObject>()) {
    return nullptr;
  }

  MOZ_ASSERT_IF(is<JSFunction>(), as<JSFunction>().isInterpreted());
  return &nonCCWGlobal();
}

#endif /* vm_EnvironmentObject_inl_h */

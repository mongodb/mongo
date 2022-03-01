/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_ClearEdgesTracer_h
#define gc_ClearEdgesTracer_h

#include "js/TracingAPI.h"

namespace js {
namespace gc {

struct ClearEdgesTracer final : public GenericTracer {
  explicit ClearEdgesTracer(JSRuntime* rt);
  ClearEdgesTracer();

  template <typename T>
  inline T* onEdge(T* thing);

  JSObject* onObjectEdge(JSObject* obj) override;
  JSString* onStringEdge(JSString* str) override;
  JS::Symbol* onSymbolEdge(JS::Symbol* sym) override;
  JS::BigInt* onBigIntEdge(JS::BigInt* bi) override;
  js::BaseScript* onScriptEdge(js::BaseScript* script) override;
  js::Shape* onShapeEdge(js::Shape* shape) override;
  js::BaseShape* onBaseShapeEdge(js::BaseShape* base) override;
  js::GetterSetter* onGetterSetterEdge(js::GetterSetter* gs) override;
  js::PropMap* onPropMapEdge(js::PropMap* map) override;
  js::jit::JitCode* onJitCodeEdge(js::jit::JitCode* code) override;
  js::Scope* onScopeEdge(js::Scope* scope) override;
  js::RegExpShared* onRegExpSharedEdge(js::RegExpShared* shared) override;
};

}  // namespace gc
}  // namespace js

#endif  // gc_ClearEdgesTracer_h

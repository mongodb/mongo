/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* WindowProxy and Window implementation, for the web browser embedding. */

#include "js/friend/WindowProxy.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "js/Context.h"       // js::AssertHeapIsIdle
#include "vm/GlobalObject.h"  // js::GlobalObject
#include "vm/JSContext.h"     // JSContext, CHECK_THREAD
#include "vm/JSObject.h"      // JSObject
#include "vm/Runtime.h"       // JSRuntime

#include "vm/JSContext-inl.h"  // JSContext::check
#include "vm/JSObject-inl.h"   // JSObject::nonCCWGlobal

using JS::Handle;

void js::SetWindowProxyClass(JSContext* cx, const JSClass* clasp) {
  MOZ_ASSERT(!cx->runtime()->maybeWindowProxyClass());
  cx->runtime()->setWindowProxyClass(clasp);
}

void js::SetWindowProxy(JSContext* cx, Handle<JSObject*> global,
                        Handle<JSObject*> windowProxy) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  cx->check(global, windowProxy);
  MOZ_ASSERT(IsWindowProxy(windowProxy));

  GlobalObject& globalObj = global->as<GlobalObject>();
  if (globalObj.maybeWindowProxy() != windowProxy) {
    globalObj.setWindowProxy(windowProxy);
    globalObj.lexicalEnvironment().setWindowProxyThisObject(windowProxy);
  }
}

JSObject* js::ToWindowIfWindowProxy(JSObject* obj) {
  if (IsWindowProxy(obj)) {
    return &obj->nonCCWGlobal();
  }

  return obj;
}

JSObject* js::detail::ToWindowProxyIfWindowSlow(JSObject* obj) {
  if (JSObject* windowProxy = obj->as<GlobalObject>().maybeWindowProxy()) {
    return windowProxy;
  }

  return obj;
}

bool js::IsWindowProxy(JSObject* obj) {
  // Note: simply checking `obj == obj->global().windowProxy()` is not
  // sufficient: we may have transplanted the window proxy with a CCW.
  // Check the Class to ensure we really have a window proxy.
  return obj->getClass() ==
         obj->runtimeFromAnyThread()->maybeWindowProxyClass();
}

bool js::detail::IsWindowSlow(JSObject* obj) {
  return obj->as<GlobalObject>().maybeWindowProxy();
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* DOM proxy details that don't need to be exported as friend API. */

#ifndef proxy_DOMProxy_h
#define proxy_DOMProxy_h

#include "js/friend/DOMProxy.h"  // JS::DOMProxyShadowsCheck

class JS_PUBLIC_API JSObject;

namespace js {

extern const void* GetDOMProxyHandlerFamily();

extern JS::DOMProxyShadowsCheck GetDOMProxyShadowsCheck();

inline bool DOMProxyIsShadowing(JS::DOMProxyShadowsResult result) {
  return result == JS::DOMProxyShadowsResult::Shadows ||
         result == JS::DOMProxyShadowsResult::ShadowsViaDirectExpando ||
         result == JS::DOMProxyShadowsResult::ShadowsViaIndirectExpando;
}

extern const void* GetDOMRemoteProxyHandlerFamily();

extern bool IsDOMRemoteProxyObject(JSObject* object);

}  // namespace js

#endif  // proxy_DOMProxy_h

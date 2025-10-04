/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* DOM proxy-related functionality, including expando support. */

#include "js/friend/DOMProxy.h"  // JS::DOMProxyShadowsCheck
#include "proxy/DOMProxy.h"

#include "js/Proxy.h"  // js::GetProxyHandler, js::IsProxy

using JS::DOMProxyShadowsCheck;

static const void* gDOMProxyHandlerFamily = nullptr;
static DOMProxyShadowsCheck gDOMProxyShadowsCheck = nullptr;
static const void* gDOMRemoteProxyHandlerFamily = nullptr;

void JS::SetDOMProxyInformation(const void* domProxyHandlerFamily,
                                DOMProxyShadowsCheck domProxyShadowsCheck,
                                const void* domRemoteProxyHandlerFamily) {
  gDOMProxyHandlerFamily = domProxyHandlerFamily;
  gDOMProxyShadowsCheck = domProxyShadowsCheck;
  gDOMRemoteProxyHandlerFamily = domRemoteProxyHandlerFamily;
}

const void* js::GetDOMProxyHandlerFamily() { return gDOMProxyHandlerFamily; }

DOMProxyShadowsCheck js::GetDOMProxyShadowsCheck() {
  return gDOMProxyShadowsCheck;
}

const void* js::GetDOMRemoteProxyHandlerFamily() {
  return gDOMRemoteProxyHandlerFamily;
}

bool js::IsDOMRemoteProxyObject(JSObject* object) {
  return js::IsProxy(object) && js::GetProxyHandler(object)->family() ==
                                    js::GetDOMRemoteProxyHandlerFamily();
}

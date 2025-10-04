/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JIT info so SpiderMonkey can efficiently work with Gecko XrayWrapper
 * instances.
 *
 * This header is completely irrelevant to non-Gecko embedders.
 */

#ifndef js_friend_XrayJitInfo_h
#define js_friend_XrayJitInfo_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

class JS_PUBLIC_API JSObject;

namespace js {

class JS_PUBLIC_API BaseProxyHandler;

}  // namespace js

namespace JS {

// Callbacks and other information for use by the JITs when optimizing accesses
// on xray wrappers.
struct XrayJitInfo {
  // Test whether a proxy handler is a cross compartment xray with no
  // security checks.
  bool (*isCrossCompartmentXray)(const js::BaseProxyHandler* handler);

  // Test whether xrays in |obj|'s compartment have expandos of their own,
  // instead of sharing them with Xrays from other compartments.
  bool (*compartmentHasExclusiveExpandos)(JSObject* obj);

  // Proxy reserved slot used by xrays in sandboxes to store their holder
  // object.
  size_t xrayHolderSlot;

  // Reserved slot used by xray holders to store the xray's expando object.
  size_t holderExpandoSlot;

  // Reserved slot used by xray expandos to store a custom prototype.
  size_t expandoProtoSlot;
};

extern JS_PUBLIC_API void SetXrayJitInfo(XrayJitInfo* info);

}  // namespace JS

#endif  // js_friend_XrayJitInfo_h

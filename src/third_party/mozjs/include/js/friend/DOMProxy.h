/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Specify information about DOMProxy proxies in the DOM, for use by ICs.
 *
 * Embedders who don't need to define particularly high-performance proxies that
 * can have random properties added to them can ignore this header.
 */

#ifndef js_friend_DOMProxy_h
#define js_friend_DOMProxy_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint64_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Id.h"          // JS::PropertyKey
#include "js/RootingAPI.h"  // JS::Handle, JS::Heap
#include "js/Value.h"       // JS::UndefinedValue, JS::Value

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

/*
 * The DOMProxyShadowsCheck function will be called to check if the property for
 * id should be gotten from the prototype, or if there is an own property that
 * shadows it.
 * * If ShadowsViaDirectExpando is returned, then the slot at
 *   listBaseExpandoSlot contains an expando object which has the property in
 *   question.
 * * If ShadowsViaIndirectExpando is returned, then the slot at
 *   listBaseExpandoSlot contains a private pointer to an ExpandoAndGeneration
 *   and the expando object in the ExpandoAndGeneration has the property in
 *   question.
 * * If DoesntShadow is returned then the slot at listBaseExpandoSlot should
 *   either be undefined or point to an expando object that would contain the
 *   own property.
 * * If DoesntShadowUnique is returned then the slot at listBaseExpandoSlot
 *   should contain a private pointer to a ExpandoAndGeneration, which contains
 *   a JS::Value that should either be undefined or point to an expando object,
 *   and a uint64 value. If that value changes then the IC for getting a
 *   property will be invalidated.
 * * If Shadows is returned, that means the property is an own property of the
 *   proxy but doesn't live on the expando object.
 */

struct ExpandoAndGeneration {
  ExpandoAndGeneration() : expando(JS::UndefinedValue()), generation(0) {}

  void OwnerUnlinked() { ++generation; }

  static constexpr size_t offsetOfExpando() {
    return offsetof(ExpandoAndGeneration, expando);
  }

  static constexpr size_t offsetOfGeneration() {
    return offsetof(ExpandoAndGeneration, generation);
  }

  Heap<Value> expando;
  uint64_t generation;
};

enum class DOMProxyShadowsResult {
  ShadowCheckFailed,
  Shadows,
  DoesntShadow,
  DoesntShadowUnique,
  ShadowsViaDirectExpando,
  ShadowsViaIndirectExpando
};

using DOMProxyShadowsCheck = DOMProxyShadowsResult (*)(JSContext*,
                                                       Handle<JSObject*>,
                                                       Handle<JS::PropertyKey>);

extern JS_PUBLIC_API void SetDOMProxyInformation(
    const void* domProxyHandlerFamily,
    DOMProxyShadowsCheck domProxyShadowsCheck,
    const void* domRemoteProxyHandlerFamily);

}  // namespace JS

#endif  // js_friend_DOMProxy_h

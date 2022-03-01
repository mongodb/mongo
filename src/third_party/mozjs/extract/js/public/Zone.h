/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef js_Zone_h
#define js_Zone_h

#include "jspubtd.h"
#include "js/Context.h"

// [SMDOC] Nested GC Data Structures (Compartments and Zones)
//
// The GC has two nested data structures, Zones and Compartents. Each has
// distint responsibilities. Zones contain compartments, along with other GC
// resources used by a tab. Compartments contain realms, which are specification
// defined.
//
// See also the SMDoc on Realms.
//
// Compartment
// -----------
// Security membrane; when an object from compartment A is used in compartment
// B, a cross-compartment wrapper (a kind of proxy) is used. In the browser,
// same-origin realms can share a compartment.
//
// Zone
// ----
// A Zone is a group of compartments that share GC resources (arenas, strings,
// etc) for memory usage and performance reasons. Zone is the GC unit: the GC
// can operate on one or more zones at a time. The browser uses roughly one zone
// per tab.

using JSDestroyZoneCallback = void (*)(JSFreeOp*, JS::Zone*);

using JSDestroyCompartmentCallback = void (*)(JSFreeOp*, JS::Compartment*);

using JSSizeOfIncludingThisCompartmentCallback =
    size_t (*)(mozilla::MallocSizeOf, JS::Compartment*);

extern JS_PUBLIC_API void JS_SetDestroyZoneCallback(
    JSContext* cx, JSDestroyZoneCallback callback);

extern JS_PUBLIC_API void JS_SetDestroyCompartmentCallback(
    JSContext* cx, JSDestroyCompartmentCallback callback);

extern JS_PUBLIC_API void JS_SetSizeOfIncludingThisCompartmentCallback(
    JSContext* cx, JSSizeOfIncludingThisCompartmentCallback callback);

extern JS_PUBLIC_API void JS_SetCompartmentPrivate(JS::Compartment* compartment,
                                                   void* data);

extern JS_PUBLIC_API void* JS_GetCompartmentPrivate(
    JS::Compartment* compartment);

extern JS_PUBLIC_API void JS_SetZoneUserData(JS::Zone* zone, void* data);

extern JS_PUBLIC_API void* JS_GetZoneUserData(JS::Zone* zone);

extern JS_PUBLIC_API bool JS_RefreshCrossCompartmentWrappers(
    JSContext* cx, JS::Handle<JSObject*> obj);

/**
 * Mark a jsid after entering a new compartment. Different zones separately
 * mark the ids in a runtime, and this must be used any time an id is obtained
 * from one compartment and then used in another compartment, unless the two
 * compartments are guaranteed to be in the same zone.
 */
extern JS_PUBLIC_API void JS_MarkCrossZoneId(JSContext* cx, jsid id);

/**
 * If value stores a jsid (an atomized string or symbol), mark that id as for
 * JS_MarkCrossZoneId.
 */
extern JS_PUBLIC_API void JS_MarkCrossZoneIdValue(JSContext* cx,
                                                  const JS::Value& value);

#endif  // js_Zone_h

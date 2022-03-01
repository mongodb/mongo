/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Various interfaces to iterate over the Realms given various context such as
 * principals, compartments and GC zones.
 */

#ifndef js_RealmIterators_h
#define js_RealmIterators_h

#include "js/GCAPI.h"
#include "js/TypeDecls.h"

struct JSPrincipals;

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;

using IterateRealmCallback = void (*)(JSContext* cx, void* data, Realm* realm,
                                      const AutoRequireNoGC& nogc);

/**
 * This function calls |realmCallback| on every realm. Beware that there is no
 * guarantee that the realm will survive after the callback returns. Also,
 * barriers are disabled via the TraceSession.
 */
extern JS_PUBLIC_API void IterateRealms(JSContext* cx, void* data,
                                        IterateRealmCallback realmCallback);

/**
 * Like IterateRealms, but only call the callback for realms using |principals|.
 */
extern JS_PUBLIC_API void IterateRealmsWithPrincipals(
    JSContext* cx, JSPrincipals* principals, void* data,
    IterateRealmCallback realmCallback);

/**
 * Like IterateRealms, but only iterates realms in |compartment|.
 */
extern JS_PUBLIC_API void IterateRealmsInCompartment(
    JSContext* cx, JS::Compartment* compartment, void* data,
    IterateRealmCallback realmCallback);

/**
 * An enum that JSIterateCompartmentCallback can return to indicate
 * whether to keep iterating.
 */
enum class CompartmentIterResult { KeepGoing, Stop };

}  // namespace JS

using JSIterateCompartmentCallback =
    JS::CompartmentIterResult (*)(JSContext*, void*, JS::Compartment*);

/**
 * This function calls |compartmentCallback| on every compartment until either
 * all compartments have been iterated or CompartmentIterResult::Stop is
 * returned. Beware that there is no guarantee that the compartment will survive
 * after the callback returns. Also, barriers are disabled via the TraceSession.
 */
extern JS_PUBLIC_API void JS_IterateCompartments(
    JSContext* cx, void* data,
    JSIterateCompartmentCallback compartmentCallback);

/**
 * This function calls |compartmentCallback| on every compartment in the given
 * zone until either all compartments have been iterated or
 * CompartmentIterResult::Stop is returned. Beware that there is no guarantee
 * that the compartment will survive after the callback returns. Also, barriers
 * are disabled via the TraceSession.
 */
extern JS_PUBLIC_API void JS_IterateCompartmentsInZone(
    JSContext* cx, JS::Zone* zone, void* data,
    JSIterateCompartmentCallback compartmentCallback);

#endif /* js_RealmIterators_h */

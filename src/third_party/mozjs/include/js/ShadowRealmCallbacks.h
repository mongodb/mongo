/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_ShadowReamCallbacks_h
#define js_ShadowReamCallbacks_h

#include "jstypes.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

class RealmOptions;

using GlobalInitializeCallback = bool (*)(JSContext*, JS::Handle<JSObject*>);

// Install the HostInitializeShadowRealm callback that will be invoked when
// creating a shadow realm.
//
// The callback will be passed the realm's global object, so that it is possible
// for the embedding to make any host-determined manipulations to the global,
// such as installing interfaces or helpers that should exist even within
// ShadowRealms. (For example, in the web platform, WebIDL with the
// [Exposed=*] attribute should be installed within a shadow realm.)
extern JS_PUBLIC_API void SetShadowRealmInitializeGlobalCallback(
    JSContext* cx, GlobalInitializeCallback callback);

using GlobalCreationCallback =
    JSObject* (*)(JSContext* cx, JS::RealmOptions& creationOptions,
                  JSPrincipals* principals,
                  JS::Handle<JSObject*> enclosingGlobal);

// Create the Global object for a ShadowRealm.
//
// This isn't directly specified, however at least in Gecko, in order to
// correctly implement HostInitializeShadowRealm, there are requirements
// placed on the global for the ShadowRealm.
//
// This callback should return a Global object compatible with the
// callback installed by SetShadowRealmInitializeGlobalCallback
extern JS_PUBLIC_API void SetShadowRealmGlobalCreationCallback(
    JSContext* cx, GlobalCreationCallback callback);

}  // namespace JS

#endif  // js_ShadowReamCallbacks_h

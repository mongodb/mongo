/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Realm_h
#define js_Realm_h

#include "js/shadow/Realm.h"  // JS::shadow::Realm

#include "js/GCPolicyAPI.h"
#include "js/TypeDecls.h"  // forward-declaration of JS::Realm

/************************************************************************/

// [SMDOC] Realms
//
// Data associated with a global object. In the browser each frame has its
// own global/realm.

namespace js {
namespace gc {
JS_PUBLIC_API void TraceRealm(JSTracer* trc, JS::Realm* realm,
                              const char* name);
}  // namespace gc
}  // namespace js

namespace JS {
class JS_PUBLIC_API AutoRequireNoGC;

// Each Realm holds a weak reference to its GlobalObject.
template <>
struct GCPolicy<Realm*> : public NonGCPointerPolicy<Realm*> {
  static void trace(JSTracer* trc, Realm** vp, const char* name) {
    if (*vp) {
      ::js::gc::TraceRealm(trc, *vp, name);
    }
  }
};

// Get the current realm, if any. The ECMAScript spec calls this "the current
// Realm Record".
extern JS_PUBLIC_API Realm* GetCurrentRealmOrNull(JSContext* cx);

// Return the compartment that contains a given realm.
inline JS::Compartment* GetCompartmentForRealm(Realm* realm) {
  return shadow::Realm::get(realm)->compartment();
}

// Return an object's realm. All objects except cross-compartment wrappers are
// created in a particular realm, which never changes. Returns null if obj is
// a cross-compartment wrapper.
extern JS_PUBLIC_API Realm* GetObjectRealmOrNull(JSObject* obj);

// Get the value of the "private data" internal field of the given Realm.
// This field is initially null and is set using SetRealmPrivate.
// It's a pointer to embeddding-specific data that SpiderMonkey never uses.
extern JS_PUBLIC_API void* GetRealmPrivate(Realm* realm);

// Set the "private data" internal field of the given Realm.
extern JS_PUBLIC_API void SetRealmPrivate(Realm* realm, void* data);

typedef void (*DestroyRealmCallback)(JS::GCContext* gcx, Realm* realm);

// Set the callback SpiderMonkey calls just before garbage-collecting a realm.
// Embeddings can use this callback to free private data associated with the
// realm via SetRealmPrivate.
//
// By the time this is called, the global object for the realm has already been
// collected.
extern JS_PUBLIC_API void SetDestroyRealmCallback(
    JSContext* cx, DestroyRealmCallback callback);

using RealmNameCallback = void (*)(JSContext* cx, Realm* realm, char* buf,
                                   size_t bufsize,
                                   const JS::AutoRequireNoGC& nogc);

// Set the callback SpiderMonkey calls to get the name of a realm, for
// diagnostic output.
extern JS_PUBLIC_API void SetRealmNameCallback(JSContext* cx,
                                               RealmNameCallback callback);

// Get the global object for the given realm. This only returns nullptr during
// GC, between collecting the global object and destroying the Realm.
extern JS_PUBLIC_API JSObject* GetRealmGlobalOrNull(Realm* realm);

// Initialize standard JS class constructors, prototypes, and any top-level
// functions and constants associated with the standard classes (e.g. isNaN
// for Number).
extern JS_PUBLIC_API bool InitRealmStandardClasses(JSContext* cx);

// If the current realm has the non-standard freezeBuiltins option set to true,
// freeze the constructor object and seal the prototype.
extern JS_PUBLIC_API bool MaybeFreezeCtorAndPrototype(JSContext* cx,
                                                      HandleObject ctor,
                                                      HandleObject maybeProto);

/*
 * Ways to get various per-Realm objects. All the getters declared below operate
 * on the JSContext's current Realm.
 */

extern JS_PUBLIC_API JSObject* GetRealmObjectPrototype(JSContext* cx);
extern JS_PUBLIC_API JS::Handle<JSObject*> GetRealmObjectPrototypeHandle(
    JSContext* cx);

extern JS_PUBLIC_API JSObject* GetRealmFunctionPrototype(JSContext* cx);

extern JS_PUBLIC_API JSObject* GetRealmArrayPrototype(JSContext* cx);

extern JS_PUBLIC_API JSObject* GetRealmErrorPrototype(JSContext* cx);

extern JS_PUBLIC_API JSObject* GetRealmIteratorPrototype(JSContext* cx);

extern JS_PUBLIC_API JSObject* GetRealmAsyncIteratorPrototype(JSContext* cx);

// Returns an object that represents the realm, that can be referred from
// other realm/compartment.
// See the consumer in `MaybeCrossOriginObjectMixins::EnsureHolder` for details.
extern JS_PUBLIC_API JSObject* GetRealmKeyObject(JSContext* cx);

// Implements https://tc39.github.io/ecma262/#sec-getfunctionrealm
// 7.3.22 GetFunctionRealm ( obj )
//
// WARNING: may return a realm in a different compartment!
//
// Will throw an exception and return nullptr when a security wrapper or revoked
// proxy is encountered.
extern JS_PUBLIC_API Realm* GetFunctionRealm(JSContext* cx,
                                             HandleObject objArg);

/** NB: This API is infallible; a nullptr return value does not indicate error.
 *
 * |target| must not be a cross-compartment wrapper because CCWs are not
 * associated with a single realm.
 *
 * Entering a realm roots the realm and its global object until the matching
 * JS::LeaveRealm() call.
 */
extern JS_PUBLIC_API JS::Realm* EnterRealm(JSContext* cx, JSObject* target);

extern JS_PUBLIC_API void LeaveRealm(JSContext* cx, JS::Realm* oldRealm);

}  // namespace JS

/*
 * At any time, a JSContext has a current (possibly-nullptr) realm. The
 * preferred way to change the current realm is with JSAutoRealm:
 *
 *   void foo(JSContext* cx, JSObject* obj) {
 *     // in some realm 'r'
 *     {
 *       JSAutoRealm ar(cx, obj);  // constructor enters
 *       // in the realm of 'obj'
 *     }                           // destructor leaves
 *     // back in realm 'r'
 *   }
 *
 * The object passed to JSAutoRealm must *not* be a cross-compartment wrapper,
 * because CCWs are not associated with a single realm.
 *
 * For more complicated uses that don't neatly fit in a C++ stack frame, the
 * realm can be entered and left using separate function calls:
 *
 *   void foo(JSContext* cx, JSObject* obj) {
 *     // in 'oldRealm'
 *     JS::Realm* oldRealm = JS::EnterRealm(cx, obj);
 *     // in the realm of 'obj'
 *     JS::LeaveRealm(cx, oldRealm);
 *     // back in 'oldRealm'
 *   }
 *
 * Note: these calls must still execute in a LIFO manner w.r.t all other
 * enter/leave calls on the context. Furthermore, only the return value of a
 * JS::EnterRealm call may be passed as the 'oldRealm' argument of
 * the corresponding JS::LeaveRealm call.
 *
 * Entering a realm roots the realm and its global object for the lifetime of
 * the JSAutoRealm.
 */

class MOZ_RAII JS_PUBLIC_API JSAutoRealm {
  JSContext* cx_;
  JS::Realm* oldRealm_;

 public:
  JSAutoRealm(JSContext* cx, JSObject* target);
  JSAutoRealm(JSContext* cx, JSScript* target);
  ~JSAutoRealm();
};

class MOZ_RAII JS_PUBLIC_API JSAutoNullableRealm {
  JSContext* cx_;
  JS::Realm* oldRealm_;

 public:
  explicit JSAutoNullableRealm(JSContext* cx, JSObject* targetOrNull);
  ~JSAutoNullableRealm();
};

#endif  // js_Realm_h

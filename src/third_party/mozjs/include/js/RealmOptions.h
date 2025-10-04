/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Options specified when creating a realm to determine its behavior, immutable
 * options determining the behavior of an existing realm, and mutable options on
 * an existing realm that may be changed when desired.
 */

#ifndef js_RealmOptions_h
#define js_RealmOptions_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Maybe.h"

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Class.h"  // JSTraceOp
#include "js/RefCounted.h"

struct JS_PUBLIC_API JSContext;
class JS_PUBLIC_API JSObject;

namespace JS {

class JS_PUBLIC_API Compartment;
class JS_PUBLIC_API Realm;
class JS_PUBLIC_API Zone;

}  // namespace JS

namespace JS {

/**
 * Specification for which compartment/zone a newly created realm should use.
 */
enum class CompartmentSpecifier {
  // Create a new realm and compartment in the single runtime wide system
  // zone. The meaning of this zone is left to the embedder.
  NewCompartmentInSystemZone,

  // Create a new realm and compartment in a particular existing zone.
  NewCompartmentInExistingZone,

  // Create a new zone/compartment.
  NewCompartmentAndZone,

  // Create a new realm in an existing compartment.
  ExistingCompartment,
};

struct LocaleString : js::RefCounted<LocaleString> {
  const char* chars_;

  explicit LocaleString(const char* chars) : chars_(chars) {}

  auto chars() const { return chars_; }
};

/**
 * RealmCreationOptions specifies options relevant to creating a new realm, that
 * are either immutable characteristics of that realm or that are discarded
 * after the realm has been created.
 *
 * Access to these options on an existing realm is read-only: if you need
 * particular selections, you must make them before you create the realm.
 */
class JS_PUBLIC_API RealmCreationOptions {
 public:
  RealmCreationOptions() : comp_(nullptr) {}

  JSTraceOp getTrace() const { return traceGlobal_; }
  RealmCreationOptions& setTrace(JSTraceOp op) {
    traceGlobal_ = op;
    return *this;
  }

  Zone* zone() const {
    MOZ_ASSERT(compSpec_ == CompartmentSpecifier::NewCompartmentInExistingZone);
    return zone_;
  }
  Compartment* compartment() const {
    MOZ_ASSERT(compSpec_ == CompartmentSpecifier::ExistingCompartment);
    return comp_;
  }
  CompartmentSpecifier compartmentSpecifier() const { return compSpec_; }

  // Set the compartment/zone to use for the realm. See CompartmentSpecifier
  // above.
  RealmCreationOptions& setNewCompartmentInSystemZone();
  RealmCreationOptions& setNewCompartmentInExistingZone(JSObject* obj);
  RealmCreationOptions& setNewCompartmentAndZone();
  RealmCreationOptions& setExistingCompartment(JSObject* obj);
  RealmCreationOptions& setExistingCompartment(Compartment* compartment);

  // Certain compartments are implementation details of the embedding, and
  // references to them should never leak out to script. This flag causes this
  // realm to skip firing onNewGlobalObject and makes addDebuggee a no-op for
  // this global.
  //
  // Debugger visibility is per-compartment, not per-realm (it's only practical
  // to enforce visibility on compartment boundaries), so if a realm is being
  // created in an extant compartment, its requested visibility must match that
  // of the compartment.
  bool invisibleToDebugger() const { return invisibleToDebugger_; }
  RealmCreationOptions& setInvisibleToDebugger(bool flag) {
    invisibleToDebugger_ = flag;
    return *this;
  }

  // Determines whether this realm should preserve JIT code on non-shrinking
  // GCs.
  bool preserveJitCode() const { return preserveJitCode_; }
  RealmCreationOptions& setPreserveJitCode(bool flag) {
    preserveJitCode_ = flag;
    return *this;
  }

  // Determines whether 1) the global Atomic property is defined and atomic
  // operations are supported, and 2) whether shared-memory operations are
  // supported.
  bool getSharedMemoryAndAtomicsEnabled() const;
  RealmCreationOptions& setSharedMemoryAndAtomicsEnabled(bool flag);

  // Determines (if getSharedMemoryAndAtomicsEnabled() is true) whether the
  // global SharedArrayBuffer property is defined.  If the property is not
  // defined, shared array buffer functionality can only be invoked if the
  // host/embedding specifically acts to expose it.
  //
  // This option defaults to true: embeddings unable to tolerate a global
  // SharedAraryBuffer property must opt out of it.
  bool defineSharedArrayBufferConstructor() const {
    return defineSharedArrayBufferConstructor_;
  }
  RealmCreationOptions& setDefineSharedArrayBufferConstructor(bool flag) {
    defineSharedArrayBufferConstructor_ = flag;
    return *this;
  }

  // Structured clone operations support the cloning of shared memory objects
  // (SharedArrayBuffer or or a shared WASM Memory object) *optionally* -- at
  // the discretion of the embedder code that performs the cloning.  When a
  // structured clone operation encounters a shared memory object and cloning
  // shared memory objects has not been enabled, the clone fails and an
  // error is thrown.
  //
  // In the web embedding context, shared memory object cloning is disabled
  // either because
  //
  //   1) *no* way of supporting it is available (because the
  //      Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy HTTP
  //      headers are not respected to force the page into its own process), or
  //   2) the aforementioned HTTP headers don't specify that the page should be
  //      opened in its own process.
  //
  // These two scenarios demand different error messages, and this option can be
  // used to specify which scenario is in play.
  //
  // In the former case, if COOP/COEP support is not enabled, set this option to
  // false.  (This is the default.)
  //
  // In the latter case, if COOP/COEP weren't used to force this page into its
  // own process, set this option to true.
  //
  // (Embeddings that are not the web and do not wish to support structured
  // cloning of shared memory objects will get a "bad" web-centric error message
  // no matter what.  At present, SpiderMonkey does not offer a way for such
  // embeddings to use an embedding-specific error message.)
  bool getCoopAndCoepEnabled() const;
  RealmCreationOptions& setCoopAndCoepEnabled(bool flag);

  bool getToSourceEnabled() const { return toSource_; }
  RealmCreationOptions& setToSourceEnabled(bool flag) {
    toSource_ = flag;
    return *this;
  }

  // This flag doesn't affect JS engine behavior.  It is used by Gecko to
  // mark whether content windows and workers are "Secure Context"s. See
  // https://w3c.github.io/webappsec-secure-contexts/
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1162772#c34
  bool secureContext() const { return secureContext_; }
  RealmCreationOptions& setSecureContext(bool flag) {
    secureContext_ = flag;
    return *this;
  }

  // Non-standard option to freeze certain builtin constructors and seal their
  // prototypes. Also defines these constructors on the global as non-writable
  // and non-configurable.
  bool freezeBuiltins() const { return freezeBuiltins_; }
  RealmCreationOptions& setFreezeBuiltins(bool flag) {
    freezeBuiltins_ = flag;
    return *this;
  }

  // Force all date/time methods in JavaScript to use the UTC timezone for
  // fingerprinting protection.
  bool forceUTC() const { return forceUTC_; }
  RealmCreationOptions& setForceUTC(bool flag) {
    forceUTC_ = flag;
    return *this;
  }

  RefPtr<LocaleString> locale() const { return locale_; }
  RealmCreationOptions& setLocaleCopyZ(const char* locale);

  // Always use the fdlibm implementation of math functions instead of the
  // platform native libc implementations. Useful for fingerprinting protection
  // and cross-platform consistency.
  bool alwaysUseFdlibm() const { return alwaysUseFdlibm_; }
  RealmCreationOptions& setAlwaysUseFdlibm(bool flag) {
    alwaysUseFdlibm_ = flag;
    return *this;
  }

  uint64_t profilerRealmID() const { return profilerRealmID_; }
  RealmCreationOptions& setProfilerRealmID(uint64_t id) {
    profilerRealmID_ = id;
    return *this;
  }

 private:
  JSTraceOp traceGlobal_ = nullptr;
  CompartmentSpecifier compSpec_ = CompartmentSpecifier::NewCompartmentAndZone;
  union {
    Compartment* comp_;
    Zone* zone_;
  };
  uint64_t profilerRealmID_ = 0;
  RefPtr<LocaleString> locale_;
  bool invisibleToDebugger_ = false;
  bool preserveJitCode_ = false;
  bool sharedMemoryAndAtomics_ = false;
  bool defineSharedArrayBufferConstructor_ = true;
  bool coopAndCoep_ = false;
  bool toSource_ = false;

  bool secureContext_ = false;
  bool freezeBuiltins_ = false;
  bool forceUTC_ = false;
  bool alwaysUseFdlibm_ = false;
};

// This is a wrapper for mozilla::RTPCallerType, that can't easily
// be exposed to the JS engine for layering reasons.
struct RTPCallerTypeToken {
  uint8_t value;
};

/**
 * RealmBehaviors specifies behaviors of a realm that can be changed after the
 * realm's been created.
 */
class JS_PUBLIC_API RealmBehaviors {
 public:
  RealmBehaviors() = default;

  // When a JS::ReduceMicrosecondTimePrecisionCallback callback is defined via
  // JS::SetReduceMicrosecondTimePrecisionCallback, a JS::RTPCallerTypeToken (a
  // wrapper for mozilla::RTPCallerType) needs to be set for every Realm.
  mozilla::Maybe<RTPCallerTypeToken> reduceTimerPrecisionCallerType() const {
    return rtpCallerType;
  }
  RealmBehaviors& setReduceTimerPrecisionCallerType(RTPCallerTypeToken type) {
    rtpCallerType = mozilla::Some(type);
    return *this;
  }

  // For certain globals, we know enough about the code that will run in them
  // that we can discard script source entirely.
  bool discardSource() const { return discardSource_; }
  RealmBehaviors& setDiscardSource(bool flag) {
    discardSource_ = flag;
    return *this;
  }

  bool clampAndJitterTime() const { return clampAndJitterTime_; }
  RealmBehaviors& setClampAndJitterTime(bool flag) {
    clampAndJitterTime_ = flag;
    return *this;
  }

  // A Realm can stop being "live" in all the ways that matter before its global
  // is actually GCed.  Consumers that tear down parts of a Realm or its global
  // before that point should set isNonLive accordingly.
  bool isNonLive() const { return isNonLive_; }
  RealmBehaviors& setNonLive() {
    isNonLive_ = true;
    return *this;
  }

 private:
  mozilla::Maybe<RTPCallerTypeToken> rtpCallerType;
  bool discardSource_ = false;
  bool clampAndJitterTime_ = true;
  bool isNonLive_ = false;
};

/**
 * RealmOptions specifies realm characteristics: both those that can't be
 * changed on a realm once it's been created (RealmCreationOptions), and those
 * that can be changed on an existing realm (RealmBehaviors).
 */
class JS_PUBLIC_API RealmOptions {
 public:
  explicit RealmOptions() : creationOptions_(), behaviors_() {}

  RealmOptions(const RealmCreationOptions& realmCreation,
               const RealmBehaviors& realmBehaviors)
      : creationOptions_(realmCreation), behaviors_(realmBehaviors) {}

  // RealmCreationOptions specify fundamental realm characteristics that must
  // be specified when the realm is created, that can't be changed after the
  // realm is created.
  RealmCreationOptions& creationOptions() { return creationOptions_; }
  const RealmCreationOptions& creationOptions() const {
    return creationOptions_;
  }

  // RealmBehaviors specify realm characteristics that can be changed after
  // the realm is created.
  RealmBehaviors& behaviors() { return behaviors_; }
  const RealmBehaviors& behaviors() const { return behaviors_; }

 private:
  RealmCreationOptions creationOptions_;
  RealmBehaviors behaviors_;
};

extern JS_PUBLIC_API const RealmCreationOptions& RealmCreationOptionsRef(
    Realm* realm);

extern JS_PUBLIC_API const RealmCreationOptions& RealmCreationOptionsRef(
    JSContext* cx);

extern JS_PUBLIC_API const RealmBehaviors& RealmBehaviorsRef(Realm* realm);

extern JS_PUBLIC_API const RealmBehaviors& RealmBehaviorsRef(JSContext* cx);

extern JS_PUBLIC_API void SetRealmNonLive(Realm* realm);

// This behaves like RealmBehaviors::setReduceTimerPrecisionCallerType, but
// can be used even after the Realm has already been created.
extern JS_PUBLIC_API void SetRealmReduceTimerPrecisionCallerType(
    Realm* realm, RTPCallerTypeToken type);

}  // namespace JS

#endif  // js_RealmOptions_h

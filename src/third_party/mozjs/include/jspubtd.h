/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jspubtd_h
#define jspubtd_h

/*
 * JS public API typedefs.
 */

#include "jstypes.h"

#include "js/ProtoKey.h"
#include "js/Result.h"
#include "js/TraceKind.h"
#include "js/TypeDecls.h"

#if defined(JS_GC_ZEAL) || defined(DEBUG)
#  define JSGC_HASH_TABLE_CHECKS
#endif

namespace JS {

class CallArgs;

class JS_PUBLIC_API RealmOptions;

}  // namespace JS

/* Result of typeof operator enumeration. */
enum JSType {
  JSTYPE_UNDEFINED, /* undefined */
  JSTYPE_OBJECT,    /* object */
  JSTYPE_FUNCTION,  /* function */
  JSTYPE_STRING,    /* string */
  JSTYPE_NUMBER,    /* number */
  JSTYPE_BOOLEAN,   /* boolean */
  JSTYPE_SYMBOL,    /* symbol */
  JSTYPE_BIGINT,    /* BigInt */
#ifdef ENABLE_RECORD_TUPLE
  JSTYPE_RECORD, /* record */
  JSTYPE_TUPLE,  /* tuple */
#endif
  JSTYPE_LIMIT
};

/* Dense index into cached prototypes and class atoms for standard objects. */
enum JSProtoKey {
#define PROTOKEY_AND_INITIALIZER(name, clasp) JSProto_##name,
  JS_FOR_EACH_PROTOTYPE(PROTOKEY_AND_INITIALIZER)
#undef PROTOKEY_AND_INITIALIZER
      JSProto_LIMIT
};

/* Struct forward declarations. */
struct JSClass;
class JSErrorReport;
struct JSFunctionSpec;
struct JSPrincipals;
struct JSPropertySpec;
struct JSSecurityCallbacks;
struct JSStructuredCloneCallbacks;
struct JSStructuredCloneReader;
struct JSStructuredCloneWriter;
class JS_PUBLIC_API JSTracer;

class JSLinearString;

template <typename T>
struct JSConstScalarSpec;
using JSConstDoubleSpec = JSConstScalarSpec<double>;
using JSConstIntegerSpec = JSConstScalarSpec<int32_t>;

namespace js {

inline JS::Realm* GetContextRealm(const JSContext* cx);
inline JS::Compartment* GetContextCompartment(const JSContext* cx);
inline JS::Zone* GetContextZone(const JSContext* cx);

// Whether the current thread is permitted access to any part of the specified
// runtime or zone.
JS_PUBLIC_API bool CurrentThreadCanAccessRuntime(const JSRuntime* rt);

#ifdef DEBUG
JS_PUBLIC_API bool CurrentThreadIsMainThread();
JS_PUBLIC_API bool CurrentThreadIsPerformingGC();
#endif

}  // namespace js

namespace JS {

class JS_PUBLIC_API PropertyDescriptor;

// Decorates the Unlinking phase of CycleCollection so that accidental use
// of barriered accessors results in assertions instead of leaks.
class MOZ_STACK_CLASS JS_PUBLIC_API AutoEnterCycleCollection {
#ifdef DEBUG
  JSRuntime* runtime_;

 public:
  explicit AutoEnterCycleCollection(JSRuntime* rt);
  ~AutoEnterCycleCollection();
#else
 public:
  explicit AutoEnterCycleCollection(JSRuntime* rt) {}
  ~AutoEnterCycleCollection() {}
#endif
};

} /* namespace JS */

extern "C" {

// Defined in NSPR prio.h.
using PRFileDesc = struct PRFileDesc;
}

#endif /* jspubtd_h */

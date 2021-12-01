/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jspubtd_h
#define jspubtd_h

/*
 * JS public API typedefs.
 */

#include "mozilla/Assertions.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/LinkedList.h"
#include "mozilla/PodOperations.h"

#include "jstypes.h"

#include "js/ProtoKey.h"
#include "js/Result.h"
#include "js/TraceKind.h"
#include "js/TypeDecls.h"

#if defined(JS_GC_ZEAL) || defined(DEBUG)
# define JSGC_HASH_TABLE_CHECKS
#endif

namespace JS {

template <typename T> class AutoVector;
using AutoIdVector = AutoVector<jsid>;
using AutoValueVector = AutoVector<Value>;
using AutoObjectVector = AutoVector<JSObject*>;

class CallArgs;

class JS_FRIEND_API(CompileOptions);
class JS_FRIEND_API(ReadOnlyCompileOptions);
class JS_FRIEND_API(OwningCompileOptions);
class JS_FRIEND_API(TransitiveCompileOptions);
class JS_PUBLIC_API(CompartmentOptions);

} // namespace JS

/* Result of typeof operator enumeration. */
enum JSType {
    JSTYPE_UNDEFINED,           /* undefined */
    JSTYPE_OBJECT,              /* object */
    JSTYPE_FUNCTION,            /* function */
    JSTYPE_STRING,              /* string */
    JSTYPE_NUMBER,              /* number */
    JSTYPE_BOOLEAN,             /* boolean */
    JSTYPE_NULL,                /* null */
    JSTYPE_SYMBOL,              /* symbol */
    JSTYPE_LIMIT
};

/* Dense index into cached prototypes and class atoms for standard objects. */
enum JSProtoKey {
#define PROTOKEY_AND_INITIALIZER(name,init,clasp) JSProto_##name,
    JS_FOR_EACH_PROTOTYPE(PROTOKEY_AND_INITIALIZER)
#undef PROTOKEY_AND_INITIALIZER
    JSProto_LIMIT
};

/* Struct forward declarations. */
struct JSClass;
class JSErrorReport;
struct JSExceptionState;
struct JSFunctionSpec;
struct JSLocaleCallbacks;
struct JSPrincipals;
struct JSPropertySpec;
struct JSSecurityCallbacks;
struct JSStructuredCloneCallbacks;
struct JSStructuredCloneReader;
struct JSStructuredCloneWriter;
class JS_PUBLIC_API(JSTracer);

class JSFlatString;

typedef bool                    (*JSInitCallback)(void);

template<typename T> struct JSConstScalarSpec;
typedef JSConstScalarSpec<double> JSConstDoubleSpec;
typedef JSConstScalarSpec<int32_t> JSConstIntegerSpec;

namespace js {
namespace gc {
class AutoTraceSession;
class StoreBuffer;
} // namespace gc

class CooperatingContext;

inline JSCompartment* GetContextCompartment(const JSContext* cx);
inline JS::Zone* GetContextZone(const JSContext* cx);

// Whether the current thread is permitted access to any part of the specified
// runtime or zone.
JS_FRIEND_API(bool)
CurrentThreadCanAccessRuntime(const JSRuntime* rt);

#ifdef DEBUG
JS_FRIEND_API(bool)
CurrentThreadIsPerformingGC();
#endif

} // namespace js

namespace JS {

class JS_PUBLIC_API(AutoEnterCycleCollection);
class JS_PUBLIC_API(AutoAssertOnBarrier);
struct JS_PUBLIC_API(PropertyDescriptor);

typedef void (*OffThreadCompileCallback)(void* token, void* callbackData);

enum class HeapState {
    Idle,             // doing nothing with the GC heap
    Tracing,          // tracing the GC heap without collecting, e.g. IterateCompartments()
    MajorCollecting,  // doing a GC of the major heap
    MinorCollecting,  // doing a GC of the minor heap (nursery)
    CycleCollecting   // in the "Unlink" phase of cycle collection
};

JS_PUBLIC_API(HeapState)
CurrentThreadHeapState();

static inline bool
CurrentThreadIsHeapBusy()
{
    return CurrentThreadHeapState() != HeapState::Idle;
}

static inline bool
CurrentThreadIsHeapTracing()
{
    return CurrentThreadHeapState() == HeapState::Tracing;
}

static inline bool
CurrentThreadIsHeapMajorCollecting()
{
    return CurrentThreadHeapState() == HeapState::MajorCollecting;
}

static inline bool
CurrentThreadIsHeapMinorCollecting()
{
    return CurrentThreadHeapState() == HeapState::MinorCollecting;
}

static inline bool
CurrentThreadIsHeapCollecting()
{
    HeapState state = CurrentThreadHeapState();
    return state == HeapState::MajorCollecting || state == HeapState::MinorCollecting;
}

static inline bool
CurrentThreadIsHeapCycleCollecting()
{
    return CurrentThreadHeapState() == HeapState::CycleCollecting;
}

// Decorates the Unlinking phase of CycleCollection so that accidental use
// of barriered accessors results in assertions instead of leaks.
class MOZ_STACK_CLASS JS_PUBLIC_API(AutoEnterCycleCollection)
{
#ifdef DEBUG
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

MOZ_BEGIN_EXTERN_C

// Defined in NSPR prio.h.
typedef struct PRFileDesc PRFileDesc;

MOZ_END_EXTERN_C

#endif /* jspubtd_h */

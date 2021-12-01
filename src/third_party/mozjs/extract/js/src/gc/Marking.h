/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Marking and sweeping APIs for use by implementations of different GC cell
 * kinds.
 */

#ifndef gc_Marking_h
#define gc_Marking_h

#include "js/TypeDecls.h"
#include "vm/TaggedProto.h"

class JSLinearString;
class JSRope;
class JSTracer;

namespace js {
class BaseShape;
class GCMarker;
class LazyScript;
class NativeObject;
class ObjectGroup;
class Shape;
class WeakMapBase;

namespace jit {
class JitCode;
} // namespace jit

#ifdef DEBUG
// Return true if this trace is happening on behalf of gray buffering during
// the marking phase of incremental GC.
bool
IsBufferGrayRootsTracer(JSTracer* trc);

bool
IsUnmarkGrayTracer(JSTracer* trc);
#endif

namespace gc {

class Arena;
struct Cell;
class TenuredCell;

/*** Special Cases ***/

void
PushArena(GCMarker* gcmarker, Arena* arena);

/*** Liveness ***/

// Report whether a thing has been marked.  Things which are in zones that are
// not currently being collected or are owned by another runtime are always
// reported as being marked.
template <typename T>
bool
IsMarkedUnbarriered(JSRuntime* rt, T* thingp);

// Report whether a thing has been marked.  Things which are in zones that are
// not currently being collected or are owned by another runtime are always
// reported as being marked.
template <typename T>
bool
IsMarked(JSRuntime* rt, WriteBarrieredBase<T>* thingp);

template <typename T>
bool
IsAboutToBeFinalizedUnbarriered(T* thingp);

template <typename T>
bool
IsAboutToBeFinalized(WriteBarrieredBase<T>* thingp);

template <typename T>
bool
IsAboutToBeFinalized(ReadBarrieredBase<T>* thingp);

bool
IsAboutToBeFinalizedDuringSweep(TenuredCell& tenured);

inline Cell*
ToMarkable(const Value& v)
{
    if (v.isGCThing())
        return (Cell*)v.toGCThing();
    return nullptr;
}

inline Cell*
ToMarkable(Cell* cell)
{
    return cell;
}

// Wrap a GC thing pointer into a new Value or jsid. The type system enforces
// that the thing pointer is a wrappable type.
template <typename S, typename T>
struct RewrapTaggedPointer{};
#define DECLARE_REWRAP(S, T, method, prefix) \
    template <> struct RewrapTaggedPointer<S, T> { \
        static S wrap(T* thing) { return method ( prefix thing ); } \
    }
DECLARE_REWRAP(JS::Value, JSObject, JS::ObjectOrNullValue, );
DECLARE_REWRAP(JS::Value, JSString, JS::StringValue, );
DECLARE_REWRAP(JS::Value, JS::Symbol, JS::SymbolValue, );
DECLARE_REWRAP(jsid, JSString, NON_INTEGER_ATOM_TO_JSID, (JSAtom*));
DECLARE_REWRAP(jsid, JS::Symbol, SYMBOL_TO_JSID, );
DECLARE_REWRAP(js::TaggedProto, JSObject, js::TaggedProto, );
#undef DECLARE_REWRAP

template <typename T>
struct IsPrivateGCThingInValue
  : public mozilla::EnableIf<mozilla::IsBaseOf<Cell, T>::value &&
                             !mozilla::IsBaseOf<JSObject, T>::value &&
                             !mozilla::IsBaseOf<JSString, T>::value &&
                             !mozilla::IsBaseOf<JS::Symbol, T>::value, T>
{
    static_assert(!mozilla::IsSame<Cell, T>::value && !mozilla::IsSame<TenuredCell, T>::value,
                  "T must not be Cell or TenuredCell");
};

template <typename T>
struct RewrapTaggedPointer<Value, T>
{
    static Value wrap(typename IsPrivateGCThingInValue<T>::Type* thing) {
        return JS::PrivateGCThingValue(thing);
    }
};

} /* namespace gc */

// The return value indicates if anything was unmarked.
bool
UnmarkGrayShapeRecursively(Shape* shape);

template<typename T>
void
CheckTracedThing(JSTracer* trc, T* thing);

template<typename T>
void
CheckTracedThing(JSTracer* trc, T thing);

namespace gc {

// Functions for checking and updating GC thing pointers that might have been
// moved by compacting GC. Overloads are also provided that work with Values.
//
// IsForwarded    - check whether a pointer refers to an GC thing that has been
//                  moved.
//
// Forwarded      - return a pointer to the new location of a GC thing given a
//                  pointer to old location.
//
// MaybeForwarded - used before dereferencing a pointer that may refer to a
//                  moved GC thing without updating it. For JSObjects this will
//                  also update the object's shape pointer if it has been moved
//                  to allow slots to be accessed.

template <typename T>
inline bool IsForwarded(const T* t);
inline bool IsForwarded(const JS::Value& value);

template <typename T>
inline T* Forwarded(const T* t);

inline Value Forwarded(const JS::Value& value);

template <typename T>
inline T MaybeForwarded(T t);

inline void
MakeAccessibleAfterMovingGC(void* anyp) {}

inline void
MakeAccessibleAfterMovingGC(JSObject* obj); // Defined in jsobjinlines.h.

#ifdef JSGC_HASH_TABLE_CHECKS

template <typename T>
inline bool IsGCThingValidAfterMovingGC(T* t);

template <typename T>
inline void CheckGCThingAfterMovingGC(T* t);

template <typename T>
inline void CheckGCThingAfterMovingGC(const ReadBarriered<T*>& t);

inline void CheckValueAfterMovingGC(const JS::Value& value);

#endif // JSGC_HASH_TABLE_CHECKS

} /* namespace gc */
} /* namespace js */

#endif /* gc_Marking_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definition of GC cell kinds.
 */

#ifndef gc_AllocKind_h
#define gc_AllocKind_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/EnumeratedRange.h"

#include <stdint.h>

#include "js/TraceKind.h"
#include "js/Utility.h"

namespace js {
namespace gc {

// The GC allocation kinds.
//
// These are defined by macros which enumerate the different allocation kinds
// and supply the following information:
//
//  - the corresponding AllocKind
//  - their JS::TraceKind
//  - their C++ base type
//  - a C++ type of the correct size
//  - whether they can be finalized on the background thread
//  - whether they can be allocated in the nursery

#define FOR_EACH_OBJECT_ALLOCKIND(D) \
 /* AllocKind              TraceKind     TypeName           SizedType          BGFinal Nursery */ \
    D(FUNCTION,            Object,       JSObject,          JSFunction,        true,   true)  \
    D(FUNCTION_EXTENDED,   Object,       JSObject,          FunctionExtended,  true,   true)  \
    D(OBJECT0,             Object,       JSObject,          JSObject_Slots0,   false,  false) \
    D(OBJECT0_BACKGROUND,  Object,       JSObject,          JSObject_Slots0,   true,   true)  \
    D(OBJECT2,             Object,       JSObject,          JSObject_Slots2,   false,  false) \
    D(OBJECT2_BACKGROUND,  Object,       JSObject,          JSObject_Slots2,   true,   true)  \
    D(OBJECT4,             Object,       JSObject,          JSObject_Slots4,   false,  false) \
    D(OBJECT4_BACKGROUND,  Object,       JSObject,          JSObject_Slots4,   true,   true)  \
    D(OBJECT8,             Object,       JSObject,          JSObject_Slots8,   false,  false) \
    D(OBJECT8_BACKGROUND,  Object,       JSObject,          JSObject_Slots8,   true,   true)  \
    D(OBJECT12,            Object,       JSObject,          JSObject_Slots12,  false,  false) \
    D(OBJECT12_BACKGROUND, Object,       JSObject,          JSObject_Slots12,  true,   true)  \
    D(OBJECT16,            Object,       JSObject,          JSObject_Slots16,  false,  false) \
    D(OBJECT16_BACKGROUND, Object,       JSObject,          JSObject_Slots16,  true,   true)

#define FOR_EACH_NONOBJECT_NONNURSERY_ALLOCKIND(D) \
 /* AllocKind              TraceKind     TypeName           SizedType          BGFinal Nursery */ \
    D(SCRIPT,              Script,       JSScript,          JSScript,          false,  false) \
    D(LAZY_SCRIPT,         LazyScript,   js::LazyScript,    js::LazyScript,    true,   false) \
    D(SHAPE,               Shape,        js::Shape,         js::Shape,         true,   false) \
    D(ACCESSOR_SHAPE,      Shape,        js::AccessorShape, js::AccessorShape, true,   false) \
    D(BASE_SHAPE,          BaseShape,    js::BaseShape,     js::BaseShape,     true,   false) \
    D(OBJECT_GROUP,        ObjectGroup,  js::ObjectGroup,   js::ObjectGroup,   true,   false) \
    D(EXTERNAL_STRING,     String,       JSExternalString,  JSExternalString,  true,   false) \
    D(FAT_INLINE_ATOM,     String,       js::FatInlineAtom, js::FatInlineAtom, true,   false) \
    D(ATOM,                String,       js::NormalAtom,    js::NormalAtom,    true,   false) \
    D(SYMBOL,              Symbol,       JS::Symbol,        JS::Symbol,        true,   false) \
    D(JITCODE,             JitCode,      js::jit::JitCode,  js::jit::JitCode,  false,  false) \
    D(SCOPE,               Scope,        js::Scope,         js::Scope,         true,   false) \
    D(REGEXP_SHARED,       RegExpShared, js::RegExpShared,  js::RegExpShared,  true,   false)

#define FOR_EACH_NURSERY_STRING_ALLOCKIND(D) \
    D(FAT_INLINE_STRING,   String,        JSFatInlineString, JSFatInlineString, true,   true) \
    D(STRING,              String,        JSString,          JSString,          true,   true)

#define FOR_EACH_NONOBJECT_ALLOCKIND(D) \
    FOR_EACH_NONOBJECT_NONNURSERY_ALLOCKIND(D) \
    FOR_EACH_NURSERY_STRING_ALLOCKIND(D)

#define FOR_EACH_ALLOCKIND(D)    \
    FOR_EACH_OBJECT_ALLOCKIND(D) \
    FOR_EACH_NONOBJECT_ALLOCKIND(D)

enum class AllocKind : uint8_t {
#define DEFINE_ALLOC_KIND(allocKind, _1, _2, _3, _4, _5) allocKind,

    FOR_EACH_OBJECT_ALLOCKIND(DEFINE_ALLOC_KIND)

    OBJECT_LIMIT,
    OBJECT_LAST = OBJECT_LIMIT - 1,

    FOR_EACH_NONOBJECT_ALLOCKIND(DEFINE_ALLOC_KIND)

    LIMIT,
    LAST = LIMIT - 1,

    FIRST = 0,
    OBJECT_FIRST = FUNCTION // Hardcoded to first object kind.

#undef DEFINE_ALLOC_KIND
};

static_assert(int(AllocKind::FIRST) == 0,
              "Various places depend on AllocKind starting at 0");
static_assert(int(AllocKind::OBJECT_FIRST) == 0,
              "OBJECT_FIRST must be defined as the first object kind");

inline bool
IsAllocKind(AllocKind kind)
{
    return kind >= AllocKind::FIRST && kind <= AllocKind::LIMIT;
}

inline bool
IsValidAllocKind(AllocKind kind)
{
    return kind >= AllocKind::FIRST && kind <= AllocKind::LAST;
}

inline bool
IsObjectAllocKind(AllocKind kind)
{
    return kind >= AllocKind::OBJECT_FIRST && kind <= AllocKind::OBJECT_LAST;
}

inline bool
IsShapeAllocKind(AllocKind kind)
{
    return kind == AllocKind::SHAPE || kind == AllocKind::ACCESSOR_SHAPE;
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all alloc kinds.
inline decltype(mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT))
AllAllocKinds()
{
    return mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all object alloc kinds.
inline decltype(mozilla::MakeEnumeratedRange(AllocKind::OBJECT_FIRST, AllocKind::OBJECT_LIMIT))
ObjectAllocKinds()
{
    return mozilla::MakeEnumeratedRange(AllocKind::OBJECT_FIRST, AllocKind::OBJECT_LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over alloc kinds from |first| to |limit|, exclusive.
inline decltype(mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT))
SomeAllocKinds(AllocKind first = AllocKind::FIRST, AllocKind limit = AllocKind::LIMIT)
{
    MOZ_ASSERT(IsAllocKind(first), "|first| is not a valid AllocKind!");
    MOZ_ASSERT(IsAllocKind(limit), "|limit| is not a valid AllocKind!");
    return mozilla::MakeEnumeratedRange(first, limit);
}

// AllAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular alloc kind.
template<typename ValueType> using AllAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::LIMIT, ValueType>;

// ObjectAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular object alloc kind.
template<typename ValueType> using ObjectAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::OBJECT_LIMIT, ValueType>;

static inline JS::TraceKind
MapAllocToTraceKind(AllocKind kind)
{
    static const JS::TraceKind map[] = {
#define EXPAND_ELEMENT(allocKind, traceKind, type, sizedType, bgFinal, nursery) \
        JS::TraceKind::traceKind,
FOR_EACH_ALLOCKIND(EXPAND_ELEMENT)
#undef EXPAND_ELEMENT
    };

    static_assert(mozilla::ArrayLength(map) == size_t(AllocKind::LIMIT),
                  "AllocKind-to-TraceKind mapping must be in sync");
    return map[size_t(kind)];
}

/*
 * This must be an upper bound, but we do not need the least upper bound, so
 * we just exclude non-background objects.
 */
static const size_t MAX_BACKGROUND_FINALIZE_KINDS =
    size_t(AllocKind::LIMIT) - size_t(AllocKind::OBJECT_LIMIT) / 2;

static inline bool
IsNurseryAllocable(AllocKind kind)
{
    MOZ_ASSERT(IsValidAllocKind(kind));

    static const bool map[] = {
#define DEFINE_NURSERY_ALLOCABLE(_1, _2, _3, _4, _5, nursery) nursery,
        FOR_EACH_ALLOCKIND(DEFINE_NURSERY_ALLOCABLE)
#undef DEFINE_NURSERY_ALLOCABLE
    };

    static_assert(mozilla::ArrayLength(map) == size_t(AllocKind::LIMIT),
                  "IsNurseryAllocable sanity check");
    return map[size_t(kind)];
}

static inline bool
IsBackgroundFinalized(AllocKind kind)
{
    MOZ_ASSERT(IsValidAllocKind(kind));

    static const bool map[] = {
#define DEFINE_BACKGROUND_FINALIZED(_1, _2, _3, _4, bgFinal, _5) bgFinal,
        FOR_EACH_ALLOCKIND(DEFINE_BACKGROUND_FINALIZED)
#undef DEFINE_BG_FINALIZE
    };

    static_assert(mozilla::ArrayLength(map) == size_t(AllocKind::LIMIT),
                  "IsBackgroundFinalized sanity check");
    return map[size_t(kind)];
}

} /* namespace gc */
} /* namespace js */

#endif /* gc_AllocKind_h */

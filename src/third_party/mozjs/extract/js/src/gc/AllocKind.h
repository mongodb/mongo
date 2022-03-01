/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definition of GC cell kinds.
 */

#ifndef gc_AllocKind_h
#define gc_AllocKind_h

#include "mozilla/EnumeratedArray.h"
#include "mozilla/EnumeratedRange.h"

#include <iterator>
#include <stdint.h>

#include "js/TraceKind.h"
#include "js/Utility.h"

namespace js {

class CompactPropMap;
class NormalPropMap;
class DictionaryPropMap;

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
//  - whether they can be compacted

// clang-format off
#define FOR_EACH_OBJECT_ALLOCKIND(D) \
 /* AllocKind              TraceKind     TypeName           SizedType          BGFinal Nursery Compact */ \
    D(FUNCTION,            Object,       JSObject,          JSFunction,        true,   true,   true) \
    D(FUNCTION_EXTENDED,   Object,       JSObject,          FunctionExtended,  true,   true,   true) \
    D(OBJECT0,             Object,       JSObject,          JSObject_Slots0,   false,  false,  true) \
    D(OBJECT0_BACKGROUND,  Object,       JSObject,          JSObject_Slots0,   true,   true,   true) \
    D(OBJECT2,             Object,       JSObject,          JSObject_Slots2,   false,  false,  true) \
    D(OBJECT2_BACKGROUND,  Object,       JSObject,          JSObject_Slots2,   true,   true,   true) \
    D(ARRAYBUFFER4,        Object,       JSObject,          JSObject_Slots4,   true,   true,   true) \
    D(OBJECT4,             Object,       JSObject,          JSObject_Slots4,   false,  false,  true) \
    D(OBJECT4_BACKGROUND,  Object,       JSObject,          JSObject_Slots4,   true,   true,   true) \
    D(ARRAYBUFFER8,        Object,       JSObject,          JSObject_Slots8,   true,   true,   true) \
    D(OBJECT8,             Object,       JSObject,          JSObject_Slots8,   false,  false,  true) \
    D(OBJECT8_BACKGROUND,  Object,       JSObject,          JSObject_Slots8,   true,   true,   true) \
    D(ARRAYBUFFER12,       Object,       JSObject,          JSObject_Slots12,  true,   true,   true) \
    D(OBJECT12,            Object,       JSObject,          JSObject_Slots12,  false,  false,  true) \
    D(OBJECT12_BACKGROUND, Object,       JSObject,          JSObject_Slots12,  true,   true,   true) \
    D(ARRAYBUFFER16,       Object,       JSObject,          JSObject_Slots16,  true,   true,   true) \
    D(OBJECT16,            Object,       JSObject,          JSObject_Slots16,  false,  false,  true) \
    D(OBJECT16_BACKGROUND, Object,       JSObject,          JSObject_Slots16,  true,   true,   true)

#define FOR_EACH_NONOBJECT_NONNURSERY_ALLOCKIND(D) \
 /* AllocKind              TraceKind     TypeName               SizedType              BGFinal Nursery Compact */ \
    D(SCRIPT,              Script,       js::BaseScript,        js::BaseScript,        false,  false,  true) \
    D(SHAPE,               Shape,        js::Shape,             js::Shape,             true,   false,  true) \
    D(BASE_SHAPE,          BaseShape,    js::BaseShape,         js::BaseShape,         true,   false,  true) \
    D(GETTER_SETTER,       GetterSetter, js::GetterSetter,      js::GetterSetter,      true,   false,  true) \
    D(COMPACT_PROP_MAP,    PropMap,      js::CompactPropMap,    js::CompactPropMap,    true,   false,  true) \
    D(NORMAL_PROP_MAP,     PropMap,      js::NormalPropMap,     js::NormalPropMap,     true,   false,  true) \
    D(DICT_PROP_MAP,       PropMap,      js::DictionaryPropMap, js::DictionaryPropMap, true,   false,  true) \
    D(EXTERNAL_STRING,     String,       JSExternalString,      JSExternalString,      true,   false,  true) \
    D(FAT_INLINE_ATOM,     String,       js::FatInlineAtom,     js::FatInlineAtom,     true,   false,  false) \
    D(ATOM,                String,       js::NormalAtom,        js::NormalAtom,        true,   false,  false) \
    D(SYMBOL,              Symbol,       JS::Symbol,            JS::Symbol,            true,   false,  false) \
    D(JITCODE,             JitCode,      js::jit::JitCode,      js::jit::JitCode,      false,  false,  false) \
    D(SCOPE,               Scope,        js::Scope,             js::Scope,             true,   false,  true) \
    D(REGEXP_SHARED,       RegExpShared, js::RegExpShared,      js::RegExpShared,      true,   false,  true)

#define FOR_EACH_NONOBJECT_NURSERY_ALLOCKIND(D) \
 /* AllocKind              TraceKind     TypeName           SizedType          BGFinal Nursery Compact */ \
    D(BIGINT,              BigInt,       JS::BigInt,        JS::BigInt,        true,   true,  true)

#define FOR_EACH_NURSERY_STRING_ALLOCKIND(D) \
    D(FAT_INLINE_STRING,   String,        JSFatInlineString, JSFatInlineString, true,   true,  true) \
    D(STRING,              String,        JSString,          JSString,          true,   true,  true)
// clang-format on

#define FOR_EACH_NONOBJECT_ALLOCKIND(D)      \
  FOR_EACH_NONOBJECT_NONNURSERY_ALLOCKIND(D) \
  FOR_EACH_NONOBJECT_NURSERY_ALLOCKIND(D)    \
  FOR_EACH_NURSERY_STRING_ALLOCKIND(D)

#define FOR_EACH_ALLOCKIND(D)  \
  FOR_EACH_OBJECT_ALLOCKIND(D) \
  FOR_EACH_NONOBJECT_ALLOCKIND(D)

#define DEFINE_ALLOC_KIND(allocKind, _1, _2, _3, _4, _5, _6) allocKind,
enum class AllocKind : uint8_t {
  // clang-format off
    FOR_EACH_OBJECT_ALLOCKIND(DEFINE_ALLOC_KIND)

    OBJECT_LIMIT,
    OBJECT_LAST = OBJECT_LIMIT - 1,

    FOR_EACH_NONOBJECT_ALLOCKIND(DEFINE_ALLOC_KIND)

    LIMIT,
    LAST = LIMIT - 1,

    FIRST = 0,
    OBJECT_FIRST = FUNCTION // Hardcoded to first object kind.
  // clang-format on
};
#undef DEFINE_ALLOC_KIND

static_assert(int(AllocKind::FIRST) == 0,
              "Various places depend on AllocKind starting at 0");
static_assert(int(AllocKind::OBJECT_FIRST) == 0,
              "OBJECT_FIRST must be defined as the first object kind");

constexpr size_t AllocKindCount = size_t(AllocKind::LIMIT);

/*
 * This flag allows an allocation site to request a specific heap based upon the
 * estimated lifetime or lifetime requirements of objects allocated from that
 * site.
 */
enum InitialHeap : uint8_t { DefaultHeap, TenuredHeap };

inline bool IsAllocKind(AllocKind kind) {
  return kind >= AllocKind::FIRST && kind <= AllocKind::LIMIT;
}

inline bool IsValidAllocKind(AllocKind kind) {
  return kind >= AllocKind::FIRST && kind <= AllocKind::LAST;
}

const char* AllocKindName(AllocKind kind);

inline bool IsObjectAllocKind(AllocKind kind) {
  return kind >= AllocKind::OBJECT_FIRST && kind <= AllocKind::OBJECT_LAST;
}

inline bool IsShapeAllocKind(AllocKind kind) {
  return kind == AllocKind::SHAPE;
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all alloc kinds.
inline auto AllAllocKinds() {
  return mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all object alloc kinds.
inline auto ObjectAllocKinds() {
  return mozilla::MakeEnumeratedRange(AllocKind::OBJECT_FIRST,
                                      AllocKind::OBJECT_LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over alloc kinds from |first| to |limit|, exclusive.
inline auto SomeAllocKinds(AllocKind first = AllocKind::FIRST,
                           AllocKind limit = AllocKind::LIMIT) {
  MOZ_ASSERT(IsAllocKind(first), "|first| is not a valid AllocKind!");
  MOZ_ASSERT(IsAllocKind(limit), "|limit| is not a valid AllocKind!");
  return mozilla::MakeEnumeratedRange(first, limit);
}

// AllAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular alloc kind.
template <typename ValueType>
using AllAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::LIMIT, ValueType>;

// ObjectAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular object alloc kind.
template <typename ValueType>
using ObjectAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, AllocKind::OBJECT_LIMIT, ValueType>;

static inline JS::TraceKind MapAllocToTraceKind(AllocKind kind) {
  static const JS::TraceKind map[] = {
#define EXPAND_ELEMENT(allocKind, traceKind, type, sizedType, bgFinal, \
                       nursery, compact)                               \
  JS::TraceKind::traceKind,
      FOR_EACH_ALLOCKIND(EXPAND_ELEMENT)
#undef EXPAND_ELEMENT
  };

  static_assert(std::size(map) == AllocKindCount,
                "AllocKind-to-TraceKind mapping must be in sync");
  return map[size_t(kind)];
}

static inline bool IsNurseryAllocable(AllocKind kind) {
  MOZ_ASSERT(IsValidAllocKind(kind));

  static const bool map[] = {
#define DEFINE_NURSERY_ALLOCABLE(_1, _2, _3, _4, _5, nursery, _6) nursery,
      FOR_EACH_ALLOCKIND(DEFINE_NURSERY_ALLOCABLE)
#undef DEFINE_NURSERY_ALLOCABLE
  };

  static_assert(std::size(map) == AllocKindCount,
                "IsNurseryAllocable sanity check");
  return map[size_t(kind)];
}

static inline bool IsBackgroundFinalized(AllocKind kind) {
  MOZ_ASSERT(IsValidAllocKind(kind));

  static const bool map[] = {
#define DEFINE_BACKGROUND_FINALIZED(_1, _2, _3, _4, bgFinal, _5, _6) bgFinal,
      FOR_EACH_ALLOCKIND(DEFINE_BACKGROUND_FINALIZED)
#undef DEFINE_BACKGROUND_FINALIZED
  };

  static_assert(std::size(map) == AllocKindCount,
                "IsBackgroundFinalized sanity check");
  return map[size_t(kind)];
}

static inline bool IsForegroundFinalized(AllocKind kind) {
  return !IsBackgroundFinalized(kind);
}

static inline bool IsCompactingKind(AllocKind kind) {
  MOZ_ASSERT(IsValidAllocKind(kind));

  static const bool map[] = {
#define DEFINE_COMPACTING_KIND(_1, _2, _3, _4, _5, _6, compact) compact,
      FOR_EACH_ALLOCKIND(DEFINE_COMPACTING_KIND)
#undef DEFINE_COMPACTING_KIND
  };

  static_assert(std::size(map) == AllocKindCount,
                "IsCompactingKind sanity check");
  return map[size_t(kind)];
}

static inline bool IsMovableKind(AllocKind kind) {
  return IsNurseryAllocable(kind) || IsCompactingKind(kind);
}

} /* namespace gc */
} /* namespace js */

#endif /* gc_AllocKind_h */

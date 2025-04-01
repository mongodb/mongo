
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

class JSDependentString;
class JSExternalString;
class JSFatInlineString;
class JSLinearString;
class JSRope;
class JSThinInlineString;

namespace js {

class CompactPropMap;
class FatInlineAtom;
class ThinInlineAtom;
class NormalAtom;
class NormalPropMap;
class DictionaryPropMap;
class DictionaryShape;
class SharedShape;
class ProxyShape;
class WasmGCShape;

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
//  - whether they can be allocated in the nursery (this is true for foreground
//    finalized objects but these will can only actually be allocated in the
//    nursery if JSCLASS_SKIP_NURSERY_FINALIZE is set)
//  - whether they can be compacted

// clang-format off
#define FOR_EACH_OBJECT_ALLOCKIND(D) \
 /* AllocKind              TraceKind     TypeName           SizedType          BGFinal Nursery Compact */ \
    D(FUNCTION,            Object,       JSObject,          JSObject_Slots4,   true,   true,   true) \
    D(FUNCTION_EXTENDED,   Object,       JSObject,          JSObject_Slots7,   true,   true,   true) \
    D(OBJECT0,             Object,       JSObject,          JSObject_Slots0,   false,  true,   true) \
    D(OBJECT0_BACKGROUND,  Object,       JSObject,          JSObject_Slots0,   true,   true,   true) \
    D(OBJECT2,             Object,       JSObject,          JSObject_Slots2,   false,  true,   true) \
    D(OBJECT2_BACKGROUND,  Object,       JSObject,          JSObject_Slots2,   true,   true,   true) \
    D(ARRAYBUFFER4,        Object,       JSObject,          JSObject_Slots4,   true,   true,   true) \
    D(OBJECT4,             Object,       JSObject,          JSObject_Slots4,   false,  true,   true) \
    D(OBJECT4_BACKGROUND,  Object,       JSObject,          JSObject_Slots4,   true,   true,   true) \
    D(ARRAYBUFFER8,        Object,       JSObject,          JSObject_Slots8,   true,   true,   true) \
    D(OBJECT8,             Object,       JSObject,          JSObject_Slots8,   false,  true,   true) \
    D(OBJECT8_BACKGROUND,  Object,       JSObject,          JSObject_Slots8,   true,   true,   true) \
    D(ARRAYBUFFER12,       Object,       JSObject,          JSObject_Slots12,  true,   true,   true) \
    D(OBJECT12,            Object,       JSObject,          JSObject_Slots12,  false,  true,   true) \
    D(OBJECT12_BACKGROUND, Object,       JSObject,          JSObject_Slots12,  true,   true,   true) \
    D(ARRAYBUFFER16,       Object,       JSObject,          JSObject_Slots16,  true,   true,   true) \
    D(OBJECT16,            Object,       JSObject,          JSObject_Slots16,  false,  true,   true) \
    D(OBJECT16_BACKGROUND, Object,       JSObject,          JSObject_Slots16,  true,   true,   true)

#define FOR_EACH_NONOBJECT_NONNURSERY_ALLOCKIND(D) \
 /* AllocKind              TraceKind     TypeName               SizedType              BGFinal Nursery Compact */ \
    D(SCRIPT,              Script,       js::BaseScript,        js::BaseScript,        false,  false,  true) \
    D(SHAPE,               Shape,        js::Shape,             js::SizedShape,        true,   false,  true) \
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

    INVALID = LIMIT,

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
 * A flag specifying either the tenured heap or a default heap (which may be
 * either the nursery or the tenured heap).
 *
 * This allows an allocation site to request a heap based upon the estimated
 * lifetime or lifetime requirements of objects allocated from that site.
 *
 * Order is important as these are numerically compared.
 */
enum class Heap : uint8_t { Default = 0, Tenured = 1 };

constexpr bool IsAllocKind(AllocKind kind) {
  return kind >= AllocKind::FIRST && kind <= AllocKind::LIMIT;
}

constexpr bool IsValidAllocKind(AllocKind kind) {
  return kind >= AllocKind::FIRST && kind <= AllocKind::LAST;
}

const char* AllocKindName(AllocKind kind);

constexpr bool IsObjectAllocKind(AllocKind kind) {
  return kind >= AllocKind::OBJECT_FIRST && kind <= AllocKind::OBJECT_LAST;
}

constexpr bool IsShapeAllocKind(AllocKind kind) {
  return kind == AllocKind::SHAPE;
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all alloc kinds.
constexpr auto AllAllocKinds() {
  return mozilla::MakeEnumeratedRange(AllocKind::FIRST, AllocKind::LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over all object alloc kinds.
constexpr auto ObjectAllocKinds() {
  return mozilla::MakeEnumeratedRange(AllocKind::OBJECT_FIRST,
                                      AllocKind::OBJECT_LIMIT);
}

// Returns a sequence for use in a range-based for loop,
// to iterate over alloc kinds from |first| to |limit|, exclusive.
constexpr auto SomeAllocKinds(AllocKind first = AllocKind::FIRST,
                              AllocKind limit = AllocKind::LIMIT) {
  MOZ_ASSERT(IsAllocKind(first), "|first| is not a valid AllocKind!");
  MOZ_ASSERT(IsAllocKind(limit), "|limit| is not a valid AllocKind!");
  return mozilla::MakeEnumeratedRange(first, limit);
}

// AllAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular alloc kind.
template <typename ValueType>
using AllAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, ValueType, size_t(AllocKind::LIMIT)>;

// ObjectAllocKindArray<ValueType> gives an enumerated array of ValueTypes,
// with each index corresponding to a particular object alloc kind.
template <typename ValueType>
using ObjectAllocKindArray =
    mozilla::EnumeratedArray<AllocKind, ValueType,
                             size_t(AllocKind::OBJECT_LIMIT)>;

/*
 * Map from C++ type to alloc kind for non-object types. JSObject does not have
 * a 1:1 mapping, so must use Arena::thingSize.
 *
 * The AllocKind is available as MapTypeToAllocKind<SomeType>::kind.
 *
 * There are specializations for strings and shapes since more than one derived
 * type shares the same alloc kind.
 */
template <typename T>
struct MapTypeToAllocKind {};
#define EXPAND_MAPTYPETOALLOCKIND(allocKind, traceKind, type, sizedType, \
                                  bgFinal, nursery, compact)             \
  template <>                                                            \
  struct MapTypeToAllocKind<type> {                                      \
    static const AllocKind kind = AllocKind::allocKind;                  \
  };
FOR_EACH_NONOBJECT_ALLOCKIND(EXPAND_MAPTYPETOALLOCKIND)
#undef EXPAND_MAPTYPETOALLOCKIND

template <>
struct MapTypeToAllocKind<JSDependentString> {
  static const AllocKind kind = AllocKind::STRING;
};
template <>
struct MapTypeToAllocKind<JSRope> {
  static const AllocKind kind = AllocKind::STRING;
};
template <>
struct MapTypeToAllocKind<JSLinearString> {
  static const AllocKind kind = AllocKind::STRING;
};
template <>
struct MapTypeToAllocKind<JSThinInlineString> {
  static const AllocKind kind = AllocKind::STRING;
};
template <>
struct MapTypeToAllocKind<js::ThinInlineAtom> {
  static const AllocKind kind = AllocKind::ATOM;
};

template <>
struct MapTypeToAllocKind<js::SharedShape> {
  static const AllocKind kind = AllocKind::SHAPE;
};
template <>
struct MapTypeToAllocKind<js::DictionaryShape> {
  static const AllocKind kind = AllocKind::SHAPE;
};
template <>
struct MapTypeToAllocKind<js::ProxyShape> {
  static const AllocKind kind = AllocKind::SHAPE;
};
template <>
struct MapTypeToAllocKind<js::WasmGCShape> {
  static const AllocKind kind = AllocKind::SHAPE;
};

constexpr JS::TraceKind MapAllocToTraceKind(AllocKind kind) {
  constexpr JS::TraceKind map[] = {
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

constexpr bool IsNurseryAllocable(AllocKind kind) {
  MOZ_ASSERT(IsValidAllocKind(kind));

  constexpr bool map[] = {
#define DEFINE_NURSERY_ALLOCABLE(_1, _2, _3, _4, _5, nursery, _6) nursery,
      FOR_EACH_ALLOCKIND(DEFINE_NURSERY_ALLOCABLE)
#undef DEFINE_NURSERY_ALLOCABLE
  };

  static_assert(std::size(map) == AllocKindCount,
                "IsNurseryAllocable sanity check");
  return map[size_t(kind)];
}

constexpr bool IsBackgroundFinalized(AllocKind kind) {
  MOZ_ASSERT(IsValidAllocKind(kind));

  constexpr bool map[] = {
#define DEFINE_BACKGROUND_FINALIZED(_1, _2, _3, _4, bgFinal, _5, _6) bgFinal,
      FOR_EACH_ALLOCKIND(DEFINE_BACKGROUND_FINALIZED)
#undef DEFINE_BACKGROUND_FINALIZED
  };

  static_assert(std::size(map) == AllocKindCount,
                "IsBackgroundFinalized sanity check");
  return map[size_t(kind)];
}

constexpr bool IsForegroundFinalized(AllocKind kind) {
  return !IsBackgroundFinalized(kind);
}

constexpr bool IsCompactingKind(AllocKind kind) {
  MOZ_ASSERT(IsValidAllocKind(kind));

  constexpr bool map[] = {
#define DEFINE_COMPACTING_KIND(_1, _2, _3, _4, _5, _6, compact) compact,
      FOR_EACH_ALLOCKIND(DEFINE_COMPACTING_KIND)
#undef DEFINE_COMPACTING_KIND
  };

  static_assert(std::size(map) == AllocKindCount,
                "IsCompactingKind sanity check");
  return map[size_t(kind)];
}

constexpr bool IsMovableKind(AllocKind kind) {
  return IsNurseryAllocable(kind) || IsCompactingKind(kind);
}

} /* namespace gc */
} /* namespace js */

#endif /* gc_AllocKind_h */

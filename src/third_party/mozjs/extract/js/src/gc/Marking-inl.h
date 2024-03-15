/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Marking_inl_h
#define gc_Marking_inl_h

#include "gc/Marking.h"

#include <type_traits>

#include "gc/RelocationOverlay.h"
#include "js/Id.h"
#include "js/Value.h"
#include "vm/StringType.h"
#include "vm/TaggedProto.h"

#include "gc/Nursery-inl.h"

namespace js {
namespace gc {

// An abstraction to re-wrap any kind of typed pointer back to the tagged
// pointer it came from with |TaggedPtr<TargetType>::wrap(sourcePtr)|.
template <typename T>
struct TaggedPtr {};

template <>
struct TaggedPtr<JS::Value> {
  static JS::Value wrap(JSObject* obj) {
    if (!obj) {
      return JS::NullValue();
    }
#ifdef ENABLE_RECORD_TUPLE
    if (MaybeForwardedIsExtendedPrimitive(*obj)) {
      return JS::ExtendedPrimitiveValue(*obj);
    }
#endif
    return JS::ObjectValue(*obj);
  }
  static JS::Value wrap(JSString* str) { return JS::StringValue(str); }
  static JS::Value wrap(JS::Symbol* sym) { return JS::SymbolValue(sym); }
  static JS::Value wrap(JS::BigInt* bi) { return JS::BigIntValue(bi); }
  template <typename T>
  static JS::Value wrap(T* priv) {
    static_assert(std::is_base_of_v<Cell, T>,
                  "Type must be a GC thing derived from js::gc::Cell");
    return JS::PrivateGCThingValue(priv);
  }
  static JS::Value empty() { return JS::UndefinedValue(); }
};

template <>
struct TaggedPtr<jsid> {
  static jsid wrap(JSString* str) { return JS::PropertyKey::NonIntAtom(str); }
  static jsid wrap(JS::Symbol* sym) { return PropertyKey::Symbol(sym); }
  static jsid empty() { return JS::PropertyKey::Void(); }
};

template <>
struct TaggedPtr<TaggedProto> {
  static TaggedProto wrap(JSObject* obj) { return TaggedProto(obj); }
  static TaggedProto empty() { return TaggedProto(); }
};

template <typename T>
struct MightBeForwarded {
  static_assert(std::is_base_of_v<Cell, T>);
  static_assert(!std::is_same_v<Cell, T> && !std::is_same_v<TenuredCell, T>);

#define CAN_FORWARD_KIND_OR(_1, _2, Type, _3, _4, _5, canCompact) \
  std::is_base_of_v<Type, T> ? canCompact:

  // FOR_EACH_ALLOCKIND doesn't cover every possible type: make sure
  // to default to `true` for unknown types.
  static constexpr bool value = FOR_EACH_ALLOCKIND(CAN_FORWARD_KIND_OR) true;
#undef CAN_FORWARD_KIND_OR
};

template <typename T>
inline bool IsForwarded(const T* t) {
  if (!MightBeForwarded<T>::value) {
    MOZ_ASSERT(!t->isForwarded());
    return false;
  }

  return t->isForwarded();
}

template <typename T>
inline T* Forwarded(const T* t) {
  const RelocationOverlay* overlay = RelocationOverlay::fromCell(t);
  MOZ_ASSERT(overlay->isForwarded());
  return reinterpret_cast<T*>(overlay->forwardingAddress());
}

template <typename T>
inline T MaybeForwarded(T t) {
  if (IsForwarded(t)) {
    t = Forwarded(t);
  }
  MOZ_ASSERT(!IsForwarded(t));
  return t;
}

inline const JSClass* MaybeForwardedObjectClass(const JSObject* obj) {
  Shape* shape = MaybeForwarded(obj->shapeMaybeForwarded());
  BaseShape* baseShape = MaybeForwarded(shape->base());
  return baseShape->clasp();
}

template <typename T>
inline bool MaybeForwardedObjectIs(const JSObject* obj) {
  MOZ_ASSERT(!obj->isForwarded());
  return MaybeForwardedObjectClass(obj) == &T::class_;
}

template <typename T>
inline T& MaybeForwardedObjectAs(JSObject* obj) {
  MOZ_ASSERT(MaybeForwardedObjectIs<T>(obj));
  return *static_cast<T*>(obj);
}

inline RelocationOverlay::RelocationOverlay(Cell* dst) {
  MOZ_ASSERT(dst->flags() == 0);
  uintptr_t ptr = uintptr_t(dst);
  header_.setForwardingAddress(ptr);
}

/* static */
inline RelocationOverlay* RelocationOverlay::forwardCell(Cell* src, Cell* dst) {
  MOZ_ASSERT(!src->isForwarded());
  MOZ_ASSERT(!dst->isForwarded());
  return new (src) RelocationOverlay(dst);
}

inline bool IsAboutToBeFinalizedDuringMinorSweep(Cell** cellp) {
  MOZ_ASSERT(JS::RuntimeHeapIsMinorCollecting());

  if ((*cellp)->isTenured()) {
    return false;
  }

  return !Nursery::getForwardedPointer(cellp);
}

// Special case pre-write barrier for strings used during rope flattening. This
// avoids eager marking of ropes which does not immediately mark the cells if we
// hit OOM. This does not traverse ropes and is instead called on every node in
// a rope during flattening.
inline void PreWriteBarrierDuringFlattening(JSString* str) {
  MOZ_ASSERT(str);
  MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());

  if (IsInsideNursery(str)) {
    return;
  }

  auto* cell = reinterpret_cast<TenuredCell*>(str);
  JS::shadow::Zone* zone = cell->shadowZoneFromAnyThread();
  if (!zone->needsIncrementalBarrier()) {
    return;
  }

  MOZ_ASSERT(!str->isPermanentAndMayBeShared());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(zone->runtimeFromAnyThread()));
  PerformIncrementalBarrierDuringFlattening(str);
}

#ifdef JSGC_HASH_TABLE_CHECKS

template <typename T>
inline bool IsGCThingValidAfterMovingGC(T* t) {
  return !IsInsideNursery(t) && !t->isForwarded();
}

template <typename T>
inline void CheckGCThingAfterMovingGC(T* t) {
  if (t) {
    MOZ_RELEASE_ASSERT(IsGCThingValidAfterMovingGC(t));
  }
}

template <typename T>
inline void CheckGCThingAfterMovingGC(const WeakHeapPtr<T*>& t) {
  CheckGCThingAfterMovingGC(t.unbarrieredGet());
}

#endif  // JSGC_HASH_TABLE_CHECKS

} /* namespace gc */
} /* namespace js */

#endif  // gc_Marking_inl_h

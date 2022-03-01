/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_NurseryAwareHashMap_h
#define gc_NurseryAwareHashMap_h

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "js/GCHashTable.h"
#include "js/GCPolicyAPI.h"
#include "js/HashTable.h"

namespace js {

namespace detail {
// This class only handles the incremental case and does not deal with nursery
// pointers. The only users should be for NurseryAwareHashMap; it is defined
// externally because we need a GCPolicy for its use in the contained map.
template <typename T>
class UnsafeBareWeakHeapPtr : public ReadBarriered<T> {
 public:
  UnsafeBareWeakHeapPtr() : ReadBarriered<T>(JS::SafelyInitialized<T>()) {}
  MOZ_IMPLICIT UnsafeBareWeakHeapPtr(const T& v) : ReadBarriered<T>(v) {}
  explicit UnsafeBareWeakHeapPtr(const UnsafeBareWeakHeapPtr& v)
      : ReadBarriered<T>(v) {}
  UnsafeBareWeakHeapPtr(UnsafeBareWeakHeapPtr&& v)
      : ReadBarriered<T>(std::move(v)) {}

  UnsafeBareWeakHeapPtr& operator=(const UnsafeBareWeakHeapPtr& v) {
    this->value = v.value;
    return *this;
  }

  UnsafeBareWeakHeapPtr& operator=(const T& v) {
    this->value = v;
    return *this;
  }

  const T get() const {
    if (!InternalBarrierMethods<T>::isMarkable(this->value)) {
      return JS::SafelyInitialized<T>();
    }
    this->read();
    return this->value;
  }

  explicit operator bool() const { return bool(this->value); }

  const T unbarrieredGet() const { return this->value; }
  T* unsafeGet() { return &this->value; }
  T const* unsafeGet() const { return &this->value; }
};
}  // namespace detail

enum : bool { DuplicatesNotPossible, DuplicatesPossible };

// The "nursery aware" hash map is a special case of GCHashMap that is able to
// treat nursery allocated members weakly during a minor GC: e.g. it allows for
// nursery allocated objects to be collected during nursery GC where a normal
// hash table treats such edges strongly.
//
// Doing this requires some strong constraints on what can be stored in this
// table and how it can be accessed. At the moment, this table assumes that
// all values contain a strong reference to the key. It also requires the
// policy to contain an |isTenured| and |needsSweep| members, which is fairly
// non-standard. This limits its usefulness to the CrossCompartmentMap at the
// moment, but might serve as a useful base for other tables in future.
template <typename Key, typename Value,
          typename HashPolicy = DefaultHasher<Key>,
          typename AllocPolicy = TempAllocPolicy,
          bool AllowDuplicates = DuplicatesNotPossible>
class NurseryAwareHashMap {
  using BarrieredValue = detail::UnsafeBareWeakHeapPtr<Value>;
  using MapType =
      GCRekeyableHashMap<Key, BarrieredValue, HashPolicy, AllocPolicy>;
  MapType map;

  // Keep a list of all keys for which JS::GCPolicy<Key>::isTenured is false.
  // This lets us avoid a full traveral of the map on each minor GC, keeping
  // the minor GC times proportional to the nursery heap size.
  Vector<Key, 0, AllocPolicy> nurseryEntries;

 public:
  using Lookup = typename MapType::Lookup;
  using Ptr = typename MapType::Ptr;
  using Range = typename MapType::Range;
  using Entry = typename MapType::Entry;

  explicit NurseryAwareHashMap(AllocPolicy a = AllocPolicy())
      : map(a), nurseryEntries(std::move(a)) {}
  explicit NurseryAwareHashMap(size_t length) : map(length) {}
  NurseryAwareHashMap(AllocPolicy a, size_t length)
      : map(a, length), nurseryEntries(std::move(a)) {}

  bool empty() const { return map.empty(); }
  Ptr lookup(const Lookup& l) const { return map.lookup(l); }
  void remove(Ptr p) { map.remove(p); }
  Range all() const { return map.all(); }
  struct Enum : public MapType::Enum {
    explicit Enum(NurseryAwareHashMap& namap) : MapType::Enum(namap.map) {}
  };
  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return map.shallowSizeOfExcludingThis(mallocSizeOf) +
           nurseryEntries.sizeOfExcludingThis(mallocSizeOf);
  }
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return map.shallowSizeOfIncludingThis(mallocSizeOf) +
           nurseryEntries.sizeOfIncludingThis(mallocSizeOf);
  }

  [[nodiscard]] bool put(const Key& k, const Value& v) {
    auto p = map.lookupForAdd(k);
    if (p) {
      if (!JS::GCPolicy<Key>::isTenured(k) ||
          !JS::GCPolicy<Value>::isTenured(v)) {
        if (!nurseryEntries.append(k)) {
          return false;
        }
      }
      p->value() = v;
      return true;
    }

    bool ok = map.add(p, k, v);
    if (!ok) {
      return false;
    }

    if (!JS::GCPolicy<Key>::isTenured(k) ||
        !JS::GCPolicy<Value>::isTenured(v)) {
      if (!nurseryEntries.append(k)) {
        map.remove(k);
        return false;
      }
    }

    return true;
  }

  void sweepAfterMinorGC(JSTracer* trc) {
    for (auto& key : nurseryEntries) {
      auto p = map.lookup(key);
      if (!p) {
        continue;
      }

      // Drop the entry if the value is not marked.
      if (JS::GCPolicy<BarrieredValue>::needsSweep(&p->value())) {
        map.remove(key);
        continue;
      }

      // Update and relocate the key, if the value is still needed.
      //
      // Non-string Values will contain a strong reference to Key, as per
      // its use in the CrossCompartmentWrapperMap, so the key will never
      // be dying here. Strings do *not* have any sort of pointer from
      // wrapper to wrappee, as they are just copies. The wrapper map
      // entry is merely used as a cache to avoid re-copying the string,
      // and currently that entire cache is flushed on major GC.
      Key copy(key);
      bool sweepKey = JS::GCPolicy<Key>::needsSweep(&copy);
      if (sweepKey) {
        map.remove(key);
        continue;
      }
      if (AllowDuplicates) {
        // Drop duplicated keys.
        //
        // A key can be forwarded to another place. In this case, rekey the
        // item. If two or more different keys are forwarded to the same new
        // key, simply drop the later ones.
        if (key == copy) {
          // No rekey needed.
        } else if (map.has(copy)) {
          // Key was forwarded to the same place that another key was already
          // forwarded to.
          map.remove(key);
        } else {
          map.rekeyAs(key, copy, copy);
        }
      } else {
        MOZ_ASSERT(key == copy || !map.has(copy));
        map.rekeyIfMoved(key, copy);
      }
    }
    nurseryEntries.clear();
  }

  void sweep() { map.sweep(); }

  void clear() {
    map.clear();
    nurseryEntries.clear();
  }

  bool hasNurseryEntries() const { return !nurseryEntries.empty(); }
};

}  // namespace js

namespace JS {
template <typename T>
struct GCPolicy<js::detail::UnsafeBareWeakHeapPtr<T>> {
  static void trace(JSTracer* trc, js::detail::UnsafeBareWeakHeapPtr<T>* thingp,
                    const char* name) {
    js::TraceEdge(trc, thingp, name);
  }
  static bool needsSweep(js::detail::UnsafeBareWeakHeapPtr<T>* thingp) {
    return js::gc::IsAboutToBeFinalized(thingp);
  }
};
}  // namespace JS

#endif  // gc_NurseryAwareHashMap_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Caches_h
#define vm_Caches_h

#include "mozilla/Array.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/MruCache.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/UniquePtr.h"

#include "frontend/ScopeBindingCache.h"
#include "gc/Tracer.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "vm/JSScript.h"
#include "vm/Shape.h"
#include "vm/StringType.h"

namespace js {

struct EvalCacheEntry {
  JSLinearString* str;
  JSScript* script;
  JSScript* callerScript;
  jsbytecode* pc;

  // We sweep this cache after a nursery collection to update entries with
  // string keys that have been tenured.
  //
  // The entire cache is purged on a major GC, so we don't need to sweep it
  // then.
  bool traceWeak(JSTracer* trc) {
    MOZ_ASSERT(trc->kind() == JS::TracerKind::MinorSweeping);
    return TraceManuallyBarrieredWeakEdge(trc, &str, "EvalCacheEntry::str");
  }
};

struct EvalCacheLookup {
  JSLinearString* str = nullptr;
  JSScript* callerScript = nullptr;
  MOZ_INIT_OUTSIDE_CTOR jsbytecode* pc = nullptr;

  EvalCacheLookup() = default;
  EvalCacheLookup(JSLinearString* str, JSScript* callerScript, jsbytecode* pc)
      : str(str), callerScript(callerScript), pc(pc) {}

  void trace(JSTracer* trc);
};

struct EvalCacheHashPolicy {
  using Lookup = EvalCacheLookup;

  static HashNumber hash(const Lookup& l);
  static bool match(const EvalCacheEntry& entry, const EvalCacheLookup& l);
};

using EvalCache =
    GCHashSet<EvalCacheEntry, EvalCacheHashPolicy, SystemAllocPolicy>;

class MegamorphicCacheEntry {
  // Receiver object's shape.
  Shape* shape_ = nullptr;

  // The atom or symbol property being accessed.
  PropertyKey key_;

  // Slot offset and isFixedSlot flag of the data property.
  TaggedSlotOffset slotOffset_;

  // This entry is valid iff the generation matches the cache's generation.
  uint16_t generation_ = 0;

  // This encodes the number of hops on the prototype chain to get to the holder
  // object, along with information about the kind of property. If the high bit
  // is 0, the property is a data property. If the high bit is 1 and the value
  // is <= MaxHopsForGetterProperty, the property is a getter. Otherwise, this
  // is a sentinel value indicating a missing property lookup.
  uint8_t hopsAndKind_ = 0;

  friend class MegamorphicCache;

 public:
  // A simple flag for the JIT to check which, if false, lets it know that it's
  // just a data property N hops up the prototype chain
  static constexpr uint8_t NonDataPropertyFlag = 128;

  static constexpr uint8_t MaxHopsForGetterProperty = 253;
  static constexpr uint8_t NumHopsForMissingProperty = 254;
  static constexpr uint8_t NumHopsForMissingOwnProperty = 255;

  static constexpr uint8_t MaxHopsForDataProperty = 127;
  static constexpr uint8_t MaxHopsForAccessorProperty = 125;

  void init(Shape* shape, PropertyKey key, uint16_t generation, uint8_t numHops,
            TaggedSlotOffset slotOffset) {
    shape_ = shape;
    key_ = key;
    slotOffset_ = slotOffset;
    generation_ = generation;
    hopsAndKind_ = numHops;
    MOZ_ASSERT(hopsAndKind_ == numHops, "numHops must fit in hopsAndKind_");
  }
  bool isMissingProperty() const {
    return hopsAndKind_ == NumHopsForMissingProperty;
  }
  bool isMissingOwnProperty() const {
    return hopsAndKind_ == NumHopsForMissingOwnProperty;
  }
  bool isAccessorProperty() const {
    return hopsAndKind_ >= NonDataPropertyFlag and
           hopsAndKind_ <= MaxHopsForGetterProperty;
  }
  bool isDataProperty() const { return !(hopsAndKind_ & NonDataPropertyFlag); }
  uint16_t numHops() const {
    if (isDataProperty()) {
      return hopsAndKind_;
    } else {
      MOZ_ASSERT(isAccessorProperty());
      return hopsAndKind_ & ~NonDataPropertyFlag;
    }
  }
  TaggedSlotOffset slotOffset() const {
    MOZ_ASSERT(hopsAndKind_ <= MaxHopsForGetterProperty);
    return slotOffset_;
  }

  static constexpr size_t offsetOfShape() {
    return offsetof(MegamorphicCacheEntry, shape_);
  }

  static constexpr size_t offsetOfKey() {
    return offsetof(MegamorphicCacheEntry, key_);
  }

  static constexpr size_t offsetOfGeneration() {
    return offsetof(MegamorphicCacheEntry, generation_);
  }

  static constexpr size_t offsetOfSlotOffset() {
    return offsetof(MegamorphicCacheEntry, slotOffset_);
  }

  static constexpr size_t offsetOfHopsAndKind() {
    return offsetof(MegamorphicCacheEntry, hopsAndKind_);
  }
};

// [SMDOC] Megamorphic Property Lookup Cache (MegamorphicCache)
//
// MegamorphicCache is a data structure used to speed up megamorphic property
// lookups from JIT code. The same cache is currently used for both GetProp and
// HasProp (in, hasOwnProperty) operations.
//
// This is implemented as a fixed-size array of entries. Lookups are performed
// based on the receiver object's Shape + PropertyKey. If found in the cache,
// the result of a lookup represents either:
//
// * A data property on the receiver or on its proto chain (stored as number of
//   'hops' up the proto chain + the slot of the data property).
//
// * A missing property on the receiver or its proto chain.
//
// * A missing property on the receiver, but it might exist on the proto chain.
//   This lets us optimize hasOwnProperty better.
//
// Collisions are handled by simply overwriting the previous entry stored in the
// slot. This is sufficient to achieve a high hit rate on typical web workloads
// while ensuring cache lookups are always fast and simple.
//
// Lookups always check the receiver object's shape (ensuring the properties and
// prototype are unchanged). Because the cache also caches lookups on the proto
// chain, Watchtower is used to invalidate the cache when prototype objects are
// mutated. This is done by incrementing the cache's generation counter to
// invalidate all entries.
//
// The cache is also invalidated on each major GC.
class MegamorphicCache {
 public:
  using Entry = MegamorphicCacheEntry;

  static constexpr size_t NumEntries = 1024;
  static constexpr uint8_t ShapeHashShift1 =
      mozilla::tl::FloorLog2<alignof(Shape)>::value;
  static constexpr uint8_t ShapeHashShift2 =
      ShapeHashShift1 + mozilla::tl::FloorLog2<NumEntries>::value;

  static_assert(mozilla::IsPowerOfTwo(alignof(Shape)) &&
                    mozilla::IsPowerOfTwo(NumEntries),
                "FloorLog2 is exact because alignof(Shape) and NumEntries are "
                "both powers of two");

 private:
  mozilla::Array<Entry, NumEntries> entries_;

  // Generation counter used to invalidate all entries.
  uint16_t generation_ = 0;

  // NOTE: this logic is mirrored in MacroAssembler::emitMegamorphicCacheLookup
  Entry& getEntry(Shape* shape, PropertyKey key) {
    static_assert(mozilla::IsPowerOfTwo(NumEntries),
                  "NumEntries must be a power-of-two for fast modulo");
    uintptr_t hash = uintptr_t(shape) >> ShapeHashShift1;
    hash ^= uintptr_t(shape) >> ShapeHashShift2;
    hash += HashAtomOrSymbolPropertyKey(key);
    return entries_[hash % NumEntries];
  }

 public:
  void bumpGeneration() {
    generation_++;
    if (generation_ == 0) {
      // Generation overflowed. Invalidate the whole cache.
      for (size_t i = 0; i < NumEntries; i++) {
        entries_[i].shape_ = nullptr;
      }
    }
  }
  bool isValidForLookup(const Entry& entry, Shape* shape, PropertyKey key) {
    return (entry.shape_ == shape && entry.key_ == key &&
            entry.generation_ == generation_);
  }
  bool lookup(Shape* shape, PropertyKey key, Entry** entryp) {
    Entry& entry = getEntry(shape, key);
    *entryp = &entry;
    return isValidForLookup(entry, shape, key);
  }

  void initEntryForMissingProperty(Entry* entry, Shape* shape,
                                   PropertyKey key) {
    entry->init(shape, key, generation_, Entry::NumHopsForMissingProperty,
                TaggedSlotOffset());
  }
  void initEntryForMissingOwnProperty(Entry* entry, Shape* shape,
                                      PropertyKey key) {
    entry->init(shape, key, generation_, Entry::NumHopsForMissingOwnProperty,
                TaggedSlotOffset());
  }
  void initEntryForDataProperty(Entry* entry, Shape* shape, PropertyKey key,
                                size_t numHops, TaggedSlotOffset slotOffset) {
    if (numHops > Entry::MaxHopsForDataProperty) {
      return;
    }
    entry->init(shape, key, generation_, numHops, slotOffset);
  }
  void initEntryForAccessorProperty(Entry* entry, Shape* shape, PropertyKey key,
                                    size_t numHops,
                                    TaggedSlotOffset slotOffset) {
    if (numHops > Entry::MaxHopsForAccessorProperty) {
      return;
    }
    numHops |= MegamorphicCacheEntry::NonDataPropertyFlag;
    entry->init(shape, key, generation_, numHops, slotOffset);
  }

  static constexpr size_t offsetOfEntries() {
    return offsetof(MegamorphicCache, entries_);
  }

  static constexpr size_t offsetOfGeneration() {
    return offsetof(MegamorphicCache, generation_);
  }
};

class MegamorphicSetPropCacheEntry {
  Shape* beforeShape_ = nullptr;
  Shape* afterShape_ = nullptr;

  // The atom or symbol property being accessed.
  PropertyKey key_;

  // Slot offset and isFixedSlot flag of the data property.
  TaggedSlotOffset slotOffset_;

  // If slots need to be grown, this is the new capacity we need.
  uint16_t newCapacity_ = 0;

  // This entry is valid iff the generation matches the cache's generation.
  uint16_t generation_ = 0;

  friend class MegamorphicSetPropCache;

 public:
  void init(Shape* beforeShape, Shape* afterShape, PropertyKey key,
            uint16_t generation, TaggedSlotOffset slotOffset,
            uint16_t newCapacity) {
    beforeShape_ = beforeShape;
    afterShape_ = afterShape;
    key_ = key;
    slotOffset_ = slotOffset;
    newCapacity_ = newCapacity;
    generation_ = generation;
  }
  TaggedSlotOffset slotOffset() const { return slotOffset_; }
  Shape* afterShape() const { return afterShape_; }

  static constexpr size_t offsetOfShape() {
    return offsetof(MegamorphicSetPropCacheEntry, beforeShape_);
  }
  static constexpr size_t offsetOfAfterShape() {
    return offsetof(MegamorphicSetPropCacheEntry, afterShape_);
  }

  static constexpr size_t offsetOfKey() {
    return offsetof(MegamorphicSetPropCacheEntry, key_);
  }

  static constexpr size_t offsetOfNewCapacity() {
    return offsetof(MegamorphicSetPropCacheEntry, newCapacity_);
  }

  static constexpr size_t offsetOfGeneration() {
    return offsetof(MegamorphicSetPropCacheEntry, generation_);
  }

  static constexpr size_t offsetOfSlotOffset() {
    return offsetof(MegamorphicSetPropCacheEntry, slotOffset_);
  }
};

class MegamorphicSetPropCache {
 public:
  using Entry = MegamorphicSetPropCacheEntry;
  // We can get more hits if we increase this, but this seems to be around
  // the sweet spot where we are getting most of the hits we would get with
  // an infinitely sized cache
  static constexpr size_t NumEntries = 1024;
  static constexpr uint8_t ShapeHashShift1 =
      mozilla::tl::FloorLog2<alignof(Shape)>::value;
  static constexpr uint8_t ShapeHashShift2 =
      ShapeHashShift1 + mozilla::tl::FloorLog2<NumEntries>::value;

  static_assert(mozilla::IsPowerOfTwo(alignof(Shape)) &&
                    mozilla::IsPowerOfTwo(NumEntries),
                "FloorLog2 is exact because alignof(Shape) and NumEntries are "
                "both powers of two");

 private:
  mozilla::Array<Entry, NumEntries> entries_;

  // Generation counter used to invalidate all entries.
  uint16_t generation_ = 0;

  Entry& getEntry(Shape* beforeShape, PropertyKey key) {
    static_assert(mozilla::IsPowerOfTwo(NumEntries),
                  "NumEntries must be a power-of-two for fast modulo");
    uintptr_t hash = uintptr_t(beforeShape) >> ShapeHashShift1;
    hash ^= uintptr_t(beforeShape) >> ShapeHashShift2;
    hash += HashAtomOrSymbolPropertyKey(key);
    return entries_[hash % NumEntries];
  }

 public:
  void bumpGeneration() {
    generation_++;
    if (generation_ == 0) {
      // Generation overflowed. Invalidate the whole cache.
      for (size_t i = 0; i < NumEntries; i++) {
        entries_[i].beforeShape_ = nullptr;
      }
    }
  }
  void set(Shape* beforeShape, Shape* afterShape, PropertyKey key,
           TaggedSlotOffset slotOffset, uint32_t newCapacity) {
    uint16_t newSlots = (uint16_t)newCapacity;
    if (newSlots != newCapacity) {
      return;
    }
    Entry& entry = getEntry(beforeShape, key);
    entry.init(beforeShape, afterShape, key, generation_, slotOffset, newSlots);
  }

#ifdef DEBUG
  bool lookup(Shape* beforeShape, PropertyKey key, Entry** entryp) {
    Entry& entry = getEntry(beforeShape, key);
    *entryp = &entry;
    return (entry.beforeShape_ == beforeShape && entry.key_ == key &&
            entry.generation_ == generation_);
  }
#endif

  static constexpr size_t offsetOfEntries() {
    return offsetof(MegamorphicSetPropCache, entries_);
  }

  static constexpr size_t offsetOfGeneration() {
    return offsetof(MegamorphicSetPropCache, generation_);
  }
};

// Cache for AtomizeString, mapping JSString* or JS::Latin1Char* to the
// corresponding JSAtom*. The cache has three different optimizations:
//
// * The two most recent lookups are cached. This has a hit rate of 30-65% on
//   typical web workloads.
//
// * MruCache is used for short JS::Latin1Char strings.
//
// * For longer strings, there's also a JSLinearString* => JSAtom* HashMap,
//   because hashing the string characters repeatedly can be slow.
//   This map is also used by nursery GC to de-duplicate strings to atoms.
//
// This cache is purged on minor and major GC.
class StringToAtomCache {
 public:
  struct LastLookup {
    JSString* string = nullptr;
    JSAtom* atom = nullptr;

    static constexpr size_t offsetOfString() {
      return offsetof(LastLookup, string);
    }

    static constexpr size_t offsetOfAtom() {
      return offsetof(LastLookup, atom);
    }
  };
  static constexpr size_t NumLastLookups = 2;

  struct AtomTableKey {
    explicit AtomTableKey(const JS::Latin1Char* str, size_t len)
        : string_(str), length_(len) {
      hash_ = mozilla::HashString(string_, length_);
    }

    const JS::Latin1Char* string_;
    size_t length_;
    uint32_t hash_;
  };

 private:
  struct RopeAtomCache
      : public mozilla::MruCache<AtomTableKey, JSAtom*, RopeAtomCache> {
    static HashNumber Hash(const AtomTableKey& key) { return key.hash_; }
    static bool Match(const AtomTableKey& key, const JSAtom* val) {
      JS::AutoCheckCannotGC nogc;
      return val->length() == key.length_ &&
             EqualChars(key.string_, val->latin1Chars(nogc), key.length_);
    }
  };
  using Map =
      HashMap<JSString*, JSAtom*, PointerHasher<JSString*>, SystemAllocPolicy>;
  Map map_;
  mozilla::Array<LastLookup, NumLastLookups> lastLookups_;
  RopeAtomCache ropeCharCache_;

 public:
  // Don't use the HashMap for short strings. Hashing them is less expensive.
  // But the length needs to long enough to cover common identifiers in React.
  static constexpr size_t MinStringLength = 39;

  JSAtom* lookupInMap(JSString* s) const {
    MOZ_ASSERT(s->inStringToAtomCache());
    MOZ_ASSERT(s->length() >= MinStringLength);

    auto p = map_.lookup(s);
    JSAtom* atom = p ? p->value() : nullptr;
    return atom;
  }

  MOZ_ALWAYS_INLINE JSAtom* lookup(JSString* s) const {
    MOZ_ASSERT(!s->isAtom());
    for (const LastLookup& entry : lastLookups_) {
      if (entry.string == s) {
        return entry.atom;
      }
    }

    if (!s->inStringToAtomCache()) {
      MOZ_ASSERT(!map_.lookup(s));
      return nullptr;
    }

    return lookupInMap(s);
  }

  MOZ_ALWAYS_INLINE JSAtom* lookupWithRopeChars(
      const JS::Latin1Char* str, size_t len,
      mozilla::Maybe<AtomTableKey>& key) {
    MOZ_ASSERT(len < MinStringLength);
    key.emplace(str, len);
    if (auto p = ropeCharCache_.Lookup(key.value())) {
      return p.Data();
    }
    return nullptr;
  }

  static constexpr size_t offsetOfLastLookups() {
    return offsetof(StringToAtomCache, lastLookups_);
  }

  void maybePut(JSString* s, JSAtom* atom, mozilla::Maybe<AtomTableKey>& key) {
    if (key.isSome()) {
      ropeCharCache_.Put(key.value(), atom);
    }

    for (size_t i = NumLastLookups - 1; i > 0; i--) {
      lastLookups_[i] = lastLookups_[i - 1];
    }
    lastLookups_[0].string = s;
    lastLookups_[0].atom = atom;

    if (s->length() < MinStringLength) {
      return;
    }
    if (!map_.putNew(s, atom)) {
      return;
    }
    s->setInStringToAtomCache();
  }

  void purge() {
    map_.clearAndCompact();
    for (LastLookup& entry : lastLookups_) {
      entry.string = nullptr;
      entry.atom = nullptr;
    }

    ropeCharCache_.Clear();
  }
};

#ifdef MOZ_EXECUTION_TRACING

// Holds a handful of caches used for tracing JS execution. These effectively
// hold onto IDs which let the tracer know that it has already recorded the
// entity in question. They need to be cleared on a compacting GC since they
// are keyed by pointers. However the IDs must continue incrementing until
// the tracer is turned off since entries containing the IDs in question may
// linger in the ExecutionTracer's buffer through a GC.
class TracingCaches {
  uint32_t shapeId_ = 0;
  uint32_t atomId_ = 0;
  using TracingPointerCache =
      HashMap<uintptr_t, uint32_t, DefaultHasher<uintptr_t>, SystemAllocPolicy>;
  TracingPointerCache shapes_;
  TracingPointerCache atoms_;

  // NOTE: this cache does not need to be cleared on compaction, but still
  // needs to be cleared at the end of tracing.
  using TracingU32Set =
      HashSet<uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy>;
  TracingU32Set scriptSourcesSeen_;

 public:
  void clearOnCompaction() {
    atoms_.clear();
    shapes_.clear();
  }

  void clearAll() {
    shapeId_ = 0;
    atomId_ = 0;
    scriptSourcesSeen_.clear();
    atoms_.clear();
    shapes_.clear();
  }

  enum class GetOrPutResult {
    OOM,
    NewlyAdded,
    WasPresent,
  };

  GetOrPutResult getOrPutAtom(JSAtom* atom, uint32_t* id) {
    TracingPointerCache::AddPtr p =
        atoms_.lookupForAdd(reinterpret_cast<uintptr_t>(atom));
    if (p) {
      *id = p->value();
      return GetOrPutResult::WasPresent;
    }
    *id = atomId_++;
    if (!atoms_.add(p, reinterpret_cast<uintptr_t>(atom), *id)) {
      return GetOrPutResult::OOM;
    }
    return GetOrPutResult::NewlyAdded;
  }

  GetOrPutResult getOrPutShape(Shape* shape, uint32_t* id) {
    TracingPointerCache::AddPtr p =
        shapes_.lookupForAdd(reinterpret_cast<uintptr_t>(shape));
    if (p) {
      *id = p->value();
      return GetOrPutResult::WasPresent;
    }
    *id = shapeId_++;
    if (!shapes_.add(p, reinterpret_cast<uintptr_t>(shape), *id)) {
      return GetOrPutResult::OOM;
    }
    return GetOrPutResult::NewlyAdded;
  }

  // NOTE: scriptSourceId is js::ScriptSource::id value.
  GetOrPutResult putScriptSourceIfMissing(uint32_t scriptSourceId) {
    TracingU32Set::AddPtr p = scriptSourcesSeen_.lookupForAdd(scriptSourceId);
    if (p) {
      return GetOrPutResult::WasPresent;
    }
    if (!scriptSourcesSeen_.add(p, scriptSourceId)) {
      return GetOrPutResult::OOM;
    }
    return GetOrPutResult::NewlyAdded;
  }
};

#endif /* MOZ_EXECUTION_TRACING */

class RuntimeCaches {
 public:
  MegamorphicCache megamorphicCache;
  UniquePtr<MegamorphicSetPropCache> megamorphicSetPropCache;
  UncompressedSourceCache uncompressedSourceCache;
  EvalCache evalCache;
  StringToAtomCache stringToAtomCache;

#ifdef MOZ_EXECUTION_TRACING
  TracingCaches tracingCaches;
#endif

  // Delazification: Cache binding for runtime objects which are used during
  // delazification to quickly resolve NameLocation of bindings without linearly
  // iterating over the list of bindings.
  frontend::RuntimeScopeBindingCache scopeCache;

  void sweepAfterMinorGC(JSTracer* trc) { evalCache.traceWeak(trc); }
#ifdef JSGC_HASH_TABLE_CHECKS
  void checkEvalCacheAfterMinorGC();
#endif

  void purgeForCompaction() {
    evalCache.clear();
    stringToAtomCache.purge();
    megamorphicCache.bumpGeneration();
    if (megamorphicSetPropCache) {
      // MegamorphicSetPropCache can be null if we failed out of
      // JSRuntime::init. We will then try to destroy the runtime which will
      // do a GC and land us here.
      megamorphicSetPropCache->bumpGeneration();
    }
    scopeCache.purge();
#ifdef MOZ_EXECUTION_TRACING
    tracingCaches.clearOnCompaction();
#endif
  }

  void purge() {
    purgeForCompaction();
    uncompressedSourceCache.purge();
  }
};

}  // namespace js

#endif /* vm_Caches_h */

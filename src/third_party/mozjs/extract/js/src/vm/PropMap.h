/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PropMap_h
#define vm_PropMap_h

#include "gc/Barrier.h"
#include "gc/Cell.h"
#include "js/TypeDecls.h"
#include "js/UbiNode.h"
#include "vm/ObjectFlags.h"
#include "vm/PropertyInfo.h"
#include "vm/PropertyKey.h"

// [SMDOC] Property Maps
//
// Property maps are used to store information about native object properties.
// Each property map represents an ordered list of (PropertyKey, PropertyInfo)
// tuples.
//
// Each property map can store up to 8 properties (see PropMap::Capacity). To
// store more than eight properties, multiple maps must be linked together with
// the |previous| pointer.
//
// Shapes and Property Maps
// ------------------------
// Native object shapes represent property information as a (PropMap*, length)
// tuple. When there are no properties yet, the shape's map will be nullptr and
// the length is zero.
//
// For example, consider the following objects:
//
//   o1 = {x: 1, y: 2}
//   o2 = {x: 3, y: 4, z: 5}
//
// This is stored as follows:
//
//   +-------------+      +--------------+     +-------------------+
//   | JSObject o1 |      | Shape S1     |     | PropMap M1        |
//   |-------------+      +--------------+     +-------------------+
//   | shape: S1  -+--->  | map: M1     -+--+> | key 0: x (slot 0) |
//   | slot 0: 1   |      | mapLength: 2 |  |  | key 1: y (slot 1) |
//   | slot 1: 2   |      +--------------+  |  | key 2: z (slot 2) |
//   +-------------+                        |  | ...               |
//                                          |  +-------------------+
//                                          |
//   +-------------+      +--------------+  |
//   | JSObject o2 |      | Shape S2     |  |
//   |-------------+      +--------------+  |
//   | shape: S2  -+--->  | map: M1     -+--+
//   | slot 0: 3   |      | mapLength: 3 |
//   | slot 1: 4   |      +--------------+
//   | slot 2: 5   |
//   +-------------+
//
// There's a single map M1 shared by shapes S1 and S2. Shape S1 includes only
// the first two properties and shape S2 includes all three properties.
//
// Class Hierarchy
// ---------------
// Property maps have the following C++ class hierarchy:
//
//   PropMap (abstract)
//    |
//    +-- SharedPropMap (abstract)
//    |      |
//    |      +-- CompactPropMap
//    |      |
//    |      +-- NormalPropMap
//    |
//    +-- DictionaryPropMap
//
// * PropMap: base class. It has a flags word and an array of PropertyKeys.
//
// * SharedPropMap: base class for all shared property maps. See below for more
//                  information on shared maps.
//
// * CompactPropMap: a shared map that stores its information more compactly
//                   than the other maps. It saves four words by not storing a
//                   PropMapTable, previous pointer, and by using a more compact
//                   PropertyInfo type for slot numbers that fit in one byte.
//
// * NormalPropMap: a shared map, used when CompactPropMap can't be used.
//
// * DictionaryPropMap: an unshared map (used by a single object/shape). See
//                      below for more information on dictionary maps.
//
// Secondary hierarchy
// -------------------
// NormalPropMap and DictionaryPropMap store property information in the same
// way. This means property lookups don't have to distinguish between these two
// types. This is represented with a second class hierarchy:
//
//   PropMap (abstract)
//    |
//    +-- CompactPropMap
//    |
//    +-- LinkedPropMap (NormalPropMap or DictionaryPropMap)
//
// Property lookup and property iteration are very performance sensitive and use
// this Compact vs Linked "view" so that they don't have to handle the three map
// types separately.
//
// LinkedPropMap also stores the PropMapTable and a pointer to the |previous|
// map. Compact maps don't have these fields.
//
// To summarize these map types:
//
//   +-------------------+-------------+--------+
//   | Concrete type     | Shared/tree | Linked |
//   +-------------------+-------------+--------+
//   | CompactPropMap    | yes         | no     |
//   | NormalPropMap     | yes         | yes    |
//   | DictionaryPropMap | no          | yes    |
//   +-------------------+-------------+--------+
//
// PropMapTable
// ------------
// Finding the PropertyInfo for a particular PropertyKey requires a linear
// search if the map is small. For larger maps we can create a PropMapTable, a
// hash table that maps from PropertyKey to PropMap + index, to speed up
// property lookups.
//
// To save memory, property map tables can be discarded on GC and recreated when
// needed. AutoKeepPropMapTables can be used to avoid discarding tables in a
// particular zone. Methods to access a PropMapTable take either an
// AutoCheckCannotGC or AutoKeepPropMapTables argument, to help ensure tables
// are not purged while we're using them.
//
// Shared Property Maps
// --------------------
// Shared property maps can be shared per-Zone by objects with the same property
// keys, flags, and slot numbers. To make this work, shared maps form a tree:
//
// - Each Zone has a table that maps from first PropertyKey + PropertyInfo to
//   a SharedPropMap that begins with that property. This is used to lookup the
//   the map to use when adding the first property.
//   See ShapeZone::initialPropMaps.
//
// - When adding a property other than the first one, the property is stored in
//   the next entry of the same map when possible. If the map is full or the
//   next entry already stores a different property, a child map is created and
//   linked to the parent map.
//
// For example, imagine we want to create these objects:
//
//   o1 = {x: 1, y: 2, z: 3}
//   o2 = {x: 1, y: 2, foo: 4}
//
// This will result in the following maps being created:
//
//     +---------------------+    +---------------------+
//     | SharedPropMap M1    |    | SharedPropMap M2    |
//     +---------------------+    +---------------------+
//     | Child M2 (index 1) -+--> | Parent M1 (index 1) |
//     +---------------------+    +---------------------+
//     | 0: x                |    | 0: x                |
//     | 1: y                |    | 1: y                |
//     | 2: z                |    | 2: foo              |
//     | ...                 |    | ...                 |
//     +---------------------+    +---------------------+
//
// M1 is the map used for initial property "x". Properties "y" and "z" can be
// stored inline. When later adding "foo" following "y", the map has to be
// forked: a child map M2 is created and M1 remembers this transition at
// property index 1 so that M2 will be used the next time properties "x", "y",
// and "foo" are added to an object.
//
// Shared maps contain a TreeData struct that stores the parent and children
// links for the SharedPropMap tree. The parent link is a tagged pointer that
// stores both the parent map and the property index of the last used property
// in the parent map before the branch. The children are stored similarly: the
// parent map can store a single child map and index, or a set of children.
// See SharedChildrenPtr.
//
// Looking up a child map can then be done based on the index of the last
// property in the parent map and the new property's key and flags. So for the
// example above, the lookup key for M1 => M2 is (index 1, "foo", <flags>).
//
// Note: shared maps can have both a |previous| map and a |parent| map. They are
// equal when the previous map was full, but can be different maps when
// branching in the middle of a map like in the example above: M2 has parent M1
// but does not have a |previous| map (because it only has three properties).
//
// Dictionary Property Maps
// ------------------------
// Certain operations can't be implemented (efficiently) for shared property
// maps, for example changing or deleting a property other than the last one.
// When this happens the map is copied as a DictionaryPropMap.
//
// Dictionary maps are unshared so can be mutated in place (after generating a
// new shape for the object).
//
// Unlike shared maps, dictionary maps can have holes between two property keys
// after removing a property. When there are more holes than properties, the
// map is compacted. See DictionaryPropMap::maybeCompact.

namespace js {

enum class IntegrityLevel;

class DictionaryPropMap;
class SharedPropMap;
class LinkedPropMap;
class CompactPropMap;
class NormalPropMap;

// Template class for storing a PropMap* and a property index as tagged pointer.
template <typename T>
class MapAndIndex {
  uintptr_t data_ = 0;

  static constexpr uintptr_t IndexMask = 0b111;

 public:
  MapAndIndex() = default;

  MapAndIndex(const T* map, uint32_t index) : data_(uintptr_t(map) | index) {
    MOZ_ASSERT((uintptr_t(map) & IndexMask) == 0);
    MOZ_ASSERT(index <= IndexMask);
  }
  explicit MapAndIndex(uintptr_t data) : data_(data) {}

  void setNone() { data_ = 0; }

  bool isNone() const { return data_ == 0; }

  uintptr_t raw() const { return data_; }
  T* maybeMap() const { return reinterpret_cast<T*>(data_ & ~IndexMask); }

  uint32_t index() const {
    MOZ_ASSERT(!isNone());
    return data_ & IndexMask;
  }
  T* map() const {
    MOZ_ASSERT(!isNone());
    return maybeMap();
  }

  inline PropertyInfo propertyInfo() const;

  bool operator==(const MapAndIndex<T>& other) const {
    return data_ == other.data_;
  }
  bool operator!=(const MapAndIndex<T>& other) const {
    return !operator==(other);
  }
} JS_HAZ_GC_POINTER;
using PropMapAndIndex = MapAndIndex<PropMap>;
using SharedPropMapAndIndex = MapAndIndex<SharedPropMap>;

struct SharedChildrenHasher;
using SharedChildrenSet =
    HashSet<SharedPropMapAndIndex, SharedChildrenHasher, SystemAllocPolicy>;

// Children of shared maps. This is either:
//
// - None (no children)
// - SingleMapAndIndex (one child map, including the property index of the last
//   property before the branch)
// - SharedChildrenSet (multiple children)
//
// Because SingleMapAndIndex use all bits, this relies on the HasChildrenSet
// flag in the map to distinguish the latter two cases.
class SharedChildrenPtr {
  uintptr_t data_ = 0;

 public:
  bool isNone() const { return data_ == 0; }
  void setNone() { data_ = 0; }

  void setSingleChild(SharedPropMapAndIndex child) { data_ = child.raw(); }
  void setChildrenSet(SharedChildrenSet* set) { data_ = uintptr_t(set); }

  SharedPropMapAndIndex toSingleChild() const {
    MOZ_ASSERT(!isNone());
    return SharedPropMapAndIndex(data_);
  }

  SharedChildrenSet* toChildrenSet() const {
    MOZ_ASSERT(!isNone());
    return reinterpret_cast<SharedChildrenSet*>(data_);
  }
} JS_HAZ_GC_POINTER;

// Ensures no property map tables are purged in the current zone.
class MOZ_RAII AutoKeepPropMapTables {
  JSContext* cx_;
  bool prev_;

 public:
  void operator=(const AutoKeepPropMapTables&) = delete;
  AutoKeepPropMapTables(const AutoKeepPropMapTables&) = delete;
  explicit inline AutoKeepPropMapTables(JSContext* cx);
  inline ~AutoKeepPropMapTables();
};

// Hash table to optimize property lookups on larger maps. This maps from
// PropertyKey to PropMapAndIndex.
class PropMapTable {
  struct Hasher {
    using Key = PropMapAndIndex;
    using Lookup = PropertyKey;
    static MOZ_ALWAYS_INLINE HashNumber hash(PropertyKey key);
    static MOZ_ALWAYS_INLINE bool match(PropMapAndIndex, PropertyKey key);
  };

  // Small lookup cache. This has a hit rate of 30-60% on most workloads and is
  // a lot faster than the full HashSet lookup.
  struct CacheEntry {
    PropertyKey key;
    PropMapAndIndex result;
  };
  static constexpr uint32_t NumCacheEntries = 2;
  CacheEntry cacheEntries_[NumCacheEntries];

  using Set = HashSet<PropMapAndIndex, Hasher, SystemAllocPolicy>;
  Set set_;

  void setCacheEntry(PropertyKey key, PropMapAndIndex entry) {
    for (uint32_t i = 0; i < NumCacheEntries; i++) {
      if (cacheEntries_[i].key == key) {
        cacheEntries_[i].result = entry;
        return;
      }
    }
  }
  bool lookupInCache(PropertyKey key, PropMapAndIndex* result) const {
    for (uint32_t i = 0; i < NumCacheEntries; i++) {
      if (cacheEntries_[i].key == key) {
        *result = cacheEntries_[i].result;
#ifdef DEBUG
        auto p = lookupRaw(key);
        MOZ_ASSERT(*result == (p ? *p : PropMapAndIndex()));
#endif
        return true;
      }
    }
    return false;
  }
  void addToCache(PropertyKey key, Set::Ptr p) {
    for (uint32_t i = NumCacheEntries - 1; i > 0; i--) {
      cacheEntries_[i] = cacheEntries_[i - 1];
      MOZ_ASSERT(cacheEntries_[i].key != key);
    }
    cacheEntries_[0].key = key;
    cacheEntries_[0].result = p ? *p : PropMapAndIndex();
  }

 public:
  using Ptr = Set::Ptr;

  PropMapTable() = default;
  ~PropMapTable() = default;

  uint32_t entryCount() const { return set_.count(); }

  // This counts the PropMapTable object itself (which must be heap-allocated)
  // and its HashSet.
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
    return mallocSizeOf(this) + set_.shallowSizeOfExcludingThis(mallocSizeOf);
  }

  // init() is fallible and reports OOM to the context.
  bool init(JSContext* cx, LinkedPropMap* map);

  MOZ_ALWAYS_INLINE PropMap* lookup(PropMap* map, uint32_t mapLength,
                                    PropertyKey key, uint32_t* index);

  Set::Ptr lookupRaw(PropertyKey key) const { return set_.lookup(key); }
#ifdef DEBUG
  Set::Ptr readonlyThreadsafeLookup(PropertyKey key) const {
    return set_.readonlyThreadsafeLookup(key);
  }
#endif

  bool add(JSContext* cx, PropertyKey key, PropMapAndIndex entry) {
    if (!set_.putNew(key, entry)) {
      ReportOutOfMemory(cx);
      return false;
    }
    setCacheEntry(key, entry);
    return true;
  }

  void purgeCache() {
    for (uint32_t i = 0; i < NumCacheEntries; i++) {
      cacheEntries_[i] = CacheEntry();
    }
  }

  void remove(Ptr ptr) {
    set_.remove(ptr);
    purgeCache();
  }

  void replaceEntry(Ptr ptr, PropertyKey key, PropMapAndIndex newEntry) {
    MOZ_ASSERT(*ptr != newEntry);
    set_.replaceKey(ptr, key, newEntry);
    setCacheEntry(key, newEntry);
  }

  void trace(JSTracer* trc);
#ifdef JSGC_HASH_TABLE_CHECKS
  void checkAfterMovingGC();
#endif
};

class PropMap : public gc::TenuredCellWithFlags {
 public:
  // Number of properties that can be stored in each map. This must be small
  // enough so that every index fits in a tagged PropMap* pointer (MapAndIndex).
  static constexpr size_t Capacity = 8;

 protected:
  static_assert(gc::CellFlagBitsReservedForGC == 3,
                "PropMap must reserve enough bits for Cell");

  enum Flags {
    // Set if this is a CompactPropMap.
    IsCompactFlag = 1 << 3,

    // Set if this map has a non-null previous map pointer. Never set for
    // compact maps because they don't have a previous field.
    HasPrevFlag = 1 << 4,

    // Set if this is a DictionaryPropMap.
    IsDictionaryFlag = 1 << 5,

    // Set if this map can have a table. Never set for compact maps. Always set
    // for dictionary maps.
    CanHaveTableFlag = 1 << 6,

    // If set, this SharedPropMap has a SharedChildrenSet. Else it either has no
    // children or a single child. See SharedChildrenPtr. Never set for
    // dictionary maps.
    HasChildrenSetFlag = 1 << 7,

    // If set, this SharedPropMap was once converted to dictionary mode. This is
    // only used for heuristics. Never set for dictionary maps.
    HadDictionaryConversionFlag = 1 << 8,

    // For SharedPropMap this stores the number of previous maps, clamped to
    // NumPreviousMapsMax. This is used for heuristics.
    NumPreviousMapsMax = 0x7f,
    NumPreviousMapsShift = 9,
    NumPreviousMapsMask = NumPreviousMapsMax << NumPreviousMapsShift,
  };

  // Flags word, stored in the cell header. Note that this hides the
  // Cell::flags() method.
  uintptr_t flags() const { return headerFlagsField(); }

 private:
  GCPtr<PropertyKey> keys_[Capacity];

 protected:
  PropMap() = default;

  void initKey(uint32_t index, PropertyKey key) {
    MOZ_ASSERT(index < Capacity);
    keys_[index].init(key);
  }
  void setKey(uint32_t index, PropertyKey key) {
    MOZ_ASSERT(index < Capacity);
    keys_[index] = key;
  }

 public:
  bool isCompact() const { return flags() & IsCompactFlag; }
  bool isLinked() const { return !isCompact(); }
  bool isDictionary() const { return flags() & IsDictionaryFlag; }
  bool isShared() const { return !isDictionary(); }
  bool isNormal() const { return isShared() && !isCompact(); }

  bool hasPrevious() const { return flags() & HasPrevFlag; }
  bool canHaveTable() const { return flags() & CanHaveTableFlag; }

  inline CompactPropMap* asCompact();
  inline const CompactPropMap* asCompact() const;

  inline LinkedPropMap* asLinked();
  inline const LinkedPropMap* asLinked() const;

  inline NormalPropMap* asNormal();
  inline const NormalPropMap* asNormal() const;

  inline SharedPropMap* asShared();
  inline const SharedPropMap* asShared() const;

  inline DictionaryPropMap* asDictionary();
  inline const DictionaryPropMap* asDictionary() const;

  bool hasKey(uint32_t index) const {
    MOZ_ASSERT(index < Capacity);
    return !keys_[index].isVoid();
  }
  PropertyKey getKey(uint32_t index) const {
    MOZ_ASSERT(index < Capacity);
    return keys_[index];
  }

  uint32_t approximateEntryCount() const;

#ifdef DEBUG
  void dump(js::GenericPrinter& out) const;
  void dump() const;
  void checkConsistency(NativeObject* obj) const;
#endif

  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              size_t* children, size_t* tables) const;

  inline PropertyInfo getPropertyInfo(uint32_t index) const;

  PropertyInfoWithKey getPropertyInfoWithKey(uint32_t index) const {
    return PropertyInfoWithKey(getPropertyInfo(index), getKey(index));
  }

  MOZ_ALWAYS_INLINE PropMap* lookupLinear(uint32_t mapLength, PropertyKey key,
                                          uint32_t* index);

  MOZ_ALWAYS_INLINE PropMap* lookupPure(uint32_t mapLength, PropertyKey key,
                                        uint32_t* index);

  MOZ_ALWAYS_INLINE PropMap* lookup(JSContext* cx, uint32_t mapLength,
                                    PropertyKey key, uint32_t* index);

  static inline bool lookupForRemove(JSContext* cx, PropMap* map,
                                     uint32_t mapLength, PropertyKey key,
                                     const AutoKeepPropMapTables& keep,
                                     PropMap** propMap, uint32_t* propIndex,
                                     PropMapTable** table,
                                     PropMapTable::Ptr* ptr);

  static const JS::TraceKind TraceKind = JS::TraceKind::PropMap;

  void traceChildren(JSTracer* trc);
};

class SharedPropMap : public PropMap {
  friend class PropMap;

 protected:
  // Shared maps are stored in a tree structure. Each shared map has a TreeData
  // struct linking the map to its parent and children. Initial maps (the ones
  // stored in ShapeZone's initialPropMaps table) don't have a parent.
  struct TreeData {
    SharedChildrenPtr children;
    SharedPropMapAndIndex parent;

    void setParent(SharedPropMap* map, uint32_t index) {
      parent = SharedPropMapAndIndex(map, index);
    }
  };

 private:
  static SharedPropMap* create(JSContext* cx, Handle<SharedPropMap*> prev,
                               HandleId id, PropertyInfo prop);
  static SharedPropMap* createInitial(JSContext* cx, HandleId id,
                                      PropertyInfo prop);
  static SharedPropMap* clone(JSContext* cx, Handle<SharedPropMap*> map,
                              uint32_t length);

  inline void initProperty(uint32_t index, PropertyKey key, PropertyInfo prop);

  static bool addPropertyInternal(JSContext* cx,
                                  MutableHandle<SharedPropMap*> map,
                                  uint32_t* mapLength, HandleId id,
                                  PropertyInfo prop);

  bool addChild(JSContext* cx, SharedPropMapAndIndex child, HandleId id,
                PropertyInfo prop);
  SharedPropMap* lookupChild(uint32_t length, HandleId id, PropertyInfo prop);

 protected:
  void initNumPreviousMaps(uint32_t value) {
    MOZ_ASSERT((flags() >> NumPreviousMapsShift) == 0);
    // Clamp to NumPreviousMapsMax. This is okay because this value is only used
    // for heuristics.
    if (value > NumPreviousMapsMax) {
      value = NumPreviousMapsMax;
    }
    setHeaderFlagBits(value << NumPreviousMapsShift);
  }

  bool hasChildrenSet() const { return flags() & HasChildrenSetFlag; }
  void setHasChildrenSet() { setHeaderFlagBits(HasChildrenSetFlag); }
  void clearHasChildrenSet() { clearHeaderFlagBits(HasChildrenSetFlag); }

  void setHadDictionaryConversion() {
    setHeaderFlagBits(HadDictionaryConversionFlag);
  }

 public:
  // Heuristics used when adding a property via NativeObject::addProperty and
  // friends:
  //
  // * If numPreviousMaps >= NumPrevMapsForAddConsiderDictionary, consider
  //   converting the object to a dictionary object based on other heuristics.
  //
  // * If numPreviousMaps >= NumPrevMapsForAddAlwaysDictionary, always convert
  //   the object to a dictionary object.
  static constexpr size_t NumPrevMapsConsiderDictionary = 32;
  static constexpr size_t NumPrevMapsAlwaysDictionary = 100;

  static_assert(NumPrevMapsConsiderDictionary < NumPreviousMapsMax);
  static_assert(NumPrevMapsAlwaysDictionary < NumPreviousMapsMax);

  // The number of properties that can definitely be added to an object without
  // triggering dictionary mode conversion in NativeObject::addProperty.
  static constexpr size_t MaxPropsForNonDictionary =
      NumPrevMapsConsiderDictionary * Capacity;

  bool isDictionary() const = delete;
  bool isShared() const = delete;
  SharedPropMap* asShared() = delete;
  const SharedPropMap* asShared() const = delete;

  bool hadDictionaryConversion() const {
    return flags() & HadDictionaryConversionFlag;
  }

  uint32_t numPreviousMaps() const {
    uint32_t val = (flags() & NumPreviousMapsMask) >> NumPreviousMapsShift;
    MOZ_ASSERT_IF(hasPrevious(), val > 0);
    return val;
  }

  MOZ_ALWAYS_INLINE bool shouldConvertToDictionaryForAdd() const;

  void fixupAfterMovingGC();
  inline void sweep(JS::GCContext* gcx);
  inline void finalize(JS::GCContext* gcx);

  static inline void getPrevious(MutableHandle<SharedPropMap*> map,
                                 uint32_t* mapLength);

  bool matchProperty(uint32_t index, PropertyKey key, PropertyInfo prop) const {
    return getKey(index) == key && getPropertyInfo(index) == prop;
  }

  inline TreeData& treeDataRef();
  inline const TreeData& treeDataRef() const;

  void removeChild(JS::GCContext* gcx, SharedPropMap* child);

  uint32_t lastUsedSlot(uint32_t mapLength) const {
    return getPropertyInfo(mapLength - 1).maybeSlot();
  }

  // Number of slots required for objects with this map/mapLength.
  static uint32_t slotSpan(const JSClass* clasp, const SharedPropMap* map,
                           uint32_t mapLength) {
    MOZ_ASSERT(clasp->isNativeObject());
    uint32_t numReserved = JSCLASS_RESERVED_SLOTS(clasp);
    if (!map) {
      MOZ_ASSERT(mapLength == 0);
      return numReserved;
    }
    uint32_t lastSlot = map->lastUsedSlot(mapLength);
    if (lastSlot == SHAPE_INVALID_SLOT) {
      // The object only has custom data properties.
      return numReserved;
    }
    // Some builtin objects store properties in reserved slots. Make sure the
    // slot span >= numReserved. See addPropertyInReservedSlot.
    return std::max(lastSlot + 1, numReserved);
  }

  static uint32_t indexOfNextProperty(uint32_t index) {
    MOZ_ASSERT(index < PropMap::Capacity);
    return (index + 1) % PropMap::Capacity;
  }

  // Add a new property to this map. Returns the new map/mapLength, slot number,
  // and object flags.
  static bool addProperty(JSContext* cx, const JSClass* clasp,
                          MutableHandle<SharedPropMap*> map,
                          uint32_t* mapLength, HandleId id, PropertyFlags flags,
                          ObjectFlags* objectFlags, uint32_t* slot);

  // Like addProperty, but for when the slot number is a reserved slot. A few
  // builtin objects use this for initial properties.
  static bool addPropertyInReservedSlot(JSContext* cx, const JSClass* clasp,
                                        MutableHandle<SharedPropMap*> map,
                                        uint32_t* mapLength, HandleId id,
                                        PropertyFlags flags, uint32_t slot,
                                        ObjectFlags* objectFlags);

  // Like addProperty, but for when the caller already knows the slot number to
  // use (or wants to assert this exact slot number is used).
  static bool addPropertyWithKnownSlot(JSContext* cx, const JSClass* clasp,
                                       MutableHandle<SharedPropMap*> map,
                                       uint32_t* mapLength, HandleId id,
                                       PropertyFlags flags, uint32_t slot,
                                       ObjectFlags* objectFlags);

  // Like addProperty, but for adding a custom data property.
  static bool addCustomDataProperty(JSContext* cx, const JSClass* clasp,
                                    MutableHandle<SharedPropMap*> map,
                                    uint32_t* mapLength, HandleId id,
                                    PropertyFlags flags,
                                    ObjectFlags* objectFlags);

  // Freeze or seal all properties by creating a new shared map. Returns the new
  // map and object flags.
  static bool freezeOrSealProperties(JSContext* cx, IntegrityLevel level,
                                     const JSClass* clasp,
                                     MutableHandle<SharedPropMap*> map,
                                     uint32_t mapLength,
                                     ObjectFlags* objectFlags);

  // Create a new dictionary map as copy of this map.
  static DictionaryPropMap* toDictionaryMap(JSContext* cx,
                                            Handle<SharedPropMap*> map,
                                            uint32_t length);
};

class CompactPropMap final : public SharedPropMap {
  CompactPropertyInfo propInfos_[Capacity];
  TreeData treeData_;

  friend class PropMap;
  friend class SharedPropMap;
  friend class DictionaryPropMap;
  friend class js::gc::CellAllocator;

  CompactPropMap(JS::Handle<PropertyKey> key, PropertyInfo prop) {
    setHeaderFlagBits(IsCompactFlag);
    initProperty(0, key, prop);
  }

  CompactPropMap(JS::Handle<CompactPropMap*> orig, uint32_t length) {
    setHeaderFlagBits(IsCompactFlag);
    for (uint32_t i = 0; i < length; i++) {
      initKey(i, orig->getKey(i));
      propInfos_[i] = orig->propInfos_[i];
    }
  }

  void initProperty(uint32_t index, PropertyKey key, PropertyInfo prop) {
    MOZ_ASSERT(!hasKey(index));
    initKey(index, key);
    propInfos_[index] = CompactPropertyInfo(prop);
  }

  TreeData& treeDataRef() { return treeData_; }
  const TreeData& treeDataRef() const { return treeData_; }

 public:
  bool isDictionary() const = delete;
  bool isShared() const = delete;
  bool isCompact() const = delete;
  bool isNormal() const = delete;
  bool isLinked() const = delete;
  CompactPropMap* asCompact() = delete;
  const CompactPropMap* asCompact() const = delete;

  PropertyInfo getPropertyInfo(uint32_t index) const {
    MOZ_ASSERT(hasKey(index));
    return PropertyInfo(propInfos_[index]);
  }
};

// Layout shared by NormalPropMap and DictionaryPropMap.
class LinkedPropMap final : public PropMap {
  friend class PropMap;
  friend class SharedPropMap;
  friend class NormalPropMap;
  friend class DictionaryPropMap;

  struct Data {
    GCPtr<PropMap*> previous;
    PropMapTable* table = nullptr;
    PropertyInfo propInfos[Capacity];

    explicit Data(PropMap* prev) : previous(prev) {}
  };
  Data data_;

  bool createTable(JSContext* cx);
  void handOffTableTo(LinkedPropMap* next);

 public:
  bool isCompact() const = delete;
  bool isLinked() const = delete;
  LinkedPropMap* asLinked() = delete;
  const LinkedPropMap* asLinked() const = delete;

  PropMap* previous() const { return data_.previous; }

  bool hasTable() const { return data_.table != nullptr; }

  PropMapTable* maybeTable(JS::AutoCheckCannotGC& nogc) const {
    return data_.table;
  }
  PropMapTable* ensureTable(JSContext* cx, const JS::AutoCheckCannotGC& nogc) {
    if (!data_.table && MOZ_UNLIKELY(!createTable(cx))) {
      return nullptr;
    }
    return data_.table;
  }
  PropMapTable* ensureTable(JSContext* cx, const AutoKeepPropMapTables& keep) {
    if (!data_.table && MOZ_UNLIKELY(!createTable(cx))) {
      return nullptr;
    }
    return data_.table;
  }

  void purgeTable(JS::GCContext* gcx);

  void purgeTableCache() {
    if (data_.table) {
      data_.table->purgeCache();
    }
  }

#ifdef DEBUG
  bool canSkipMarkingTable();
#endif

  PropertyInfo getPropertyInfo(uint32_t index) const {
    MOZ_ASSERT(hasKey(index));
    return data_.propInfos[index];
  }
};

class NormalPropMap final : public SharedPropMap {
  friend class PropMap;
  friend class SharedPropMap;
  friend class DictionaryPropMap;
  friend class js::gc::CellAllocator;

  LinkedPropMap::Data linkedData_;
  TreeData treeData_;

  NormalPropMap(JS::Handle<SharedPropMap*> prev, PropertyKey key,
                PropertyInfo prop)
      : linkedData_(prev) {
    if (prev) {
      setHeaderFlagBits(HasPrevFlag);
      initNumPreviousMaps(prev->numPreviousMaps() + 1);
      if (prev->hasPrevious()) {
        setHeaderFlagBits(CanHaveTableFlag);
      }
    }
    initProperty(0, key, prop);
  }

  NormalPropMap(JS::Handle<NormalPropMap*> orig, uint32_t length)
      : linkedData_(orig->previous()) {
    if (orig->hasPrevious()) {
      setHeaderFlagBits(HasPrevFlag);
    }
    if (orig->canHaveTable()) {
      setHeaderFlagBits(CanHaveTableFlag);
    }
    initNumPreviousMaps(orig->numPreviousMaps());
    for (uint32_t i = 0; i < length; i++) {
      initProperty(i, orig->getKey(i), orig->getPropertyInfo(i));
    }
  }

  void initProperty(uint32_t index, PropertyKey key, PropertyInfo prop) {
    MOZ_ASSERT(!hasKey(index));
    initKey(index, key);
    linkedData_.propInfos[index] = prop;
  }

  bool isDictionary() const = delete;
  bool isShared() const = delete;
  bool isCompact() const = delete;
  bool isNormal() const = delete;
  bool isLinked() const = delete;
  NormalPropMap* asNormal() = delete;
  const NormalPropMap* asNormal() const = delete;

  SharedPropMap* previous() const {
    return static_cast<SharedPropMap*>(linkedData_.previous.get());
  }

  TreeData& treeDataRef() { return treeData_; }
  const TreeData& treeDataRef() const { return treeData_; }

  static void staticAsserts() {
    static_assert(offsetof(NormalPropMap, linkedData_) ==
                  offsetof(LinkedPropMap, data_));
  }
};

class DictionaryPropMap final : public PropMap {
  friend class PropMap;
  friend class SharedPropMap;
  friend class js::gc::CellAllocator;

  LinkedPropMap::Data linkedData_;

  // SHAPE_INVALID_SLOT or head of slot freelist in owning dictionary-mode
  // object.
  uint32_t freeList_ = SHAPE_INVALID_SLOT;

  // Number of holes for removed properties in this and previous maps. Used by
  // compacting heuristics.
  uint32_t holeCount_ = 0;

  DictionaryPropMap(JS::Handle<DictionaryPropMap*> prev, PropertyKey key,
                    PropertyInfo prop)
      : linkedData_(prev) {
    setHeaderFlagBits(IsDictionaryFlag | CanHaveTableFlag |
                      (prev ? HasPrevFlag : 0));
    initProperty(0, key, prop);
  }

  DictionaryPropMap(JS::Handle<NormalPropMap*> orig, uint32_t length)
      : linkedData_(nullptr) {
    setHeaderFlagBits(IsDictionaryFlag | CanHaveTableFlag);
    for (uint32_t i = 0; i < length; i++) {
      initProperty(i, orig->getKey(i), orig->getPropertyInfo(i));
    }
  }

  DictionaryPropMap(JS::Handle<CompactPropMap*> orig, uint32_t length)
      : linkedData_(nullptr) {
    setHeaderFlagBits(IsDictionaryFlag | CanHaveTableFlag);
    for (uint32_t i = 0; i < length; i++) {
      initProperty(i, orig->getKey(i), orig->getPropertyInfo(i));
    }
  }

  void initProperty(uint32_t index, PropertyKey key, PropertyInfo prop) {
    MOZ_ASSERT(!hasKey(index));
    initKey(index, key);
    linkedData_.propInfos[index] = prop;
  }

  void initPrevious(DictionaryPropMap* prev) {
    MOZ_ASSERT(prev);
    linkedData_.previous.init(prev);
    setHeaderFlagBits(HasPrevFlag);
  }
  void clearPrevious() {
    linkedData_.previous = nullptr;
    clearHeaderFlagBits(HasPrevFlag);
  }

  void clearProperty(uint32_t index) { setKey(index, PropertyKey::Void()); }

  static void skipTrailingHoles(MutableHandle<DictionaryPropMap*> map,
                                uint32_t* mapLength);

  void handOffLastMapStateTo(DictionaryPropMap* newLast);

  void incHoleCount() { holeCount_++; }
  void decHoleCount() {
    MOZ_ASSERT(holeCount_ > 0);
    holeCount_--;
  }
  static void maybeCompact(JSContext* cx, MutableHandle<DictionaryPropMap*> map,
                           uint32_t* mapLength);

 public:
  bool isDictionary() const = delete;
  bool isShared() const = delete;
  bool isCompact() const = delete;
  bool isNormal() const = delete;
  bool isLinked() const = delete;
  DictionaryPropMap* asDictionary() = delete;
  const DictionaryPropMap* asDictionary() const = delete;

  void fixupAfterMovingGC() {}
  inline void finalize(JS::GCContext* gcx);

  DictionaryPropMap* previous() const {
    return static_cast<DictionaryPropMap*>(linkedData_.previous.get());
  }

  uint32_t freeList() const { return freeList_; }
  void setFreeList(uint32_t slot) { freeList_ = slot; }

  PropertyInfo getPropertyInfo(uint32_t index) const {
    MOZ_ASSERT(hasKey(index));
    return linkedData_.propInfos[index];
  }

  // Add a new property to this map. Returns the new map/mapLength and object
  // flags. The caller is responsible for generating a new dictionary shape.
  static bool addProperty(JSContext* cx, const JSClass* clasp,
                          MutableHandle<DictionaryPropMap*> map,
                          uint32_t* mapLength, HandleId id, PropertyFlags flags,
                          uint32_t slot, ObjectFlags* objectFlags);

  // Remove the property referenced by the table pointer. Returns the new
  // map/mapLength. The caller is responsible for generating a new dictionary
  // shape.
  static void removeProperty(JSContext* cx,
                             MutableHandle<DictionaryPropMap*> map,
                             uint32_t* mapLength, PropMapTable* table,
                             PropMapTable::Ptr& ptr);

  // Turn all sparse elements into dense elements. The caller is responsible
  // for checking all sparse elements are plain data properties and must
  // generate a new shape for the object.
  static void densifyElements(JSContext* cx,
                              MutableHandle<DictionaryPropMap*> map,
                              uint32_t* mapLength, NativeObject* obj);

  // Freeze or seal all properties in this map. Returns the new object flags.
  // The caller is responsible for generating a new shape for the object.
  void freezeOrSealProperties(JSContext* cx, IntegrityLevel level,
                              const JSClass* clasp, uint32_t mapLength,
                              ObjectFlags* objectFlags);

  // Change a property's slot number and/or flags and return the new object
  // flags. The caller is responsible for generating a new shape.
  void changeProperty(JSContext* cx, const JSClass* clasp, uint32_t index,
                      PropertyFlags flags, uint32_t slot,
                      ObjectFlags* objectFlags);

  // Like changeProperty, but doesn't change the slot number.
  void changePropertyFlags(JSContext* cx, const JSClass* clasp, uint32_t index,
                           PropertyFlags flags, ObjectFlags* objectFlags) {
    uint32_t slot = getPropertyInfo(index).maybeSlot();
    changeProperty(cx, clasp, index, flags, slot, objectFlags);
  }

  static void staticAsserts() {
    static_assert(offsetof(DictionaryPropMap, linkedData_) ==
                  offsetof(LinkedPropMap, data_));
  }
};

inline CompactPropMap* PropMap::asCompact() {
  MOZ_ASSERT(isCompact());
  return static_cast<CompactPropMap*>(this);
}
inline const CompactPropMap* PropMap::asCompact() const {
  MOZ_ASSERT(isCompact());
  return static_cast<const CompactPropMap*>(this);
}
inline LinkedPropMap* PropMap::asLinked() {
  MOZ_ASSERT(isLinked());
  return static_cast<LinkedPropMap*>(this);
}
inline const LinkedPropMap* PropMap::asLinked() const {
  MOZ_ASSERT(isLinked());
  return static_cast<const LinkedPropMap*>(this);
}
inline NormalPropMap* PropMap::asNormal() {
  MOZ_ASSERT(isNormal());
  return static_cast<NormalPropMap*>(this);
}
inline const NormalPropMap* PropMap::asNormal() const {
  MOZ_ASSERT(isNormal());
  return static_cast<const NormalPropMap*>(this);
}
inline SharedPropMap* PropMap::asShared() {
  MOZ_ASSERT(isShared());
  return static_cast<SharedPropMap*>(this);
}
inline const SharedPropMap* PropMap::asShared() const {
  MOZ_ASSERT(isShared());
  return static_cast<const SharedPropMap*>(this);
}
inline DictionaryPropMap* PropMap::asDictionary() {
  MOZ_ASSERT(isDictionary());
  return static_cast<DictionaryPropMap*>(this);
}
inline const DictionaryPropMap* PropMap::asDictionary() const {
  MOZ_ASSERT(isDictionary());
  return static_cast<const DictionaryPropMap*>(this);
}

inline PropertyInfo PropMap::getPropertyInfo(uint32_t index) const {
  return isCompact() ? asCompact()->getPropertyInfo(index)
                     : asLinked()->getPropertyInfo(index);
}

inline SharedPropMap::TreeData& SharedPropMap::treeDataRef() {
  return isCompact() ? asCompact()->treeDataRef() : asNormal()->treeDataRef();
}

inline const SharedPropMap::TreeData& SharedPropMap::treeDataRef() const {
  return isCompact() ? asCompact()->treeDataRef() : asNormal()->treeDataRef();
}

inline void SharedPropMap::initProperty(uint32_t index, PropertyKey key,
                                        PropertyInfo prop) {
  if (isCompact()) {
    asCompact()->initProperty(index, key, prop);
  } else {
    asNormal()->initProperty(index, key, prop);
  }
}

template <typename T>
inline PropertyInfo MapAndIndex<T>::propertyInfo() const {
  MOZ_ASSERT(!isNone());
  return map()->getPropertyInfo(index());
}

MOZ_ALWAYS_INLINE HashNumber PropMapTable::Hasher::hash(PropertyKey key) {
  return HashPropertyKey(key);
}
MOZ_ALWAYS_INLINE bool PropMapTable::Hasher::match(PropMapAndIndex entry,
                                                   PropertyKey key) {
  MOZ_ASSERT(entry.map()->hasKey(entry.index()));
  return entry.map()->getKey(entry.index()) == key;
}

// Hash policy for SharedPropMap children.
struct SharedChildrenHasher {
  using Key = SharedPropMapAndIndex;

  struct Lookup {
    PropertyKey key;
    PropertyInfo prop;
    uint8_t index;

    Lookup(PropertyKey key, PropertyInfo prop, uint8_t index)
        : key(key), prop(prop), index(index) {}
    Lookup(PropertyInfoWithKey prop, uint8_t index)
        : key(prop.key()), prop(prop), index(index) {}
  };

  static HashNumber hash(const Lookup& l) {
    HashNumber hash = HashPropertyKey(l.key);
    return mozilla::AddToHash(hash, l.prop.toRaw(), l.index);
  }
  static bool match(SharedPropMapAndIndex k, const Lookup& l) {
    SharedPropMap* map = k.map();
    uint32_t index = k.index();
    uint32_t newIndex = SharedPropMap::indexOfNextProperty(index);
    return index == l.index && map->matchProperty(newIndex, l.key, l.prop);
  }
};

}  // namespace js

// JS::ubi::Nodes can point to PropMaps; they're js::gc::Cell instances
// with no associated compartment.
namespace JS {
namespace ubi {

template <>
class Concrete<js::PropMap> : TracerConcrete<js::PropMap> {
 protected:
  explicit Concrete(js::PropMap* ptr) : TracerConcrete<js::PropMap>(ptr) {}

 public:
  static void construct(void* storage, js::PropMap* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif  // vm_PropMap_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Shape_h
#define vm_Shape_h

#include "js/shadow/Shape.h"  // JS::shadow::Shape, JS::shadow::BaseShape

#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/TemplateLib.h"

#include <algorithm>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "gc/FreeOp.h"
#include "gc/MaybeRooted.h"
#include "gc/Rooting.h"
#include "js/HashTable.h"
#include "js/Id.h"  // JS::PropertyKey
#include "js/MemoryMetrics.h"
#include "js/RootingAPI.h"
#include "js/UbiNode.h"
#include "util/EnumFlags.h"
#include "vm/JSAtom.h"
#include "vm/ObjectFlags.h"
#include "vm/Printer.h"
#include "vm/PropertyInfo.h"
#include "vm/PropertyKey.h"
#include "vm/PropMap.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"

// [SMDOC] Shapes
//
// A Shape represents the layout of an object. It stores and implies:
//
//  * The object's JSClass, Realm, prototype (see BaseShape).
//  * For native objects, the object's properties (PropMap and map length).
//  * The fixed slot capacity of the object (numFixedSlots).
//  * The object's flags (ObjectFlags).
//
// The shape implies the property structure (keys, attributes, property order
// for enumeration) but not the property values. The values are stored in object
// slots.
//
// Every JSObject has a pointer, |shape_|, accessible via shape(), to the
// current shape of the object. This pointer permits fast object layout tests.
//
// There are two kinds of shapes:
//
// * Shared shapes. Either initial shapes (no property map) or SharedPropMap
//   shapes (for native objects with properties).
//
//   These are immutable tuples stored in a hash table, so that objects with the
//   same structure end up with the same shape (this both saves memory and
//   allows JIT optimizations based on this shape).
//
//   To avoid hash table lookups on the hot addProperty path, shapes have a
//   ShapeCachePtr that's used as cache for this. This cache is purged on GC.
//   The shape cache is also used as cache for prototype shapes, to point to the
//   initial shape for objects using that shape.
//
// * Dictionary shapes. Used only for native objects. An object with a
//   dictionary shape is "in dictionary mode". Certain property operations
//   are not supported for shared maps so in these cases we need to convert the
//   object to dictionary mode by creating a dictionary property map and a
//   dictionary shape. An object is converted to dictionary mode in the
//   following cases:
//
//   - Changing a property's flags/attributes and the property is not the last
//     property.
//   - Removing a property other than the object's last property.
//   - The object has many properties. See maybeConvertToDictionaryForAdd for
//     the heuristics.
//   - For prototype objects: when a shadowing property is added to an object
//     with this object on its prototype chain. This is used to invalidate the
//     shape teleporting optimization. See reshapeForShadowedProp.
//
//   Dictionary shapes are unshared, private to a single object, and always have
//   a DictionaryPropMap that's similarly unshared. Dictionary shape mutations
//   do require allocating a new dictionary shape for the object, to properly
//   invalidate JIT inline caches and other shape guards.
//   See NativeObject::generateNewDictionaryShape.
//
// Because many Shapes have similar data, there is actually a secondary type
// called a BaseShape that holds some of a Shape's data (the JSClass, Realm,
// prototype). Many shapes can share a single BaseShape.

MOZ_ALWAYS_INLINE size_t JSSLOT_FREE(const JSClass* clasp) {
  // Proxy classes have reserved slots, but proxies manage their own slot
  // layout.
  MOZ_ASSERT(!clasp->isProxyObject());
  return JSCLASS_RESERVED_SLOTS(clasp);
}

namespace js {

class Shape;

// Hash policy for ShapeCachePtr's ShapeSetForAdd. Maps the new property key and
// flags to the new shape.
struct ShapeForAddHasher : public DefaultHasher<Shape*> {
  using Key = Shape*;

  struct Lookup {
    PropertyKey key;
    PropertyFlags flags;

    Lookup(PropertyKey key, PropertyFlags flags) : key(key), flags(flags) {}
  };

  static MOZ_ALWAYS_INLINE HashNumber hash(const Lookup& l);
  static MOZ_ALWAYS_INLINE bool match(Shape* shape, const Lookup& l);
};
using ShapeSetForAdd = HashSet<Shape*, ShapeForAddHasher, SystemAllocPolicy>;

// Each shape has a cache pointer that's either:
//
// * None
// * For shared shapes, a single shape used to speed up addProperty.
// * For shared shapes, a set of shapes used to speed up addProperty.
// * For prototype shapes, the most recently used initial shape allocated for a
//   prototype object with this shape.
//
// The cache is purely an optimization and is purged on GC (all shapes with a
// non-None ShapeCachePtr are added to a vector in the Zone).
class ShapeCachePtr {
  enum {
    SINGLE_SHAPE_FOR_ADD = 0,
    SHAPE_SET_FOR_ADD = 1,
    SHAPE_WITH_PROTO = 2,
    MASK = 3
  };

  uintptr_t bits = 0;

 public:
  bool isNone() const { return !bits; }
  void setNone() { bits = 0; }

  bool isSingleShapeForAdd() const {
    return (bits & MASK) == SINGLE_SHAPE_FOR_ADD && !isNone();
  }
  Shape* toSingleShapeForAdd() const {
    MOZ_ASSERT(isSingleShapeForAdd());
    return reinterpret_cast<Shape*>(bits & ~uintptr_t(MASK));
  }
  void setSingleShapeForAdd(Shape* shape) {
    MOZ_ASSERT(shape);
    MOZ_ASSERT((uintptr_t(shape) & MASK) == 0);
    MOZ_ASSERT(!isShapeSetForAdd());  // Don't leak the ShapeSet.
    bits = uintptr_t(shape) | SINGLE_SHAPE_FOR_ADD;
  }

  bool isShapeSetForAdd() const { return (bits & MASK) == SHAPE_SET_FOR_ADD; }
  ShapeSetForAdd* toShapeSetForAdd() const {
    MOZ_ASSERT(isShapeSetForAdd());
    return reinterpret_cast<ShapeSetForAdd*>(bits & ~uintptr_t(MASK));
  }
  void setShapeSetForAdd(ShapeSetForAdd* hash) {
    MOZ_ASSERT(hash);
    MOZ_ASSERT((uintptr_t(hash) & MASK) == 0);
    bits = uintptr_t(hash) | SHAPE_SET_FOR_ADD;
  }

  bool isShapeWithProto() const { return (bits & MASK) == SHAPE_WITH_PROTO; }
  Shape* toShapeWithProto() const {
    MOZ_ASSERT(isShapeWithProto());
    return reinterpret_cast<Shape*>(bits & ~uintptr_t(MASK));
  }
  void setShapeWithProto(Shape* shape) {
    MOZ_ASSERT(shape);
    MOZ_ASSERT((uintptr_t(shape) & MASK) == 0);
    MOZ_ASSERT(!isShapeSetForAdd());  // Don't leak the ShapeSet.
    bits = uintptr_t(shape) | SHAPE_WITH_PROTO;
  }
} JS_HAZ_GC_POINTER;

class TenuringTracer;

// BaseShapes store the object's class, realm and prototype. BaseShapes are
// immutable tuples stored in a per-Zone hash table.
class BaseShape : public gc::TenuredCellWithNonGCPointer<const JSClass> {
 public:
  /* Class of referring object, stored in the cell header */
  const JSClass* clasp() const { return headerPtr(); }

 private:
  JS::Realm* realm_;
  GCPtr<TaggedProto> proto_;

  BaseShape(const BaseShape& base) = delete;
  BaseShape& operator=(const BaseShape& other) = delete;

 public:
  void finalize(JSFreeOp* fop) {}

  BaseShape(const JSClass* clasp, JS::Realm* realm, TaggedProto proto);

  /* Not defined: BaseShapes must not be stack allocated. */
  ~BaseShape() = delete;

  JS::Realm* realm() const { return realm_; }
  JS::Compartment* compartment() const {
    return JS::GetCompartmentForRealm(realm());
  }
  JS::Compartment* maybeCompartment() const { return compartment(); }

  TaggedProto proto() const { return proto_; }

  void setRealmForMergeRealms(JS::Realm* realm) { realm_ = realm; }
  void setProtoForMergeRealms(TaggedProto proto) { proto_ = proto; }

  /*
   * Lookup base shapes from the zone's baseShapes table, adding if not
   * already found.
   */
  static BaseShape* get(JSContext* cx, const JSClass* clasp, JS::Realm* realm,
                        Handle<TaggedProto> proto);

  static const JS::TraceKind TraceKind = JS::TraceKind::BaseShape;

  void traceChildren(JSTracer* trc);

  static constexpr size_t offsetOfClasp() { return offsetOfHeaderPtr(); }

  static constexpr size_t offsetOfRealm() {
    return offsetof(BaseShape, realm_);
  }

  static constexpr size_t offsetOfProto() {
    return offsetof(BaseShape, proto_);
  }

 private:
  static void staticAsserts() {
    static_assert(offsetOfClasp() == offsetof(JS::shadow::BaseShape, clasp));
    static_assert(offsetOfRealm() == offsetof(JS::shadow::BaseShape, realm));
    static_assert(sizeof(BaseShape) % gc::CellAlignBytes == 0,
                  "Things inheriting from gc::Cell must have a size that's "
                  "a multiple of gc::CellAlignBytes");
    // Sanity check BaseShape size is what we expect.
#ifdef JS_64BIT
    static_assert(sizeof(BaseShape) == 3 * sizeof(void*));
#else
    static_assert(sizeof(BaseShape) == 4 * sizeof(void*));
#endif
  }
};

class Shape : public gc::CellWithTenuredGCPointer<gc::TenuredCell, BaseShape> {
  friend class ::JSObject;
  friend class ::JSFunction;
  friend class GCMarker;
  friend class NativeObject;
  friend class SharedShape;
  friend class PropertyTree;
  friend class TenuringTracer;
  friend class JS::ubi::Concrete<Shape>;
  friend class js::gc::RelocationOverlay;

 public:
  // Base shape, stored in the cell header.
  BaseShape* base() const { return headerPtr(); }

 protected:
  // Flags that are not modified after the Shape is created. Off-thread Ion
  // compilation can access the immutableFlags word, so we don't want any
  // mutable state here to avoid (TSan) races.
  enum ImmutableFlags : uint32_t {
    // The length associated with the property map. This is a value in the range
    // [0, PropMap::Capacity]. A length of 0 indicates the object is empty (has
    // no properties).
    MAP_LENGTH_MASK = BitMask(4),

    // If set, this is a dictionary shape.
    IS_DICTIONARY = 1 << 4,

    // Number of fixed slots in objects with this shape.
    // FIXED_SLOTS_MAX is the biggest count of fixed slots a Shape can store.
    FIXED_SLOTS_MAX = 0x1f,
    FIXED_SLOTS_SHIFT = 5,
    FIXED_SLOTS_MASK = uint32_t(FIXED_SLOTS_MAX << FIXED_SLOTS_SHIFT),

    // For non-dictionary shapes: the slot span of the object, if it fits in a
    // single byte. If the value is SMALL_SLOTSPAN_MAX, the slot span has to be
    // computed based on the property map (which is slower).
    //
    // Note: NativeObject::addProperty will convert to dictionary mode before we
    // reach this limit, but there are other places where we add properties to
    // shapes, for example environment object shapes.
    SMALL_SLOTSPAN_MAX = 0x3ff,  // 10 bits.
    SMALL_SLOTSPAN_SHIFT = 10,
    SMALL_SLOTSPAN_MASK = uint32_t(SMALL_SLOTSPAN_MAX << SMALL_SLOTSPAN_SHIFT),
  };

  uint32_t immutableFlags;   // Immutable flags, see above.
  ObjectFlags objectFlags_;  // Immutable object flags, see ObjectFlags.

  // The shape's property map. This is either nullptr for shared initial (empty)
  // shapes, a SharedPropMap for SharedPropMap shapes, or a DictionaryPropMap
  // for dictionary shapes.
  GCPtr<PropMap*> propMap_;

  // Cache used to speed up common operations on shapes.
  ShapeCachePtr cache_;

  // Give the object a shape that's similar to its current shape, but with the
  // passed objectFlags, proto, and nfixed values.
  static bool replaceShape(JSContext* cx, HandleObject obj,
                           ObjectFlags objectFlags, TaggedProto proto,
                           uint32_t nfixed);

  void setObjectFlags(ObjectFlags flags) {
    MOZ_ASSERT(isDictionary());
    objectFlags_ = flags;
  }

 public:
  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              JS::ShapeInfo* info) const {
    if (cache_.isShapeSetForAdd()) {
      info->shapesMallocHeapCache +=
          cache_.toShapeSetForAdd()->shallowSizeOfIncludingThis(mallocSizeOf);
    }
  }

  PropMap* propMap() const { return propMap_; }

  ShapeCachePtr& cacheRef() { return cache_; }
  ShapeCachePtr cache() const { return cache_; }

  SharedPropMap* sharedPropMap() const {
    MOZ_ASSERT(!isDictionary());
    return propMap_ ? propMap_->asShared() : nullptr;
  }
  DictionaryPropMap* dictionaryPropMap() const {
    MOZ_ASSERT(isDictionary());
    MOZ_ASSERT(propMap_);
    return propMap_->asDictionary();
  }

  uint32_t propMapLength() const { return immutableFlags & MAP_LENGTH_MASK; }

  PropertyInfoWithKey lastProperty() const {
    MOZ_ASSERT(propMapLength() > 0);
    size_t index = propMapLength() - 1;
    return propMap()->getPropertyInfoWithKey(index);
  }

  MOZ_ALWAYS_INLINE PropMap* lookup(JSContext* cx, PropertyKey key,
                                    uint32_t* index);
  MOZ_ALWAYS_INLINE PropMap* lookupPure(PropertyKey key, uint32_t* index);

  bool lastPropertyMatchesForAdd(PropertyKey key, PropertyFlags flags,
                                 uint32_t* slot) const {
    MOZ_ASSERT(propMapLength() > 0);
    MOZ_ASSERT(!isDictionary());
    uint32_t index = propMapLength() - 1;
    SharedPropMap* map = sharedPropMap();
    if (map->getKey(index) != key) {
      return false;
    }
    PropertyInfo prop = map->getPropertyInfo(index);
    if (prop.flags() != flags) {
      return false;
    }
    *slot = prop.maybeSlot();
    return true;
  }

  const JSClass* getObjectClass() const { return base()->clasp(); }
  JS::Realm* realm() const { return base()->realm(); }

  JS::Compartment* compartment() const { return base()->compartment(); }
  JS::Compartment* maybeCompartment() const {
    return base()->maybeCompartment();
  }

  TaggedProto proto() const { return base()->proto(); }

  ObjectFlags objectFlags() const { return objectFlags_; }
  bool hasObjectFlag(ObjectFlag flag) const {
    return objectFlags_.hasFlag(flag);
  }

 protected:
  Shape(BaseShape* base, ObjectFlags objectFlags, uint32_t nfixed, PropMap* map,
        uint32_t mapLength, bool isDictionary)
      : CellWithTenuredGCPointer(base),
        immutableFlags((isDictionary ? IS_DICTIONARY : 0) |
                       (nfixed << FIXED_SLOTS_SHIFT) | mapLength),
        objectFlags_(objectFlags),
        propMap_(map) {
    MOZ_ASSERT(base);
    MOZ_ASSERT(mapLength <= PropMap::Capacity);
    if (!isDictionary && base->clasp()->isNativeObject()) {
      initSmallSlotSpan();
    }
  }

  Shape(const Shape& other) = delete;

 public:
  bool isDictionary() const { return immutableFlags & IS_DICTIONARY; }

  uint32_t slotSpanSlow() const {
    MOZ_ASSERT(!isDictionary());
    const JSClass* clasp = getObjectClass();
    return SharedPropMap::slotSpan(clasp, sharedPropMap(), propMapLength());
  }

  void initSmallSlotSpan() {
    MOZ_ASSERT(!isDictionary());
    uint32_t slotSpan = slotSpanSlow();
    if (slotSpan > SMALL_SLOTSPAN_MAX) {
      slotSpan = SMALL_SLOTSPAN_MAX;
    }
    MOZ_ASSERT((immutableFlags & SMALL_SLOTSPAN_MASK) == 0);
    immutableFlags |= (slotSpan << SMALL_SLOTSPAN_SHIFT);
  }

  uint32_t slotSpan() const {
    MOZ_ASSERT(!isDictionary());
    MOZ_ASSERT(getObjectClass()->isNativeObject());
    uint32_t span =
        (immutableFlags & SMALL_SLOTSPAN_MASK) >> SMALL_SLOTSPAN_SHIFT;
    if (MOZ_LIKELY(span < SMALL_SLOTSPAN_MAX)) {
      MOZ_ASSERT(slotSpanSlow() == span);
      return span;
    }
    return slotSpanSlow();
  }

  uint32_t numFixedSlots() const {
    return (immutableFlags & FIXED_SLOTS_MASK) >> FIXED_SLOTS_SHIFT;
  }

  void setNumFixedSlots(uint32_t nfixed) {
    MOZ_ASSERT(nfixed < FIXED_SLOTS_MAX);
    immutableFlags = immutableFlags & ~FIXED_SLOTS_MASK;
    immutableFlags = immutableFlags | (nfixed << FIXED_SLOTS_SHIFT);
  }

  void setBase(BaseShape* base) {
    MOZ_ASSERT(base);
    MOZ_ASSERT(isDictionary());
    setHeaderPtr(base);
  }

 public:
#ifdef DEBUG
  void dump(js::GenericPrinter& out) const;
  void dump() const;
#endif

  inline void purgeCache(JSFreeOp* fop);
  inline void finalize(JSFreeOp* fop);

  static const JS::TraceKind TraceKind = JS::TraceKind::Shape;

  void traceChildren(JSTracer* trc);

  // For JIT usage.
  static constexpr size_t offsetOfBaseShape() { return offsetOfHeaderPtr(); }

  static constexpr size_t offsetOfObjectFlags() {
    return offsetof(Shape, objectFlags_);
  }

#ifdef DEBUG
  static inline size_t offsetOfImmutableFlags() {
    return offsetof(Shape, immutableFlags);
  }
  static inline uint32_t fixedSlotsMask() { return FIXED_SLOTS_MASK; }
#endif

 private:
  void updateNewDictionaryShape(ObjectFlags flags, DictionaryPropMap* map,
                                uint32_t mapLength) {
    MOZ_ASSERT(isDictionary());
    objectFlags_ = flags;
    propMap_ = map;
    immutableFlags = (immutableFlags & ~MAP_LENGTH_MASK) | mapLength;
    MOZ_ASSERT(propMapLength() == mapLength);
  }

  static void staticAsserts() {
    static_assert(offsetOfBaseShape() == offsetof(JS::shadow::Shape, base));
    static_assert(offsetof(Shape, immutableFlags) ==
                  offsetof(JS::shadow::Shape, immutableFlags));
    static_assert(FIXED_SLOTS_SHIFT == JS::shadow::Shape::FIXED_SLOTS_SHIFT);
    static_assert(FIXED_SLOTS_MASK == JS::shadow::Shape::FIXED_SLOTS_MASK);
    // Sanity check Shape size is what we expect.
#ifdef JS_64BIT
    static_assert(sizeof(Shape) == 4 * sizeof(void*));
#else
    static_assert(sizeof(Shape) == 6 * sizeof(void*));
#endif
  }
};

class SharedShape : public js::Shape {
  SharedShape(BaseShape* base, ObjectFlags objectFlags, uint32_t nfixed,
              PropMap* map, uint32_t mapLength)
      : Shape(base, objectFlags, nfixed, map, mapLength,
              /* isDictionary = */ false) {}

  static Shape* new_(JSContext* cx, Handle<BaseShape*> base,
                     ObjectFlags objectFlags, uint32_t nfixed,
                     Handle<SharedPropMap*> map, uint32_t mapLength);

 public:
  /*
   * Lookup an initial shape matching the given parameters, creating an empty
   * shape if none was found.
   */
  static Shape* getInitialShape(JSContext* cx, const JSClass* clasp,
                                JS::Realm* realm, TaggedProto proto,
                                size_t nfixed, ObjectFlags objectFlags = {});
  static Shape* getInitialShape(JSContext* cx, const JSClass* clasp,
                                JS::Realm* realm, TaggedProto proto,
                                gc::AllocKind kind,
                                ObjectFlags objectFlags = {});

  static Shape* getPropMapShape(JSContext* cx, BaseShape* base, size_t nfixed,
                                Handle<SharedPropMap*> map, uint32_t mapLength,
                                ObjectFlags objectFlags,
                                bool* allocatedNewShape = nullptr);

  static Shape* getInitialOrPropMapShape(JSContext* cx, const JSClass* clasp,
                                         JS::Realm* realm, TaggedProto proto,
                                         size_t nfixed,
                                         Handle<SharedPropMap*> map,
                                         uint32_t mapLength,
                                         ObjectFlags objectFlags);

  /*
   * Reinsert an alternate initial shape, to be returned by future
   * getInitialShape calls, until the new shape becomes unreachable in a GC
   * and the table entry is purged.
   */
  static void insertInitialShape(JSContext* cx, HandleShape shape);

  /*
   * Some object subclasses are allocated with a built-in set of properties.
   * The first time such an object is created, these built-in properties must
   * be set manually, to compute an initial shape.  Afterward, that initial
   * shape can be reused for newly-created objects that use the subclass's
   * standard prototype.  This method should be used in a post-allocation
   * init method, to ensure that objects of such subclasses compute and cache
   * the initial shape, if it hasn't already been computed.
   */
  template <class ObjectSubclass>
  static inline bool ensureInitialCustomShape(JSContext* cx,
                                              Handle<ObjectSubclass*> obj);
};

class DictionaryShape : public js::Shape {
  DictionaryShape(BaseShape* base, ObjectFlags objectFlags, uint32_t nfixed,
                  PropMap* map, uint32_t mapLength)
      : Shape(base, objectFlags, nfixed, map, mapLength,
              /* isDictionary = */ true) {
    MOZ_ASSERT(map);
  }

 public:
  static Shape* new_(JSContext* cx, Handle<BaseShape*> base,
                     ObjectFlags objectFlags, uint32_t nfixed,
                     Handle<DictionaryPropMap*> map, uint32_t mapLength);
};

// Iterator for iterating over a shape's properties. It can be used like this:
//
//   for (ShapePropertyIter<NoGC> iter(nobj->shape()); !iter.done(); iter++) {
//     PropertyKey key = iter->key();
//     if (iter->isDataProperty() && iter->enumerable()) { .. }
//   }
//
// Properties are iterated in reverse order (i.e., iteration starts at the most
// recently added property).
template <AllowGC allowGC>
class MOZ_RAII ShapePropertyIter {
 protected:
  friend class Shape;

  typename MaybeRooted<PropMap*, allowGC>::RootType map_;
  uint32_t mapLength_;
  const bool isDictionary_;

 public:
  ShapePropertyIter(JSContext* cx, Shape* shape)
      : map_(cx, shape->propMap()),
        mapLength_(shape->propMapLength()),
        isDictionary_(shape->isDictionary()) {
    static_assert(allowGC == CanGC);
    MOZ_ASSERT(shape->getObjectClass()->isNativeObject());
  }

  explicit ShapePropertyIter(Shape* shape)
      : map_(nullptr, shape->propMap()),
        mapLength_(shape->propMapLength()),
        isDictionary_(shape->isDictionary()) {
    static_assert(allowGC == NoGC);
    MOZ_ASSERT(shape->getObjectClass()->isNativeObject());
  }

  bool done() const { return mapLength_ == 0; }

  void operator++(int) {
    do {
      MOZ_ASSERT(!done());
      if (mapLength_ > 1) {
        mapLength_--;
      } else if (map_->hasPrevious()) {
        map_ = map_->asLinked()->previous();
        mapLength_ = PropMap::Capacity;
      } else {
        // Done iterating.
        map_ = nullptr;
        mapLength_ = 0;
        return;
      }
      // Dictionary maps can have "holes" for removed properties, so keep going
      // until we find a non-hole slot.
    } while (MOZ_UNLIKELY(isDictionary_ && !map_->hasKey(mapLength_ - 1)));
  }

  PropertyInfoWithKey get() const {
    MOZ_ASSERT(!done());
    return map_->getPropertyInfoWithKey(mapLength_ - 1);
  }

  PropertyInfoWithKey operator*() const { return get(); }

  // Fake pointer struct to make operator-> work.
  // See https://stackoverflow.com/a/52856349.
  struct FakePtr {
    PropertyInfoWithKey val_;
    const PropertyInfoWithKey* operator->() const { return &val_; }
  };
  FakePtr operator->() const { return {get()}; }
};

}  // namespace js

// JS::ubi::Nodes can point to Shapes and BaseShapes; they're js::gc::Cell
// instances that occupy a compartment.
namespace JS {
namespace ubi {

template <>
class Concrete<js::Shape> : TracerConcrete<js::Shape> {
 protected:
  explicit Concrete(js::Shape* ptr) : TracerConcrete<js::Shape>(ptr) {}

 public:
  static void construct(void* storage, js::Shape* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

template <>
class Concrete<js::BaseShape> : TracerConcrete<js::BaseShape> {
 protected:
  explicit Concrete(js::BaseShape* ptr) : TracerConcrete<js::BaseShape>(ptr) {}

 public:
  static void construct(void* storage, js::BaseShape* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif /* vm_Shape_h */

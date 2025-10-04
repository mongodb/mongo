/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Shape_h
#define vm_Shape_h

#include "js/shadow/Shape.h"  // JS::shadow::Shape, JS::shadow::BaseShape

#include "mozilla/Attributes.h"
#include "mozilla/MemoryReporting.h"

#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "gc/MaybeRooted.h"
#include "js/HashTable.h"
#include "js/Id.h"  // JS::PropertyKey
#include "js/MemoryMetrics.h"
#include "js/Printer.h"  // js::GenericPrinter
#include "js/RootingAPI.h"
#include "js/UbiNode.h"
#include "util/EnumFlags.h"
#include "vm/ObjectFlags.h"
#include "vm/PropertyInfo.h"
#include "vm/PropMap.h"
#include "vm/TaggedProto.h"

// [SMDOC] Shapes
//
// A Shape represents the layout of an object. It stores and implies:
//
//  * The object's JSClass, Realm, prototype (see BaseShape section below).
//  * The object's flags (ObjectFlags).
//  * For native objects, the object's properties (PropMap and map length).
//  * For native objects, the fixed slot capacity of the object (numFixedSlots).
//
// For native objects, the shape implies the property structure (keys,
// attributes, property order for enumeration) but not the property values.
// The values are stored in object slots.
//
// Every JSObject has a pointer, |shape_|, accessible via shape(), to the
// current shape of the object. This pointer permits fast object layout tests.
//
// Shapes use the following C++ class hierarchy:
//
// C++ Type                     Used by
// ============================ ====================================
// Shape (abstract)             JSObject
//  |
//  +-- NativeShape (abstract)  NativeObject
//  |    |
//  |    +-- SharedShape        NativeObject with a shared shape
//  |    |
//  |    +-- DictionaryShape    NativeObject with a dictionary shape
//  |
//  +-- ProxyShape              ProxyObject
//  |
//  +-- WasmGCShape             WasmGCObject
//
// Classes marked with (abstract) above are not literally C++ Abstract Base
// Classes (since there are no virtual functions, pure or not, in this
// hierarchy), but have the same meaning: there are no shapes with this type as
// its most-derived type.
//
// SharedShape
// ===========
// Used only for native objects. This is either an initial shape (no property
// map) or SharedPropMap shape (for objects with at least one property).
//
// These are immutable tuples stored in a hash table, so that objects with the
// same structure end up with the same shape (this both saves memory and allows
// JIT optimizations based on this shape).
//
// To avoid hash table lookups on the hot addProperty path, shapes have a
// ShapeCachePtr that's used as cache for this. This cache is purged on GC.
// The shape cache is also used as cache for prototype shapes, to point to the
// initial shape for objects using that shape, and for cached iterators.
//
// DictionaryShape
// ===============
// Used only for native objects. An object with a dictionary shape is "in
// dictionary mode". Certain property operations are not supported for shared
// maps so in these cases we need to convert the object to dictionary mode by
// creating a dictionary property map and a dictionary shape. An object is
// converted to dictionary mode in the following cases:
//
// - Changing a property's flags/attributes and the property is not the last
//   property.
// - Removing a property other than the object's last property.
// - The object has many properties. See maybeConvertToDictionaryForAdd for the
//   heuristics.
//
// Dictionary shapes are unshared, private to a single object, and always have a
// a DictionaryPropMap that's similarly unshared. Dictionary shape mutations do
// require allocating a new dictionary shape for the object, to properly
// invalidate JIT inline caches and other shape guards.
// See NativeObject::generateNewDictionaryShape.
//
// ProxyShape
// ==========
// Shape used for proxy objects (including wrappers). Proxies with the same
// JSClass, Realm, prototype and ObjectFlags will have the same shape.
//
// WasmGCShape
// ===========
// Shape used for Wasm GC objects. Wasm GC objects with the same JSClass, Realm,
// prototype and ObjectFlags will have the same shape.
//
// BaseShape
// =========
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

class JSONPrinter;
class NativeShape;
class Shape;
class PropertyIteratorObject;

namespace gc {
class TenuringTracer;
}  // namespace gc

namespace wasm {
class RecGroup;
}  // namespace wasm

// Hash policy for ShapeCachePtr's ShapeSetForAdd. Maps the new property key and
// flags to the new shape.
struct ShapeForAddHasher : public DefaultHasher<Shape*> {
  using Key = SharedShape*;

  struct Lookup {
    PropertyKey key;
    PropertyFlags flags;

    Lookup(PropertyKey key, PropertyFlags flags) : key(key), flags(flags) {}
  };

  static MOZ_ALWAYS_INLINE HashNumber hash(const Lookup& l);
  static MOZ_ALWAYS_INLINE bool match(SharedShape* shape, const Lookup& l);
};
using ShapeSetForAdd =
    HashSet<SharedShape*, ShapeForAddHasher, SystemAllocPolicy>;

// Each shape has a cache pointer that's either:
//
// * None
// * For shared shapes, a single shape used to speed up addProperty.
// * For shared shapes, a set of shapes used to speed up addProperty.
// * For prototype shapes, the most recently used initial shape allocated for a
//   prototype object with this shape.
// * For any shape, a PropertyIteratorObject used to speed up GetIterator.
//
// The cache is purely an optimization and is purged on GC (all shapes with a
// non-None ShapeCachePtr are added to a vector in the Zone).
class ShapeCachePtr {
  enum {
    SINGLE_SHAPE_FOR_ADD = 0,
    SHAPE_SET_FOR_ADD = 1,
    SHAPE_WITH_PROTO = 2,
    ITERATOR = 3,
    MASK = 3
  };

  uintptr_t bits = 0;

 public:
  bool isNone() const { return !bits; }
  void setNone() { bits = 0; }

  bool isSingleShapeForAdd() const {
    return (bits & MASK) == SINGLE_SHAPE_FOR_ADD && !isNone();
  }
  SharedShape* toSingleShapeForAdd() const {
    MOZ_ASSERT(isSingleShapeForAdd());
    return reinterpret_cast<SharedShape*>(bits & ~uintptr_t(MASK));
  }
  void setSingleShapeForAdd(SharedShape* shape) {
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

  bool isForAdd() const { return isSingleShapeForAdd() || isShapeSetForAdd(); }

  bool isShapeWithProto() const { return (bits & MASK) == SHAPE_WITH_PROTO; }
  SharedShape* toShapeWithProto() const {
    MOZ_ASSERT(isShapeWithProto());
    return reinterpret_cast<SharedShape*>(bits & ~uintptr_t(MASK));
  }
  void setShapeWithProto(SharedShape* shape) {
    MOZ_ASSERT(shape);
    MOZ_ASSERT((uintptr_t(shape) & MASK) == 0);
    MOZ_ASSERT(!isShapeSetForAdd());  // Don't leak the ShapeSet.
    bits = uintptr_t(shape) | SHAPE_WITH_PROTO;
  }

  bool isIterator() const { return (bits & MASK) == ITERATOR; }
  PropertyIteratorObject* toIterator() const {
    MOZ_ASSERT(isIterator());
    return reinterpret_cast<PropertyIteratorObject*>(bits & ~uintptr_t(MASK));
  }
  void setIterator(PropertyIteratorObject* iter) {
    MOZ_ASSERT(iter);
    MOZ_ASSERT((uintptr_t(iter) & MASK) == 0);
    MOZ_ASSERT(!isShapeSetForAdd());  // Don't leak the ShapeSet.
    bits = uintptr_t(iter) | ITERATOR;
  }
  friend class js::jit::MacroAssembler;
} JS_HAZ_GC_POINTER;

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
  void finalize(JS::GCContext* gcx) {}

  BaseShape(JSContext* cx, const JSClass* clasp, JS::Realm* realm,
            TaggedProto proto);

  /* Not defined: BaseShapes must not be stack allocated. */
  ~BaseShape() = delete;

  JS::Realm* realm() const { return realm_; }
  JS::Compartment* compartment() const {
    return JS::GetCompartmentForRealm(realm());
  }
  JS::Compartment* maybeCompartment() const { return compartment(); }

  TaggedProto proto() const { return proto_; }

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

 public:
#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump() const;
  void dump(js::GenericPrinter& out) const;
  void dump(js::JSONPrinter& json) const;

  void dumpFields(js::JSONPrinter& json) const;
#endif
};

class Shape : public gc::CellWithTenuredGCPointer<gc::TenuredCell, BaseShape> {
  friend class ::JSObject;
  friend class ::JSFunction;
  friend class GCMarker;
  friend class NativeObject;
  friend class SharedShape;
  friend class PropertyTree;
  friend class gc::TenuringTracer;
  friend class JS::ubi::Concrete<Shape>;
  friend class gc::RelocationOverlay;

 public:
  // Base shape, stored in the cell header.
  BaseShape* base() const { return headerPtr(); }

  using Kind = JS::shadow::Shape::Kind;

 protected:
  // Flags that are not modified after the Shape is created. Off-thread Ion
  // compilation can access the immutableFlags word, so we don't want any
  // mutable state here to avoid (TSan) races.
  enum ImmutableFlags : uint32_t {
    // For NativeShape: the length associated with the property map. This is a
    // value in the range [0, PropMap::Capacity]. A length of 0 indicates the
    // object is empty (has no properties).
    MAP_LENGTH_MASK = BitMask(4),

    // The Shape Kind. The NativeObject kinds have the low bit set.
    KIND_SHIFT = 4,
    KIND_MASK = 0b11,
    IS_NATIVE_BIT = 0x1 << KIND_SHIFT,

    // For NativeShape: the number of fixed slots in objects with this shape.
    // FIXED_SLOTS_MAX is the biggest count of fixed slots a Shape can store.
    FIXED_SLOTS_MAX = 0x1f,
    FIXED_SLOTS_SHIFT = 6,
    FIXED_SLOTS_MASK = uint32_t(FIXED_SLOTS_MAX << FIXED_SLOTS_SHIFT),

    // For SharedShape: the slot span of the object, if it fits in a single
    // byte. If the value is SMALL_SLOTSPAN_MAX, the slot span has to be
    // computed based on the property map (which is slower).
    //
    // Note: NativeObject::addProperty will convert to dictionary mode before we
    // reach this limit, but there are other places where we add properties to
    // shapes, for example environment object shapes.
    SMALL_SLOTSPAN_MAX = 0x3ff,  // 10 bits.
    SMALL_SLOTSPAN_SHIFT = 11,
    SMALL_SLOTSPAN_MASK = uint32_t(SMALL_SLOTSPAN_MAX << SMALL_SLOTSPAN_SHIFT),
  };

  uint32_t immutableFlags;   // Immutable flags, see above.
  ObjectFlags objectFlags_;  // Immutable object flags, see ObjectFlags.

  // Cache used to speed up common operations on shapes.
  ShapeCachePtr cache_;

  // Give the object a shape that's similar to its current shape, but with the
  // passed objectFlags, proto, and nfixed values.
  static bool replaceShape(JSContext* cx, HandleObject obj,
                           ObjectFlags objectFlags, TaggedProto proto,
                           uint32_t nfixed);

 public:
  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              JS::ShapeInfo* info) const {
    if (cache_.isShapeSetForAdd()) {
      info->shapesMallocHeapCache +=
          cache_.toShapeSetForAdd()->shallowSizeOfIncludingThis(mallocSizeOf);
    }
  }

  ShapeCachePtr& cacheRef() { return cache_; }
  ShapeCachePtr cache() const { return cache_; }

  void maybeCacheIterator(JSContext* cx, PropertyIteratorObject* iter);

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
  Shape(Kind kind, BaseShape* base, ObjectFlags objectFlags)
      : CellWithTenuredGCPointer(base),
        immutableFlags(uint32_t(kind) << KIND_SHIFT),
        objectFlags_(objectFlags) {
    MOZ_ASSERT(base);
    MOZ_ASSERT(this->kind() == kind, "kind must fit in KIND_MASK");
    MOZ_ASSERT(isNative() == base->clasp()->isNativeObject());
  }

  Shape(const Shape& other) = delete;

 public:
  Kind kind() const { return Kind((immutableFlags >> KIND_SHIFT) & KIND_MASK); }

  bool isNative() const {
    // Note: this is equivalent to `isShared() || isDictionary()`.
    return immutableFlags & IS_NATIVE_BIT;
  }

  bool isShared() const { return kind() == Kind::Shared; }
  bool isDictionary() const { return kind() == Kind::Dictionary; }
  bool isProxy() const { return kind() == Kind::Proxy; }
  bool isWasmGC() const { return kind() == Kind::WasmGC; }

  inline NativeShape& asNative();
  inline SharedShape& asShared();
  inline DictionaryShape& asDictionary();
  inline WasmGCShape& asWasmGC();

  inline const NativeShape& asNative() const;
  inline const SharedShape& asShared() const;
  inline const DictionaryShape& asDictionary() const;
  inline const WasmGCShape& asWasmGC() const;

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump() const;
  void dump(js::GenericPrinter& out) const;
  void dump(js::JSONPrinter& json) const;

  void dumpFields(js::JSONPrinter& json) const;
  void dumpStringContent(js::GenericPrinter& out) const;
#endif

  inline void purgeCache(JS::GCContext* gcx);
  inline void finalize(JS::GCContext* gcx);

  static const JS::TraceKind TraceKind = JS::TraceKind::Shape;

  void traceChildren(JSTracer* trc);

  // For JIT usage.
  static constexpr size_t offsetOfBaseShape() { return offsetOfHeaderPtr(); }

  static constexpr size_t offsetOfObjectFlags() {
    return offsetof(Shape, objectFlags_);
  }

  static inline size_t offsetOfImmutableFlags() {
    return offsetof(Shape, immutableFlags);
  }

  static constexpr uint32_t kindShift() { return KIND_SHIFT; }
  static constexpr uint32_t kindMask() { return KIND_MASK; }
  static constexpr uint32_t isNativeBit() { return IS_NATIVE_BIT; }

  static constexpr size_t offsetOfCachePtr() { return offsetof(Shape, cache_); }

 private:
  static void staticAsserts() {
    static_assert(offsetOfBaseShape() == offsetof(JS::shadow::Shape, base));
    static_assert(offsetof(Shape, immutableFlags) ==
                  offsetof(JS::shadow::Shape, immutableFlags));
    static_assert(KIND_SHIFT == JS::shadow::Shape::KIND_SHIFT);
    static_assert(KIND_MASK == JS::shadow::Shape::KIND_MASK);
    static_assert(FIXED_SLOTS_SHIFT == JS::shadow::Shape::FIXED_SLOTS_SHIFT);
    static_assert(FIXED_SLOTS_MASK == JS::shadow::Shape::FIXED_SLOTS_MASK);
  }
};

// Shared or dictionary shape for a NativeObject.
class NativeShape : public Shape {
 protected:
  // The shape's property map. This is either nullptr (for an
  // initial SharedShape with no properties), a SharedPropMap (for
  // SharedShape) or a DictionaryPropMap (for DictionaryShape).
  GCPtr<PropMap*> propMap_;

  NativeShape(Kind kind, BaseShape* base, ObjectFlags objectFlags,
              uint32_t nfixed, PropMap* map, uint32_t mapLength)
      : Shape(kind, base, objectFlags), propMap_(map) {
    MOZ_ASSERT(base->clasp()->isNativeObject());
    MOZ_ASSERT(mapLength <= PropMap::Capacity);
    immutableFlags |= (nfixed << FIXED_SLOTS_SHIFT) | mapLength;
  }

 public:
  void traceChildren(JSTracer* trc);

  PropMap* propMap() const { return propMap_; }
  uint32_t propMapLength() const { return immutableFlags & MAP_LENGTH_MASK; }

  PropertyInfoWithKey lastProperty() const {
    MOZ_ASSERT(propMapLength() > 0);
    size_t index = propMapLength() - 1;
    return propMap()->getPropertyInfoWithKey(index);
  }

  MOZ_ALWAYS_INLINE PropMap* lookup(JSContext* cx, PropertyKey key,
                                    uint32_t* index);
  MOZ_ALWAYS_INLINE PropMap* lookupPure(PropertyKey key, uint32_t* index);

  uint32_t numFixedSlots() const {
    return (immutableFlags & FIXED_SLOTS_MASK) >> FIXED_SLOTS_SHIFT;
  }

  // For JIT usage.
  static constexpr uint32_t fixedSlotsMask() { return FIXED_SLOTS_MASK; }
  static constexpr uint32_t fixedSlotsShift() { return FIXED_SLOTS_SHIFT; }
};

// Shared shape for a NativeObject.
class SharedShape : public NativeShape {
  friend class js::gc::CellAllocator;
  SharedShape(BaseShape* base, ObjectFlags objectFlags, uint32_t nfixed,
              SharedPropMap* map, uint32_t mapLength)
      : NativeShape(Kind::Shared, base, objectFlags, nfixed, map, mapLength) {
    initSmallSlotSpan();
  }

  static SharedShape* new_(JSContext* cx, Handle<BaseShape*> base,
                           ObjectFlags objectFlags, uint32_t nfixed,
                           Handle<SharedPropMap*> map, uint32_t mapLength);

  void initSmallSlotSpan() {
    MOZ_ASSERT(isShared());
    uint32_t slotSpan = slotSpanSlow();
    if (slotSpan > SMALL_SLOTSPAN_MAX) {
      slotSpan = SMALL_SLOTSPAN_MAX;
    }
    MOZ_ASSERT((immutableFlags & SMALL_SLOTSPAN_MASK) == 0);
    immutableFlags |= (slotSpan << SMALL_SLOTSPAN_SHIFT);
  }

 public:
  SharedPropMap* propMap() const {
    MOZ_ASSERT(isShared());
    return propMap_ ? propMap_->asShared() : nullptr;
  }
  inline SharedPropMap* propMapMaybeForwarded() const;

  bool lastPropertyMatchesForAdd(PropertyKey key, PropertyFlags flags,
                                 uint32_t* slot) const {
    MOZ_ASSERT(isShared());
    MOZ_ASSERT(propMapLength() > 0);
    uint32_t index = propMapLength() - 1;
    SharedPropMap* map = propMap();
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

  uint32_t slotSpanSlow() const {
    MOZ_ASSERT(isShared());
    const JSClass* clasp = getObjectClass();
    return SharedPropMap::slotSpan(clasp, propMap(), propMapLength());
  }
  uint32_t slotSpan() const {
    MOZ_ASSERT(isShared());
    uint32_t span =
        (immutableFlags & SMALL_SLOTSPAN_MASK) >> SMALL_SLOTSPAN_SHIFT;
    if (MOZ_LIKELY(span < SMALL_SLOTSPAN_MAX)) {
      MOZ_ASSERT(slotSpanSlow() == span);
      return span;
    }
    return slotSpanSlow();
  }

  /*
   * Lookup an initial shape matching the given parameters, creating an empty
   * shape if none was found.
   */
  static SharedShape* getInitialShape(JSContext* cx, const JSClass* clasp,
                                      JS::Realm* realm, TaggedProto proto,
                                      size_t nfixed,
                                      ObjectFlags objectFlags = {});
  static SharedShape* getInitialShape(JSContext* cx, const JSClass* clasp,
                                      JS::Realm* realm, TaggedProto proto,
                                      gc::AllocKind kind,
                                      ObjectFlags objectFlags = {});

  static SharedShape* getPropMapShape(JSContext* cx, BaseShape* base,
                                      size_t nfixed, Handle<SharedPropMap*> map,
                                      uint32_t mapLength,
                                      ObjectFlags objectFlags,
                                      bool* allocatedNewShape = nullptr);

  static SharedShape* getInitialOrPropMapShape(
      JSContext* cx, const JSClass* clasp, JS::Realm* realm, TaggedProto proto,
      size_t nfixed, Handle<SharedPropMap*> map, uint32_t mapLength,
      ObjectFlags objectFlags);

  /*
   * Reinsert an alternate initial shape, to be returned by future
   * getInitialShape calls, until the new shape becomes unreachable in a GC
   * and the table entry is purged.
   */
  static void insertInitialShape(JSContext* cx, Handle<SharedShape*> shape);

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

// Dictionary shape for a NativeObject.
class DictionaryShape : public NativeShape {
  friend class ::JSObject;
  friend class js::gc::CellAllocator;
  friend class NativeObject;

  DictionaryShape(BaseShape* base, ObjectFlags objectFlags, uint32_t nfixed,
                  DictionaryPropMap* map, uint32_t mapLength)
      : NativeShape(Kind::Dictionary, base, objectFlags, nfixed, map,
                    mapLength) {
    MOZ_ASSERT(map);
  }
  explicit DictionaryShape(NativeObject* nobj);

  // Methods to set fields of a new dictionary shape. Must not be used for
  // shapes that might have been exposed to script.
  void updateNewShape(ObjectFlags flags, DictionaryPropMap* map,
                      uint32_t mapLength) {
    MOZ_ASSERT(isDictionary());
    objectFlags_ = flags;
    propMap_ = map;
    immutableFlags = (immutableFlags & ~MAP_LENGTH_MASK) | mapLength;
    MOZ_ASSERT(propMapLength() == mapLength);
  }
  void setObjectFlagsOfNewShape(ObjectFlags flags) {
    MOZ_ASSERT(isDictionary());
    objectFlags_ = flags;
  }

 public:
  static DictionaryShape* new_(JSContext* cx, Handle<BaseShape*> base,
                               ObjectFlags objectFlags, uint32_t nfixed,
                               Handle<DictionaryPropMap*> map,
                               uint32_t mapLength);
  static DictionaryShape* new_(JSContext* cx, Handle<NativeObject*> obj);

  DictionaryPropMap* propMap() const {
    MOZ_ASSERT(isDictionary());
    MOZ_ASSERT(propMap_);
    return propMap_->asDictionary();
  }
};

// Shape used for a ProxyObject.
class ProxyShape : public Shape {
  // Needed to maintain the same size as other shapes.
  uintptr_t padding_;

  friend class js::gc::CellAllocator;
  ProxyShape(BaseShape* base, ObjectFlags objectFlags)
      : Shape(Kind::Proxy, base, objectFlags) {
    MOZ_ASSERT(base->clasp()->isProxyObject());
  }

  static ProxyShape* new_(JSContext* cx, Handle<BaseShape*> base,
                          ObjectFlags objectFlags);

 public:
  static ProxyShape* getShape(JSContext* cx, const JSClass* clasp,
                              JS::Realm* realm, TaggedProto proto,
                              ObjectFlags objectFlags);

 private:
  static void staticAsserts() {
    // Silence unused field warning.
    static_assert(sizeof(padding_) == sizeof(uintptr_t));
  }
};

// Shape used for a WasmGCObject.
class WasmGCShape : public Shape {
  // The shape's recursion group.
  const wasm::RecGroup* recGroup_;

  friend class js::gc::CellAllocator;
  WasmGCShape(BaseShape* base, const wasm::RecGroup* recGroup,
              ObjectFlags objectFlags)
      : Shape(Kind::WasmGC, base, objectFlags), recGroup_(recGroup) {
    MOZ_ASSERT(!base->clasp()->isProxyObject());
    MOZ_ASSERT(!base->clasp()->isNativeObject());
  }

  static WasmGCShape* new_(JSContext* cx, Handle<BaseShape*> base,
                           const wasm::RecGroup* recGroup,
                           ObjectFlags objectFlags);

  // Take a reference to the recursion group.
  inline void init();

 public:
  static WasmGCShape* getShape(JSContext* cx, const JSClass* clasp,
                               JS::Realm* realm, TaggedProto proto,
                               const wasm::RecGroup* recGroup,
                               ObjectFlags objectFlags);

  // Release the reference to the recursion group.
  inline void finalize(JS::GCContext* gcx);

  const wasm::RecGroup* recGroup() const {
    MOZ_ASSERT(isWasmGC());
    return recGroup_;
  }
};

// A type that can be used to get the size of the Shape alloc kind.
class SizedShape : public Shape {
  // The various shape kinds have an extra word that is used defined
  // differently depending on the type.
  uintptr_t padding_;

  static void staticAsserts() {
    // Silence unused field warning.
    static_assert(sizeof(padding_) == sizeof(uintptr_t));

    // Sanity check Shape size is what we expect.
#ifdef JS_64BIT
    static_assert(sizeof(SizedShape) == 4 * sizeof(void*));
#else
    static_assert(sizeof(SizedShape) == 6 * sizeof(void*));
#endif

    // All shape kinds must have the same size.
    static_assert(sizeof(NativeShape) == sizeof(SizedShape));
    static_assert(sizeof(SharedShape) == sizeof(SizedShape));
    static_assert(sizeof(DictionaryShape) == sizeof(SizedShape));
    static_assert(sizeof(ProxyShape) == sizeof(SizedShape));
    static_assert(sizeof(WasmGCShape) == sizeof(SizedShape));
  }
};

inline NativeShape& js::Shape::asNative() {
  MOZ_ASSERT(isNative());
  return *static_cast<NativeShape*>(this);
}

inline SharedShape& js::Shape::asShared() {
  MOZ_ASSERT(isShared());
  return *static_cast<SharedShape*>(this);
}

inline DictionaryShape& js::Shape::asDictionary() {
  MOZ_ASSERT(isDictionary());
  return *static_cast<DictionaryShape*>(this);
}

inline WasmGCShape& js::Shape::asWasmGC() {
  MOZ_ASSERT(isWasmGC());
  return *static_cast<WasmGCShape*>(this);
}

inline const NativeShape& js::Shape::asNative() const {
  MOZ_ASSERT(isNative());
  return *static_cast<const NativeShape*>(this);
}

inline const SharedShape& js::Shape::asShared() const {
  MOZ_ASSERT(isShared());
  return *static_cast<const SharedShape*>(this);
}

inline const DictionaryShape& js::Shape::asDictionary() const {
  MOZ_ASSERT(isDictionary());
  return *static_cast<const DictionaryShape*>(this);
}

inline const WasmGCShape& js::Shape::asWasmGC() const {
  MOZ_ASSERT(isWasmGC());
  return *static_cast<const WasmGCShape*>(this);
}

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
  typename MaybeRooted<PropMap*, allowGC>::RootType map_;
  uint32_t mapLength_;
  const bool isDictionary_;

 protected:
  ShapePropertyIter(JSContext* cx, NativeShape* shape, bool isDictionary)
      : map_(cx, shape->propMap()),
        mapLength_(shape->propMapLength()),
        isDictionary_(isDictionary) {
    static_assert(allowGC == CanGC);
    MOZ_ASSERT(shape->isDictionary() == isDictionary);
    MOZ_ASSERT(shape->isNative());
  }
  ShapePropertyIter(NativeShape* shape, bool isDictionary)
      : map_(nullptr, shape->propMap()),
        mapLength_(shape->propMapLength()),
        isDictionary_(isDictionary) {
    static_assert(allowGC == NoGC);
    MOZ_ASSERT(shape->isDictionary() == isDictionary);
    MOZ_ASSERT(shape->isNative());
  }

 public:
  ShapePropertyIter(JSContext* cx, NativeShape* shape)
      : ShapePropertyIter(cx, shape, shape->isDictionary()) {}

  explicit ShapePropertyIter(NativeShape* shape)
      : ShapePropertyIter(shape, shape->isDictionary()) {}

  // Deleted constructors: use SharedShapePropertyIter instead.
  ShapePropertyIter(JSContext* cx, SharedShape* shape) = delete;
  explicit ShapePropertyIter(SharedShape* shape) = delete;

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

// Optimized version of ShapePropertyIter for non-dictionary shapes. It passes
// `false` for `isDictionary_`, which will let the compiler optimize away the
// loop structure in ShapePropertyIter::operator++.
template <AllowGC allowGC>
class MOZ_RAII SharedShapePropertyIter : public ShapePropertyIter<allowGC> {
 public:
  SharedShapePropertyIter(JSContext* cx, SharedShape* shape)
      : ShapePropertyIter<allowGC>(cx, shape, /* isDictionary = */ false) {}

  explicit SharedShapePropertyIter(SharedShape* shape)
      : ShapePropertyIter<allowGC>(shape, /* isDictionary = */ false) {}
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

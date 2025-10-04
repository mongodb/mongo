/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/MapObject.h"

#include "jsapi.h"

#include "ds/OrderedHashTable.h"
#include "gc/GCContext.h"
#include "jit/InlinableNatives.h"
#include "js/MapAndSet.h"
#include "js/PropertyAndElement.h"  // JS_DefineFunctions
#include "js/PropertySpec.h"
#include "js/Utility.h"
#include "vm/BigIntType.h"
#include "vm/EqualityOperations.h"  // js::SameValue
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/SelfHosting.h"
#include "vm/SymbolType.h"

#ifdef ENABLE_RECORD_TUPLE
#  include "vm/RecordType.h"
#  include "vm/TupleType.h"
#endif

#include "gc/GCContext-inl.h"
#include "gc/Marking-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::NumberEqualsInt32;

/*** HashableValue **********************************************************/

static PreBarriered<Value> NormalizeDoubleValue(double d) {
  int32_t i;
  if (NumberEqualsInt32(d, &i)) {
    // Normalize int32_t-valued doubles to int32_t for faster hashing and
    // testing. Note: we use NumberEqualsInt32 here instead of NumberIsInt32
    // because we want -0 and 0 to be normalized to the same thing.
    return Int32Value(i);
  }

  // Normalize the sign bit of a NaN.
  return JS::CanonicalizedDoubleValue(d);
}

bool HashableValue::setValue(JSContext* cx, HandleValue v) {
  if (v.isString()) {
    // Atomize so that hash() and operator==() are fast and infallible.
    JSString* str = AtomizeString(cx, v.toString());
    if (!str) {
      return false;
    }
    value = StringValue(str);
  } else if (v.isDouble()) {
    value = NormalizeDoubleValue(v.toDouble());
#ifdef ENABLE_RECORD_TUPLE
  } else if (v.isExtendedPrimitive()) {
    JSObject& obj = v.toExtendedPrimitive();
    if (obj.is<RecordType>()) {
      if (!obj.as<RecordType>().ensureAtomized(cx)) {
        return false;
      }
    } else {
      MOZ_ASSERT(obj.is<TupleType>());
      if (!obj.as<TupleType>().ensureAtomized(cx)) {
        return false;
      }
    }
    value = v;
#endif
  } else {
    value = v;
  }

  MOZ_ASSERT(value.isUndefined() || value.isNull() || value.isBoolean() ||
             value.isNumber() || value.isString() || value.isSymbol() ||
             value.isObject() || value.isBigInt() ||
             IF_RECORD_TUPLE(value.isExtendedPrimitive(), false));
  return true;
}

static HashNumber HashValue(const Value& v,
                            const mozilla::HashCodeScrambler& hcs) {
  // HashableValue::setValue normalizes values so that the SameValue relation
  // on HashableValues is the same as the == relationship on
  // value.asRawBits(). So why not just return that? Security.
  //
  // To avoid revealing GC of atoms, string-based hash codes are computed
  // from the string contents rather than any pointer; to avoid revealing
  // addresses, pointer-based hash codes are computed using the
  // HashCodeScrambler.

  if (v.isString()) {
    return v.toString()->asAtom().hash();
  }
  if (v.isSymbol()) {
    return v.toSymbol()->hash();
  }
  if (v.isBigInt()) {
    return MaybeForwarded(v.toBigInt())->hash();
  }
#ifdef ENABLE_RECORD_TUPLE
  if (v.isExtendedPrimitive()) {
    JSObject* obj = MaybeForwarded(&v.toExtendedPrimitive());
    auto hasher = [&hcs](const Value& v) {
      return HashValue(
          v.isDouble() ? NormalizeDoubleValue(v.toDouble()).get() : v, hcs);
    };

    if (obj->is<RecordType>()) {
      return obj->as<RecordType>().hash(hasher);
    }
    MOZ_ASSERT(obj->is<TupleType>());
    return obj->as<TupleType>().hash(hasher);
  }
#endif
  if (v.isObject()) {
    return hcs.scramble(v.asRawBits());
  }

  MOZ_ASSERT(!v.isGCThing(), "do not reveal pointers via hash codes");
  return mozilla::HashGeneric(v.asRawBits());
}

HashNumber HashableValue::hash(const mozilla::HashCodeScrambler& hcs) const {
  return HashValue(value, hcs);
}

#ifdef ENABLE_RECORD_TUPLE
inline bool SameExtendedPrimitiveType(const PreBarriered<Value>& a,
                                      const PreBarriered<Value>& b) {
  return a.toExtendedPrimitive().getClass() ==
         b.toExtendedPrimitive().getClass();
}
#endif

bool HashableValue::equals(const HashableValue& other) const {
  // Two HashableValues are equal if they have equal bits.
  bool b = (value.asRawBits() == other.value.asRawBits());

  if (!b && (value.type() == other.value.type())) {
    if (value.isBigInt()) {
      // BigInt values are considered equal if they represent the same
      // mathematical value.
      b = BigInt::equal(value.toBigInt(), other.value.toBigInt());
    }
#ifdef ENABLE_RECORD_TUPLE
    else if (value.isExtendedPrimitive() &&
             SameExtendedPrimitiveType(value, other.value)) {
      b = js::SameValueZeroLinear(value, other.value);
    }
#endif
  }

#ifdef DEBUG
  bool same;
  JSContext* cx = TlsContext.get();
  RootedValue valueRoot(cx, value);
  RootedValue otherRoot(cx, other.value);
  MOZ_ASSERT(SameValueZero(cx, valueRoot, otherRoot, &same));
  MOZ_ASSERT(same == b);
#endif
  return b;
}

/*** MapIterator ************************************************************/

namespace {} /* anonymous namespace */

static const JSClassOps MapIteratorObjectClassOps = {
    nullptr,                      // addProperty
    nullptr,                      // delProperty
    nullptr,                      // enumerate
    nullptr,                      // newEnumerate
    nullptr,                      // resolve
    nullptr,                      // mayResolve
    MapIteratorObject::finalize,  // finalize
    nullptr,                      // call
    nullptr,                      // construct
    nullptr,                      // trace
};

static const ClassExtension MapIteratorObjectClassExtension = {
    MapIteratorObject::objectMoved,  // objectMovedOp
};

const JSClass MapIteratorObject::class_ = {
    "Map Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(MapIteratorObject::SlotCount) |
        JSCLASS_FOREGROUND_FINALIZE | JSCLASS_SKIP_NURSERY_FINALIZE,
    &MapIteratorObjectClassOps, JS_NULL_CLASS_SPEC,
    &MapIteratorObjectClassExtension};

const JSFunctionSpec MapIteratorObject::methods[] = {
    JS_SELF_HOSTED_FN("next", "MapIteratorNext", 0, 0), JS_FS_END};

static inline ValueMap::Range* MapIteratorObjectRange(NativeObject* obj) {
  MOZ_ASSERT(obj->is<MapIteratorObject>());
  return obj->maybePtrFromReservedSlot<ValueMap::Range>(
      MapIteratorObject::RangeSlot);
}

inline MapObject::IteratorKind MapIteratorObject::kind() const {
  int32_t i = getReservedSlot(KindSlot).toInt32();
  MOZ_ASSERT(i == MapObject::Keys || i == MapObject::Values ||
             i == MapObject::Entries);
  return MapObject::IteratorKind(i);
}

/* static */
bool GlobalObject::initMapIteratorProto(JSContext* cx,
                                        Handle<GlobalObject*> global) {
  Rooted<JSObject*> base(
      cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
  if (!base) {
    return false;
  }
  Rooted<PlainObject*> proto(
      cx, GlobalObject::createBlankPrototypeInheriting<PlainObject>(cx, base));
  if (!proto) {
    return false;
  }
  if (!JS_DefineFunctions(cx, proto, MapIteratorObject::methods) ||
      !DefineToStringTag(cx, proto, cx->names().Map_Iterator_)) {
    return false;
  }
  global->initBuiltinProto(ProtoKind::MapIteratorProto, proto);
  return true;
}

template <typename TableObject>
static inline bool HasNurseryMemory(TableObject* t) {
  return t->getReservedSlot(TableObject::HasNurseryMemorySlot).toBoolean();
}

template <typename TableObject>
static inline void SetHasNurseryMemory(TableObject* t, bool b) {
  t->setReservedSlot(TableObject::HasNurseryMemorySlot, JS::BooleanValue(b));
}

MapIteratorObject* MapIteratorObject::create(JSContext* cx, HandleObject obj,
                                             const ValueMap* data,
                                             MapObject::IteratorKind kind) {
  Handle<MapObject*> mapobj(obj.as<MapObject>());
  Rooted<GlobalObject*> global(cx, &mapobj->global());
  Rooted<JSObject*> proto(
      cx, GlobalObject::getOrCreateMapIteratorPrototype(cx, global));
  if (!proto) {
    return nullptr;
  }

  MapIteratorObject* iterobj =
      NewObjectWithGivenProto<MapIteratorObject>(cx, proto);
  if (!iterobj) {
    return nullptr;
  }

  iterobj->init(mapobj, kind);

  constexpr size_t BufferSize =
      RoundUp(sizeof(ValueMap::Range), gc::CellAlignBytes);

  Nursery& nursery = cx->nursery();
  void* buffer =
      nursery.allocateBufferSameLocation(iterobj, BufferSize, js::MallocArena);
  if (!buffer) {
    // Retry with |iterobj| and |buffer| forcibly tenured.
    iterobj = NewTenuredObjectWithGivenProto<MapIteratorObject>(cx, proto);
    if (!iterobj) {
      return nullptr;
    }

    iterobj->init(mapobj, kind);

    buffer = nursery.allocateBufferSameLocation(iterobj, BufferSize,
                                                js::MallocArena);
    if (!buffer) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
  }

  bool insideNursery = IsInsideNursery(iterobj);
  MOZ_ASSERT(insideNursery == nursery.isInside(buffer));

  if (insideNursery && !HasNurseryMemory(mapobj.get())) {
    if (!cx->nursery().addMapWithNurseryMemory(mapobj)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    SetHasNurseryMemory(mapobj.get(), true);
  }

  auto range = data->createRange(buffer, insideNursery);
  iterobj->setReservedSlot(RangeSlot, PrivateValue(range));

  return iterobj;
}

void MapIteratorObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());
  MOZ_ASSERT(!IsInsideNursery(obj));

  auto range = MapIteratorObjectRange(&obj->as<NativeObject>());
  MOZ_ASSERT(!gcx->runtime()->gc.nursery().isInside(range));

  // Bug 1560019: Malloc memory associated with MapIteratorObjects is not
  // currently tracked.
  gcx->deleteUntracked(range);
}

size_t MapIteratorObject::objectMoved(JSObject* obj, JSObject* old) {
  if (!IsInsideNursery(old)) {
    return 0;
  }

  MapIteratorObject* iter = &obj->as<MapIteratorObject>();
  ValueMap::Range* range = MapIteratorObjectRange(iter);
  if (!range) {
    return 0;
  }

  Nursery& nursery = iter->runtimeFromMainThread()->gc.nursery();
  if (!nursery.isInside(range)) {
    nursery.removeMallocedBufferDuringMinorGC(range);
  }

  size_t size = RoundUp(sizeof(ValueMap::Range), gc::CellAlignBytes);
  AutoEnterOOMUnsafeRegion oomUnsafe;
  void* buffer = nursery.allocateBufferSameLocation(obj, size, js::MallocArena);
  if (!buffer) {
    oomUnsafe.crash("MapIteratorObject::objectMoved");
  }

  bool iteratorIsInNursery = IsInsideNursery(obj);
  MOZ_ASSERT(iteratorIsInNursery == nursery.isInside(buffer));
  auto* newRange = new (buffer) ValueMap::Range(*range, iteratorIsInNursery);
  range->~Range();
  iter->setReservedSlot(MapIteratorObject::RangeSlot, PrivateValue(newRange));

  if (iteratorIsInNursery && iter->target()) {
    SetHasNurseryMemory(iter->target(), true);
  }

  return size;
}

MapObject* MapIteratorObject::target() const {
  Value value = getFixedSlot(TargetSlot);
  if (value.isUndefined()) {
    return nullptr;
  }

  return &MaybeForwarded(&value.toObject())->as<MapObject>();
}

template <typename Range>
static void DestroyRange(JSObject* iterator, Range* range) {
  MOZ_ASSERT(IsInsideNursery(iterator) ==
             iterator->runtimeFromMainThread()->gc.nursery().isInside(range));

  range->~Range();
  if (!IsInsideNursery(iterator)) {
    js_free(range);
  }
}

bool MapIteratorObject::next(MapIteratorObject* mapIterator,
                             ArrayObject* resultPairObj) {
  // IC code calls this directly.
  AutoUnsafeCallWithABI unsafe;

  // Check invariants for inlined GetNextMapEntryForIterator.

  // The array should be tenured, so that post-barrier can be done simply.
  MOZ_ASSERT(resultPairObj->isTenured());

  // The array elements should be fixed.
  MOZ_ASSERT(resultPairObj->hasFixedElements());
  MOZ_ASSERT(resultPairObj->getDenseInitializedLength() == 2);
  MOZ_ASSERT(resultPairObj->getDenseCapacity() >= 2);

  ValueMap::Range* range = MapIteratorObjectRange(mapIterator);
  if (!range) {
    return true;
  }

  if (range->empty()) {
    DestroyRange<ValueMap::Range>(mapIterator, range);
    mapIterator->setReservedSlot(RangeSlot, PrivateValue(nullptr));
    return true;
  }

  switch (mapIterator->kind()) {
    case MapObject::Keys:
      resultPairObj->setDenseElement(0, range->front().key.get());
      break;

    case MapObject::Values:
      resultPairObj->setDenseElement(1, range->front().value);
      break;

    case MapObject::Entries: {
      resultPairObj->setDenseElement(0, range->front().key.get());
      resultPairObj->setDenseElement(1, range->front().value);
      break;
    }
  }
  range->popFront();
  return false;
}

/* static */
JSObject* MapIteratorObject::createResultPair(JSContext* cx) {
  Rooted<ArrayObject*> resultPairObj(
      cx, NewDenseFullyAllocatedArray(cx, 2, TenuredObject));
  if (!resultPairObj) {
    return nullptr;
  }

  resultPairObj->setDenseInitializedLength(2);
  resultPairObj->initDenseElement(0, NullValue());
  resultPairObj->initDenseElement(1, NullValue());

  return resultPairObj;
}

/*** Map ********************************************************************/

struct js::UnbarrieredHashPolicy {
  using Lookup = Value;
  static HashNumber hash(const Lookup& v,
                         const mozilla::HashCodeScrambler& hcs) {
    return HashValue(v, hcs);
  }
  static bool match(const Value& k, const Lookup& l) { return k == l; }
  static bool isEmpty(const Value& v) { return v.isMagic(JS_HASH_KEY_EMPTY); }
  static void makeEmpty(Value* vp) { vp->setMagic(JS_HASH_KEY_EMPTY); }
};

// ValueMap, MapObject::UnbarrieredTable and MapObject::PreBarrieredTable must
// all have the same memory layout.
static_assert(sizeof(ValueMap) == sizeof(MapObject::UnbarrieredTable));
static_assert(sizeof(ValueMap::Entry) ==
              sizeof(MapObject::UnbarrieredTable::Entry));
static_assert(sizeof(ValueMap) == sizeof(MapObject::PreBarrieredTable));
static_assert(sizeof(ValueMap::Entry) ==
              sizeof(MapObject::PreBarrieredTable::Entry));

const JSClassOps MapObject::classOps_ = {
    nullptr,   // addProperty
    nullptr,   // delProperty
    nullptr,   // enumerate
    nullptr,   // newEnumerate
    nullptr,   // resolve
    nullptr,   // mayResolve
    finalize,  // finalize
    nullptr,   // call
    nullptr,   // construct
    trace,     // trace
};

const ClassSpec MapObject::classSpec_ = {
    GenericCreateConstructor<MapObject::construct, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<MapObject>,
    MapObject::staticMethods,
    MapObject::staticProperties,
    MapObject::methods,
    MapObject::properties,
    MapObject::finishInit,
};

const JSClass MapObject::class_ = {
    "Map",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(MapObject::SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Map) | JSCLASS_FOREGROUND_FINALIZE |
        JSCLASS_SKIP_NURSERY_FINALIZE,
    &MapObject::classOps_,
    &MapObject::classSpec_,
};

const JSClass MapObject::protoClass_ = {
    "Map.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Map),
    JS_NULL_CLASS_OPS,
    &MapObject::classSpec_,
};

const JSPropertySpec MapObject::properties[] = {
    JS_PSG("size", size, 0),
    JS_STRING_SYM_PS(toStringTag, "Map", JSPROP_READONLY),
    JS_PS_END,
};

const JSFunctionSpec MapObject::methods[] = {
    JS_INLINABLE_FN("get", get, 1, 0, MapGet),
    JS_INLINABLE_FN("has", has, 1, 0, MapHas),
    JS_FN("set", set, 2, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("keys", keys, 0, 0),
    JS_FN("values", values, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "MapForEach", 2, 0),
    JS_FN("entries", entries, 0, 0),
    // @@iterator is re-defined in finishInit so that it has the
    // same identity as |entries|.
    JS_SYM_FN(iterator, entries, 0, 0),
    JS_FS_END,
};

const JSPropertySpec MapObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "$MapSpecies", 0),
    JS_PS_END,
};

const JSFunctionSpec MapObject::staticMethods[] = {
    JS_SELF_HOSTED_FN("groupBy", "MapGroupBy", 2, 0),
    JS_FS_END,
};

/* static */ bool MapObject::finishInit(JSContext* cx, HandleObject ctor,
                                        HandleObject proto) {
  Handle<NativeObject*> nativeProto = proto.as<NativeObject>();

  RootedValue entriesFn(cx);
  RootedId entriesId(cx, NameToId(cx->names().entries));
  if (!NativeGetProperty(cx, nativeProto, entriesId, &entriesFn)) {
    return false;
  }

  // 23.1.3.12 Map.prototype[@@iterator]()
  // The initial value of the @@iterator property is the same function object
  // as the initial value of the "entries" property.
  RootedId iteratorId(cx, PropertyKey::Symbol(cx->wellKnownSymbols().iterator));
  return NativeDefineDataProperty(cx, nativeProto, iteratorId, entriesFn, 0);
}

void MapObject::trace(JSTracer* trc, JSObject* obj) {
  if (ValueMap* map = obj->as<MapObject>().getTableUnchecked()) {
    map->trace(trc);
  }
}

using NurseryKeysVector = GCVector<Value, 0, SystemAllocPolicy>;

template <typename TableObject>
static NurseryKeysVector* GetNurseryKeys(TableObject* t) {
  Value value = t->getReservedSlot(TableObject::NurseryKeysSlot);
  return reinterpret_cast<NurseryKeysVector*>(value.toPrivate());
}

template <typename TableObject>
static NurseryKeysVector* AllocNurseryKeys(TableObject* t) {
  MOZ_ASSERT(!GetNurseryKeys(t));
  auto keys = js_new<NurseryKeysVector>();
  if (!keys) {
    return nullptr;
  }

  t->setReservedSlot(TableObject::NurseryKeysSlot, PrivateValue(keys));
  return keys;
}

template <typename TableObject>
static void DeleteNurseryKeys(TableObject* t) {
  auto keys = GetNurseryKeys(t);
  MOZ_ASSERT(keys);
  js_delete(keys);
  t->setReservedSlot(TableObject::NurseryKeysSlot, PrivateValue(nullptr));
}

// A generic store buffer entry that traces all nursery keys for an ordered hash
// map or set.
template <typename ObjectT>
class js::OrderedHashTableRef : public gc::BufferableRef {
  ObjectT* object;

 public:
  explicit OrderedHashTableRef(ObjectT* obj) : object(obj) {}

  void trace(JSTracer* trc) override {
    MOZ_ASSERT(!IsInsideNursery(object));
    auto realTable = object->getTableUnchecked();
    auto unbarrieredTable =
        reinterpret_cast<typename ObjectT::UnbarrieredTable*>(realTable);
    NurseryKeysVector* keys = GetNurseryKeys(object);
    MOZ_ASSERT(keys);

    keys->mutableEraseIf([&](Value& key) {
      MOZ_ASSERT(
          unbarrieredTable->hash(key) ==
          realTable->hash(*reinterpret_cast<const HashableValue*>(&key)));
      MOZ_ASSERT(IsInsideNursery(key.toGCThing()));

      auto result =
          unbarrieredTable->rekeyOneEntry(key, [trc](const Value& prior) {
            Value key = prior;
            TraceManuallyBarrieredEdge(trc, &key, "ordered hash table key");
            return key;
          });

      if (result.isNothing()) {
        return true;  // Key removed.
      }

      key = result.value();
      return !IsInsideNursery(key.toGCThing());
    });

    if (!keys->empty()) {
      trc->runtime()->gc.storeBuffer().putGeneric(
          OrderedHashTableRef<ObjectT>(object));
      return;
    }

    DeleteNurseryKeys(object);
  }
};

template <typename ObjectT>
[[nodiscard]] inline static bool PostWriteBarrierImpl(ObjectT* obj,
                                                      const Value& keyValue) {
  if (MOZ_LIKELY(!keyValue.hasObjectPayload() && !keyValue.isBigInt())) {
    MOZ_ASSERT_IF(keyValue.isGCThing(), !IsInsideNursery(keyValue.toGCThing()));
    return true;
  }

  if (!IsInsideNursery(keyValue.toGCThing())) {
    return true;
  }

  NurseryKeysVector* keys = GetNurseryKeys(obj);
  if (!keys) {
    keys = AllocNurseryKeys(obj);
    if (!keys) {
      return false;
    }

    keyValue.toGCThing()->storeBuffer()->putGeneric(
        OrderedHashTableRef<ObjectT>(obj));
  }

  return keys->append(keyValue);
}

[[nodiscard]] inline static bool PostWriteBarrier(MapObject* map,
                                                  const Value& key) {
  MOZ_ASSERT(!IsInsideNursery(map));
  return PostWriteBarrierImpl(map, key);
}

[[nodiscard]] inline static bool PostWriteBarrier(SetObject* set,
                                                  const Value& key) {
  if (IsInsideNursery(set)) {
    return true;
  }

  return PostWriteBarrierImpl(set, key);
}

bool MapObject::getKeysAndValuesInterleaved(
    HandleObject obj, JS::MutableHandle<GCVector<JS::Value>> entries) {
  const ValueMap* map = obj->as<MapObject>().getData();
  if (!map) {
    return false;
  }

  for (ValueMap::Range r = map->all(); !r.empty(); r.popFront()) {
    if (!entries.append(r.front().key.get()) ||
        !entries.append(r.front().value)) {
      return false;
    }
  }

  return true;
}

bool MapObject::set(JSContext* cx, HandleObject obj, HandleValue k,
                    HandleValue v) {
  MapObject* mapObject = &obj->as<MapObject>();
  Rooted<HashableValue> key(cx);
  if (!key.setValue(cx, k)) {
    return false;
  }

  return setWithHashableKey(cx, mapObject, key, v);
}

/* static */
inline bool MapObject::setWithHashableKey(JSContext* cx, MapObject* obj,
                                          Handle<HashableValue> key,
                                          Handle<Value> value) {
  ValueMap* table = obj->getTableUnchecked();
  if (!table) {
    return false;
  }

  bool needsPostBarriers = obj->isTenured();
  if (needsPostBarriers) {
    // Use the ValueMap representation which has post barriers.
    if (!PostWriteBarrier(obj, key.get()) || !table->put(key.get(), value)) {
      ReportOutOfMemory(cx);
      return false;
    }
  } else {
    // Use the PreBarrieredTable representation which does not.
    auto* preBarriedTable = reinterpret_cast<PreBarrieredTable*>(table);
    if (!preBarriedTable->put(key.get(), value.get())) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  return true;
}

MapObject* MapObject::create(JSContext* cx,
                             HandleObject proto /* = nullptr */) {
  auto map = cx->make_unique<ValueMap>(cx->zone(),
                                       cx->realm()->randomHashCodeScrambler());
  if (!map) {
    return nullptr;
  }

  if (!map->init()) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  AutoSetNewObjectMetadata metadata(cx);
  MapObject* mapObj = NewObjectWithClassProto<MapObject>(cx, proto);
  if (!mapObj) {
    return nullptr;
  }

  bool insideNursery = IsInsideNursery(mapObj);
  if (insideNursery && !cx->nursery().addMapWithNurseryMemory(mapObj)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  InitReservedSlot(mapObj, DataSlot, map.release(), MemoryUse::MapObjectTable);
  mapObj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
  mapObj->initReservedSlot(HasNurseryMemorySlot,
                           JS::BooleanValue(insideNursery));
  return mapObj;
}

size_t MapObject::sizeOfData(mozilla::MallocSizeOf mallocSizeOf) {
  size_t size = 0;
  if (const ValueMap* map = getData()) {
    size += map->sizeOfIncludingThis(mallocSizeOf);
  }
  if (NurseryKeysVector* nurseryKeys = GetNurseryKeys(this)) {
    size += nurseryKeys->sizeOfIncludingThis(mallocSizeOf);
  }
  return size;
}

void MapObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());
  ValueMap* table = obj->as<MapObject>().getTableUnchecked();
  if (!table) {
    return;
  }

  MOZ_ASSERT_IF(obj->isTenured(), !table->hasNurseryRanges());

  bool needsPostBarriers = obj->isTenured();
  if (needsPostBarriers) {
    // Use the ValueMap representation which has post barriers.
    gcx->delete_(obj, table, MemoryUse::MapObjectTable);
  } else {
    // Use the PreBarrieredTable representation which does not.
    auto* preBarriedTable = reinterpret_cast<PreBarrieredTable*>(table);
    gcx->delete_(obj, preBarriedTable, MemoryUse::MapObjectTable);
  }
}

void MapObject::clearNurseryRangesBeforeMinorGC() {
  getTableUnchecked()->destroyNurseryRanges();
  SetHasNurseryMemory(this, false);
}

/* static */
MapObject* MapObject::sweepAfterMinorGC(JS::GCContext* gcx, MapObject* mapobj) {
  Nursery& nursery = gcx->runtime()->gc.nursery();
  bool wasInCollectedRegion = nursery.inCollectedRegion(mapobj);
  if (wasInCollectedRegion && !IsForwarded(mapobj)) {
    finalize(gcx, mapobj);
    return nullptr;
  }

  mapobj = MaybeForwarded(mapobj);

  bool insideNursery = IsInsideNursery(mapobj);
  if (insideNursery) {
    SetHasNurseryMemory(mapobj, true);
  }

  if (wasInCollectedRegion && mapobj->isTenured()) {
    AddCellMemory(mapobj, sizeof(ValueMap), MemoryUse::MapObjectTable);
  }

  if (!HasNurseryMemory(mapobj)) {
    return nullptr;
  }

  return mapobj;
}

bool MapObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Map");
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Map")) {
    return false;
  }

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Map, &proto)) {
    return false;
  }

  Rooted<MapObject*> obj(cx, MapObject::create(cx, proto));
  if (!obj) {
    return false;
  }

  if (!args.get(0).isNullOrUndefined()) {
    FixedInvokeArgs<1> args2(cx);
    args2[0].set(args[0]);

    RootedValue thisv(cx, ObjectValue(*obj));
    if (!CallSelfHostedFunction(cx, cx->names().MapConstructorInit, thisv,
                                args2, args2.rval())) {
      return false;
    }
  }

  args.rval().setObject(*obj);
  return true;
}

bool MapObject::is(HandleValue v) {
  return v.isObject() && v.toObject().hasClass(&class_) &&
         !v.toObject().as<MapObject>().getReservedSlot(DataSlot).isUndefined();
}

bool MapObject::is(HandleObject o) {
  return o->hasClass(&class_) &&
         !o->as<MapObject>().getReservedSlot(DataSlot).isUndefined();
}

#define ARG0_KEY(cx, args, key)  \
  Rooted<HashableValue> key(cx); \
  if (args.length() > 0 && !key.setValue(cx, args[0])) return false

const ValueMap& MapObject::extract(HandleObject o) {
  MOZ_ASSERT(o->hasClass(&MapObject::class_));
  return *o->as<MapObject>().getData();
}

const ValueMap& MapObject::extract(const CallArgs& args) {
  MOZ_ASSERT(args.thisv().isObject());
  MOZ_ASSERT(args.thisv().toObject().hasClass(&MapObject::class_));
  return *args.thisv().toObject().as<MapObject>().getData();
}

uint32_t MapObject::size(JSContext* cx, HandleObject obj) {
  const ValueMap& map = extract(obj);
  static_assert(sizeof(map.count()) <= sizeof(uint32_t),
                "map count must be precisely representable as a JS number");
  return map.count();
}

bool MapObject::size_impl(JSContext* cx, const CallArgs& args) {
  RootedObject obj(cx, &args.thisv().toObject());
  args.rval().setNumber(size(cx, obj));
  return true;
}

bool MapObject::size(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "size");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::size_impl>(cx, args);
}

bool MapObject::get(JSContext* cx, HandleObject obj, HandleValue key,
                    MutableHandleValue rval) {
  const ValueMap& map = extract(obj);
  Rooted<HashableValue> k(cx);

  if (!k.setValue(cx, key)) {
    return false;
  }

  if (const ValueMap::Entry* p = map.get(k)) {
    rval.set(p->value);
  } else {
    rval.setUndefined();
  }

  return true;
}

bool MapObject::get_impl(JSContext* cx, const CallArgs& args) {
  RootedObject obj(cx, &args.thisv().toObject());
  return get(cx, obj, args.get(0), args.rval());
}

bool MapObject::get(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "get");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::get_impl>(cx, args);
}

bool MapObject::has(JSContext* cx, HandleObject obj, HandleValue key,
                    bool* rval) {
  const ValueMap& map = extract(obj);
  Rooted<HashableValue> k(cx);

  if (!k.setValue(cx, key)) {
    return false;
  }

  *rval = map.has(k);
  return true;
}

bool MapObject::has_impl(JSContext* cx, const CallArgs& args) {
  bool found;
  RootedObject obj(cx, &args.thisv().toObject());
  if (has(cx, obj, args.get(0), &found)) {
    args.rval().setBoolean(found);
    return true;
  }
  return false;
}

bool MapObject::has(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "has");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::has_impl>(cx, args);
}

bool MapObject::set_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(MapObject::is(args.thisv()));

  MapObject* obj = &args.thisv().toObject().as<MapObject>();
  ARG0_KEY(cx, args, key);
  if (!setWithHashableKey(cx, obj, key, args.get(1))) {
    return false;
  }

  args.rval().set(args.thisv());
  return true;
}

bool MapObject::set(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "set");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::set_impl>(cx, args);
}

bool MapObject::delete_(JSContext* cx, HandleObject obj, HandleValue key,
                        bool* rval) {
  MapObject* mapObject = &obj->as<MapObject>();
  Rooted<HashableValue> k(cx);

  if (!k.setValue(cx, key)) {
    return false;
  }

  bool ok;
  if (mapObject->isTenured()) {
    ok = mapObject->tenuredTable()->remove(k, rval);
  } else {
    ok = mapObject->nurseryTable()->remove(k, rval);
  }

  if (!ok) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool MapObject::delete_impl(JSContext* cx, const CallArgs& args) {
  // MapObject::trace does not trace deleted entries. Incremental GC therefore
  // requires that no HeapPtr<Value> objects pointing to heap values be left
  // alive in the ValueMap.
  //
  // OrderedHashMap::remove() doesn't destroy the removed entry. It merely
  // calls OrderedHashMap::MapOps::makeEmpty. But that is sufficient, because
  // makeEmpty clears the value by doing e->value = Value(), and in the case
  // of a ValueMap, Value() means HeapPtr<Value>(), which is the same as
  // HeapPtr<Value>(UndefinedValue()).
  MOZ_ASSERT(MapObject::is(args.thisv()));
  RootedObject obj(cx, &args.thisv().toObject());

  bool found;
  if (!delete_(cx, obj, args.get(0), &found)) {
    return false;
  }

  args.rval().setBoolean(found);
  return true;
}

bool MapObject::delete_(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "delete");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::delete_impl>(cx, args);
}

bool MapObject::iterator(JSContext* cx, IteratorKind kind, HandleObject obj,
                         MutableHandleValue iter) {
  const ValueMap& map = extract(obj);
  Rooted<JSObject*> iterobj(cx, MapIteratorObject::create(cx, obj, &map, kind));
  if (!iterobj) {
    return false;
  }
  iter.setObject(*iterobj);
  return true;
}

bool MapObject::iterator_impl(JSContext* cx, const CallArgs& args,
                              IteratorKind kind) {
  RootedObject obj(cx, &args.thisv().toObject());
  return iterator(cx, kind, obj, args.rval());
}

bool MapObject::keys_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, Keys);
}

bool MapObject::keys(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "keys");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, keys_impl, args);
}

bool MapObject::values_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, Values);
}

bool MapObject::values(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "values");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, values_impl, args);
}

bool MapObject::entries_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, Entries);
}

bool MapObject::entries(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "entries");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, entries_impl, args);
}

bool MapObject::clear_impl(JSContext* cx, const CallArgs& args) {
  RootedObject obj(cx, &args.thisv().toObject());
  args.rval().setUndefined();
  return clear(cx, obj);
}

bool MapObject::clear(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "clear");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, clear_impl, args);
}

bool MapObject::clear(JSContext* cx, HandleObject obj) {
  MapObject* mapObject = &obj->as<MapObject>();

  bool ok;
  if (mapObject->isTenured()) {
    ok = mapObject->tenuredTable()->clear();
  } else {
    ok = mapObject->nurseryTable()->clear();
  }

  if (!ok) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

/*** SetIterator ************************************************************/

static const JSClassOps SetIteratorObjectClassOps = {
    nullptr,                      // addProperty
    nullptr,                      // delProperty
    nullptr,                      // enumerate
    nullptr,                      // newEnumerate
    nullptr,                      // resolve
    nullptr,                      // mayResolve
    SetIteratorObject::finalize,  // finalize
    nullptr,                      // call
    nullptr,                      // construct
    nullptr,                      // trace
};

static const ClassExtension SetIteratorObjectClassExtension = {
    SetIteratorObject::objectMoved,  // objectMovedOp
};

const JSClass SetIteratorObject::class_ = {
    "Set Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(SetIteratorObject::SlotCount) |
        JSCLASS_FOREGROUND_FINALIZE | JSCLASS_SKIP_NURSERY_FINALIZE,
    &SetIteratorObjectClassOps, JS_NULL_CLASS_SPEC,
    &SetIteratorObjectClassExtension};

const JSFunctionSpec SetIteratorObject::methods[] = {
    JS_SELF_HOSTED_FN("next", "SetIteratorNext", 0, 0), JS_FS_END};

static inline ValueSet::Range* SetIteratorObjectRange(NativeObject* obj) {
  MOZ_ASSERT(obj->is<SetIteratorObject>());
  return obj->maybePtrFromReservedSlot<ValueSet::Range>(
      SetIteratorObject::RangeSlot);
}

inline SetObject::IteratorKind SetIteratorObject::kind() const {
  int32_t i = getReservedSlot(KindSlot).toInt32();
  MOZ_ASSERT(i == SetObject::Values || i == SetObject::Entries);
  return SetObject::IteratorKind(i);
}

/* static */
bool GlobalObject::initSetIteratorProto(JSContext* cx,
                                        Handle<GlobalObject*> global) {
  Rooted<JSObject*> base(
      cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
  if (!base) {
    return false;
  }
  Rooted<PlainObject*> proto(
      cx, GlobalObject::createBlankPrototypeInheriting<PlainObject>(cx, base));
  if (!proto) {
    return false;
  }
  if (!JS_DefineFunctions(cx, proto, SetIteratorObject::methods) ||
      !DefineToStringTag(cx, proto, cx->names().Set_Iterator_)) {
    return false;
  }
  global->initBuiltinProto(ProtoKind::SetIteratorProto, proto);
  return true;
}

SetIteratorObject* SetIteratorObject::create(JSContext* cx, HandleObject obj,
                                             ValueSet* data,
                                             SetObject::IteratorKind kind) {
  MOZ_ASSERT(kind != SetObject::Keys);

  Handle<SetObject*> setobj(obj.as<SetObject>());
  Rooted<GlobalObject*> global(cx, &setobj->global());
  Rooted<JSObject*> proto(
      cx, GlobalObject::getOrCreateSetIteratorPrototype(cx, global));
  if (!proto) {
    return nullptr;
  }

  SetIteratorObject* iterobj =
      NewObjectWithGivenProto<SetIteratorObject>(cx, proto);
  if (!iterobj) {
    return nullptr;
  }

  iterobj->init(setobj, kind);

  constexpr size_t BufferSize =
      RoundUp(sizeof(ValueSet::Range), gc::CellAlignBytes);

  Nursery& nursery = cx->nursery();
  void* buffer =
      nursery.allocateBufferSameLocation(iterobj, BufferSize, js::MallocArena);
  if (!buffer) {
    // Retry with |iterobj| and |buffer| forcibly tenured.
    iterobj = NewTenuredObjectWithGivenProto<SetIteratorObject>(cx, proto);
    if (!iterobj) {
      return nullptr;
    }

    iterobj->init(setobj, kind);

    buffer = nursery.allocateBufferSameLocation(iterobj, BufferSize,
                                                js::MallocArena);
    if (!buffer) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
  }

  bool insideNursery = IsInsideNursery(iterobj);
  MOZ_ASSERT(insideNursery == nursery.isInside(buffer));

  if (insideNursery && !HasNurseryMemory(setobj.get())) {
    if (!cx->nursery().addSetWithNurseryMemory(setobj)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    SetHasNurseryMemory(setobj.get(), true);
  }

  auto range = data->createRange(buffer, insideNursery);
  iterobj->setReservedSlot(RangeSlot, PrivateValue(range));

  return iterobj;
}

void SetIteratorObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());
  MOZ_ASSERT(!IsInsideNursery(obj));

  auto range = SetIteratorObjectRange(&obj->as<NativeObject>());
  MOZ_ASSERT(!gcx->runtime()->gc.nursery().isInside(range));

  // Bug 1560019: Malloc memory associated with SetIteratorObjects is not
  // currently tracked.
  gcx->deleteUntracked(range);
}

size_t SetIteratorObject::objectMoved(JSObject* obj, JSObject* old) {
  if (!IsInsideNursery(old)) {
    return 0;
  }

  SetIteratorObject* iter = &obj->as<SetIteratorObject>();
  ValueSet::Range* range = SetIteratorObjectRange(iter);
  if (!range) {
    return 0;
  }

  Nursery& nursery = iter->runtimeFromMainThread()->gc.nursery();
  if (!nursery.isInside(range)) {
    nursery.removeMallocedBufferDuringMinorGC(range);
  }

  size_t size = RoundUp(sizeof(ValueSet::Range), gc::CellAlignBytes);
  ;
  AutoEnterOOMUnsafeRegion oomUnsafe;
  void* buffer = nursery.allocateBufferSameLocation(obj, size, js::MallocArena);
  if (!buffer) {
    oomUnsafe.crash("SetIteratorObject::objectMoved");
  }

  bool iteratorIsInNursery = IsInsideNursery(obj);
  MOZ_ASSERT(iteratorIsInNursery == nursery.isInside(buffer));
  auto* newRange = new (buffer) ValueSet::Range(*range, iteratorIsInNursery);
  range->~Range();
  iter->setReservedSlot(SetIteratorObject::RangeSlot, PrivateValue(newRange));

  if (iteratorIsInNursery && iter->target()) {
    SetHasNurseryMemory(iter->target(), true);
  }

  return size;
}

SetObject* SetIteratorObject::target() const {
  Value value = getFixedSlot(TargetSlot);
  if (value.isUndefined()) {
    return nullptr;
  }

  return &MaybeForwarded(&value.toObject())->as<SetObject>();
}

bool SetIteratorObject::next(SetIteratorObject* setIterator,
                             ArrayObject* resultObj) {
  // IC code calls this directly.
  AutoUnsafeCallWithABI unsafe;

  // Check invariants for inlined _GetNextSetEntryForIterator.

  // The array should be tenured, so that post-barrier can be done simply.
  MOZ_ASSERT(resultObj->isTenured());

  // The array elements should be fixed.
  MOZ_ASSERT(resultObj->hasFixedElements());
  MOZ_ASSERT(resultObj->getDenseInitializedLength() == 1);
  MOZ_ASSERT(resultObj->getDenseCapacity() >= 1);

  ValueSet::Range* range = SetIteratorObjectRange(setIterator);
  if (!range) {
    return true;
  }

  if (range->empty()) {
    DestroyRange<ValueSet::Range>(setIterator, range);
    setIterator->setReservedSlot(RangeSlot, PrivateValue(nullptr));
    return true;
  }

  resultObj->setDenseElement(0, range->front().get());
  range->popFront();
  return false;
}

/* static */
JSObject* SetIteratorObject::createResult(JSContext* cx) {
  Rooted<ArrayObject*> resultObj(
      cx, NewDenseFullyAllocatedArray(cx, 1, TenuredObject));
  if (!resultObj) {
    return nullptr;
  }

  resultObj->setDenseInitializedLength(1);
  resultObj->initDenseElement(0, NullValue());

  return resultObj;
}

/*** Set ********************************************************************/

const JSClassOps SetObject::classOps_ = {
    nullptr,   // addProperty
    nullptr,   // delProperty
    nullptr,   // enumerate
    nullptr,   // newEnumerate
    nullptr,   // resolve
    nullptr,   // mayResolve
    finalize,  // finalize
    nullptr,   // call
    nullptr,   // construct
    trace,     // trace
};

const ClassSpec SetObject::classSpec_ = {
    GenericCreateConstructor<SetObject::construct, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<SetObject>,
    nullptr,
    SetObject::staticProperties,
    SetObject::methods,
    SetObject::properties,
    SetObject::finishInit,
};

const JSClass SetObject::class_ = {
    "Set",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(SetObject::SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Set) | JSCLASS_FOREGROUND_FINALIZE |
        JSCLASS_SKIP_NURSERY_FINALIZE,
    &SetObject::classOps_,
    &SetObject::classSpec_,
};

const JSClass SetObject::protoClass_ = {
    "Set.prototype",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Set),
    JS_NULL_CLASS_OPS,
    &SetObject::classSpec_,
};

const JSPropertySpec SetObject::properties[] = {
    JS_PSG("size", size, 0),
    JS_STRING_SYM_PS(toStringTag, "Set", JSPROP_READONLY),
    JS_PS_END,
};

const JSFunctionSpec SetObject::methods[] = {
    JS_INLINABLE_FN("has", has, 1, 0, SetHas),
    JS_FN("add", add, 1, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("entries", entries, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "SetForEach", 2, 0),
    JS_SELF_HOSTED_FN("union", "SetUnion", 1, 0),
    JS_SELF_HOSTED_FN("difference", "SetDifference", 1, 0),
    JS_SELF_HOSTED_FN("intersection", "SetIntersection", 1, 0),
    JS_SELF_HOSTED_FN("symmetricDifference", "SetSymmetricDifference", 1, 0),
    JS_SELF_HOSTED_FN("isSubsetOf", "SetIsSubsetOf", 1, 0),
    JS_SELF_HOSTED_FN("isSupersetOf", "SetIsSupersetOf", 1, 0),
    JS_SELF_HOSTED_FN("isDisjointFrom", "SetIsDisjointFrom", 1, 0),
    JS_FN("values", values, 0, 0),
    // @@iterator and |keys| re-defined in finishInit so that they have the
    // same identity as |values|.
    JS_FN("keys", values, 0, 0),
    JS_SYM_FN(iterator, values, 0, 0),
    JS_FS_END,
};
// clang-format on

const JSPropertySpec SetObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "$SetSpecies", 0),
    JS_PS_END,
};

/* static */ bool SetObject::finishInit(JSContext* cx, HandleObject ctor,
                                        HandleObject proto) {
  Handle<NativeObject*> nativeProto = proto.as<NativeObject>();

  RootedValue valuesFn(cx);
  RootedId valuesId(cx, NameToId(cx->names().values));
  if (!NativeGetProperty(cx, nativeProto, valuesId, &valuesFn)) {
    return false;
  }

  // 23.2.3.8 Set.prototype.keys()
  // The initial value of the "keys" property is the same function object
  // as the initial value of the "values" property.
  RootedId keysId(cx, NameToId(cx->names().keys));
  if (!NativeDefineDataProperty(cx, nativeProto, keysId, valuesFn, 0)) {
    return false;
  }

  // 23.2.3.11 Set.prototype[@@iterator]()
  // See above.
  RootedId iteratorId(cx, PropertyKey::Symbol(cx->wellKnownSymbols().iterator));
  return NativeDefineDataProperty(cx, nativeProto, iteratorId, valuesFn, 0);
}

bool SetObject::keys(JSContext* cx, HandleObject obj,
                     JS::MutableHandle<GCVector<JS::Value>> keys) {
  ValueSet* set = obj->as<SetObject>().getData();
  if (!set) {
    return false;
  }

  for (ValueSet::Range r = set->all(); !r.empty(); r.popFront()) {
    if (!keys.append(r.front().get())) {
      return false;
    }
  }

  return true;
}

bool SetObject::add(JSContext* cx, HandleObject obj, HandleValue k) {
  ValueSet* set = obj->as<SetObject>().getData();
  if (!set) {
    return false;
  }

  Rooted<HashableValue> key(cx);
  if (!key.setValue(cx, k)) {
    return false;
  }

  if (!PostWriteBarrier(&obj->as<SetObject>(), key.get()) ||
      !set->put(key.get())) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

SetObject* SetObject::create(JSContext* cx,
                             HandleObject proto /* = nullptr */) {
  auto set = cx->make_unique<ValueSet>(cx->zone(),
                                       cx->realm()->randomHashCodeScrambler());
  if (!set) {
    return nullptr;
  }

  if (!set->init()) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  AutoSetNewObjectMetadata metadata(cx);
  SetObject* obj = NewObjectWithClassProto<SetObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  bool insideNursery = IsInsideNursery(obj);
  if (insideNursery && !cx->nursery().addSetWithNurseryMemory(obj)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  InitReservedSlot(obj, DataSlot, set.release(), MemoryUse::MapObjectTable);
  obj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
  obj->initReservedSlot(HasNurseryMemorySlot, JS::BooleanValue(insideNursery));
  return obj;
}

void SetObject::trace(JSTracer* trc, JSObject* obj) {
  SetObject* setobj = static_cast<SetObject*>(obj);
  if (ValueSet* set = setobj->getData()) {
    set->trace(trc);
  }
}

size_t SetObject::sizeOfData(mozilla::MallocSizeOf mallocSizeOf) {
  size_t size = 0;
  if (ValueSet* set = getData()) {
    size += set->sizeOfIncludingThis(mallocSizeOf);
  }
  if (NurseryKeysVector* nurseryKeys = GetNurseryKeys(this)) {
    size += nurseryKeys->sizeOfIncludingThis(mallocSizeOf);
  }
  return size;
}

void SetObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());
  SetObject* setobj = static_cast<SetObject*>(obj);
  if (ValueSet* set = setobj->getData()) {
    MOZ_ASSERT_IF(obj->isTenured(), !set->hasNurseryRanges());
    gcx->delete_(obj, set, MemoryUse::MapObjectTable);
  }
}

void SetObject::clearNurseryRangesBeforeMinorGC() {
  getTableUnchecked()->destroyNurseryRanges();
  SetHasNurseryMemory(this, false);
}

/* static */
SetObject* SetObject::sweepAfterMinorGC(JS::GCContext* gcx, SetObject* setobj) {
  Nursery& nursery = gcx->runtime()->gc.nursery();
  bool wasInCollectedRegion = nursery.inCollectedRegion(setobj);
  if (wasInCollectedRegion && !IsForwarded(setobj)) {
    finalize(gcx, setobj);
    return nullptr;
  }

  setobj = MaybeForwarded(setobj);

  bool insideNursery = IsInsideNursery(setobj);
  if (insideNursery) {
    SetHasNurseryMemory(setobj, true);
  }

  if (wasInCollectedRegion && setobj->isTenured()) {
    AddCellMemory(setobj, sizeof(ValueSet), MemoryUse::MapObjectTable);
  }

  if (!HasNurseryMemory(setobj)) {
    return nullptr;
  }

  return setobj;
}

bool SetObject::isBuiltinAdd(HandleValue add) {
  return IsNativeFunction(add, SetObject::add);
}

bool SetObject::construct(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Set");
  CallArgs args = CallArgsFromVp(argc, vp);

  if (!ThrowIfNotConstructing(cx, args, "Set")) {
    return false;
  }

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Set, &proto)) {
    return false;
  }

  Rooted<SetObject*> obj(cx, SetObject::create(cx, proto));
  if (!obj) {
    return false;
  }

  if (!args.get(0).isNullOrUndefined()) {
    RootedValue iterable(cx, args[0]);
    bool optimized = false;
    if (!IsOptimizableInitForSet<GlobalObject::getOrCreateSetPrototype,
                                 isBuiltinAdd>(cx, obj, iterable, &optimized)) {
      return false;
    }

    if (optimized) {
      RootedValue keyVal(cx);
      Rooted<HashableValue> key(cx);
      ValueSet* set = obj->getData();
      Rooted<ArrayObject*> array(cx, &iterable.toObject().as<ArrayObject>());
      for (uint32_t index = 0; index < array->getDenseInitializedLength();
           ++index) {
        keyVal.set(array->getDenseElement(index));
        MOZ_ASSERT(!keyVal.isMagic(JS_ELEMENTS_HOLE));

        if (!key.setValue(cx, keyVal)) {
          return false;
        }
        if (!PostWriteBarrier(obj, key.get()) || !set->put(key.get())) {
          ReportOutOfMemory(cx);
          return false;
        }
      }
    } else {
      FixedInvokeArgs<1> args2(cx);
      args2[0].set(args[0]);

      RootedValue thisv(cx, ObjectValue(*obj));
      if (!CallSelfHostedFunction(cx, cx->names().SetConstructorInit, thisv,
                                  args2, args2.rval())) {
        return false;
      }
    }
  }

  args.rval().setObject(*obj);
  return true;
}

bool SetObject::is(HandleValue v) {
  return v.isObject() && v.toObject().hasClass(&class_) &&
         !v.toObject().as<SetObject>().getReservedSlot(DataSlot).isUndefined();
}

bool SetObject::is(HandleObject o) {
  return o->hasClass(&class_) &&
         !o->as<SetObject>().getReservedSlot(DataSlot).isUndefined();
}

ValueSet& SetObject::extract(HandleObject o) {
  MOZ_ASSERT(o->hasClass(&SetObject::class_));
  return *o->as<SetObject>().getData();
}

ValueSet& SetObject::extract(const CallArgs& args) {
  MOZ_ASSERT(args.thisv().isObject());
  MOZ_ASSERT(args.thisv().toObject().hasClass(&SetObject::class_));
  return *static_cast<SetObject&>(args.thisv().toObject()).getData();
}

uint32_t SetObject::size(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(SetObject::is(obj));
  ValueSet& set = extract(obj);
  static_assert(sizeof(set.count()) <= sizeof(uint32_t),
                "set count must be precisely representable as a JS number");
  return set.count();
}

bool SetObject::size_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  ValueSet& set = extract(args);
  static_assert(sizeof(set.count()) <= sizeof(uint32_t),
                "set count must be precisely representable as a JS number");
  args.rval().setNumber(set.count());
  return true;
}

bool SetObject::size(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "size");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<SetObject::is, SetObject::size_impl>(cx, args);
}

bool SetObject::has_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  ValueSet& set = extract(args);
  ARG0_KEY(cx, args, key);
  args.rval().setBoolean(set.has(key));
  return true;
}

bool SetObject::has(JSContext* cx, HandleObject obj, HandleValue key,
                    bool* rval) {
  MOZ_ASSERT(SetObject::is(obj));

  ValueSet& set = extract(obj);
  Rooted<HashableValue> k(cx);

  if (!k.setValue(cx, key)) {
    return false;
  }

  *rval = set.has(k);
  return true;
}

bool SetObject::has(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "has");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<SetObject::is, SetObject::has_impl>(cx, args);
}

bool SetObject::add_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  ValueSet& set = extract(args);
  ARG0_KEY(cx, args, key);
  if (!PostWriteBarrier(&args.thisv().toObject().as<SetObject>(), key.get()) ||
      !set.put(key.get())) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().set(args.thisv());
  return true;
}

bool SetObject::add(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "add");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<SetObject::is, SetObject::add_impl>(cx, args);
}

bool SetObject::delete_(JSContext* cx, HandleObject obj, HandleValue key,
                        bool* rval) {
  MOZ_ASSERT(SetObject::is(obj));

  ValueSet& set = extract(obj);
  Rooted<HashableValue> k(cx);

  if (!k.setValue(cx, key)) {
    return false;
  }

  if (!set.remove(k, rval)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

bool SetObject::delete_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(is(args.thisv()));

  ValueSet& set = extract(args);
  ARG0_KEY(cx, args, key);
  bool found;
  if (!set.remove(key, &found)) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().setBoolean(found);
  return true;
}

bool SetObject::delete_(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "delete");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<SetObject::is, SetObject::delete_impl>(cx, args);
}

bool SetObject::iterator(JSContext* cx, IteratorKind kind, HandleObject obj,
                         MutableHandleValue iter) {
  MOZ_ASSERT(SetObject::is(obj));
  ValueSet& set = extract(obj);
  Rooted<JSObject*> iterobj(cx, SetIteratorObject::create(cx, obj, &set, kind));
  if (!iterobj) {
    return false;
  }
  iter.setObject(*iterobj);
  return true;
}

bool SetObject::iterator_impl(JSContext* cx, const CallArgs& args,
                              IteratorKind kind) {
  Rooted<SetObject*> setobj(cx, &args.thisv().toObject().as<SetObject>());
  ValueSet& set = *setobj->getData();
  Rooted<JSObject*> iterobj(cx,
                            SetIteratorObject::create(cx, setobj, &set, kind));
  if (!iterobj) {
    return false;
  }
  args.rval().setObject(*iterobj);
  return true;
}

bool SetObject::values_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, Values);
}

bool SetObject::values(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "values");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, values_impl, args);
}

bool SetObject::entries_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, Entries);
}

bool SetObject::entries(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "entries");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, entries_impl, args);
}

bool SetObject::clear(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(SetObject::is(obj));
  ValueSet& set = extract(obj);
  if (!set.clear()) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

bool SetObject::clear_impl(JSContext* cx, const CallArgs& args) {
  Rooted<SetObject*> setobj(cx, &args.thisv().toObject().as<SetObject>());
  if (!setobj->getData()->clear()) {
    ReportOutOfMemory(cx);
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool SetObject::clear(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "clear");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, clear_impl, args);
}

bool SetObject::copy(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(SetObject::is(args[0]));

  auto* result = SetObject::create(cx);
  if (!result) {
    return false;
  }

  ValueSet* set = result->getData();
  MOZ_ASSERT(set);

  auto* from = &args[0].toObject().as<SetObject>();
  for (auto range = from->getData()->all(); !range.empty(); range.popFront()) {
    HashableValue value = range.front().get();

    if (!PostWriteBarrier(result, value) || !set->put(value)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  args.rval().setObject(*result);
  return true;
}

/*** JS static utility functions ********************************************/

static bool forEach(const char* funcName, JSContext* cx, HandleObject obj,
                    HandleValue callbackFn, HandleValue thisArg) {
  CHECK_THREAD(cx);

  RootedId forEachId(cx, NameToId(cx->names().forEach));
  RootedFunction forEachFunc(
      cx, JS::GetSelfHostedFunction(cx, funcName, forEachId, 2));
  if (!forEachFunc) {
    return false;
  }

  RootedValue fval(cx, ObjectValue(*forEachFunc));
  return Call(cx, fval, obj, callbackFn, thisArg, &fval);
}

// Handles Clear/Size for public jsapi map/set access
template <typename RetT>
RetT CallObjFunc(RetT (*ObjFunc)(JSContext*, HandleObject), JSContext* cx,
                 HandleObject obj) {
  CHECK_THREAD(cx);
  cx->check(obj);

  // Always unwrap, in case this is an xray or cross-compartment wrapper.
  RootedObject unwrappedObj(cx);
  unwrappedObj = UncheckedUnwrap(obj);

  // Enter the realm of the backing object before calling functions on
  // it.
  JSAutoRealm ar(cx, unwrappedObj);
  return ObjFunc(cx, unwrappedObj);
}

// Handles Has/Delete for public jsapi map/set access
bool CallObjFunc(bool (*ObjFunc)(JSContext* cx, HandleObject obj,
                                 HandleValue key, bool* rval),
                 JSContext* cx, HandleObject obj, HandleValue key, bool* rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key);

  // Always unwrap, in case this is an xray or cross-compartment wrapper.
  RootedObject unwrappedObj(cx);
  unwrappedObj = UncheckedUnwrap(obj);
  JSAutoRealm ar(cx, unwrappedObj);

  // If we're working with a wrapped map/set, rewrap the key into the
  // compartment of the unwrapped map/set.
  RootedValue wrappedKey(cx, key);
  if (obj != unwrappedObj) {
    if (!JS_WrapValue(cx, &wrappedKey)) {
      return false;
    }
  }
  return ObjFunc(cx, unwrappedObj, wrappedKey, rval);
}

// Handles iterator generation for public jsapi map/set access
template <typename Iter>
bool CallObjFunc(bool (*ObjFunc)(JSContext* cx, Iter kind, HandleObject obj,
                                 MutableHandleValue iter),
                 JSContext* cx, Iter iterType, HandleObject obj,
                 MutableHandleValue rval) {
  CHECK_THREAD(cx);
  cx->check(obj);

  // Always unwrap, in case this is an xray or cross-compartment wrapper.
  RootedObject unwrappedObj(cx);
  unwrappedObj = UncheckedUnwrap(obj);
  {
    // Retrieve the iterator while in the unwrapped map/set's compartment,
    // otherwise we'll crash on a compartment assert.
    JSAutoRealm ar(cx, unwrappedObj);
    if (!ObjFunc(cx, iterType, unwrappedObj, rval)) {
      return false;
    }
  }

  // If the caller is in a different compartment than the map/set, rewrap the
  // iterator object into the caller's compartment.
  if (obj != unwrappedObj) {
    if (!JS_WrapValue(cx, rval)) {
      return false;
    }
  }
  return true;
}

/*** JS public APIs *********************************************************/

JS_PUBLIC_API JSObject* JS::NewMapObject(JSContext* cx) {
  return MapObject::create(cx);
}

JS_PUBLIC_API uint32_t JS::MapSize(JSContext* cx, HandleObject obj) {
  return CallObjFunc<uint32_t>(&MapObject::size, cx, obj);
}

JS_PUBLIC_API bool JS::MapGet(JSContext* cx, HandleObject obj, HandleValue key,
                              MutableHandleValue rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key, rval);

  // Unwrap the object, and enter its realm. If object isn't wrapped,
  // this is essentially a noop.
  RootedObject unwrappedObj(cx);
  unwrappedObj = UncheckedUnwrap(obj);
  {
    JSAutoRealm ar(cx, unwrappedObj);
    RootedValue wrappedKey(cx, key);

    // If we passed in a wrapper, wrap our key into its compartment now.
    if (obj != unwrappedObj) {
      if (!JS_WrapValue(cx, &wrappedKey)) {
        return false;
      }
    }
    if (!MapObject::get(cx, unwrappedObj, wrappedKey, rval)) {
      return false;
    }
  }

  // If we passed in a wrapper, wrap our return value on the way out.
  if (obj != unwrappedObj) {
    if (!JS_WrapValue(cx, rval)) {
      return false;
    }
  }
  return true;
}

JS_PUBLIC_API bool JS::MapSet(JSContext* cx, HandleObject obj, HandleValue key,
                              HandleValue val) {
  CHECK_THREAD(cx);
  cx->check(obj, key, val);

  // Unwrap the object, and enter its compartment. If object isn't wrapped,
  // this is essentially a noop.
  RootedObject unwrappedObj(cx);
  unwrappedObj = UncheckedUnwrap(obj);
  {
    JSAutoRealm ar(cx, unwrappedObj);

    // If we passed in a wrapper, wrap both key and value before adding to
    // the map
    RootedValue wrappedKey(cx, key);
    RootedValue wrappedValue(cx, val);
    if (obj != unwrappedObj) {
      if (!JS_WrapValue(cx, &wrappedKey) || !JS_WrapValue(cx, &wrappedValue)) {
        return false;
      }
    }
    return MapObject::set(cx, unwrappedObj, wrappedKey, wrappedValue);
  }
}

JS_PUBLIC_API bool JS::MapHas(JSContext* cx, HandleObject obj, HandleValue key,
                              bool* rval) {
  return CallObjFunc(MapObject::has, cx, obj, key, rval);
}

JS_PUBLIC_API bool JS::MapDelete(JSContext* cx, HandleObject obj,
                                 HandleValue key, bool* rval) {
  return CallObjFunc(MapObject::delete_, cx, obj, key, rval);
}

JS_PUBLIC_API bool JS::MapClear(JSContext* cx, HandleObject obj) {
  return CallObjFunc(&MapObject::clear, cx, obj);
}

JS_PUBLIC_API bool JS::MapKeys(JSContext* cx, HandleObject obj,
                               MutableHandleValue rval) {
  return CallObjFunc(&MapObject::iterator, cx, MapObject::Keys, obj, rval);
}

JS_PUBLIC_API bool JS::MapValues(JSContext* cx, HandleObject obj,
                                 MutableHandleValue rval) {
  return CallObjFunc(&MapObject::iterator, cx, MapObject::Values, obj, rval);
}

JS_PUBLIC_API bool JS::MapEntries(JSContext* cx, HandleObject obj,
                                  MutableHandleValue rval) {
  return CallObjFunc(&MapObject::iterator, cx, MapObject::Entries, obj, rval);
}

JS_PUBLIC_API bool JS::MapForEach(JSContext* cx, HandleObject obj,
                                  HandleValue callbackFn, HandleValue thisVal) {
  return forEach("MapForEach", cx, obj, callbackFn, thisVal);
}

JS_PUBLIC_API JSObject* JS::NewSetObject(JSContext* cx) {
  return SetObject::create(cx);
}

JS_PUBLIC_API uint32_t JS::SetSize(JSContext* cx, HandleObject obj) {
  return CallObjFunc<uint32_t>(&SetObject::size, cx, obj);
}

JS_PUBLIC_API bool JS::SetAdd(JSContext* cx, HandleObject obj,
                              HandleValue key) {
  CHECK_THREAD(cx);
  cx->check(obj, key);

  // Unwrap the object, and enter its compartment. If object isn't wrapped,
  // this is essentially a noop.
  RootedObject unwrappedObj(cx);
  unwrappedObj = UncheckedUnwrap(obj);
  {
    JSAutoRealm ar(cx, unwrappedObj);

    // If we passed in a wrapper, wrap key before adding to the set
    RootedValue wrappedKey(cx, key);
    if (obj != unwrappedObj) {
      if (!JS_WrapValue(cx, &wrappedKey)) {
        return false;
      }
    }
    return SetObject::add(cx, unwrappedObj, wrappedKey);
  }
}

JS_PUBLIC_API bool JS::SetHas(JSContext* cx, HandleObject obj, HandleValue key,
                              bool* rval) {
  return CallObjFunc(SetObject::has, cx, obj, key, rval);
}

JS_PUBLIC_API bool JS::SetDelete(JSContext* cx, HandleObject obj,
                                 HandleValue key, bool* rval) {
  return CallObjFunc(SetObject::delete_, cx, obj, key, rval);
}

JS_PUBLIC_API bool JS::SetClear(JSContext* cx, HandleObject obj) {
  return CallObjFunc(&SetObject::clear, cx, obj);
}

JS_PUBLIC_API bool JS::SetKeys(JSContext* cx, HandleObject obj,
                               MutableHandleValue rval) {
  return SetValues(cx, obj, rval);
}

JS_PUBLIC_API bool JS::SetValues(JSContext* cx, HandleObject obj,
                                 MutableHandleValue rval) {
  return CallObjFunc(&SetObject::iterator, cx, SetObject::Values, obj, rval);
}

JS_PUBLIC_API bool JS::SetEntries(JSContext* cx, HandleObject obj,
                                  MutableHandleValue rval) {
  return CallObjFunc(&SetObject::iterator, cx, SetObject::Entries, obj, rval);
}

JS_PUBLIC_API bool JS::SetForEach(JSContext* cx, HandleObject obj,
                                  HandleValue callbackFn, HandleValue thisVal) {
  return forEach("SetForEach", cx, obj, callbackFn, thisVal);
}

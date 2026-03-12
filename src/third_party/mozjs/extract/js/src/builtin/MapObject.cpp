/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/MapObject-inl.h"

#include "jsapi.h"

#include "builtin/OrderedHashTableObject.h"
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

#include "builtin/OrderedHashTableObject-inl.h"
#include "gc/GCContext-inl.h"
#include "gc/Marking-inl.h"
#include "gc/ObjectKind-inl.h"
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

bool HashableValue::setValue(JSContext* cx, const Value& v) {
  if (v.isString()) {
    // Atomize so that hash() and operator==() are fast and infallible.
    JSString* str = AtomizeString(cx, v.toString());
    if (!str) {
      return false;
    }
    value = StringValue(str);
  } else if (v.isDouble()) {
    value = NormalizeDoubleValue(v.toDouble());
  } else {
    value = v;
  }

  MOZ_ASSERT(value.isUndefined() || value.isNull() || value.isBoolean() ||
             value.isNumber() || value.isString() || value.isSymbol() ||
             value.isObject() || value.isBigInt());
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
  if (v.isObject()) {
    return hcs.scramble(v.asRawBits());
  }

  MOZ_ASSERT(!v.isGCThing(), "do not reveal pointers via hash codes");
  return mozilla::HashGeneric(v.asRawBits());
}

HashNumber HashableValue::hash(const mozilla::HashCodeScrambler& hcs) const {
  return HashValue(value, hcs);
}

bool HashableValue::equals(const HashableValue& other) const {
  // Two HashableValues are equal if they have equal bits.
  bool b = (value.asRawBits() == other.value.asRawBits());

  if (!b && (value.type() == other.value.type())) {
    if (value.isBigInt()) {
      // BigInt values are considered equal if they represent the same
      // mathematical value.
      b = BigInt::equal(value.toBigInt(), other.value.toBigInt());
    }
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
    &MapIteratorObjectClassOps,
    JS_NULL_CLASS_SPEC,
    &MapIteratorObjectClassExtension,
};

const JSFunctionSpec MapIteratorObject::methods[] = {
    JS_SELF_HOSTED_FN("next", "MapIteratorNext", 0, 0),
    JS_FS_END,
};

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
  if (!JSObject::setHasFuseProperty(cx, proto)) {
    return false;
  }
  global->initBuiltinProto(ProtoKind::MapIteratorProto, proto);
  return true;
}

template <typename TableObject>
static inline bool HasRegisteredNurseryIterators(TableObject* t) {
  Value v = t->getReservedSlot(TableObject::RegisteredNurseryIteratorsSlot);
  return v.toBoolean();
}

template <typename TableObject>
static inline void SetRegisteredNurseryIterators(TableObject* t, bool b) {
  t->setReservedSlot(TableObject::RegisteredNurseryIteratorsSlot,
                     JS::BooleanValue(b));
}

MapIteratorObject* MapIteratorObject::create(JSContext* cx,
                                             Handle<MapObject*> mapobj,
                                             Kind kind) {
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

  if (IsInsideNursery(iterobj) &&
      !HasRegisteredNurseryIterators(mapobj.get())) {
    if (!cx->nursery().addMapWithNurseryIterators(mapobj)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    SetRegisteredNurseryIterators(mapobj.get(), true);
  }

  MapObject::Table(mapobj).initIterator(iterobj, kind);

  return iterobj;
}

void MapIteratorObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());
  MOZ_ASSERT(!IsInsideNursery(obj));
  if (obj->as<MapIteratorObject>().isActive()) {
    obj->as<MapIteratorObject>().unlink();
  }
}

size_t MapIteratorObject::objectMoved(JSObject* obj, JSObject* old) {
  MapIteratorObject* iter = &obj->as<MapIteratorObject>();
  if (!iter->isActive()) {
    return 0;
  }
  if (IsInsideNursery(old)) {
    MapObject* mapObj = iter->target();
    MapObject::Table(mapObj).relinkNurseryIterator(iter);
  } else {
    iter->updateListAfterMove(&old->as<MapIteratorObject>());
  }
  return 0;
}

MapObject* MapIteratorObject::target() const {
  MOZ_ASSERT(isActive(), "only active iterators have a target object");
  Value value = getFixedSlot(TargetSlot);
  return &MaybeForwarded(&value.toObject())->as<MapObject>();
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

  if (!mapIterator->isActive()) {
    // Already done.
    return true;
  }

  auto storeResult = [resultPairObj](Kind kind, const auto& element) {
    switch (kind) {
      case Kind::Keys:
        resultPairObj->setDenseElement(0, element.key.get());
        break;

      case Kind::Values:
        resultPairObj->setDenseElement(1, element.value);
        break;

      case Kind::Entries: {
        resultPairObj->setDenseElement(0, element.key.get());
        resultPairObj->setDenseElement(1, element.value);
        break;
      }
    }
  };
  MapObject* mapObj = mapIterator->target();
  return MapObject::Table(mapObj).iteratorNext(mapIterator, storeResult);
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

// MapObject::Table, ::UnbarrieredTable and ::PreBarrieredTable must all have
// the same memory layout.
static_assert(sizeof(MapObject::Table::Entry) ==
              sizeof(MapObject::UnbarrieredTable::Entry));
static_assert(sizeof(MapObject::Table::Entry) ==
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
    GenericCreateConstructor<MapObject::construct, 0, gc::AllocKind::FUNCTION,
                             &jit::JitInfo_MapConstructor>,
    GenericCreatePrototype<MapObject>,
    MapObject::staticMethods,
    MapObject::staticProperties,
    MapObject::methods,
    MapObject::properties,
    MapObject::finishInit,
};

const ClassExtension MapObject::classExtension_ = {
    MapObject::objectMoved,  // objectMovedOp
};

const JSClass MapObject::class_ = {
    "Map",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(MapObject::SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Map) | JSCLASS_BACKGROUND_FINALIZE |
        JSCLASS_SKIP_NURSERY_FINALIZE,
    &MapObject::classOps_, &MapObject::classSpec_, &MapObject::classExtension_};

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
    JS_INLINABLE_FN("set", set, 2, 0, MapSet),
    JS_INLINABLE_FN("delete", delete_, 1, 0, MapDelete),
    JS_FN("keys", keys, 0, 0),
    JS_FN("values", values, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "MapForEach", 2, 0),
#ifdef NIGHTLY_BUILD
    JS_FN("getOrInsert", getOrInsert, 2, 0),
    JS_SELF_HOSTED_FN("getOrInsertComputed", "MapGetOrInsertComputed", 2, 0),
#endif
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
  if (!NativeDefineDataProperty(cx, nativeProto, iteratorId, entriesFn, 0)) {
    return false;
  }

  return JSObject::setHasFuseProperty(cx, nativeProto);
}

void MapObject::trace(JSTracer* trc, JSObject* obj) {
  MapObject* mapObj = &obj->as<MapObject>();
  Table(mapObj).trace(trc);
}

using NurseryKeysVector = GCVector<Value, 4, SystemAllocPolicy>;

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
    NurseryKeysVector* keys = GetNurseryKeys(object);
    MOZ_ASSERT(keys);

    keys->mutableEraseIf([&](Value& key) {
      MOZ_ASSERT(typename ObjectT::UnbarrieredTable(object).hash(key) ==
                 typename ObjectT::Table(object).hash(
                     *reinterpret_cast<const HashableValue*>(&key)));
      MOZ_ASSERT(IsInsideNursery(key.toGCThing()));

      auto result = typename ObjectT::UnbarrieredTable(object).rekeyOneEntry(
          key, [trc](const Value& prior) {
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
[[nodiscard]] inline static bool PostWriteBarrier(ObjectT* obj,
                                                  const Value& keyValue) {
  MOZ_ASSERT(!IsInsideNursery(obj));

  if (MOZ_LIKELY(!keyValue.isObject() && !keyValue.isBigInt())) {
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

bool MapObject::getKeysAndValuesInterleaved(
    JS::MutableHandle<GCVector<JS::Value>> entries) {
  auto appendEntry = [&entries](auto& entry) {
    return entries.append(entry.key.get()) && entries.append(entry.value);
  };
  return Table(this).forEachEntry(appendEntry);
}

bool MapObject::set(JSContext* cx, const Value& key, const Value& val) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }
  return setWithHashableKey(cx, k, val);
}

bool MapObject::setWithHashableKey(JSContext* cx, const HashableValue& key,
                                   const Value& value) {
  bool needsPostBarriers = isTenured();
  if (needsPostBarriers) {
    // Use the Table representation which has post barriers.
    if (!PostWriteBarrier(this, key)) {
      ReportOutOfMemory(cx);
      return false;
    }
    if (!Table(this).put(cx, key, value)) {
      return false;
    }
  } else {
    // Use the PreBarrieredTable representation which does not.
    if (!PreBarrieredTable(this).put(cx, key, value)) {
      return false;
    }
  }

  return true;
}

#ifdef NIGHTLY_BUILD
bool MapObject::getOrInsert(JSContext* cx, const Value& key, const Value& val,
                            MutableHandleValue rval) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }

  bool needsPostBarriers = isTenured();
  if (needsPostBarriers) {
    if (!PostWriteBarrier(this, k)) {
      ReportOutOfMemory(cx);
      return false;
    }
    // Use the Table representation which has post barriers.
    if (const Table::Entry* p = Table(this).getOrAdd(cx, k, val)) {
      rval.set(p->value);
    } else {
      return false;
    }
  } else {
    // Use the PreBarrieredTable representation which does not.
    if (const PreBarrieredTable::Entry* p =
            PreBarrieredTable(this).getOrAdd(cx, k, val)) {
      rval.set(p->value);
    } else {
      return false;
    }
  }
  return true;
}
#endif  // #ifdef NIGHTLY_BUILD

MapObject* MapObject::createWithProto(JSContext* cx, HandleObject proto,
                                      NewObjectKind newKind) {
  MOZ_ASSERT(proto);

  gc::AllocKind allocKind = gc::GetGCObjectKind(SlotCount);

  AutoSetNewObjectMetadata metadata(cx);
  auto* mapObj =
      NewObjectWithGivenProtoAndKinds<MapObject>(cx, proto, allocKind, newKind);
  if (!mapObj) {
    return nullptr;
  }

  UnbarrieredTable(mapObj).initSlots();
  mapObj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
  mapObj->initReservedSlot(RegisteredNurseryIteratorsSlot, BooleanValue(false));
  return mapObj;
}

MapObject* MapObject::create(JSContext* cx,
                             HandleObject proto /* = nullptr */) {
  if (proto) {
    return createWithProto(cx, proto, GenericObject);
  }

  // This is the common case so use the template object's shape to optimize the
  // allocation.
  MapObject* templateObj = GlobalObject::getOrCreateMapTemplateObject(cx);
  if (!templateObj) {
    return nullptr;
  }

  gc::AllocKind allocKind = templateObj->asTenured().getAllocKind();
  MOZ_ASSERT(gc::GetGCKindSlots(allocKind) >= SlotCount);
  MOZ_ASSERT(gc::IsBackgroundFinalized(allocKind));

  AutoSetNewObjectMetadata metadata(cx);
  Rooted<SharedShape*> shape(cx, templateObj->sharedShape());
  auto* mapObj =
      NativeObject::create<MapObject>(cx, allocKind, gc::Heap::Default, shape);
  if (!mapObj) {
    return nullptr;
  }

  UnbarrieredTable(mapObj).initSlots();
  mapObj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
  mapObj->initReservedSlot(RegisteredNurseryIteratorsSlot, BooleanValue(false));
  return mapObj;
}

// static
MapObject* GlobalObject::getOrCreateMapTemplateObject(JSContext* cx) {
  GlobalObjectData& data = cx->global()->data();
  if (MapObject* obj = data.mapObjectTemplate) {
    return obj;
  }

  Rooted<JSObject*> proto(cx,
                          GlobalObject::getOrCreatePrototype(cx, JSProto_Map));
  if (!proto) {
    return nullptr;
  }
  auto* mapObj = MapObject::createWithProto(cx, proto, TenuredObject);
  if (!mapObj) {
    return nullptr;
  }

  data.mapObjectTemplate.init(mapObj);
  return mapObj;
}

size_t MapObject::sizeOfData(mozilla::MallocSizeOf mallocSizeOf) {
  size_t size = 0;
  size += Table(this).sizeOfExcludingObject(mallocSizeOf);
  if (NurseryKeysVector* nurseryKeys = GetNurseryKeys(this)) {
    size += nurseryKeys->sizeOfIncludingThis(mallocSizeOf);
  }
  return size;
}

void MapObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MapObject* mapObj = &obj->as<MapObject>();
  MOZ_ASSERT(!IsInsideNursery(mapObj));
  MOZ_ASSERT(!UnbarrieredTable(mapObj).hasNurseryIterators());

#ifdef DEBUG
  // If we're finalizing a tenured map then it cannot contain nursery things,
  // because we evicted the nursery at the start of collection and writing a
  // nursery thing into the table would require it to be live, which means it
  // would have been marked.
  UnbarrieredTable(mapObj).forEachEntryUpTo(1000, [](auto& entry) {
    Value key = entry.key;
    MOZ_ASSERT_IF(key.isGCThing(), !IsInsideNursery(key.toGCThing()));
    Value value = entry.value;
    MOZ_ASSERT_IF(value.isGCThing(), !IsInsideNursery(value.toGCThing()));
  });
#endif

  // Finalized tenured maps do not contain nursery GC things, so do not require
  // post barriers. Pre barriers are not required for finalization.
  UnbarrieredTable(mapObj).destroy(gcx);
}

size_t MapObject::objectMoved(JSObject* obj, JSObject* old) {
  auto* mapObj = &obj->as<MapObject>();

  Table(mapObj).updateIteratorsAfterMove(&old->as<MapObject>());

  if (IsInsideNursery(old)) {
    Nursery& nursery = mapObj->runtimeFromMainThread()->gc.nursery();
    Table(mapObj).maybeMoveBufferOnPromotion(nursery);
  }

  return 0;
}

void MapObject::clearNurseryIteratorsBeforeMinorGC() {
  Table(this).clearNurseryIterators();
}

/* static */
MapObject* MapObject::sweepAfterMinorGC(JS::GCContext* gcx, MapObject* mapobj) {
  Nursery& nursery = gcx->runtime()->gc.nursery();
  bool wasInCollectedRegion = nursery.inCollectedRegion(mapobj);
  if (wasInCollectedRegion && !IsForwarded(mapobj)) {
    // This MapObject is dead.
    return nullptr;
  }

  mapobj = MaybeForwarded(mapobj);

  // Keep |mapobj| registered with the nursery if it still has nursery
  // iterators.
  bool hasNurseryIterators = Table(mapobj).hasNurseryIterators();
  SetRegisteredNurseryIterators(mapobj, hasNurseryIterators);
  return hasNurseryIterators ? mapobj : nullptr;
}

bool MapObject::tryOptimizeCtorWithIterable(JSContext* cx,
                                            const Value& iterableVal,
                                            bool* optimized) {
  MOZ_ASSERT(!iterableVal.isNullOrUndefined());
  MOZ_ASSERT(!*optimized);

  if (!CanOptimizeMapOrSetCtorWithIterable<JSProto_Map>(MapObject::set, this,
                                                        cx)) {
    return true;
  }

  if (!iterableVal.isObject()) {
    return true;
  }
  JSObject* iterable = &iterableVal.toObject();

  // Fast path for `new Map(array)`.
  if (IsOptimizableArrayForMapOrSetCtor<MapOrSet::Map>(iterable, cx)) {
    ArrayObject* array = &iterable->as<ArrayObject>();
    uint32_t len = array->getDenseInitializedLength();

    for (uint32_t index = 0; index < len; index++) {
      Value element = array->getDenseElement(index);
      MOZ_ASSERT(IsPackedArray(&element.toObject()));

      auto* elementArray = &element.toObject().as<ArrayObject>();
      Value key = elementArray->getDenseElement(0);
      Value value = elementArray->getDenseElement(1);

      MOZ_ASSERT(!key.isMagic(JS_ELEMENTS_HOLE));
      MOZ_ASSERT(!value.isMagic(JS_ELEMENTS_HOLE));

      if (!set(cx, key, value)) {
        return false;
      }
    }

    *optimized = true;
    return true;
  }

  // Fast path for `new Map(map)`.
  if (IsMapObjectWithDefaultIterator(iterable, cx)) {
    auto* iterableMap = &iterable->as<MapObject>();
    auto addEntry = [cx, this](auto& entry) {
      return setWithHashableKey(cx, entry.key, entry.value);
    };
    if (!Table(iterableMap).forEachEntry(addEntry)) {
      return false;
    }
    *optimized = true;
    return true;
  }

  return true;
}

// static
MapObject* MapObject::createFromIterable(JSContext* cx, Handle<JSObject*> proto,
                                         Handle<Value> iterable,
                                         Handle<MapObject*> allocatedFromJit) {
  // A null-or-undefined |iterable| is quite common and we check for this in JIT
  // code.
  MOZ_ASSERT_IF(allocatedFromJit, !iterable.isNullOrUndefined());

  Rooted<MapObject*> obj(cx, allocatedFromJit);
  if (!obj) {
    obj = MapObject::create(cx, proto);
    if (!obj) {
      return nullptr;
    }
  }

  if (!iterable.isNullOrUndefined()) {
    bool optimized = false;
    if (!obj->tryOptimizeCtorWithIterable(cx, iterable, &optimized)) {
      return nullptr;
    }
    if (!optimized) {
      FixedInvokeArgs<1> args(cx);
      args[0].set(iterable);

      RootedValue thisv(cx, ObjectValue(*obj));
      if (!CallSelfHostedFunction(cx, cx->names().MapConstructorInit, thisv,
                                  args, args.rval())) {
        return nullptr;
      }
    }
  }

  return obj;
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

  MapObject* obj = MapObject::createFromIterable(cx, proto, args.get(0));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool MapObject::is(HandleValue v) {
  return v.isObject() && v.toObject().hasClass(&class_);
}

bool MapObject::is(HandleObject o) { return o->hasClass(&class_); }

uint32_t MapObject::size() {
  static_assert(sizeof(Table(this).count()) <= sizeof(uint32_t),
                "map count must be precisely representable as a JS number");
  return Table(this).count();
}

bool MapObject::size_impl(JSContext* cx, const CallArgs& args) {
  auto* mapObj = &args.thisv().toObject().as<MapObject>();
  args.rval().setNumber(mapObj->size());
  return true;
}

bool MapObject::size(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "size");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::size_impl>(cx, args);
}

bool MapObject::get(JSContext* cx, const Value& key, MutableHandleValue rval) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }

  if (const Table::Entry* p = Table(this).get(k)) {
    rval.set(p->value);
  } else {
    rval.setUndefined();
  }

  return true;
}

bool MapObject::get_impl(JSContext* cx, const CallArgs& args) {
  auto* mapObj = &args.thisv().toObject().as<MapObject>();
  return mapObj->get(cx, args.get(0), args.rval());
}

bool MapObject::get(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "get");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::get_impl>(cx, args);
}

bool MapObject::has(JSContext* cx, const Value& key, bool* rval) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }

  *rval = Table(this).has(k);
  return true;
}

bool MapObject::has_impl(JSContext* cx, const CallArgs& args) {
  auto* mapObj = &args.thisv().toObject().as<MapObject>();
  bool found;
  if (!mapObj->has(cx, args.get(0), &found)) {
    return false;
  }
  args.rval().setBoolean(found);
  return true;
}

bool MapObject::has(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "has");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::has_impl>(cx, args);
}

bool MapObject::set_impl(JSContext* cx, const CallArgs& args) {
  auto* mapObj = &args.thisv().toObject().as<MapObject>();
  if (!mapObj->set(cx, args.get(0), args.get(1))) {
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

#ifdef NIGHTLY_BUILD
bool MapObject::getOrInsert_impl(JSContext* cx, const CallArgs& args) {
  auto* mapObj = &args.thisv().toObject().as<MapObject>();
  return mapObj->getOrInsert(cx, args.get(0), args.get(1), args.rval());
}

bool MapObject::getOrInsert(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "getOrInsert");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<MapObject::is, MapObject::getOrInsert_impl>(cx,
                                                                          args);
}
#endif  // #ifdef NIGHTLY_BUILD

bool MapObject::delete_(JSContext* cx, const Value& key, bool* rval) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }

  if (isTenured()) {
    *rval = Table(this).remove(cx, k);
  } else {
    *rval = PreBarrieredTable(this).remove(cx, k);
  }
  return true;
}

bool MapObject::delete_impl(JSContext* cx, const CallArgs& args) {
  // MapObject::trace does not trace deleted entries. Incremental GC therefore
  // requires that no HeapPtr<Value> objects pointing to heap values be left
  // alive in the hash table.
  //
  // OrderedHashMapImpl::remove() doesn't destroy the removed entry. It merely
  // calls OrderedHashMapImpl::MapOps::makeEmpty. But that is sufficient,
  // because makeEmpty clears the value by doing e->value = Value(), and in the
  // case of Table, Value() means HeapPtr<Value>(), which is the same as
  // HeapPtr<Value>(UndefinedValue()).

  auto* mapObj = &args.thisv().toObject().as<MapObject>();
  bool found;
  if (!mapObj->delete_(cx, args.get(0), &found)) {
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

bool MapObject::iterator(JSContext* cx, IteratorKind kind,
                         Handle<MapObject*> obj, MutableHandleValue iter) {
  JSObject* iterobj = MapIteratorObject::create(cx, obj, kind);
  if (!iterobj) {
    return false;
  }
  iter.setObject(*iterobj);
  return true;
}

bool MapObject::iterator_impl(JSContext* cx, const CallArgs& args,
                              IteratorKind kind) {
  Rooted<MapObject*> mapObj(cx, &args.thisv().toObject().as<MapObject>());
  return iterator(cx, kind, mapObj, args.rval());
}

bool MapObject::keys_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, IteratorKind::Keys);
}

bool MapObject::keys(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "keys");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, keys_impl, args);
}

bool MapObject::values_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, IteratorKind::Values);
}

bool MapObject::values(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "values");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, values_impl, args);
}

bool MapObject::entries_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, IteratorKind::Entries);
}

bool MapObject::entries(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "entries");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, entries_impl, args);
}

bool MapObject::clear_impl(JSContext* cx, const CallArgs& args) {
  auto* mapObj = &args.thisv().toObject().as<MapObject>();
  mapObj->clear(cx);
  args.rval().setUndefined();
  return true;
}

bool MapObject::clear(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Map.prototype", "clear");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, clear_impl, args);
}

void MapObject::clear(JSContext* cx) {
  if (isTenured()) {
    Table(this).clear(cx);
  } else {
    PreBarrieredTable(this).clear(cx);
  }
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
    &SetIteratorObjectClassOps,
    JS_NULL_CLASS_SPEC,
    &SetIteratorObjectClassExtension,
};

const JSFunctionSpec SetIteratorObject::methods[] = {
    JS_SELF_HOSTED_FN("next", "SetIteratorNext", 0, 0),
    JS_FS_END,
};

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
  if (!JSObject::setHasFuseProperty(cx, proto)) {
    return false;
  }
  global->initBuiltinProto(ProtoKind::SetIteratorProto, proto);
  return true;
}

SetIteratorObject* SetIteratorObject::create(JSContext* cx,
                                             Handle<SetObject*> setobj,
                                             Kind kind) {
  MOZ_ASSERT(kind != Kind::Keys);

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

  if (IsInsideNursery(iterobj) &&
      !HasRegisteredNurseryIterators(setobj.get())) {
    if (!cx->nursery().addSetWithNurseryIterators(setobj)) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    SetRegisteredNurseryIterators(setobj.get(), true);
  }

  SetObject::Table(setobj).initIterator(iterobj, kind);

  return iterobj;
}

void SetIteratorObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());
  MOZ_ASSERT(!IsInsideNursery(obj));
  if (obj->as<SetIteratorObject>().isActive()) {
    obj->as<SetIteratorObject>().unlink();
  }
}

size_t SetIteratorObject::objectMoved(JSObject* obj, JSObject* old) {
  SetIteratorObject* iter = &obj->as<SetIteratorObject>();
  if (!iter->isActive()) {
    return 0;
  }
  if (IsInsideNursery(old)) {
    SetObject* setObj = iter->target();
    SetObject::Table(setObj).relinkNurseryIterator(iter);
  } else {
    iter->updateListAfterMove(&old->as<SetIteratorObject>());
  }
  return 0;
}

SetObject* SetIteratorObject::target() const {
  MOZ_ASSERT(isActive(), "only active iterators have a target object");
  Value value = getFixedSlot(TargetSlot);
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

  if (!setIterator->isActive()) {
    // Already done.
    return true;
  }

  auto storeResult = [resultObj](Kind kind, const auto& element) {
    resultObj->setDenseElement(0, element.get());
  };
  SetObject* setObj = setIterator->target();
  return SetObject::Table(setObj).iteratorNext(setIterator, storeResult);
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
    GenericCreateConstructor<SetObject::construct, 0, gc::AllocKind::FUNCTION,
                             &jit::JitInfo_SetConstructor>,
    GenericCreatePrototype<SetObject>,
    nullptr,
    SetObject::staticProperties,
    SetObject::methods,
    SetObject::properties,
    SetObject::finishInit,
};

const ClassExtension SetObject::classExtension_ = {
    SetObject::objectMoved,  // objectMovedOp
};

const JSClass SetObject::class_ = {
    "Set",
    JSCLASS_DELAY_METADATA_BUILDER |
        JSCLASS_HAS_RESERVED_SLOTS(SetObject::SlotCount) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Set) | JSCLASS_BACKGROUND_FINALIZE |
        JSCLASS_SKIP_NURSERY_FINALIZE,
    &SetObject::classOps_, &SetObject::classSpec_, &SetObject::classExtension_};

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
    JS_INLINABLE_FN("add", add, 1, 0, SetAdd),
    JS_INLINABLE_FN("delete", delete_, 1, 0, SetDelete),
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
  if (!NativeDefineDataProperty(cx, nativeProto, iteratorId, valuesFn, 0)) {
    return false;
  }

  return JSObject::setHasFuseProperty(cx, nativeProto);
}

bool SetObject::keys(JS::MutableHandle<GCVector<JS::Value>> keys) {
  auto appendEntry = [&keys](auto& entry) { return keys.append(entry.get()); };
  return Table(this).forEachEntry(appendEntry);
}

bool SetObject::add(JSContext* cx, const Value& key) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }
  return addHashableValue(cx, k);
}

bool SetObject::addHashableValue(JSContext* cx, const HashableValue& value) {
  bool needsPostBarriers = isTenured();
  if (needsPostBarriers && !PostWriteBarrier(this, value)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return Table(this).put(cx, value);
}

SetObject* SetObject::createWithProto(JSContext* cx, HandleObject proto,
                                      NewObjectKind newKind) {
  MOZ_ASSERT(proto);

  gc::AllocKind allocKind = gc::GetGCObjectKind(SlotCount);

  AutoSetNewObjectMetadata metadata(cx);
  auto* setObj =
      NewObjectWithGivenProtoAndKinds<SetObject>(cx, proto, allocKind, newKind);
  if (!setObj) {
    return nullptr;
  }

  UnbarrieredTable(setObj).initSlots();
  setObj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
  setObj->initReservedSlot(RegisteredNurseryIteratorsSlot, BooleanValue(false));
  return setObj;
}

SetObject* SetObject::create(JSContext* cx,
                             HandleObject proto /* = nullptr */) {
  if (proto) {
    return createWithProto(cx, proto, GenericObject);
  }

  // This is the common case so use the template object's shape to optimize the
  // allocation.
  SetObject* templateObj = GlobalObject::getOrCreateSetTemplateObject(cx);
  if (!templateObj) {
    return nullptr;
  }

  gc::AllocKind allocKind = templateObj->asTenured().getAllocKind();
  MOZ_ASSERT(gc::GetGCKindSlots(allocKind) >= SlotCount);
  MOZ_ASSERT(gc::IsBackgroundFinalized(allocKind));

  AutoSetNewObjectMetadata metadata(cx);
  Rooted<SharedShape*> shape(cx, templateObj->sharedShape());
  auto* setObj =
      NativeObject::create<SetObject>(cx, allocKind, gc::Heap::Default, shape);
  if (!setObj) {
    return nullptr;
  }

  UnbarrieredTable(setObj).initSlots();
  setObj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
  setObj->initReservedSlot(RegisteredNurseryIteratorsSlot, BooleanValue(false));
  return setObj;
}

// static
SetObject* GlobalObject::getOrCreateSetTemplateObject(JSContext* cx) {
  GlobalObjectData& data = cx->global()->data();
  if (SetObject* obj = data.setObjectTemplate) {
    return obj;
  }

  Rooted<JSObject*> proto(cx,
                          GlobalObject::getOrCreatePrototype(cx, JSProto_Set));
  if (!proto) {
    return nullptr;
  }
  auto* setObj = SetObject::createWithProto(cx, proto, TenuredObject);
  if (!setObj) {
    return nullptr;
  }

  data.setObjectTemplate.init(setObj);
  return setObj;
}

void SetObject::trace(JSTracer* trc, JSObject* obj) {
  SetObject* setobj = static_cast<SetObject*>(obj);
  Table(setobj).trace(trc);
}

size_t SetObject::sizeOfData(mozilla::MallocSizeOf mallocSizeOf) {
  size_t size = 0;
  size += Table(this).sizeOfExcludingObject(mallocSizeOf);
  if (NurseryKeysVector* nurseryKeys = GetNurseryKeys(this)) {
    size += nurseryKeys->sizeOfIncludingThis(mallocSizeOf);
  }
  return size;
}

void SetObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  SetObject* setObj = &obj->as<SetObject>();
  MOZ_ASSERT(!IsInsideNursery(setObj));
  MOZ_ASSERT(!UnbarrieredTable(setObj).hasNurseryIterators());

#ifdef DEBUG
  // If we're finalizing a tenured set then it cannot contain nursery things,
  // because we evicted the nursery at the start of collection and writing a
  // nursery thing into the set would require it to be live, which means it
  // would have been marked.
  UnbarrieredTable(setObj).forEachEntryUpTo(1000, [](auto& entry) {
    Value key = entry;
    MOZ_ASSERT_IF(key.isGCThing(), !IsInsideNursery(key.toGCThing()));
  });
#endif

  // Finalized tenured sets do not contain nursery GC things, so do not require
  // post barriers. Pre barriers are not required for finalization.
  UnbarrieredTable(setObj).destroy(gcx);
}

size_t SetObject::objectMoved(JSObject* obj, JSObject* old) {
  auto* setObj = &obj->as<SetObject>();

  Table(setObj).updateIteratorsAfterMove(&old->as<SetObject>());

  if (IsInsideNursery(old)) {
    Nursery& nursery = setObj->runtimeFromMainThread()->gc.nursery();
    Table(setObj).maybeMoveBufferOnPromotion(nursery);
  }

  return 0;
}

void SetObject::clearNurseryIteratorsBeforeMinorGC() {
  Table(this).clearNurseryIterators();
}

/* static */
SetObject* SetObject::sweepAfterMinorGC(JS::GCContext* gcx, SetObject* setobj) {
  Nursery& nursery = gcx->runtime()->gc.nursery();
  bool wasInCollectedRegion = nursery.inCollectedRegion(setobj);
  if (wasInCollectedRegion && !IsForwarded(setobj)) {
    // This SetObject is dead.
    return nullptr;
  }

  setobj = MaybeForwarded(setobj);

  // Keep |setobj| registered with the nursery if it still has nursery
  // iterators.
  bool hasNurseryIterators = Table(setobj).hasNurseryIterators();
  SetRegisteredNurseryIterators(setobj, hasNurseryIterators);
  return hasNurseryIterators ? setobj : nullptr;
}

bool SetObject::tryOptimizeCtorWithIterable(JSContext* cx,
                                            const Value& iterableVal,
                                            bool* optimized) {
  MOZ_ASSERT(!iterableVal.isNullOrUndefined());
  MOZ_ASSERT(!*optimized);

  if (!CanOptimizeMapOrSetCtorWithIterable<JSProto_Set>(SetObject::add, this,
                                                        cx)) {
    return true;
  }

  if (!iterableVal.isObject()) {
    return true;
  }
  JSObject* iterable = &iterableVal.toObject();

  // Fast path for `new Set(array)`.
  if (IsOptimizableArrayForMapOrSetCtor<MapOrSet::Set>(iterable, cx)) {
    ArrayObject* array = &iterable->as<ArrayObject>();
    uint32_t len = array->getDenseInitializedLength();

    for (uint32_t index = 0; index < len; index++) {
      Value keyVal = array->getDenseElement(index);
      MOZ_ASSERT(!keyVal.isMagic(JS_ELEMENTS_HOLE));
      if (!add(cx, keyVal)) {
        return false;
      }
    }

    *optimized = true;
    return true;
  }

  // Fast path for `new Set(set)`.
  if (IsSetObjectWithDefaultIterator(iterable, cx)) {
    auto* iterableSet = &iterable->as<SetObject>();
    if (!IsSetObjectWithDefaultIterator(iterableSet, cx)) {
      return true;
    }
    auto addEntry = [cx, this](auto& entry) {
      return addHashableValue(cx, entry);
    };
    if (!Table(iterableSet).forEachEntry(addEntry)) {
      return false;
    }
    *optimized = true;
    return true;
  }

  return true;
}

// static
SetObject* SetObject::createFromIterable(JSContext* cx, Handle<JSObject*> proto,
                                         Handle<Value> iterable,
                                         Handle<SetObject*> allocatedFromJit) {
  // A null-or-undefined |iterable| is quite common and we check for this in JIT
  // code.
  MOZ_ASSERT_IF(allocatedFromJit, !iterable.isNullOrUndefined());

  Rooted<SetObject*> obj(cx, allocatedFromJit);
  if (!obj) {
    obj = SetObject::create(cx, proto);
    if (!obj) {
      return nullptr;
    }
  }

  if (!iterable.isNullOrUndefined()) {
    bool optimized = false;
    if (!obj->tryOptimizeCtorWithIterable(cx, iterable, &optimized)) {
      return nullptr;
    }
    if (!optimized) {
      FixedInvokeArgs<1> args(cx);
      args[0].set(iterable);

      RootedValue thisv(cx, ObjectValue(*obj));
      if (!CallSelfHostedFunction(cx, cx->names().SetConstructorInit, thisv,
                                  args, args.rval())) {
        return nullptr;
      }
    }
  }

  return obj;
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

  SetObject* obj = SetObject::createFromIterable(cx, proto, args.get(0));
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

bool SetObject::is(HandleValue v) {
  return v.isObject() && v.toObject().hasClass(&class_);
}

bool SetObject::is(HandleObject o) { return o->hasClass(&class_); }

uint32_t SetObject::size() {
  static_assert(sizeof(Table(this).count()) <= sizeof(uint32_t),
                "set count must be precisely representable as a JS number");
  return Table(this).count();
}

bool SetObject::size_impl(JSContext* cx, const CallArgs& args) {
  auto* setObj = &args.thisv().toObject().as<SetObject>();
  args.rval().setNumber(setObj->size());
  return true;
}

bool SetObject::size(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "size");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<SetObject::is, SetObject::size_impl>(cx, args);
}

bool SetObject::has_impl(JSContext* cx, const CallArgs& args) {
  auto* setObj = &args.thisv().toObject().as<SetObject>();
  bool found;
  if (!setObj->has(cx, args.get(0), &found)) {
    return false;
  }
  args.rval().setBoolean(found);
  return true;
}

bool SetObject::has(JSContext* cx, const Value& key, bool* rval) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }

  *rval = Table(this).has(k);
  return true;
}

bool SetObject::has(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "has");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<SetObject::is, SetObject::has_impl>(cx, args);
}

bool SetObject::add_impl(JSContext* cx, const CallArgs& args) {
  auto* setObj = &args.thisv().toObject().as<SetObject>();
  if (!setObj->add(cx, args.get(0))) {
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

bool SetObject::delete_(JSContext* cx, const Value& key, bool* rval) {
  HashableValue k;
  if (!k.setValue(cx, key)) {
    return false;
  }

  *rval = Table(this).remove(cx, k);
  return true;
}

bool SetObject::delete_impl(JSContext* cx, const CallArgs& args) {
  auto* setObj = &args.thisv().toObject().as<SetObject>();
  bool found;
  if (!setObj->delete_(cx, args.get(0), &found)) {
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

bool SetObject::iterator(JSContext* cx, IteratorKind kind,
                         Handle<SetObject*> obj, MutableHandleValue iter) {
  JSObject* iterobj = SetIteratorObject::create(cx, obj, kind);
  if (!iterobj) {
    return false;
  }
  iter.setObject(*iterobj);
  return true;
}

bool SetObject::iterator_impl(JSContext* cx, const CallArgs& args,
                              IteratorKind kind) {
  Rooted<SetObject*> setObj(cx, &args.thisv().toObject().as<SetObject>());
  return iterator(cx, kind, setObj, args.rval());
}

bool SetObject::values_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, IteratorKind::Values);
}

bool SetObject::values(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "values");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, values_impl, args);
}

bool SetObject::entries_impl(JSContext* cx, const CallArgs& args) {
  return iterator_impl(cx, args, IteratorKind::Entries);
}

bool SetObject::entries(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Set.prototype", "entries");
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod(cx, is, entries_impl, args);
}

void SetObject::clear(JSContext* cx) { Table(this).clear(cx); }

bool SetObject::clear_impl(JSContext* cx, const CallArgs& args) {
  auto* setObj = &args.thisv().toObject().as<SetObject>();
  setObj->clear(cx);
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

  auto* from = &args[0].toObject().as<SetObject>();

  auto addToResult = [cx, result](auto& entry) {
    return result->addHashableValue(cx, entry);
  };
  if (!Table(from).forEachEntry(addToResult)) {
    return false;
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

// RAII class that unwraps a wrapped Map or Set object and then enters its
// realm.
template <typename TableObject>
class MOZ_RAII AutoEnterTableRealm {
  mozilla::Maybe<AutoRealm> ar_;
  Rooted<TableObject*> unwrapped_;

 public:
  AutoEnterTableRealm(JSContext* cx, JSObject* obj) : unwrapped_(cx) {
    JSObject* unwrapped = UncheckedUnwrap(obj);
    MOZ_ASSERT(unwrapped != obj);
    MOZ_RELEASE_ASSERT(unwrapped->is<TableObject>());
    unwrapped_ = &unwrapped->as<TableObject>();
    ar_.emplace(cx, unwrapped_);
  }
  Handle<TableObject*> unwrapped() const { return unwrapped_; }
};

/*** JS public APIs *********************************************************/

JS_PUBLIC_API JSObject* JS::NewMapObject(JSContext* cx) {
  return MapObject::create(cx);
}

JS_PUBLIC_API uint32_t JS::MapSize(JSContext* cx, HandleObject obj) {
  CHECK_THREAD(cx);
  cx->check(obj);

  if (obj->is<MapObject>()) {
    return obj.as<MapObject>()->size();
  }

  AutoEnterTableRealm<MapObject> enter(cx, obj);
  return enter.unwrapped()->size();
}

JS_PUBLIC_API bool JS::MapGet(JSContext* cx, HandleObject obj, HandleValue key,
                              MutableHandleValue rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key, rval);

  if (obj->is<MapObject>()) {
    return obj.as<MapObject>()->get(cx, key, rval);
  }

  {
    AutoEnterTableRealm<MapObject> enter(cx, obj);
    Rooted<Value> wrappedKey(cx, key);
    if (!JS_WrapValue(cx, &wrappedKey)) {
      return false;
    }
    if (!enter.unwrapped()->get(cx, wrappedKey, rval)) {
      return false;
    }
  }
  return JS_WrapValue(cx, rval);
}

JS_PUBLIC_API bool JS::MapSet(JSContext* cx, HandleObject obj, HandleValue key,
                              HandleValue val) {
  CHECK_THREAD(cx);
  cx->check(obj, key, val);

  if (obj->is<MapObject>()) {
    return obj.as<MapObject>()->set(cx, key, val);
  }

  AutoEnterTableRealm<MapObject> enter(cx, obj);
  Rooted<Value> wrappedKey(cx, key);
  Rooted<Value> wrappedValue(cx, val);
  if (!JS_WrapValue(cx, &wrappedKey) || !JS_WrapValue(cx, &wrappedValue)) {
    return false;
  }
  return enter.unwrapped()->set(cx, wrappedKey, wrappedValue);
}

JS_PUBLIC_API bool JS::MapHas(JSContext* cx, HandleObject obj, HandleValue key,
                              bool* rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key);

  if (obj->is<MapObject>()) {
    return obj.as<MapObject>()->has(cx, key, rval);
  }

  AutoEnterTableRealm<MapObject> enter(cx, obj);
  Rooted<Value> wrappedKey(cx, key);
  if (!JS_WrapValue(cx, &wrappedKey)) {
    return false;
  }
  return enter.unwrapped()->has(cx, wrappedKey, rval);
}

#ifdef NIGHTLY_BUILD
JS_PUBLIC_API bool JS::MapGetOrInsert(JSContext* cx, HandleObject obj,
                                      HandleValue key, HandleValue val,
                                      MutableHandleValue rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key, val);

  if (obj->is<MapObject>()) {
    return obj.as<MapObject>()->getOrInsert(cx, key, val, rval);
  }
  {
    AutoEnterTableRealm<MapObject> enter(cx, obj);
    Rooted<Value> wrappedKey(cx, key);
    Rooted<Value> wrappedValue(cx, val);
    if (!JS_WrapValue(cx, &wrappedKey) || !JS_WrapValue(cx, &wrappedValue)) {
      return false;
    }
    if (!enter.unwrapped()->getOrInsert(cx, wrappedKey, wrappedValue, rval)) {
      return false;
    }
  }
  return JS_WrapValue(cx, rval);
}
#endif  // #ifdef NIGHTLY_BUILD

JS_PUBLIC_API bool JS::MapDelete(JSContext* cx, HandleObject obj,
                                 HandleValue key, bool* rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key);

  if (obj->is<MapObject>()) {
    return obj.as<MapObject>()->delete_(cx, key, rval);
  }

  AutoEnterTableRealm<MapObject> enter(cx, obj);
  Rooted<Value> wrappedKey(cx, key);
  if (!JS_WrapValue(cx, &wrappedKey)) {
    return false;
  }
  return enter.unwrapped()->delete_(cx, wrappedKey, rval);
}

JS_PUBLIC_API bool JS::MapClear(JSContext* cx, HandleObject obj) {
  CHECK_THREAD(cx);
  cx->check(obj);

  if (obj->is<MapObject>()) {
    obj.as<MapObject>()->clear(cx);
    return true;
  }

  AutoEnterTableRealm<MapObject> enter(cx, obj);
  enter.unwrapped()->clear(cx);
  return true;
}

template <typename TableObject>
[[nodiscard]] static bool CreateIterator(
    JSContext* cx, typename TableObject::IteratorKind kind,
    Handle<JSObject*> obj, MutableHandle<Value> rval) {
  CHECK_THREAD(cx);
  cx->check(obj);

  if (obj->is<TableObject>()) {
    return TableObject::iterator(cx, kind, obj.as<TableObject>(), rval);
  }

  {
    AutoEnterTableRealm<TableObject> enter(cx, obj);
    if (!TableObject::iterator(cx, kind, enter.unwrapped(), rval)) {
      return false;
    }
  }
  return JS_WrapValue(cx, rval);
}

JS_PUBLIC_API bool JS::MapKeys(JSContext* cx, HandleObject obj,
                               MutableHandleValue rval) {
  return CreateIterator<MapObject>(cx, MapObject::IteratorKind::Keys, obj,
                                   rval);
}

JS_PUBLIC_API bool JS::MapValues(JSContext* cx, HandleObject obj,
                                 MutableHandleValue rval) {
  return CreateIterator<MapObject>(cx, MapObject::IteratorKind::Values, obj,
                                   rval);
}

JS_PUBLIC_API bool JS::MapEntries(JSContext* cx, HandleObject obj,
                                  MutableHandleValue rval) {
  return CreateIterator<MapObject>(cx, MapObject::IteratorKind::Entries, obj,
                                   rval);
}

JS_PUBLIC_API bool JS::MapForEach(JSContext* cx, HandleObject obj,
                                  HandleValue callbackFn, HandleValue thisVal) {
  return forEach("MapForEach", cx, obj, callbackFn, thisVal);
}

JS_PUBLIC_API JSObject* JS::NewSetObject(JSContext* cx) {
  return SetObject::create(cx);
}

JS_PUBLIC_API uint32_t JS::SetSize(JSContext* cx, HandleObject obj) {
  CHECK_THREAD(cx);
  cx->check(obj);

  if (obj->is<SetObject>()) {
    return obj.as<SetObject>()->size();
  }

  AutoEnterTableRealm<SetObject> enter(cx, obj);
  return enter.unwrapped()->size();
}

JS_PUBLIC_API bool JS::SetAdd(JSContext* cx, HandleObject obj,
                              HandleValue key) {
  CHECK_THREAD(cx);
  cx->check(obj, key);

  if (obj->is<SetObject>()) {
    return obj.as<SetObject>()->add(cx, key);
  }

  AutoEnterTableRealm<SetObject> enter(cx, obj);
  Rooted<Value> wrappedKey(cx, key);
  if (!JS_WrapValue(cx, &wrappedKey)) {
    return false;
  }
  return enter.unwrapped()->add(cx, wrappedKey);
}

JS_PUBLIC_API bool JS::SetHas(JSContext* cx, HandleObject obj, HandleValue key,
                              bool* rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key);

  if (obj->is<SetObject>()) {
    return obj.as<SetObject>()->has(cx, key, rval);
  }

  AutoEnterTableRealm<SetObject> enter(cx, obj);
  Rooted<Value> wrappedKey(cx, key);
  if (!JS_WrapValue(cx, &wrappedKey)) {
    return false;
  }
  return enter.unwrapped()->has(cx, wrappedKey, rval);
}

JS_PUBLIC_API bool JS::SetDelete(JSContext* cx, HandleObject obj,
                                 HandleValue key, bool* rval) {
  CHECK_THREAD(cx);
  cx->check(obj, key);

  if (obj->is<SetObject>()) {
    return obj.as<SetObject>()->delete_(cx, key, rval);
  }

  AutoEnterTableRealm<SetObject> enter(cx, obj);
  Rooted<Value> wrappedKey(cx, key);
  if (!JS_WrapValue(cx, &wrappedKey)) {
    return false;
  }
  return enter.unwrapped()->delete_(cx, wrappedKey, rval);
}

JS_PUBLIC_API bool JS::SetClear(JSContext* cx, HandleObject obj) {
  CHECK_THREAD(cx);
  cx->check(obj);

  if (obj->is<SetObject>()) {
    obj.as<SetObject>()->clear(cx);
    return true;
  }

  AutoEnterTableRealm<SetObject> enter(cx, obj);
  enter.unwrapped()->clear(cx);
  return true;
}

JS_PUBLIC_API bool JS::SetKeys(JSContext* cx, HandleObject obj,
                               MutableHandleValue rval) {
  return SetValues(cx, obj, rval);
}

JS_PUBLIC_API bool JS::SetValues(JSContext* cx, HandleObject obj,
                                 MutableHandleValue rval) {
  return CreateIterator<SetObject>(cx, SetObject::IteratorKind::Values, obj,
                                   rval);
}

JS_PUBLIC_API bool JS::SetEntries(JSContext* cx, HandleObject obj,
                                  MutableHandleValue rval) {
  return CreateIterator<SetObject>(cx, SetObject::IteratorKind::Entries, obj,
                                   rval);
}

JS_PUBLIC_API bool JS::SetForEach(JSContext* cx, HandleObject obj,
                                  HandleValue callbackFn, HandleValue thisVal) {
  return forEach("SetForEach", cx, obj, callbackFn, thisVal);
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/MapObject.h"

#include "ds/OrderedHashTable.h"
#include "gc/FreeOp.h"
#include "js/Utility.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/SelfHosting.h"
#include "vm/SymbolType.h"

#include "gc/Marking-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::IsNaN;
using mozilla::NumberEqualsInt32;

using JS::DoubleNaNValue;


/*** HashableValue *******************************************************************************/

bool
HashableValue::setValue(JSContext* cx, HandleValue v)
{
    if (v.isString()) {
        // Atomize so that hash() and operator==() are fast and infallible.
        JSString* str = AtomizeString(cx, v.toString(), DoNotPinAtom);
        if (!str)
            return false;
        value = StringValue(str);
    } else if (v.isDouble()) {
        double d = v.toDouble();
        int32_t i;
        if (NumberEqualsInt32(d, &i)) {
            // Normalize int32_t-valued doubles to int32_t for faster hashing and testing.
            value = Int32Value(i);
        } else if (IsNaN(d)) {
            // NaNs with different bits must hash and test identically.
            value = DoubleNaNValue();
        } else {
            value = v;
        }
    } else {
        value = v;
    }

    MOZ_ASSERT(value.isUndefined() || value.isNull() || value.isBoolean() || value.isNumber() ||
               value.isString() || value.isSymbol() || value.isObject());
    return true;
}

static HashNumber
HashValue(const Value& v, const mozilla::HashCodeScrambler& hcs)
{
    // HashableValue::setValue normalizes values so that the SameValue relation
    // on HashableValues is the same as the == relationship on
    // value.asRawBits(). So why not just return that? Security.
    //
    // To avoid revealing GC of atoms, string-based hash codes are computed
    // from the string contents rather than any pointer; to avoid revealing
    // addresses, pointer-based hash codes are computed using the
    // HashCodeScrambler.

    if (v.isString())
        return v.toString()->asAtom().hash();
    if (v.isSymbol())
        return v.toSymbol()->hash();
    if (v.isObject())
        return hcs.scramble(v.asRawBits());

    MOZ_ASSERT(!v.isGCThing(), "do not reveal pointers via hash codes");
    return mozilla::HashGeneric(v.asRawBits());
}

HashNumber
HashableValue::hash(const mozilla::HashCodeScrambler& hcs) const
{
    return HashValue(value, hcs);
}

bool
HashableValue::operator==(const HashableValue& other) const
{
    // Two HashableValues are equal if they have equal bits.
    bool b = (value.asRawBits() == other.value.asRawBits());

#ifdef DEBUG
    bool same;
    JSContext* cx = TlsContext.get();
    RootedValue valueRoot(cx, value);
    RootedValue otherRoot(cx, other.value);
    MOZ_ASSERT(SameValue(nullptr, valueRoot, otherRoot, &same));
    MOZ_ASSERT(same == b);
#endif
    return b;
}

HashableValue
HashableValue::trace(JSTracer* trc) const
{
    HashableValue hv(*this);
    TraceEdge(trc, &hv.value, "key");
    return hv;
}


/*** MapIterator *********************************************************************************/

namespace {

} /* anonymous namespace */

static const ClassOps MapIteratorObjectClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    MapIteratorObject::finalize
};

static const ClassExtension MapIteratorObjectClassExtension = {
    nullptr, /* weakmapKeyDelegateOp */
    MapIteratorObject::objectMoved
};

const Class MapIteratorObject::class_ = {
    "Map Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(MapIteratorObject::SlotCount) |
    JSCLASS_FOREGROUND_FINALIZE |
    JSCLASS_SKIP_NURSERY_FINALIZE,
    &MapIteratorObjectClassOps,
    JS_NULL_CLASS_SPEC,
    &MapIteratorObjectClassExtension
};

const JSFunctionSpec MapIteratorObject::methods[] = {
    JS_SELF_HOSTED_FN("next", "MapIteratorNext", 0, 0),
    JS_FS_END
};

static inline ValueMap::Range*
MapIteratorObjectRange(NativeObject* obj)
{
    MOZ_ASSERT(obj->is<MapIteratorObject>());
    Value value = obj->getSlot(MapIteratorObject::RangeSlot);
    if (value.isUndefined())
        return nullptr;

    return static_cast<ValueMap::Range*>(value.toPrivate());
}

inline MapObject::IteratorKind
MapIteratorObject::kind() const
{
    int32_t i = getSlot(KindSlot).toInt32();
    MOZ_ASSERT(i == MapObject::Keys || i == MapObject::Values || i == MapObject::Entries);
    return MapObject::IteratorKind(i);
}

/* static */ bool
GlobalObject::initMapIteratorProto(JSContext* cx, Handle<GlobalObject*> global)
{
    Rooted<JSObject*> base(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!base)
        return false;
    RootedPlainObject proto(cx, NewObjectWithGivenProto<PlainObject>(cx, base));
    if (!proto)
        return false;
    if (!JS_DefineFunctions(cx, proto, MapIteratorObject::methods) ||
        !DefineToStringTag(cx, proto, cx->names().MapIterator))
    {
        return false;
    }
    global->setReservedSlot(MAP_ITERATOR_PROTO, ObjectValue(*proto));
    return true;
}

template <typename TableObject>
static inline bool
HasNurseryMemory(TableObject* t)
{
    return t->getReservedSlot(TableObject::HasNurseryMemorySlot).toBoolean();
}

template <typename TableObject>
static inline void
SetHasNurseryMemory(TableObject* t, bool b)
{
    t->setReservedSlot(TableObject::HasNurseryMemorySlot, JS::BooleanValue(b));
}

MapIteratorObject*
MapIteratorObject::create(JSContext* cx, HandleObject obj, ValueMap* data,
                          MapObject::IteratorKind kind)
{
    Handle<MapObject*> mapobj(obj.as<MapObject>());
    Rooted<GlobalObject*> global(cx, &mapobj->global());
    Rooted<JSObject*> proto(cx, GlobalObject::getOrCreateMapIteratorPrototype(cx, global));
    if (!proto)
        return nullptr;

    Nursery& nursery = cx->nursery();

    MapIteratorObject* iterobj;
    void *buffer;
    NewObjectKind objectKind = GenericObject;
    while (true) {
        iterobj = NewObjectWithGivenProto<MapIteratorObject>(cx, proto, objectKind);
        if (!iterobj)
            return nullptr;

        iterobj->setSlot(TargetSlot, ObjectValue(*mapobj));
        iterobj->setSlot(RangeSlot, PrivateValue(nullptr));
        iterobj->setSlot(KindSlot, Int32Value(int32_t(kind)));

        const size_t size = JS_ROUNDUP(sizeof(ValueMap::Range), gc::CellAlignBytes);
        buffer = nursery.allocateBufferSameLocation(iterobj, size);
        if (buffer)
            break;

        if (!IsInsideNursery(iterobj)) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        // There was space in the nursery for the object but not the
        // Range. Try again in the tenured heap.
        MOZ_ASSERT(objectKind == GenericObject);
        objectKind = TenuredObject;
    }

    bool insideNursery = IsInsideNursery(iterobj);
    MOZ_ASSERT(insideNursery == nursery.isInside(buffer));
    if (insideNursery && !HasNurseryMemory(mapobj.get())) {
        if (!cx->compartment()->addMapWithNurseryMemory(mapobj)) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
        SetHasNurseryMemory(mapobj.get(), true);
    }

    auto range = data->createRange(buffer, insideNursery);
    iterobj->setSlot(RangeSlot, PrivateValue(range));

    return iterobj;
}

void
MapIteratorObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());
    MOZ_ASSERT(!IsInsideNursery(obj));

    auto range = MapIteratorObjectRange(&obj->as<NativeObject>());
    MOZ_ASSERT(!obj->zone()->group()->nursery().isInside(range));

    fop->delete_(range);
}

size_t
MapIteratorObject::objectMoved(JSObject* obj, JSObject* old)
{
    if (!IsInsideNursery(old))
        return 0;

    MapIteratorObject* iter = &obj->as<MapIteratorObject>();
    ValueMap::Range* range = MapIteratorObjectRange(iter);
    if (!range)
        return 0;

    Nursery& nursery = iter->zone()->group()->nursery();
    if (!nursery.isInside(range)) {
        nursery.removeMallocedBuffer(range);
        return 0;
    }

    AutoEnterOOMUnsafeRegion oomUnsafe;
    auto newRange = iter->zone()->pod_malloc<ValueMap::Range>();
    if (!newRange)
        oomUnsafe.crash("MapIteratorObject failed to allocate Range data while tenuring.");

    new (newRange) ValueMap::Range(*range);
    range->~Range();
    iter->setReservedSlot(MapIteratorObject::RangeSlot, PrivateValue(newRange));
    return sizeof(ValueMap::Range);
}

template <typename Range>
static void
DestroyRange(JSObject* iterator, Range* range)
{
    range->~Range();
    if (!IsInsideNursery(iterator))
        js_free(range);
}

bool
MapIteratorObject::next(Handle<MapIteratorObject*> mapIterator, HandleArrayObject resultPairObj,
                        JSContext* cx)
{
    // Check invariants for inlined _GetNextMapEntryForIterator.

    // The array should be tenured, so that post-barrier can be done simply.
    MOZ_ASSERT(resultPairObj->isTenured());

    // The array elements should be fixed.
    MOZ_ASSERT(resultPairObj->hasFixedElements());
    MOZ_ASSERT(resultPairObj->getDenseInitializedLength() == 2);
    MOZ_ASSERT(resultPairObj->getDenseCapacity() >= 2);

    ValueMap::Range* range = MapIteratorObjectRange(mapIterator);
    if (!range)
        return true;

    if (range->empty()) {
        DestroyRange<ValueMap::Range>(mapIterator, range);
        mapIterator->setReservedSlot(RangeSlot, PrivateValue(nullptr));
        return true;
    }

    switch (mapIterator->kind()) {
      case MapObject::Keys:
        resultPairObj->setDenseElementWithType(cx, 0, range->front().key.get());
        break;

      case MapObject::Values:
        resultPairObj->setDenseElementWithType(cx, 1, range->front().value);
        break;

      case MapObject::Entries: {
        resultPairObj->setDenseElementWithType(cx, 0, range->front().key.get());
        resultPairObj->setDenseElementWithType(cx, 1, range->front().value);
        break;
      }
    }
    range->popFront();
    return false;
}

/* static */ JSObject*
MapIteratorObject::createResultPair(JSContext* cx)
{
    RootedArrayObject resultPairObj(cx, NewDenseFullyAllocatedArray(cx, 2, nullptr, TenuredObject));
    if (!resultPairObj)
        return nullptr;

    Rooted<TaggedProto> proto(cx, resultPairObj->taggedProto());
    ObjectGroup* group = ObjectGroupCompartment::makeGroup(cx, resultPairObj->getClass(), proto);
    if (!group)
        return nullptr;
    resultPairObj->setGroup(group);

    resultPairObj->setDenseInitializedLength(2);
    resultPairObj->initDenseElement(0, NullValue());
    resultPairObj->initDenseElement(1, NullValue());

    // See comments in MapIteratorObject::next.
    AddTypePropertyId(cx, resultPairObj, JSID_VOID, TypeSet::UnknownType());

    return resultPairObj;
}


/*** Map *****************************************************************************************/

static JSObject*
CreateMapPrototype(JSContext* cx, JSProtoKey key)
{
    return GlobalObject::createBlankPrototype(cx, cx->global(), &MapObject::protoClass_);
}

const ClassOps MapObject::classOps_ = {
    nullptr, // addProperty
    nullptr, // delProperty
    nullptr, // enumerate
    nullptr, // newEnumerate
    nullptr, // resolve
    nullptr, // mayResolve
    finalize,
    nullptr, // call
    nullptr, // hasInstance
    nullptr, // construct
    trace
};

const ClassSpec MapObject::classSpec_ = {
    GenericCreateConstructor<MapObject::construct, 0, gc::AllocKind::FUNCTION>,
    CreateMapPrototype,
    nullptr,
    MapObject::staticProperties,
    MapObject::methods,
    MapObject::properties,
};

const Class MapObject::class_ = {
    "Map",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(MapObject::SlotCount) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Map) |
    JSCLASS_FOREGROUND_FINALIZE |
    JSCLASS_SKIP_NURSERY_FINALIZE,
    &MapObject::classOps_,
    &MapObject::classSpec_
};

const Class MapObject::protoClass_ = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Map),
    JS_NULL_CLASS_OPS,
    &MapObject::classSpec_
};

const JSPropertySpec MapObject::properties[] = {
    JS_PSG("size", size, 0),
    JS_STRING_SYM_PS(toStringTag, "Map", JSPROP_READONLY),
    JS_PS_END
};

const JSFunctionSpec MapObject::methods[] = {
    JS_FN("get", get, 1, 0),
    JS_FN("has", has, 1, 0),
    JS_FN("set", set, 2, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("keys", keys, 0, 0),
    JS_FN("values", values, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "MapForEach", 2, 0),
    // MapEntries only exists to preseve the equal identity of
    // entries and @@iterator.
    JS_SELF_HOSTED_FN("entries", "MapEntries", 0, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "MapEntries", 0, 0),
    JS_FS_END
};

const JSPropertySpec MapObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "MapSpecies", 0),
    JS_PS_END
};

template <class Range>
static void
TraceKey(Range& r, const HashableValue& key, JSTracer* trc)
{
    HashableValue newKey = key.trace(trc);

    if (newKey.get() != key.get()) {
        // The hash function only uses the bits of the Value, so it is safe to
        // rekey even when the object or string has been modified by the GC.
        r.rekeyFront(newKey);
    }
}

void
MapObject::trace(JSTracer* trc, JSObject* obj)
{
    if (ValueMap* map = obj->as<MapObject>().getData()) {
        for (ValueMap::Range r = map->all(); !r.empty(); r.popFront()) {
            TraceKey(r, r.front().key, trc);
            TraceEdge(trc, &r.front().value, "value");
        }
    }
}

struct js::UnbarrieredHashPolicy {
    typedef Value Lookup;
    static HashNumber hash(const Lookup& v, const mozilla::HashCodeScrambler& hcs) {
        return HashValue(v, hcs);
    }
    static bool match(const Value& k, const Lookup& l) { return k == l; }
    static bool isEmpty(const Value& v) { return v.isMagic(JS_HASH_KEY_EMPTY); }
    static void makeEmpty(Value* vp) { vp->setMagic(JS_HASH_KEY_EMPTY); }
};

using NurseryKeysVector = Vector<JSObject*, 0, SystemAllocPolicy>;

template <typename TableObject>
static NurseryKeysVector*
GetNurseryKeys(TableObject* t)
{
    Value value = t->getReservedSlot(TableObject::NurseryKeysSlot);
    return reinterpret_cast<NurseryKeysVector*>(value.toPrivate());
}

template <typename TableObject>
static NurseryKeysVector*
AllocNurseryKeys(TableObject* t)
{
    MOZ_ASSERT(!GetNurseryKeys(t));
    auto keys = js_new<NurseryKeysVector>();
    if (!keys)
        return nullptr;

    t->setReservedSlot(TableObject::NurseryKeysSlot, PrivateValue(keys));
    return keys;
}

template <typename TableObject>
static void
DeleteNurseryKeys(TableObject* t)
{
    auto keys = GetNurseryKeys(t);
    MOZ_ASSERT(keys);
    js_delete(keys);
    t->setReservedSlot(TableObject::NurseryKeysSlot, PrivateValue(nullptr));
}

// A generic store buffer entry that traces all nursery keys for an ordered hash
// map or set.
template <typename ObjectT>
class js::OrderedHashTableRef : public gc::BufferableRef
{
    ObjectT* object;

  public:
    explicit OrderedHashTableRef(ObjectT* obj) : object(obj) {}

    void trace(JSTracer* trc) override {
        MOZ_ASSERT(!IsInsideNursery(object));
        auto realTable = object->getData();
        auto unbarrieredTable = reinterpret_cast<typename ObjectT::UnbarrieredTable*>(realTable);
        NurseryKeysVector* keys = GetNurseryKeys(object);
        MOZ_ASSERT(keys);
        for (JSObject* obj : *keys) {
            MOZ_ASSERT(obj);
            Value key = ObjectValue(*obj);
            Value prior = key;
            MOZ_ASSERT(unbarrieredTable->hash(key) ==
                       realTable->hash(*reinterpret_cast<HashableValue*>(&key)));
            TraceManuallyBarrieredEdge(trc, &key, "ordered hash table key");
            unbarrieredTable->rekeyOneEntry(prior, key);
        }
        DeleteNurseryKeys(object);
    }
};

template <typename ObjectT>
inline static MOZ_MUST_USE bool
WriteBarrierPostImpl(ObjectT* obj, const Value& keyValue)
{
    if (MOZ_LIKELY(!keyValue.isObject()))
        return true;

    if (IsInsideNursery(obj))
        return true;

    JSObject* key = &keyValue.toObject();
    if (!IsInsideNursery(key))
        return true;

    NurseryKeysVector* keys = GetNurseryKeys(obj);
    if (!keys) {
        keys = AllocNurseryKeys(obj);
        if (!keys)
            return false;

        key->zone()->group()->storeBuffer().putGeneric(OrderedHashTableRef<ObjectT>(obj));
    }

    if (!keys->append(key))
        return false;

    return true;
}

inline static MOZ_MUST_USE bool
WriteBarrierPost(MapObject* map, const Value& key)
{
    return WriteBarrierPostImpl(map, key);
}

inline static MOZ_MUST_USE bool
WriteBarrierPost(SetObject* set, const Value& key)
{
    return WriteBarrierPostImpl(set, key);
}

bool
MapObject::getKeysAndValuesInterleaved(HandleObject obj,
                                       JS::MutableHandle<GCVector<JS::Value>> entries)
{
    ValueMap* map = obj->as<MapObject>().getData();
    if (!map)
        return false;

    for (ValueMap::Range r = map->all(); !r.empty(); r.popFront()) {
        if (!entries.append(r.front().key.get()) ||
            !entries.append(r.front().value))
        {
            return false;
        }
    }

    return true;
}

bool
MapObject::set(JSContext* cx, HandleObject obj, HandleValue k, HandleValue v)
{
    ValueMap* map = obj->as<MapObject>().getData();
    if (!map)
        return false;

    Rooted<HashableValue> key(cx);
    if (!key.setValue(cx, k))
        return false;

    if (!WriteBarrierPost(&obj->as<MapObject>(), key.value()) ||
        !map->put(key, v))
    {
        ReportOutOfMemory(cx);
        return false;
    }

    return true;
}

MapObject*
MapObject::create(JSContext* cx, HandleObject proto /* = nullptr */)
{
    auto map = cx->make_unique<ValueMap>(cx->zone(),
                                         cx->compartment()->randomHashCodeScrambler());
    if (!map || !map->init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    MapObject* mapObj = NewObjectWithClassProto<MapObject>(cx,  proto);
    if (!mapObj)
        return nullptr;

    bool insideNursery = IsInsideNursery(mapObj);
    if (insideNursery && !cx->compartment()->addMapWithNurseryMemory(mapObj)) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    mapObj->initPrivate(map.release());
    mapObj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
    mapObj->initReservedSlot(HasNurseryMemorySlot, JS::BooleanValue(insideNursery));
    return mapObj;
}

void
MapObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());
    if (ValueMap* map = obj->as<MapObject>().getData())
        fop->delete_(map);
}

/* static */ void
MapObject::sweepAfterMinorGC(FreeOp* fop, MapObject* mapobj)
{
    if (IsInsideNursery(mapobj) && !IsForwarded(mapobj)) {
        finalize(fop, mapobj);
        return;
    }

    mapobj = MaybeForwarded(mapobj);
    mapobj->getData()->destroyNurseryRanges();
    SetHasNurseryMemory(mapobj, false);
}

bool
MapObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Map"))
        return false;

    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
        return false;

    Rooted<MapObject*> obj(cx, MapObject::create(cx, proto));
    if (!obj)
        return false;

    if (!args.get(0).isNullOrUndefined()) {
        FixedInvokeArgs<1> args2(cx);
        args2[0].set(args[0]);

        RootedValue thisv(cx, ObjectValue(*obj));
        if (!CallSelfHostedFunction(cx, cx->names().MapConstructorInit, thisv, args2, args2.rval()))
            return false;
    }

    args.rval().setObject(*obj);
    return true;
}

bool
MapObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().hasClass(&class_) && v.toObject().as<MapObject>().getPrivate();
}

bool
MapObject::is(HandleObject o)
{
    return o->hasClass(&class_) && o->as<MapObject>().getPrivate();
}

#define ARG0_KEY(cx, args, key)                                               \
    Rooted<HashableValue> key(cx);                                            \
    if (args.length() > 0 && !key.setValue(cx, args[0]))                      \
        return false

ValueMap&
MapObject::extract(HandleObject o)
{
    MOZ_ASSERT(o->hasClass(&MapObject::class_));
    return *o->as<MapObject>().getData();
}

ValueMap&
MapObject::extract(const CallArgs& args)
{
    MOZ_ASSERT(args.thisv().isObject());
    MOZ_ASSERT(args.thisv().toObject().hasClass(&MapObject::class_));
    return *args.thisv().toObject().as<MapObject>().getData();
}

uint32_t
MapObject::size(JSContext* cx, HandleObject obj)
{
    ValueMap& map = extract(obj);
    static_assert(sizeof(map.count()) <= sizeof(uint32_t),
                  "map count must be precisely representable as a JS number");
    return map.count();
}

bool
MapObject::size_impl(JSContext* cx, const CallArgs& args)
{
    RootedObject obj(cx, &args.thisv().toObject());
    args.rval().setNumber(size(cx, obj));
    return true;
}

bool
MapObject::size(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::size_impl>(cx, args);
}

bool
MapObject::get(JSContext* cx, HandleObject obj,
               HandleValue key, MutableHandleValue rval)
{
    ValueMap& map = extract(obj);
    Rooted<HashableValue> k(cx);

    if (!k.setValue(cx, key))
        return false;

    if (ValueMap::Entry* p = map.get(k))
        rval.set(p->value);
    else
        rval.setUndefined();

    return true;
}

bool
MapObject::get_impl(JSContext* cx, const CallArgs& args)
{
    RootedObject obj(cx, &args.thisv().toObject());
    return get(cx, obj, args.get(0), args.rval());
}

bool
MapObject::get(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::get_impl>(cx, args);
}

bool
MapObject::has(JSContext* cx, HandleObject obj, HandleValue key, bool* rval)
{
    ValueMap& map = extract(obj);
    Rooted<HashableValue> k(cx);

    if (!k.setValue(cx, key))
        return false;

    *rval = map.has(k);
    return true;
}

bool
MapObject::has_impl(JSContext* cx, const CallArgs& args)
{
    bool found;
    RootedObject obj(cx, &args.thisv().toObject());
    if (has(cx, obj, args.get(0), &found)) {
        args.rval().setBoolean(found);
        return true;
    }
    return false;
}

bool
MapObject::has(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::has_impl>(cx, args);
}

bool
MapObject::set_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(MapObject::is(args.thisv()));

    ValueMap& map = extract(args);
    ARG0_KEY(cx, args, key);
    if (!WriteBarrierPost(&args.thisv().toObject().as<MapObject>(), key.value()) ||
        !map.put(key, args.get(1)))
    {
        ReportOutOfMemory(cx);
        return false;
    }

    args.rval().set(args.thisv());
    return true;
}

bool
MapObject::set(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::set_impl>(cx, args);
}

bool
MapObject::delete_(JSContext *cx, HandleObject obj, HandleValue key, bool *rval)
{
    ValueMap &map = extract(obj);
    Rooted<HashableValue> k(cx);

    if (!k.setValue(cx, key))
        return false;

    if (!map.remove(k, rval)) {
        ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

bool
MapObject::delete_impl(JSContext *cx, const CallArgs& args)
{
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

    ValueMap& map = extract(args);
    ARG0_KEY(cx, args, key);
    bool found;
    if (!map.remove(key, &found)) {
        ReportOutOfMemory(cx);
        return false;
    }
    args.rval().setBoolean(found);
    return true;
}

bool
MapObject::delete_(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<MapObject::is, MapObject::delete_impl>(cx, args);
}

bool
MapObject::iterator(JSContext* cx, IteratorKind kind,
                    HandleObject obj, MutableHandleValue iter)
{
    ValueMap& map = extract(obj);
    Rooted<JSObject*> iterobj(cx, MapIteratorObject::create(cx, obj, &map, kind));
    if (!iterobj)
        return false;
    iter.setObject(*iterobj);
    return true;
}

bool
MapObject::iterator_impl(JSContext* cx, const CallArgs& args, IteratorKind kind)
{
    RootedObject obj(cx, &args.thisv().toObject());
    return iterator(cx, kind, obj, args.rval());
}

bool
MapObject::keys_impl(JSContext* cx, const CallArgs& args)
{
    return iterator_impl(cx, args, Keys);
}

bool
MapObject::keys(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, keys_impl, args);
}

bool
MapObject::values_impl(JSContext* cx, const CallArgs& args)
{
    return iterator_impl(cx, args, Values);
}

bool
MapObject::values(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, values_impl, args);
}

bool
MapObject::entries_impl(JSContext* cx, const CallArgs& args)
{
    return iterator_impl(cx, args, Entries);
}

bool
MapObject::entries(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, entries_impl, args);
}

bool
MapObject::clear_impl(JSContext* cx, const CallArgs& args)
{
    RootedObject obj(cx, &args.thisv().toObject());
    args.rval().setUndefined();
    return clear(cx, obj);
}

bool
MapObject::clear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, clear_impl, args);
}

bool
MapObject::clear(JSContext* cx, HandleObject obj)
{
    ValueMap& map = extract(obj);
    if (!map.clear()) {
        ReportOutOfMemory(cx);
        return false;
    }
    return true;
}


/*** SetIterator *********************************************************************************/

static const ClassOps SetIteratorObjectClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    SetIteratorObject::finalize
};

static const ClassExtension SetIteratorObjectClassExtension = {
    nullptr, /* weakmapKeyDelegateOp */
    SetIteratorObject::objectMoved
};

const Class SetIteratorObject::class_ = {
    "Set Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(SetIteratorObject::SlotCount) |
    JSCLASS_FOREGROUND_FINALIZE |
    JSCLASS_SKIP_NURSERY_FINALIZE,
    &SetIteratorObjectClassOps,
    JS_NULL_CLASS_SPEC,
    &SetIteratorObjectClassExtension
};

const JSFunctionSpec SetIteratorObject::methods[] = {
    JS_SELF_HOSTED_FN("next", "SetIteratorNext", 0, 0),
    JS_FS_END
};

static inline ValueSet::Range*
SetIteratorObjectRange(NativeObject* obj)
{
    MOZ_ASSERT(obj->is<SetIteratorObject>());
    Value value = obj->getSlot(SetIteratorObject::RangeSlot);
    if (value.isUndefined())
        return nullptr;

    return static_cast<ValueSet::Range*>(value.toPrivate());
}

inline SetObject::IteratorKind
SetIteratorObject::kind() const
{
    int32_t i = getSlot(KindSlot).toInt32();
    MOZ_ASSERT(i == SetObject::Values || i == SetObject::Entries);
    return SetObject::IteratorKind(i);
}

/* static */ bool
GlobalObject::initSetIteratorProto(JSContext* cx, Handle<GlobalObject*> global)
{
    Rooted<JSObject*> base(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!base)
        return false;
    RootedPlainObject proto(cx, NewObjectWithGivenProto<PlainObject>(cx, base));
    if (!proto)
        return false;
    if (!JS_DefineFunctions(cx, proto, SetIteratorObject::methods) ||
        !DefineToStringTag(cx, proto, cx->names().SetIterator))
    {
        return false;
    }
    global->setReservedSlot(SET_ITERATOR_PROTO, ObjectValue(*proto));
    return true;
}

SetIteratorObject*
SetIteratorObject::create(JSContext* cx, HandleObject obj, ValueSet* data,
                          SetObject::IteratorKind kind)
{
    MOZ_ASSERT(kind != SetObject::Keys);

    Handle<SetObject*> setobj(obj.as<SetObject>());
    Rooted<GlobalObject*> global(cx, &setobj->global());
    Rooted<JSObject*> proto(cx, GlobalObject::getOrCreateSetIteratorPrototype(cx, global));
    if (!proto)
        return nullptr;

    Nursery& nursery = cx->nursery();

    SetIteratorObject* iterobj;
    void *buffer;
    NewObjectKind objectKind = GenericObject;
    while (true) {
        iterobj = NewObjectWithGivenProto<SetIteratorObject>(cx, proto, objectKind);
        if (!iterobj)
            return nullptr;

        iterobj->setSlot(TargetSlot, ObjectValue(*setobj));
        iterobj->setSlot(RangeSlot, PrivateValue(nullptr));
        iterobj->setSlot(KindSlot, Int32Value(int32_t(kind)));

        const size_t size = JS_ROUNDUP(sizeof(ValueSet::Range), gc::CellAlignBytes);
        buffer = nursery.allocateBufferSameLocation(iterobj, size);
        if (buffer)
            break;

        if (!IsInsideNursery(iterobj)) {
            ReportOutOfMemory(cx);
            return nullptr;
        }

        // There was space in the nursery for the object but not the
        // Range. Try again in the tenured heap.
        MOZ_ASSERT(objectKind == GenericObject);
        objectKind = TenuredObject;
    }

    bool insideNursery = IsInsideNursery(iterobj);
    MOZ_ASSERT(insideNursery == nursery.isInside(buffer));
    if (insideNursery && !HasNurseryMemory(setobj.get())) {
        if (!cx->compartment()->addSetWithNurseryMemory(setobj)) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
        SetHasNurseryMemory(setobj.get(), true);
    }

    auto range = data->createRange(buffer, insideNursery);
    iterobj->setSlot(RangeSlot, PrivateValue(range));

    return iterobj;
}

void
SetIteratorObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());
    MOZ_ASSERT(!IsInsideNursery(obj));

    auto range = SetIteratorObjectRange(&obj->as<NativeObject>());
    MOZ_ASSERT(!obj->zone()->group()->nursery().isInside(range));

    fop->delete_(range);
}

size_t
SetIteratorObject::objectMoved(JSObject* obj, JSObject* old)
{
    if (!IsInsideNursery(old))
        return 0;

    SetIteratorObject* iter = &obj->as<SetIteratorObject>();
    ValueSet::Range* range = SetIteratorObjectRange(iter);
    if (!range)
        return 0;

    Nursery& nursery = iter->zone()->group()->nursery();
    if (!nursery.isInside(range)) {
        nursery.removeMallocedBuffer(range);
        return 0;
    }

    AutoEnterOOMUnsafeRegion oomUnsafe;
    auto newRange = iter->zone()->pod_malloc<ValueSet::Range>();
    if (!newRange)
        oomUnsafe.crash("SetIteratorObject failed to allocate Range data while tenuring.");

    new (newRange) ValueSet::Range(*range);
    range->~Range();
    iter->setReservedSlot(SetIteratorObject::RangeSlot, PrivateValue(newRange));
    return sizeof(ValueSet::Range);
}

bool
SetIteratorObject::next(Handle<SetIteratorObject*> setIterator, HandleArrayObject resultObj,
                        JSContext* cx)
{
    // Check invariants for inlined _GetNextSetEntryForIterator.

    // The array should be tenured, so that post-barrier can be done simply.
    MOZ_ASSERT(resultObj->isTenured());

    // The array elements should be fixed.
    MOZ_ASSERT(resultObj->hasFixedElements());
    MOZ_ASSERT(resultObj->getDenseInitializedLength() == 1);
    MOZ_ASSERT(resultObj->getDenseCapacity() >= 1);

    ValueSet::Range* range = SetIteratorObjectRange(setIterator);
    if (!range)
        return true;

    if (range->empty()) {
        DestroyRange<ValueSet::Range>(setIterator, range);
        setIterator->setReservedSlot(RangeSlot, PrivateValue(nullptr));
        return true;
    }

    resultObj->setDenseElementWithType(cx, 0, range->front().get());
    range->popFront();
    return false;
}

/* static */ JSObject*
SetIteratorObject::createResult(JSContext* cx)
{
    RootedArrayObject resultObj(cx, NewDenseFullyAllocatedArray(cx, 1, nullptr, TenuredObject));
    if (!resultObj)
        return nullptr;

    Rooted<TaggedProto> proto(cx, resultObj->taggedProto());
    ObjectGroup* group = ObjectGroupCompartment::makeGroup(cx, resultObj->getClass(), proto);
    if (!group)
        return nullptr;
    resultObj->setGroup(group);

    resultObj->setDenseInitializedLength(1);
    resultObj->initDenseElement(0, NullValue());

    // See comments in SetIteratorObject::next.
    AddTypePropertyId(cx, resultObj, JSID_VOID, TypeSet::UnknownType());

    return resultObj;
}


/*** Set *****************************************************************************************/

static JSObject*
CreateSetPrototype(JSContext* cx, JSProtoKey key)
{
    return GlobalObject::createBlankPrototype(cx, cx->global(), &SetObject::protoClass_);
}

const ClassOps SetObject::classOps_ = {
    nullptr, // addProperty
    nullptr, // delProperty
    nullptr, // enumerate
    nullptr, // newEnumerate
    nullptr, // resolve
    nullptr, // mayResolve
    finalize,
    nullptr, // call
    nullptr, // hasInstance
    nullptr, // construct
    trace
};

const ClassSpec SetObject::classSpec_ = {
    GenericCreateConstructor<SetObject::construct, 0, gc::AllocKind::FUNCTION>,
    CreateSetPrototype,
    nullptr,
    SetObject::staticProperties,
    SetObject::methods,
    SetObject::properties,
};

const Class SetObject::class_ = {
    "Set",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_RESERVED_SLOTS(SetObject::SlotCount) |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Set) |
    JSCLASS_FOREGROUND_FINALIZE |
    JSCLASS_SKIP_NURSERY_FINALIZE,
    &SetObject::classOps_,
    &SetObject::classSpec_,
};

const Class SetObject::protoClass_ = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Set),
    JS_NULL_CLASS_OPS,
    &SetObject::classSpec_
};

const JSPropertySpec SetObject::properties[] = {
    JS_PSG("size", size, 0),
    JS_STRING_SYM_PS(toStringTag, "Set", JSPROP_READONLY),
    JS_PS_END
};

const JSFunctionSpec SetObject::methods[] = {
    JS_FN("has", has, 1, 0),
    JS_FN("add", add, 1, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("entries", entries, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "SetForEach", 2, 0),
    // SetValues only exists to preseve the equal identity of
    // values, keys and @@iterator.
    JS_SELF_HOSTED_FN("values", "SetValues", 0, 0),
    JS_SELF_HOSTED_FN("keys", "SetValues", 0, 0),
    JS_SELF_HOSTED_SYM_FN(iterator, "SetValues", 0, 0),
    JS_FS_END
};

const JSPropertySpec SetObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "SetSpecies", 0),
    JS_PS_END
};

bool
SetObject::keys(JSContext* cx, HandleObject obj, JS::MutableHandle<GCVector<JS::Value>> keys)
{
    ValueSet* set = obj->as<SetObject>().getData();
    if (!set)
        return false;

    for (ValueSet::Range r = set->all(); !r.empty(); r.popFront()) {
        if (!keys.append(r.front().get()))
            return false;
    }

    return true;
}

bool
SetObject::add(JSContext* cx, HandleObject obj, HandleValue k)
{
    ValueSet* set = obj->as<SetObject>().getData();
    if (!set)
        return false;

    Rooted<HashableValue> key(cx);
    if (!key.setValue(cx, k))
        return false;

    if (!WriteBarrierPost(&obj->as<SetObject>(), key.value()) ||
        !set->put(key))
    {
        ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

SetObject*
SetObject::create(JSContext* cx, HandleObject proto /* = nullptr */)
{
    auto set = cx->make_unique<ValueSet>(cx->zone(),
                                         cx->compartment()->randomHashCodeScrambler());
    if (!set || !set->init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    SetObject* obj = NewObjectWithClassProto<SetObject>(cx, proto);
    if (!obj)
        return nullptr;

    bool insideNursery = IsInsideNursery(obj);
    if (insideNursery && !cx->compartment()->addSetWithNurseryMemory(obj)) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    obj->initPrivate(set.release());
    obj->initReservedSlot(NurseryKeysSlot, PrivateValue(nullptr));
    obj->initReservedSlot(HasNurseryMemorySlot, JS::BooleanValue(insideNursery));
    return obj;
}

void
SetObject::trace(JSTracer* trc, JSObject* obj)
{
    SetObject* setobj = static_cast<SetObject*>(obj);
    if (ValueSet* set = setobj->getData()) {
        for (ValueSet::Range r = set->all(); !r.empty(); r.popFront())
            TraceKey(r, r.front(), trc);
    }
}

void
SetObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());
    SetObject* setobj = static_cast<SetObject*>(obj);
    if (ValueSet* set = setobj->getData())
        fop->delete_(set);
}

/* static */ void
SetObject::sweepAfterMinorGC(FreeOp* fop, SetObject* setobj)
{
    if (IsInsideNursery(setobj) && !IsForwarded(setobj)) {
        finalize(fop, setobj);
        return;
    }

    setobj = MaybeForwarded(setobj);
    setobj->getData()->destroyNurseryRanges();
    SetHasNurseryMemory(setobj, false);
}

bool
SetObject::isBuiltinAdd(HandleValue add)
{
    return IsNativeFunction(add, SetObject::add);
}

bool
SetObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Set"))
        return false;

    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
        return false;

    Rooted<SetObject*> obj(cx, SetObject::create(cx, proto));
    if (!obj)
        return false;

    if (!args.get(0).isNullOrUndefined()) {
        RootedValue iterable(cx, args[0]);
        bool optimized = false;
        if (!IsOptimizableInitForSet<GlobalObject::getOrCreateSetPrototype, isBuiltinAdd>(cx, obj, iterable, &optimized))
            return false;

        if (optimized) {
            RootedValue keyVal(cx);
            Rooted<HashableValue> key(cx);
            ValueSet* set = obj->getData();
            ArrayObject* array = &iterable.toObject().as<ArrayObject>();
            for (uint32_t index = 0; index < array->getDenseInitializedLength(); ++index) {
                keyVal.set(array->getDenseElement(index));
                MOZ_ASSERT(!keyVal.isMagic(JS_ELEMENTS_HOLE));

                if (!key.setValue(cx, keyVal))
                    return false;
                if (!WriteBarrierPost(obj, keyVal) ||
                    !set->put(key))
                {
                    ReportOutOfMemory(cx);
                    return false;
                }
            }
        } else {
            FixedInvokeArgs<1> args2(cx);
            args2[0].set(args[0]);

            RootedValue thisv(cx, ObjectValue(*obj));
            if (!CallSelfHostedFunction(cx, cx->names().SetConstructorInit, thisv, args2, args2.rval()))
                return false;
        }
    }

    args.rval().setObject(*obj);
    return true;
}

bool
SetObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().hasClass(&class_) && v.toObject().as<SetObject>().getPrivate();
}

bool
SetObject::is(HandleObject o)
{
    return o->hasClass(&class_) && o->as<SetObject>().getPrivate();
}

ValueSet &
SetObject::extract(HandleObject o)
{
    MOZ_ASSERT(o->hasClass(&SetObject::class_));
    return *o->as<SetObject>().getData();
}

ValueSet &
SetObject::extract(const CallArgs& args)
{
    MOZ_ASSERT(args.thisv().isObject());
    MOZ_ASSERT(args.thisv().toObject().hasClass(&SetObject::class_));
    return *static_cast<SetObject&>(args.thisv().toObject()).getData();
}

uint32_t
SetObject::size(JSContext *cx, HandleObject obj)
{
    MOZ_ASSERT(SetObject::is(obj));
    ValueSet &set = extract(obj);
    static_assert(sizeof(set.count()) <= sizeof(uint32_t),
                  "set count must be precisely representable as a JS number");
    return set.count();
}

bool
SetObject::size_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    ValueSet& set = extract(args);
    static_assert(sizeof(set.count()) <= sizeof(uint32_t),
                  "set count must be precisely representable as a JS number");
    args.rval().setNumber(set.count());
    return true;
}

bool
SetObject::size(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::size_impl>(cx, args);
}

bool
SetObject::has_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    ValueSet& set = extract(args);
    ARG0_KEY(cx, args, key);
    args.rval().setBoolean(set.has(key));
    return true;
}

bool
SetObject::has(JSContext *cx, HandleObject obj, HandleValue key, bool *rval)
{
    MOZ_ASSERT(SetObject::is(obj));

    ValueSet &set = extract(obj);
    Rooted<HashableValue> k(cx);

    if (!k.setValue(cx, key))
        return false;

    *rval = set.has(k);
    return true;
}

bool
SetObject::has(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::has_impl>(cx, args);
}

bool
SetObject::add_impl(JSContext* cx, const CallArgs& args)
{
    MOZ_ASSERT(is(args.thisv()));

    ValueSet& set = extract(args);
    ARG0_KEY(cx, args, key);
    if (!WriteBarrierPost(&args.thisv().toObject().as<SetObject>(), key.value()) ||
        !set.put(key))
    {
        ReportOutOfMemory(cx);
        return false;
    }
    args.rval().set(args.thisv());
    return true;
}

bool
SetObject::add(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::add_impl>(cx, args);
}

bool
SetObject::delete_(JSContext *cx, HandleObject obj, HandleValue key, bool *rval)
{
    MOZ_ASSERT(SetObject::is(obj));

    ValueSet &set = extract(obj);
    Rooted<HashableValue> k(cx);

    if (!k.setValue(cx, key))
        return false;

    if (!set.remove(k, rval)) {
        ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

bool
SetObject::delete_impl(JSContext *cx, const CallArgs& args)
{
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

bool
SetObject::delete_(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<SetObject::is, SetObject::delete_impl>(cx, args);
}

bool
SetObject::iterator(JSContext *cx, IteratorKind kind,
                    HandleObject obj, MutableHandleValue iter)
{
    MOZ_ASSERT(SetObject::is(obj));
    ValueSet &set = extract(obj);
    Rooted<JSObject*> iterobj(cx, SetIteratorObject::create(cx, obj, &set, kind));
    if (!iterobj)
        return false;
    iter.setObject(*iterobj);
    return true;
}

bool
SetObject::iterator_impl(JSContext *cx, const CallArgs& args, IteratorKind kind)
{
    Rooted<SetObject*> setobj(cx, &args.thisv().toObject().as<SetObject>());
    ValueSet& set = *setobj->getData();
    Rooted<JSObject*> iterobj(cx, SetIteratorObject::create(cx, setobj, &set, kind));
    if (!iterobj)
        return false;
    args.rval().setObject(*iterobj);
    return true;
}

bool
SetObject::values_impl(JSContext* cx, const CallArgs& args)
{
    return iterator_impl(cx, args, Values);
}

bool
SetObject::values(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, values_impl, args);
}

bool
SetObject::entries_impl(JSContext* cx, const CallArgs& args)
{
    return iterator_impl(cx, args, Entries);
}

bool
SetObject::entries(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, entries_impl, args);
}

bool
SetObject::clear(JSContext *cx, HandleObject obj)
{
    MOZ_ASSERT(SetObject::is(obj));
    ValueSet &set = extract(obj);
    if (!set.clear()) {
        ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

bool
SetObject::clear_impl(JSContext *cx, const CallArgs& args)
{
    Rooted<SetObject*> setobj(cx, &args.thisv().toObject().as<SetObject>());
    if (!setobj->getData()->clear()) {
        ReportOutOfMemory(cx);
        return false;
    }
    args.rval().setUndefined();
    return true;
}

bool
SetObject::clear(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, clear_impl, args);
}

/*** JS static utility functions *********************************************/

static bool
forEach(const char* funcName, JSContext *cx, HandleObject obj, HandleValue callbackFn, HandleValue thisArg)
{
    CHECK_REQUEST(cx);

    RootedId forEachId(cx, NameToId(cx->names().forEach));
    RootedFunction forEachFunc(cx, JS::GetSelfHostedFunction(cx, funcName, forEachId, 2));
    if (!forEachFunc)
        return false;

    RootedValue fval(cx, ObjectValue(*forEachFunc));
    return Call(cx, fval, obj, callbackFn, thisArg, &fval);
}

// Handles Clear/Size for public jsapi map/set access
template<typename RetT>
RetT
CallObjFunc(RetT(*ObjFunc)(JSContext*, HandleObject), JSContext* cx, HandleObject obj)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    // Always unwrap, in case this is an xray or cross-compartment wrapper.
    RootedObject unwrappedObj(cx);
    unwrappedObj = UncheckedUnwrap(obj);

    // Enter the compartment of the backing object before calling functions on
    // it.
    JSAutoCompartment ac(cx, unwrappedObj);
    return ObjFunc(cx, unwrappedObj);
}

// Handles Has/Delete for public jsapi map/set access
bool
CallObjFunc(bool(*ObjFunc)(JSContext *cx, HandleObject obj, HandleValue key, bool *rval),
            JSContext *cx, HandleObject obj, HandleValue key, bool *rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, key);

    // Always unwrap, in case this is an xray or cross-compartment wrapper.
    RootedObject unwrappedObj(cx);
    unwrappedObj = UncheckedUnwrap(obj);
    JSAutoCompartment ac(cx, unwrappedObj);

    // If we're working with a wrapped map/set, rewrap the key into the
    // compartment of the unwrapped map/set.
    RootedValue wrappedKey(cx, key);
    if (obj != unwrappedObj) {
        if (!JS_WrapValue(cx, &wrappedKey))
            return false;
    }
    return ObjFunc(cx, unwrappedObj, wrappedKey, rval);
}

// Handles iterator generation for public jsapi map/set access
template<typename Iter>
bool
CallObjFunc(bool(*ObjFunc)(JSContext* cx, Iter kind,
                           HandleObject obj, MutableHandleValue iter),
            JSContext *cx, Iter iterType, HandleObject obj, MutableHandleValue rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    // Always unwrap, in case this is an xray or cross-compartment wrapper.
    RootedObject unwrappedObj(cx);
    unwrappedObj = UncheckedUnwrap(obj);
    {
        // Retrieve the iterator while in the unwrapped map/set's compartment,
        // otherwise we'll crash on a compartment assert.
        JSAutoCompartment ac(cx, unwrappedObj);
        if (!ObjFunc(cx, iterType, unwrappedObj, rval))
            return false;
    }

    // If the caller is in a different compartment than the map/set, rewrap the
    // iterator object into the caller's compartment.
    if (obj != unwrappedObj) {
        if (!JS_WrapValue(cx, rval))
            return false;
    }
    return true;
}

/*** JS public APIs **********************************************************/

JS_PUBLIC_API(JSObject*)
JS::NewMapObject(JSContext* cx)
{
    return MapObject::create(cx);
}

JS_PUBLIC_API(uint32_t)
JS::MapSize(JSContext* cx, HandleObject obj)
{
    return CallObjFunc<uint32_t>(&MapObject::size, cx, obj);
}

JS_PUBLIC_API(bool)
JS::MapGet(JSContext* cx, HandleObject obj, HandleValue key, MutableHandleValue rval)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, key, rval);

    // Unwrap the object, and enter its compartment. If object isn't wrapped,
    // this is essentially a noop.
    RootedObject unwrappedObj(cx);
    unwrappedObj = UncheckedUnwrap(obj);
    {
        JSAutoCompartment ac(cx, unwrappedObj);
        RootedValue wrappedKey(cx, key);

        // If we passed in a wrapper, wrap our key into its compartment now.
        if (obj != unwrappedObj) {
            if (!JS_WrapValue(cx, &wrappedKey))
                return false;
        }
        if (!MapObject::get(cx, unwrappedObj, wrappedKey, rval))
            return false;
    }

    // If we passed in a wrapper, wrap our return value on the way out.
    if (obj != unwrappedObj) {
        if (!JS_WrapValue(cx, rval))
            return false;
    }
    return true;
}

JS_PUBLIC_API(bool)
JS::MapSet(JSContext *cx, HandleObject obj, HandleValue key, HandleValue val)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, key, val);

    // Unwrap the object, and enter its compartment. If object isn't wrapped,
    // this is essentially a noop.
    RootedObject unwrappedObj(cx);
    unwrappedObj = UncheckedUnwrap(obj);
    {
        JSAutoCompartment ac(cx, unwrappedObj);

        // If we passed in a wrapper, wrap both key and value before adding to
        // the map
        RootedValue wrappedKey(cx, key);
        RootedValue wrappedValue(cx, val);
        if (obj != unwrappedObj) {
            if (!JS_WrapValue(cx, &wrappedKey) ||
                !JS_WrapValue(cx, &wrappedValue)) {
                return false;
            }
        }
        return MapObject::set(cx, unwrappedObj, wrappedKey, wrappedValue);
    }
}

JS_PUBLIC_API(bool)
JS::MapHas(JSContext* cx, HandleObject obj, HandleValue key, bool* rval)
{
    return CallObjFunc(MapObject::has, cx, obj, key, rval);
}

JS_PUBLIC_API(bool)
JS::MapDelete(JSContext *cx, HandleObject obj, HandleValue key, bool* rval)
{
    return CallObjFunc(MapObject::delete_, cx, obj, key, rval);
}

JS_PUBLIC_API(bool)
JS::MapClear(JSContext* cx, HandleObject obj)
{
    return CallObjFunc(&MapObject::clear, cx, obj);
}

JS_PUBLIC_API(bool)
JS::MapKeys(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    return CallObjFunc(&MapObject::iterator, cx, MapObject::Keys, obj, rval);
}

JS_PUBLIC_API(bool)
JS::MapValues(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    return CallObjFunc(&MapObject::iterator, cx, MapObject::Values, obj, rval);
}

JS_PUBLIC_API(bool)
JS::MapEntries(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    return CallObjFunc(&MapObject::iterator, cx, MapObject::Entries, obj, rval);
}

JS_PUBLIC_API(bool)
JS::MapForEach(JSContext *cx, HandleObject obj, HandleValue callbackFn, HandleValue thisVal)
{
    return forEach("MapForEach", cx, obj, callbackFn, thisVal);
}

JS_PUBLIC_API(JSObject *)
JS::NewSetObject(JSContext *cx)
{
    return SetObject::create(cx);
}

JS_PUBLIC_API(uint32_t)
JS::SetSize(JSContext *cx, HandleObject obj)
{
    return CallObjFunc<uint32_t>(&SetObject::size, cx, obj);
}

JS_PUBLIC_API(bool)
JS::SetAdd(JSContext *cx, HandleObject obj, HandleValue key)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, key);

    // Unwrap the object, and enter its compartment. If object isn't wrapped,
    // this is essentially a noop.
    RootedObject unwrappedObj(cx);
    unwrappedObj = UncheckedUnwrap(obj);
    {
        JSAutoCompartment ac(cx, unwrappedObj);

        // If we passed in a wrapper, wrap key before adding to the set
        RootedValue wrappedKey(cx, key);
        if (obj != unwrappedObj) {
            if (!JS_WrapValue(cx, &wrappedKey))
                return false;
        }
        return SetObject::add(cx, unwrappedObj, wrappedKey);
    }
}

JS_PUBLIC_API(bool)
JS::SetHas(JSContext* cx, HandleObject obj, HandleValue key, bool* rval)
{
    return CallObjFunc(SetObject::has, cx, obj, key, rval);
}

JS_PUBLIC_API(bool)
JS::SetDelete(JSContext *cx, HandleObject obj, HandleValue key, bool *rval)
{
    return CallObjFunc(SetObject::delete_, cx, obj, key, rval);
}

JS_PUBLIC_API(bool)
JS::SetClear(JSContext* cx, HandleObject obj)
{
    return CallObjFunc(&SetObject::clear, cx, obj);
}

JS_PUBLIC_API(bool)
JS::SetKeys(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    return SetValues(cx, obj, rval);
}

JS_PUBLIC_API(bool)
JS::SetValues(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    return CallObjFunc(&SetObject::iterator, cx, SetObject::Values, obj, rval);
}

JS_PUBLIC_API(bool)
JS::SetEntries(JSContext* cx, HandleObject obj, MutableHandleValue rval)
{
    return CallObjFunc(&SetObject::iterator, cx, SetObject::Entries, obj, rval);
}

JS_PUBLIC_API(bool)
JS::SetForEach(JSContext *cx, HandleObject obj, HandleValue callbackFn, HandleValue thisVal)
{
    return forEach("SetForEach", cx, obj, callbackFn, thisVal);
}

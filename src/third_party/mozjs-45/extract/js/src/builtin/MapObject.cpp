/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/MapObject.h"

#include "jscntxt.h"
#include "jsiter.h"
#include "jsobj.h"

#include "ds/OrderedHashTable.h"
#include "gc/Marking.h"
#include "js/Utility.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"

#include "jsobjinlines.h"

#include "vm/Interpreter-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::IsNaN;
using mozilla::NumberEqualsInt32;

using JS::DoubleNaNValue;
using JS::ForOfIterator;


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

HashNumber
HashableValue::hash() const
{
    // HashableValue::setValue normalizes values so that the SameValue relation
    // on HashableValues is the same as the == relationship on
    // value.data.asBits.
    return value.asRawBits();
}

bool
HashableValue::operator==(const HashableValue& other) const
{
    // Two HashableValues are equal if they have equal bits.
    bool b = (value.asRawBits() == other.value.asRawBits());

#ifdef DEBUG
    bool same;
    PerThreadData* data = TlsPerThreadData.get();
    RootedValue valueRoot(data, value);
    RootedValue otherRoot(data, other.value);
    MOZ_ASSERT(SameValue(nullptr, valueRoot, otherRoot, &same));
    MOZ_ASSERT(same == b);
#endif
    return b;
}

HashableValue
HashableValue::mark(JSTracer* trc) const
{
    HashableValue hv(*this);
    TraceEdge(trc, &hv.value, "key");
    return hv;
}


/*** MapIterator *********************************************************************************/

namespace {

} /* anonymous namespace */

const Class MapIteratorObject::class_ = {
    "Map Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(MapIteratorObject::SlotCount),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    MapIteratorObject::finalize
};

const JSFunctionSpec MapIteratorObject::methods[] = {
    JS_SELF_HOSTED_FN("next", "MapIteratorNext", 0, 0),
    JS_FS_END
};

static inline ValueMap::Range*
MapIteratorObjectRange(NativeObject* obj)
{
    MOZ_ASSERT(obj->is<MapIteratorObject>());
    return static_cast<ValueMap::Range*>(obj->getSlot(MapIteratorObject::RangeSlot).toPrivate());
}

inline MapObject::IteratorKind
MapIteratorObject::kind() const
{
    int32_t i = getSlot(KindSlot).toInt32();
    MOZ_ASSERT(i == MapObject::Keys || i == MapObject::Values || i == MapObject::Entries);
    return MapObject::IteratorKind(i);
}

bool
GlobalObject::initMapIteratorProto(JSContext* cx, Handle<GlobalObject*> global)
{
    Rooted<JSObject*> base(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!base)
        return false;
    RootedPlainObject proto(cx, NewObjectWithGivenProto<PlainObject>(cx, base));
    if (!proto)
        return false;
    if (!JS_DefineFunctions(cx, proto, MapIteratorObject::methods))
        return false;
    global->setReservedSlot(MAP_ITERATOR_PROTO, ObjectValue(*proto));
    return true;
}

MapIteratorObject*
MapIteratorObject::create(JSContext* cx, HandleObject mapobj, ValueMap* data,
                          MapObject::IteratorKind kind)
{
    Rooted<GlobalObject*> global(cx, &mapobj->global());
    Rooted<JSObject*> proto(cx, GlobalObject::getOrCreateMapIteratorPrototype(cx, global));
    if (!proto)
        return nullptr;

    ValueMap::Range* range = cx->new_<ValueMap::Range>(data->all());
    if (!range)
        return nullptr;

    MapIteratorObject* iterobj = NewObjectWithGivenProto<MapIteratorObject>(cx, proto);
    if (!iterobj) {
        js_delete(range);
        return nullptr;
    }
    iterobj->setSlot(TargetSlot, ObjectValue(*mapobj));
    iterobj->setSlot(RangeSlot, PrivateValue(range));
    iterobj->setSlot(KindSlot, Int32Value(int32_t(kind)));
    return iterobj;
}

void
MapIteratorObject::finalize(FreeOp* fop, JSObject* obj)
{
    fop->delete_(MapIteratorObjectRange(static_cast<NativeObject*>(obj)));
}

bool
MapIteratorObject::next(JSContext* cx, Handle<MapIteratorObject*> mapIterator,
                        HandleArrayObject resultPairObj)
{
    MOZ_ASSERT(resultPairObj->getDenseInitializedLength() == 2);

    ValueMap::Range* range = MapIteratorObjectRange(mapIterator);
    if (!range || range->empty()) {
        js_delete(range);
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


/*** Map *****************************************************************************************/

const Class MapObject::class_ = {
    "Map",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Map),
    nullptr, // addProperty
    nullptr, // delProperty
    nullptr, // getProperty
    nullptr, // setProperty
    nullptr, // enumerate
    nullptr, // resolve
    nullptr, // mayResolve
    finalize,
    nullptr, // call
    nullptr, // hasInstance
    nullptr, // construct
    mark
};

const JSPropertySpec MapObject::properties[] = {
    JS_PSG("size", size, 0),
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
    JS_FS_END
};

const JSPropertySpec MapObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "MapSpecies", 0),
    JS_PS_END
};

static JSObject*
InitClass(JSContext* cx, Handle<GlobalObject*> global, const Class* clasp, JSProtoKey key, Native construct,
          const JSPropertySpec* properties, const JSFunctionSpec* methods,
          const JSPropertySpec* staticProperties)
{
    RootedPlainObject proto(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!proto)
        return nullptr;

    Rooted<JSFunction*> ctor(cx, global->createConstructor(cx, construct, ClassName(key, cx), 0));
    if (!ctor ||
        !JS_DefineProperties(cx, ctor, staticProperties) ||
        !LinkConstructorAndPrototype(cx, ctor, proto) ||
        !DefinePropertiesAndFunctions(cx, proto, properties, methods) ||
        !GlobalObject::initBuiltinConstructor(cx, global, key, ctor, proto))
    {
        return nullptr;
    }
    return proto;
}

JSObject*
MapObject::initClass(JSContext* cx, JSObject* obj)
{
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    RootedObject proto(cx,
        InitClass(cx, global, &class_, JSProto_Map, construct, properties, methods,
                  staticProperties));
    if (proto) {
        // Define the "entries" method.
        JSFunction* fun = JS_DefineFunction(cx, proto, "entries", entries, 0, 0);
        if (!fun)
            return nullptr;

        // Define its alias.
        RootedValue funval(cx, ObjectValue(*fun));
        RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
        if (!JS_DefinePropertyById(cx, proto, iteratorId, funval, 0))
            return nullptr;
    }
    return proto;
}

template <class Range>
static void
MarkKey(Range& r, const HashableValue& key, JSTracer* trc)
{
    HashableValue newKey = key.mark(trc);

    if (newKey.get() != key.get()) {
        // The hash function only uses the bits of the Value, so it is safe to
        // rekey even when the object or string has been modified by the GC.
        r.rekeyFront(newKey);
    }
}

void
MapObject::mark(JSTracer* trc, JSObject* obj)
{
    if (ValueMap* map = obj->as<MapObject>().getData()) {
        for (ValueMap::Range r = map->all(); !r.empty(); r.popFront()) {
            MarkKey(r, r.front().key, trc);
            TraceEdge(trc, &r.front().value, "value");
        }
    }
}

struct UnbarrieredHashPolicy {
    typedef Value Lookup;
    static HashNumber hash(const Lookup& v) { return v.asRawBits(); }
    static bool match(const Value& k, const Lookup& l) { return k == l; }
    static bool isEmpty(const Value& v) { return v.isMagic(JS_HASH_KEY_EMPTY); }
    static void makeEmpty(Value* vp) { vp->setMagic(JS_HASH_KEY_EMPTY); }
};

template <typename TableType>
class OrderedHashTableRef : public gc::BufferableRef
{
    TableType* table;
    Value key;

  public:
    explicit OrderedHashTableRef(TableType* t, const Value& k) : table(t), key(k) {}

    void trace(JSTracer* trc) override {
        MOZ_ASSERT(UnbarrieredHashPolicy::hash(key) ==
                   HashableValue::Hasher::hash(*reinterpret_cast<HashableValue*>(&key)));
        Value prior = key;
        TraceManuallyBarrieredEdge(trc, &key, "ordered hash table key");
        table->rekeyOneEntry(prior, key);
    }
};

inline static void
WriteBarrierPost(JSRuntime* rt, ValueMap* map, const Value& key)
{
    typedef OrderedHashMap<Value, Value, UnbarrieredHashPolicy, RuntimeAllocPolicy> UnbarrieredMap;
    if (MOZ_UNLIKELY(key.isObject() && IsInsideNursery(&key.toObject()))) {
        rt->gc.storeBuffer.putGeneric(OrderedHashTableRef<UnbarrieredMap>(
                    reinterpret_cast<UnbarrieredMap*>(map), key));
    }
}

inline static void
WriteBarrierPost(JSRuntime* rt, ValueSet* set, const Value& key)
{
    typedef OrderedHashSet<Value, UnbarrieredHashPolicy, RuntimeAllocPolicy> UnbarrieredSet;
    if (MOZ_UNLIKELY(key.isObject() && IsInsideNursery(&key.toObject()))) {
        rt->gc.storeBuffer.putGeneric(OrderedHashTableRef<UnbarrieredSet>(
                    reinterpret_cast<UnbarrieredSet*>(set), key));
    }
}

bool
MapObject::getKeysAndValuesInterleaved(JSContext* cx, HandleObject obj,
                                       JS::AutoValueVector* entries)
{
    ValueMap* map = obj->as<MapObject>().getData();
    if (!map)
        return false;

    for (ValueMap::Range r = map->all(); !r.empty(); r.popFront()) {
        if (!entries->append(r.front().key.get()) ||
            !entries->append(r.front().value))
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

    RelocatableValue rval(v);
    if (!map->put(key, rval)) {
        ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), map, key.value());
    return true;
}

MapObject*
MapObject::create(JSContext* cx, HandleObject proto /* = nullptr */)
{
    auto map = cx->make_unique<ValueMap>(cx->runtime());
    if (!map || !map->init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    MapObject* mapObj = NewObjectWithClassProto<MapObject>(cx,  proto);
    if (!mapObj)
        return nullptr;

    mapObj->setPrivate(map.release());
    return mapObj;
}

void
MapObject::finalize(FreeOp* fop, JSObject* obj)
{
    if (ValueMap* map = obj->as<MapObject>().getData())
        fop->delete_(map);
}

bool
MapObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Map"))
        return false;

    RootedObject proto(cx);
    RootedObject newTarget(cx, &args.newTarget().toObject());
    if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
        return false;

    Rooted<MapObject*> obj(cx, MapObject::create(cx, proto));
    if (!obj)
        return false;

    if (!args.get(0).isNullOrUndefined()) {
        RootedValue adderVal(cx);
        if (!GetProperty(cx, obj, obj, cx->names().set, &adderVal))
            return false;

        if (!IsCallable(adderVal))
            return ReportIsNotFunction(cx, adderVal);

        bool isOriginalAdder = IsNativeFunction(adderVal, MapObject::set);
        RootedValue mapVal(cx, ObjectValue(*obj));
        FastInvokeGuard fig(cx, adderVal);
        InvokeArgs& args2 = fig.args();

        ForOfIterator iter(cx);
        if (!iter.init(args[0]))
            return false;
        RootedValue pairVal(cx);
        RootedObject pairObj(cx);
        Rooted<HashableValue> hkey(cx);
        ValueMap* map = obj->getData();
        while (true) {
            bool done;
            if (!iter.next(&pairVal, &done))
                return false;
            if (done)
                break;
            if (!pairVal.isObject()) {
                JS_ReportErrorNumber(cx, GetErrorMessage, nullptr,
                                     JSMSG_INVALID_MAP_ITERABLE, "Map");
                return false;
            }

            pairObj = &pairVal.toObject();
            if (!pairObj)
                return false;

            RootedValue key(cx);
            if (!GetElement(cx, pairObj, pairObj, 0, &key))
                return false;

            RootedValue val(cx);
            if (!GetElement(cx, pairObj, pairObj, 1, &val))
                return false;

            if (isOriginalAdder) {
                if (!hkey.setValue(cx, key))
                    return false;

                RelocatableValue rval(val);
                if (!map->put(hkey, rval)) {
                    ReportOutOfMemory(cx);
                    return false;
                }
                WriteBarrierPost(cx->runtime(), map, key);
            } else {
                if (!args2.init(2))
                    return false;

                args2.setCallee(adderVal);
                args2.setThis(mapVal);
                args2[0].set(key);
                args2[1].set(val);

                if (!fig.invoke(cx))
                    return false;
            }
        }
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
    Rooted<HashableValue> key(cx);                                          \
    if (args.length() > 0 && !key.setValue(cx, args[0]))                      \
        return false

ValueMap&
MapObject::extract(HandleObject o)
{
    MOZ_ASSERT(o->hasClass(&MapObject::class_));
    return *o->as<MapObject>().getData();
}

ValueMap&
MapObject::extract(CallReceiver call)
{
    MOZ_ASSERT(call.thisv().isObject());
    MOZ_ASSERT(call.thisv().toObject().hasClass(&MapObject::class_));
    return *call.thisv().toObject().as<MapObject>().getData();
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
    RelocatableValue rval(args.get(1));
    if (!map.put(key, rval)) {
        ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), &map, key.value());
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
    // MapObject::mark does not mark deleted entries. Incremental GC therefore
    // requires that no RelocatableValue objects pointing to heap values be
    // left alive in the ValueMap.
    //
    // OrderedHashMap::remove() doesn't destroy the removed entry. It merely
    // calls OrderedHashMap::MapOps::makeEmpty. But that is sufficient, because
    // makeEmpty clears the value by doing e->value = Value(), and in the case
    // of a ValueMap, Value() means RelocatableValue(), which is the same as
    // RelocatableValue(UndefinedValue()).
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
    return iterobj && (iter.setObject(*iterobj), true);
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

JSObject*
js::InitMapClass(JSContext* cx, HandleObject obj)
{
    return MapObject::initClass(cx, obj);
}


/*** SetIterator *********************************************************************************/

namespace {

class SetIteratorObject : public NativeObject
{
  public:
    static const Class class_;

    enum { TargetSlot, KindSlot, RangeSlot, SlotCount };
    static const JSFunctionSpec methods[];
    static SetIteratorObject* create(JSContext* cx, HandleObject setobj, ValueSet* data,
                                     SetObject::IteratorKind kind);
    static bool next(JSContext* cx, unsigned argc, Value* vp);
    static void finalize(FreeOp* fop, JSObject* obj);

  private:
    static inline bool is(HandleValue v);
    inline ValueSet::Range* range();
    inline SetObject::IteratorKind kind() const;
    static bool next_impl(JSContext* cx, const CallArgs& args);
};

} /* anonymous namespace */

const Class SetIteratorObject::class_ = {
    "Set Iterator",
    JSCLASS_HAS_RESERVED_SLOTS(SetIteratorObject::SlotCount),
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    SetIteratorObject::finalize
};

const JSFunctionSpec SetIteratorObject::methods[] = {
    JS_FN("next", next, 0, 0),
    JS_FS_END
};

inline ValueSet::Range*
SetIteratorObject::range()
{
    return static_cast<ValueSet::Range*>(getSlot(RangeSlot).toPrivate());
}

inline SetObject::IteratorKind
SetIteratorObject::kind() const
{
    int32_t i = getSlot(KindSlot).toInt32();
    MOZ_ASSERT(i == SetObject::Values || i == SetObject::Entries);
    return SetObject::IteratorKind(i);
}

bool
GlobalObject::initSetIteratorProto(JSContext* cx, Handle<GlobalObject*> global)
{
    Rooted<JSObject*> base(cx, GlobalObject::getOrCreateIteratorPrototype(cx, global));
    if (!base)
        return false;
    RootedPlainObject proto(cx, NewObjectWithGivenProto<PlainObject>(cx, base));
    if (!proto)
        return false;
    if (!JS_DefineFunctions(cx, proto, SetIteratorObject::methods))
        return false;
    global->setReservedSlot(SET_ITERATOR_PROTO, ObjectValue(*proto));
    return true;
}

SetIteratorObject*
SetIteratorObject::create(JSContext* cx, HandleObject setobj, ValueSet* data,
                          SetObject::IteratorKind kind)
{
    Rooted<GlobalObject*> global(cx, &setobj->global());
    Rooted<JSObject*> proto(cx, GlobalObject::getOrCreateSetIteratorPrototype(cx, global));
    if (!proto)
        return nullptr;

    ValueSet::Range* range = cx->new_<ValueSet::Range>(data->all());
    if (!range)
        return nullptr;

    SetIteratorObject* iterobj = NewObjectWithGivenProto<SetIteratorObject>(cx, proto);
    if (!iterobj) {
        js_delete(range);
        return nullptr;
    }
    iterobj->setSlot(TargetSlot, ObjectValue(*setobj));
    iterobj->setSlot(KindSlot, Int32Value(int32_t(kind)));
    iterobj->setSlot(RangeSlot, PrivateValue(range));
    return iterobj;
}

void
SetIteratorObject::finalize(FreeOp* fop, JSObject* obj)
{
    fop->delete_(obj->as<SetIteratorObject>().range());
}

bool
SetIteratorObject::is(HandleValue v)
{
    return v.isObject() && v.toObject().is<SetIteratorObject>();
}

bool
SetIteratorObject::next_impl(JSContext* cx, const CallArgs& args)
{
    SetIteratorObject& thisobj = args.thisv().toObject().as<SetIteratorObject>();
    ValueSet::Range* range = thisobj.range();
    RootedValue value(cx);
    bool done;

    if (!range || range->empty()) {
        js_delete(range);
        thisobj.setReservedSlot(RangeSlot, PrivateValue(nullptr));
        value.setUndefined();
        done = true;
    } else {
        switch (thisobj.kind()) {
          case SetObject::Values:
            value = range->front().get();
            break;

          case SetObject::Entries: {
            JS::AutoValueArray<2> pair(cx);
            pair[0].set(range->front().get());
            pair[1].set(range->front().get());

            JSObject* pairObj = NewDenseCopiedArray(cx, 2, pair.begin());
            if (!pairObj)
              return false;
            value.setObject(*pairObj);
            break;
          }
        }
        range->popFront();
        done = false;
    }

    RootedObject result(cx, CreateItrResultObject(cx, value, done));
    if (!result)
        return false;
    args.rval().setObject(*result);

    return true;
}

bool
SetIteratorObject::next(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, is, next_impl, args);
}


/*** Set *****************************************************************************************/

const Class SetObject::class_ = {
    "Set",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_HAS_CACHED_PROTO(JSProto_Set),
    nullptr, // addProperty
    nullptr, // delProperty
    nullptr, // getProperty
    nullptr, // setProperty
    nullptr, // enumerate
    nullptr, // resolve
    nullptr, // mayResolve
    finalize,
    nullptr, // call
    nullptr, // hasInstance
    nullptr, // construct
    mark
};

const JSPropertySpec SetObject::properties[] = {
    JS_PSG("size", size, 0),
    JS_PS_END
};

const JSFunctionSpec SetObject::methods[] = {
    JS_FN("has", has, 1, 0),
    JS_FN("add", add, 1, 0),
    JS_FN("delete", delete_, 1, 0),
    JS_FN("entries", entries, 0, 0),
    JS_FN("clear", clear, 0, 0),
    JS_SELF_HOSTED_FN("forEach", "SetForEach", 2, 0),
    JS_FS_END
};

const JSPropertySpec SetObject::staticProperties[] = {
    JS_SELF_HOSTED_SYM_GET(species, "SetSpecies", 0),
    JS_PS_END
};

JSObject*
SetObject::initClass(JSContext* cx, JSObject* obj)
{
    Rooted<GlobalObject*> global(cx, &obj->as<GlobalObject>());
    RootedObject proto(cx,
        InitClass(cx, global, &class_, JSProto_Set, construct, properties, methods,
                  staticProperties));
    if (proto) {
        // Define the "values" method.
        JSFunction* fun = JS_DefineFunction(cx, proto, "values", values, 0, 0);
        if (!fun)
            return nullptr;

        // Define its aliases.
        RootedValue funval(cx, ObjectValue(*fun));
        if (!JS_DefineProperty(cx, proto, "keys", funval, 0))
            return nullptr;

        RootedId iteratorId(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().iterator));
        if (!JS_DefinePropertyById(cx, proto, iteratorId, funval, 0))
            return nullptr;
    }
    return proto;
}


bool
SetObject::keys(JSContext* cx, HandleObject obj, JS::AutoValueVector* keys)
{
    ValueSet* set = obj->as<SetObject>().getData();
    if (!set)
        return false;

    for (ValueSet::Range r = set->all(); !r.empty(); r.popFront()) {
        if (!keys->append(r.front().get()))
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

    if (!set->put(key)) {
        ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), set, key.value());
    return true;
}

SetObject*
SetObject::create(JSContext* cx, HandleObject proto /* = nullptr */)
{
    auto set = cx->make_unique<ValueSet>(cx->runtime());
    if (!set || !set->init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    SetObject* obj = NewObjectWithClassProto<SetObject>(cx, proto);
    if (!obj)
        return nullptr;

    obj->setPrivate(set.release());
    return obj;
}

void
SetObject::mark(JSTracer* trc, JSObject* obj)
{
    SetObject* setobj = static_cast<SetObject*>(obj);
    if (ValueSet* set = setobj->getData()) {
        for (ValueSet::Range r = set->all(); !r.empty(); r.popFront())
            MarkKey(r, r.front(), trc);
    }
}

void
SetObject::finalize(FreeOp* fop, JSObject* obj)
{
    SetObject* setobj = static_cast<SetObject*>(obj);
    if (ValueSet* set = setobj->getData())
        fop->delete_(set);
}

bool
SetObject::construct(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!ThrowIfNotConstructing(cx, args, "Set"))
        return false;

    RootedObject proto(cx);
    RootedObject newTarget(cx, &args.newTarget().toObject());
    if (!GetPrototypeFromConstructor(cx, newTarget, &proto))
        return false;

    Rooted<SetObject*> obj(cx, SetObject::create(cx, proto));
    if (!obj)
        return false;

    if (!args.get(0).isNullOrUndefined()) {
        RootedValue adderVal(cx);
        if (!GetProperty(cx, obj, obj, cx->names().add, &adderVal))
            return false;

        if (!IsCallable(adderVal))
            return ReportIsNotFunction(cx, adderVal);

        bool isOriginalAdder = IsNativeFunction(adderVal, SetObject::add);
        RootedValue setVal(cx, ObjectValue(*obj));
        FastInvokeGuard fig(cx, adderVal);
        InvokeArgs& args2 = fig.args();

        RootedValue keyVal(cx);
        ForOfIterator iter(cx);
        if (!iter.init(args[0]))
            return false;
        Rooted<HashableValue> key(cx);
        ValueSet* set = obj->getData();
        while (true) {
            bool done;
            if (!iter.next(&keyVal, &done))
                return false;
            if (done)
                break;

            if (isOriginalAdder) {
                if (!key.setValue(cx, keyVal))
                    return false;
                if (!set->put(key)) {
                    ReportOutOfMemory(cx);
                    return false;
                }
                WriteBarrierPost(cx->runtime(), set, keyVal);
            } else {
                if (!args2.init(1))
                    return false;

                args2.setCallee(adderVal);
                args2.setThis(setVal);
                args2[0].set(keyVal);

                if (!fig.invoke(cx))
                    return false;
            }
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
SetObject::extract(CallReceiver call)
{
    MOZ_ASSERT(call.thisv().isObject());
    MOZ_ASSERT(call.thisv().toObject().hasClass(&SetObject::class_));
    return *static_cast<SetObject&>(call.thisv().toObject()).getData();
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
    if (!set.put(key)) {
        ReportOutOfMemory(cx);
        return false;
    }
    WriteBarrierPost(cx->runtime(), &set, key.value());
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
    return iterobj && (iter.setObject(*iterobj), true);
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

JSObject*
js::InitSetClass(JSContext* cx, HandleObject obj)
{
    return SetObject::initClass(cx, obj);
}

const JSFunctionSpec selfhosting_collection_iterator_methods[] = {
    JS_FN("std_Set_iterator_next", SetIteratorObject::next, 0, 0),
    JS_FS_END
};

bool
js::InitSelfHostingCollectionIteratorFunctions(JSContext* cx, HandleObject obj)
{
    return JS_DefineFunctions(cx, obj, selfhosting_collection_iterator_methods);
}

/*** JS static utility functions *********************************************/

static
bool
forEach(const char* funcName, JSContext *cx, HandleObject obj, HandleValue callbackFn, HandleValue thisArg)
{
    CHECK_REQUEST(cx);
    RootedId forEachId(cx, NameToId(cx->names().forEach));
    RootedFunction forEachFunc(cx, JS::GetSelfHostedFunction(cx, funcName, forEachId, 2));
    if (!forEachFunc)
        return false;
    InvokeArgs args(cx);
    if (!args.init(2))
        return false;
    args.setCallee(JS::ObjectValue(*forEachFunc));
    args.setThis(JS::ObjectValue(*obj));
    args[0].set(callbackFn);
    args[1].set(thisArg);
    return Invoke(cx, args);
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

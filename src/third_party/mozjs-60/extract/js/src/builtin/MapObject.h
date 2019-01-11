/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_MapObject_h
#define builtin_MapObject_h

#include "builtin/SelfHostingDefines.h"
#include "vm/GlobalObject.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/PIC.h"
#include "vm/Runtime.h"

namespace js {

/*
 * Comparing two ropes for equality can fail. The js::HashTable template
 * requires infallible hash() and match() operations. Therefore we require
 * all values to be converted to hashable form before being used as a key
 * in a Map or Set object.
 *
 * All values except ropes are hashable as-is.
 */
class HashableValue
{
    PreBarrieredValue value;

  public:
    struct Hasher {
        typedef HashableValue Lookup;
        static HashNumber hash(const Lookup& v, const mozilla::HashCodeScrambler& hcs) {
            return v.hash(hcs);
        }
        static bool match(const HashableValue& k, const Lookup& l) { return k == l; }
        static bool isEmpty(const HashableValue& v) { return v.value.isMagic(JS_HASH_KEY_EMPTY); }
        static void makeEmpty(HashableValue* vp) { vp->value = MagicValue(JS_HASH_KEY_EMPTY); }
    };

    HashableValue() : value(UndefinedValue()) {}

    MOZ_MUST_USE bool setValue(JSContext* cx, HandleValue v);
    HashNumber hash(const mozilla::HashCodeScrambler& hcs) const;
    bool operator==(const HashableValue& other) const;
    HashableValue trace(JSTracer* trc) const;
    Value get() const { return value.get(); }

    void trace(JSTracer* trc) {
        TraceEdge(trc, &value, "HashableValue");
    }
};

template <typename Wrapper>
class WrappedPtrOperations<HashableValue, Wrapper>
{
  public:
    Value value() const {
        return static_cast<const Wrapper*>(this)->get().get();
    }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<HashableValue, Wrapper>
  : public WrappedPtrOperations<HashableValue, Wrapper>
{
  public:
    MOZ_MUST_USE bool setValue(JSContext* cx, HandleValue v) {
        return static_cast<Wrapper*>(this)->get().setValue(cx, v);
    }
};

template <class Key, class Value, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashMap;

template <class T, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashSet;

typedef OrderedHashMap<HashableValue,
                       HeapPtr<Value>,
                       HashableValue::Hasher,
                       ZoneAllocPolicy> ValueMap;

typedef OrderedHashSet<HashableValue,
                       HashableValue::Hasher,
                       ZoneAllocPolicy> ValueSet;

template <typename ObjectT>
class OrderedHashTableRef;

struct UnbarrieredHashPolicy;

class MapObject : public NativeObject {
  public:
    enum IteratorKind { Keys, Values, Entries };
    static_assert(Keys == ITEM_KIND_KEY,
                  "IteratorKind Keys must match self-hosting define for item kind key.");
    static_assert(Values == ITEM_KIND_VALUE,
                  "IteratorKind Values must match self-hosting define for item kind value.");
    static_assert(Entries == ITEM_KIND_KEY_AND_VALUE,
                  "IteratorKind Entries must match self-hosting define for item kind "
                  "key-and-value.");

    static const Class class_;
    static const Class protoClass_;

    enum { NurseryKeysSlot, HasNurseryMemorySlot, SlotCount };

    static MOZ_MUST_USE bool getKeysAndValuesInterleaved(HandleObject obj,
                                            JS::MutableHandle<GCVector<JS::Value>> entries);
    static MOZ_MUST_USE bool entries(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool has(JSContext* cx, unsigned argc, Value* vp);
    static MapObject* create(JSContext* cx, HandleObject proto = nullptr);

    // Publicly exposed Map calls for JSAPI access (webidl maplike/setlike
    // interfaces, etc.)
    static uint32_t size(JSContext *cx, HandleObject obj);
    static MOZ_MUST_USE bool get(JSContext *cx, HandleObject obj, HandleValue key,
                                 MutableHandleValue rval);
    static MOZ_MUST_USE bool has(JSContext *cx, HandleObject obj, HandleValue key, bool* rval);
    static MOZ_MUST_USE bool delete_(JSContext *cx, HandleObject obj, HandleValue key, bool* rval);

    // Set call for public JSAPI exposure. Does not actually return map object
    // as stated in spec, expects caller to return a value. for instance, with
    // webidl maplike/setlike, should return interface object.
    static MOZ_MUST_USE bool set(JSContext *cx, HandleObject obj, HandleValue key, HandleValue val);
    static MOZ_MUST_USE bool clear(JSContext *cx, HandleObject obj);
    static MOZ_MUST_USE bool iterator(JSContext *cx, IteratorKind kind, HandleObject obj,
                                      MutableHandleValue iter);

    using UnbarrieredTable = OrderedHashMap<Value, Value, UnbarrieredHashPolicy, ZoneAllocPolicy>;
    friend class OrderedHashTableRef<MapObject>;

    static void sweepAfterMinorGC(FreeOp* fop, MapObject* mapobj);

  private:
    static const ClassSpec classSpec_;
    static const ClassOps classOps_;

    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    static const JSPropertySpec staticProperties[];
    ValueMap* getData() { return static_cast<ValueMap*>(getPrivate()); }
    static ValueMap& extract(HandleObject o);
    static ValueMap& extract(const CallArgs& args);
    static void trace(JSTracer* trc, JSObject* obj);
    static void finalize(FreeOp* fop, JSObject* obj);
    static MOZ_MUST_USE bool construct(JSContext* cx, unsigned argc, Value* vp);

    static bool is(HandleValue v);
    static bool is(HandleObject o);

    static MOZ_MUST_USE bool iterator_impl(JSContext* cx, const CallArgs& args, IteratorKind kind);

    static MOZ_MUST_USE bool size_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool size(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool get_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool get(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool has_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool set_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool set(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool delete_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool delete_(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool keys_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool keys(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool values_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool values(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool entries_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool clear_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool clear(JSContext* cx, unsigned argc, Value* vp);
};

class MapIteratorObject : public NativeObject
{
  public:
    static const Class class_;

    enum { TargetSlot, RangeSlot, KindSlot, SlotCount };

    static_assert(TargetSlot == ITERATOR_SLOT_TARGET,
                  "TargetSlot must match self-hosting define for iterated object slot.");
    static_assert(RangeSlot == ITERATOR_SLOT_RANGE,
                  "RangeSlot must match self-hosting define for range or index slot.");
    static_assert(KindSlot == ITERATOR_SLOT_ITEM_KIND,
                  "KindSlot must match self-hosting define for item kind slot.");

    static const JSFunctionSpec methods[];
    static MapIteratorObject* create(JSContext* cx, HandleObject mapobj, ValueMap* data,
                                     MapObject::IteratorKind kind);
    static void finalize(FreeOp* fop, JSObject* obj);
    static size_t objectMoved(JSObject* obj, JSObject* old);

    static MOZ_MUST_USE bool next(Handle<MapIteratorObject*> mapIterator,
                                  HandleArrayObject resultPairObj, JSContext* cx);

    static JSObject* createResultPair(JSContext* cx);

  private:
    inline MapObject::IteratorKind kind() const;
};

class SetObject : public NativeObject {
  public:
    enum IteratorKind { Keys, Values, Entries };

    static_assert(Keys == ITEM_KIND_KEY,
                  "IteratorKind Keys must match self-hosting define for item kind key.");
    static_assert(Values == ITEM_KIND_VALUE,
                  "IteratorKind Values must match self-hosting define for item kind value.");
    static_assert(Entries == ITEM_KIND_KEY_AND_VALUE,
                  "IteratorKind Entries must match self-hosting define for item kind "
                  "key-and-value.");

    static const Class class_;
    static const Class protoClass_;

    enum { NurseryKeysSlot, HasNurseryMemorySlot, SlotCount };

    static MOZ_MUST_USE bool keys(JSContext *cx, HandleObject obj,
                                  JS::MutableHandle<GCVector<JS::Value>> keys);
    static MOZ_MUST_USE bool values(JSContext *cx, unsigned argc, Value *vp);
    static MOZ_MUST_USE bool add(JSContext *cx, HandleObject obj, HandleValue key);
    static MOZ_MUST_USE bool has(JSContext *cx, unsigned argc, Value *vp);

    // Publicly exposed Set calls for JSAPI access (webidl maplike/setlike
    // interfaces, etc.)
    static SetObject* create(JSContext *cx, HandleObject proto = nullptr);
    static uint32_t size(JSContext *cx, HandleObject obj);
    static MOZ_MUST_USE bool has(JSContext *cx, HandleObject obj, HandleValue key, bool* rval);
    static MOZ_MUST_USE bool clear(JSContext *cx, HandleObject obj);
    static MOZ_MUST_USE bool iterator(JSContext *cx, IteratorKind kind, HandleObject obj,
                                      MutableHandleValue iter);
    static MOZ_MUST_USE bool delete_(JSContext *cx, HandleObject obj, HandleValue key, bool *rval);

    using UnbarrieredTable = OrderedHashSet<Value, UnbarrieredHashPolicy, ZoneAllocPolicy>;
    friend class OrderedHashTableRef<SetObject>;

    static void sweepAfterMinorGC(FreeOp* fop, SetObject* setobj);

  private:
    static const ClassSpec classSpec_;
    static const ClassOps classOps_;

    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    static const JSPropertySpec staticProperties[];

    ValueSet* getData() { return static_cast<ValueSet*>(getPrivate()); }
    static ValueSet& extract(HandleObject o);
    static ValueSet& extract(const CallArgs& args);
    static void trace(JSTracer* trc, JSObject* obj);
    static void finalize(FreeOp* fop, JSObject* obj);
    static bool construct(JSContext* cx, unsigned argc, Value* vp);

    static bool is(HandleValue v);
    static bool is(HandleObject o);

    static bool isBuiltinAdd(HandleValue add);

    static MOZ_MUST_USE bool iterator_impl(JSContext* cx, const CallArgs& args, IteratorKind kind);

    static MOZ_MUST_USE bool size_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool size(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool has_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool add_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool add(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool delete_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool delete_(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool values_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool entries_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool entries(JSContext* cx, unsigned argc, Value* vp);
    static MOZ_MUST_USE bool clear_impl(JSContext* cx, const CallArgs& args);
    static MOZ_MUST_USE bool clear(JSContext* cx, unsigned argc, Value* vp);
};

class SetIteratorObject : public NativeObject
{
  public:
    static const Class class_;

    enum { TargetSlot, RangeSlot, KindSlot, SlotCount };

    static_assert(TargetSlot == ITERATOR_SLOT_TARGET,
                  "TargetSlot must match self-hosting define for iterated object slot.");
    static_assert(RangeSlot == ITERATOR_SLOT_RANGE,
                  "RangeSlot must match self-hosting define for range or index slot.");
    static_assert(KindSlot == ITERATOR_SLOT_ITEM_KIND,
                  "KindSlot must match self-hosting define for item kind slot.");

    static const JSFunctionSpec methods[];
    static SetIteratorObject* create(JSContext* cx, HandleObject setobj, ValueSet* data,
                                     SetObject::IteratorKind kind);
    static void finalize(FreeOp* fop, JSObject* obj);
    static size_t objectMoved(JSObject* obj, JSObject* old);

    static MOZ_MUST_USE bool next(Handle<SetIteratorObject*> setIterator,
                                  HandleArrayObject resultObj, JSContext* cx);

    static JSObject* createResult(JSContext* cx);

  private:
    inline SetObject::IteratorKind kind() const;
};

using SetInitGetPrototypeOp = NativeObject* (*)(JSContext*, Handle<GlobalObject*>);
using SetInitIsBuiltinOp = bool (*)(HandleValue);

template <SetInitGetPrototypeOp getPrototypeOp, SetInitIsBuiltinOp isBuiltinOp>
static MOZ_MUST_USE bool
IsOptimizableInitForSet(JSContext* cx, HandleObject setObject, HandleValue iterable, bool* optimized)
{
    MOZ_ASSERT(!*optimized);

    if (!iterable.isObject())
        return true;

    RootedObject array(cx, &iterable.toObject());
    if (!IsPackedArray(array))
        return true;

    // Get the canonical prototype object.
    RootedNativeObject setProto(cx, getPrototypeOp(cx, cx->global()));
    if (!setProto)
        return false;

    // Ensures setObject's prototype is the canonical prototype.
    if (setObject->staticPrototype() != setProto)
        return true;

    // Look up the 'add' value on the prototype object.
    Shape* addShape = setProto->lookup(cx, cx->names().add);
    if (!addShape || !addShape->isDataProperty())
        return true;

    // Get the referred value, ensure it holds the canonical add function.
    RootedValue add(cx, setProto->getSlot(addShape->slot()));
    if (!isBuiltinOp(add))
        return true;

    ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
    if (!stubChain)
        return false;

    return stubChain->tryOptimizeArray(cx, array.as<ArrayObject>(), optimized);
}

} /* namespace js */

#endif /* builtin_MapObject_h */

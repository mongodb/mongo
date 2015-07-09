/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_MapObject_h
#define builtin_MapObject_h

#include "jsobj.h"

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
class HashableValue {
    PreBarrieredValue value;

  public:
    struct Hasher {
        typedef HashableValue Lookup;
        static HashNumber hash(const Lookup& v) { return v.hash(); }
        static bool match(const HashableValue& k, const Lookup& l) { return k == l; }
        static bool isEmpty(const HashableValue& v) { return v.value.isMagic(JS_HASH_KEY_EMPTY); }
        static void makeEmpty(HashableValue* vp) { vp->value = MagicValue(JS_HASH_KEY_EMPTY); }
    };

    HashableValue() : value(UndefinedValue()) {}

    bool setValue(JSContext* cx, HandleValue v);
    HashNumber hash() const;
    bool operator==(const HashableValue& other) const;
    HashableValue mark(JSTracer* trc) const;
    Value get() const { return value.get(); }
};

class AutoHashableValueRooter : private JS::AutoGCRooter
{
  public:
    explicit AutoHashableValueRooter(JSContext* cx
                                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : JS::AutoGCRooter(cx, HASHABLEVALUE)
        {
            MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        }

    bool setValue(JSContext* cx, HandleValue v) {
        return value.setValue(cx, v);
    }

    operator const HashableValue & () {
        return value;
    }

    Value get() const { return value.get(); }

    friend void JS::AutoGCRooter::trace(JSTracer* trc);
    void trace(JSTracer* trc);

  private:
    HashableValue value;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

template <class Key, class Value, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashMap;

template <class T, class OrderedHashPolicy, class AllocPolicy>
class OrderedHashSet;

typedef OrderedHashMap<HashableValue,
                       RelocatableValue,
                       HashableValue::Hasher,
                       RuntimeAllocPolicy> ValueMap;

typedef OrderedHashSet<HashableValue,
                       HashableValue::Hasher,
                       RuntimeAllocPolicy> ValueSet;

class MapObject : public NativeObject {
  public:
    enum IteratorKind { Keys, Values, Entries };

    static JSObject* initClass(JSContext* cx, JSObject* obj);
    static const Class class_;

    static bool getKeysAndValuesInterleaved(JSContext* cx, HandleObject obj,
                                            JS::AutoValueVector* entries);
    static bool entries(JSContext* cx, unsigned argc, Value* vp);
    static bool has(JSContext* cx, unsigned argc, Value* vp);
    static MapObject* create(JSContext* cx);

    static uint32_t size(JSContext* cx, HandleObject obj);
    static bool get(JSContext* cx, HandleObject obj, HandleValue key, MutableHandleValue rval);
    static bool has(JSContext* cx, HandleObject obj, HandleValue key, bool* rval);
    static bool set(JSContext* cx, HandleObject obj, HandleValue key, HandleValue val);
    static bool clear(JSContext* cx, HandleObject obj);
    static bool iterator(JSContext* cx, IteratorKind kind, HandleObject obj, MutableHandleValue iter);

  private:
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    ValueMap* getData() { return static_cast<ValueMap*>(getPrivate()); }
    static ValueMap & extract(HandleObject o);
    static ValueMap & extract(CallReceiver call);
    static void mark(JSTracer* trc, JSObject* obj);
    static void finalize(FreeOp* fop, JSObject* obj);
    static bool construct(JSContext* cx, unsigned argc, Value* vp);

    static bool is(HandleValue v);
    static bool is(HandleObject o);

    static bool iterator_impl(JSContext* cx, CallArgs args, IteratorKind kind);

    static bool size_impl(JSContext* cx, CallArgs args);
    static bool size(JSContext* cx, unsigned argc, Value* vp);
    static bool get_impl(JSContext* cx, CallArgs args);
    static bool get(JSContext* cx, unsigned argc, Value* vp);
    static bool has_impl(JSContext* cx, CallArgs args);
    static bool set_impl(JSContext* cx, CallArgs args);
    static bool set(JSContext* cx, unsigned argc, Value* vp);
    static bool delete_impl(JSContext* cx, CallArgs args);
    static bool delete_(JSContext* cx, unsigned argc, Value* vp);
    static bool keys_impl(JSContext* cx, CallArgs args);
    static bool keys(JSContext* cx, unsigned argc, Value* vp);
    static bool values_impl(JSContext* cx, CallArgs args);
    static bool values(JSContext* cx, unsigned argc, Value* vp);
    static bool entries_impl(JSContext* cx, CallArgs args);
    static bool clear_impl(JSContext* cx, CallArgs args);
    static bool clear(JSContext* cx, unsigned argc, Value* vp);
};

class SetObject : public NativeObject {
  public:
    enum IteratorKind { Values, Entries };
    static JSObject* initClass(JSContext* cx, JSObject* obj);
    static const Class class_;

    static bool keys(JSContext* cx, HandleObject obj, JS::AutoValueVector* keys);
    static bool values(JSContext* cx, unsigned argc, Value* vp);
    static bool add(JSContext* cx, HandleObject obj, HandleValue key);
    static bool has(JSContext* cx, unsigned argc, Value* vp);
    static SetObject* create(JSContext* cx);

  private:
    static const JSPropertySpec properties[];
    static const JSFunctionSpec methods[];
    ValueSet* getData() { return static_cast<ValueSet*>(getPrivate()); }
    static ValueSet & extract(CallReceiver call);
    static void mark(JSTracer* trc, JSObject* obj);
    static void finalize(FreeOp* fop, JSObject* obj);
    static bool construct(JSContext* cx, unsigned argc, Value* vp);

    static bool is(HandleValue v);

    static bool iterator_impl(JSContext* cx, CallArgs args, IteratorKind kind);

    static bool size_impl(JSContext* cx, CallArgs args);
    static bool size(JSContext* cx, unsigned argc, Value* vp);
    static bool has_impl(JSContext* cx, CallArgs args);
    static bool add_impl(JSContext* cx, CallArgs args);
    static bool add(JSContext* cx, unsigned argc, Value* vp);
    static bool delete_impl(JSContext* cx, CallArgs args);
    static bool delete_(JSContext* cx, unsigned argc, Value* vp);
    static bool values_impl(JSContext* cx, CallArgs args);
    static bool entries_impl(JSContext* cx, CallArgs args);
    static bool entries(JSContext* cx, unsigned argc, Value* vp);
    static bool clear_impl(JSContext* cx, CallArgs args);
    static bool clear(JSContext* cx, unsigned argc, Value* vp);
};

extern bool
InitSelfHostingCollectionIteratorFunctions(JSContext* cx, js::HandleObject obj);

} /* namespace js */

extern JSObject*
js_InitMapClass(JSContext* cx, js::HandleObject obj);

extern JSObject*
js_InitSetClass(JSContext* cx, js::HandleObject obj);

#endif /* builtin_MapObject_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_MapObject_h
#define builtin_MapObject_h

#include "mozilla/MemoryReporting.h"

#include "builtin/OrderedHashTableObject.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"

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
  Value value;

 public:
  HashableValue() : value(UndefinedValue()) {}
  explicit HashableValue(JSWhyMagic whyMagic) : value(MagicValue(whyMagic)) {}

  [[nodiscard]] bool setValue(JSContext* cx, const Value& v);
  HashNumber hash(const mozilla::HashCodeScrambler& hcs) const;

  // Value equality. Separate BigInt instances may compare equal.
  bool equals(const HashableValue& other) const;

  // Bitwise equality.
  bool operator==(const HashableValue& other) const {
    return value == other.value;
  }
  bool operator!=(const HashableValue& other) const {
    return !(*this == other);
  }

  const Value& get() const { return value; }
  operator Value() const { return get(); }

  void trace(JSTracer* trc) {
    TraceManuallyBarrieredEdge(trc, &value, "HashableValue");
  }
};

template <typename Wrapper>
class WrappedPtrOperations<HashableValue, Wrapper> {
 public:
  Value get() const { return static_cast<const Wrapper*>(this)->get().get(); }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<HashableValue, Wrapper>
    : public WrappedPtrOperations<HashableValue, Wrapper> {
 public:
  [[nodiscard]] bool setValue(JSContext* cx, HandleValue v) {
    return static_cast<Wrapper*>(this)->get().setValue(cx, v);
  }
};

template <>
struct InternalBarrierMethods<HashableValue> {
  static bool isMarkable(const HashableValue& v) { return v.get().isGCThing(); }

  static void preBarrier(const HashableValue& v) {
    if (isMarkable(v)) {
      gc::ValuePreWriteBarrier(v.get());
    }
  }

#ifdef DEBUG
  static void assertThingIsNotGray(const HashableValue& v) {
    JS::AssertValueIsNotGray(v.get());
  }
#endif
};

struct HashableValueHasher {
  using Key = PreBarriered<HashableValue>;
  using Lookup = HashableValue;

  static HashNumber hash(const Lookup& v,
                         const mozilla::HashCodeScrambler& hcs) {
    return v.hash(hcs);
  }
  static bool match(const Key& k, const Lookup& l) { return k.get().equals(l); }
  static bool isEmpty(const Key& v) {
    return v.get().get().isMagic(JS_HASH_KEY_EMPTY);
  }
  static void makeEmpty(Key* vp) { vp->set(HashableValue(JS_HASH_KEY_EMPTY)); }
};

template <typename ObjectT>
class OrderedHashTableRef;

struct UnbarrieredHashPolicy;

class MapObject : public OrderedHashMapObject {
 public:
  using Table = OrderedHashMapImpl<PreBarriered<HashableValue>, HeapPtr<Value>,
                                   HashableValueHasher>;

  // PreBarrieredTable has the same memory layout as Table but doesn't have
  // wrappers that perform post barriers on the keys/values. Used when the
  // MapObject is in the nursery.
  using PreBarrieredTable =
      OrderedHashMapImpl<PreBarriered<HashableValue>, PreBarriered<Value>,
                         HashableValueHasher>;

  // UnbarrieredTable has the same memory layout as Table but doesn't have any
  // wrappers that perform barriers on the keys/values. Used to allocate and
  // delete the table and when updating the nursery allocated keys map during
  // minor GC.
  using UnbarrieredTable =
      OrderedHashMapImpl<Value, Value, UnbarrieredHashPolicy>;

  friend class OrderedHashTableRef<MapObject>;

  enum {
    NurseryKeysSlot = Table::SlotCount,
    RegisteredNurseryIteratorsSlot,
    SlotCount
  };

  using IteratorKind = TableIteratorObject::Kind;

  static const JSClass class_;
  static const JSClass protoClass_;

  [[nodiscard]] bool getKeysAndValuesInterleaved(
      JS::MutableHandle<GCVector<JS::Value>> entries);
  [[nodiscard]] static bool entries(JSContext* cx, unsigned argc, Value* vp);

  static MapObject* createWithProto(JSContext* cx, HandleObject proto,
                                    NewObjectKind newKind);
  static MapObject* create(JSContext* cx, HandleObject proto = nullptr);
  static MapObject* createFromIterable(
      JSContext* cx, Handle<JSObject*> proto, Handle<Value> iterable,
      Handle<MapObject*> allocatedFromJit = nullptr);

  // Publicly exposed Map calls for JSAPI access (webidl maplike/setlike
  // interfaces, etc.)
  uint32_t size();
  [[nodiscard]] bool get(JSContext* cx, const Value& key,
                         MutableHandleValue rval);
  [[nodiscard]] bool has(JSContext* cx, const Value& key, bool* rval);
#ifdef NIGHTLY_BUILD
  [[nodiscard]] bool getOrInsert(JSContext* cx, const Value& key,
                                 const Value& val, MutableHandleValue rval);
#endif  // #ifdef NIGHTLY_BUILD
  [[nodiscard]] bool delete_(JSContext* cx, const Value& key, bool* rval);

  // Set call for public JSAPI exposure. Does not actually return map object
  // as stated in spec, expects caller to return a value. for instance, with
  // webidl maplike/setlike, should return interface object.
  [[nodiscard]] bool set(JSContext* cx, const Value& key, const Value& val);
  void clear(JSContext* cx);
  [[nodiscard]] static bool iterator(JSContext* cx, IteratorKind kind,
                                     Handle<MapObject*> obj,
                                     MutableHandleValue iter);

  void clearNurseryIteratorsBeforeMinorGC();

  // Sweeps a map that had nursery memory associated with it after a minor
  // GC. This may finalize the map if it was in the nursery and has died.
  //
  // Returns a pointer to the map if it still has nursery memory associated with
  // it, or nullptr.
  static MapObject* sweepAfterMinorGC(JS::GCContext* gcx, MapObject* mapobj);

  size_t sizeOfData(mozilla::MallocSizeOf mallocSizeOf);

  [[nodiscard]] static bool get(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool set(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool has(JSContext* cx, unsigned argc, Value* vp);

  static bool isOriginalSizeGetter(Native native) {
    return native == static_cast<Native>(MapObject::size);
  }

 private:
  static const ClassSpec classSpec_;
  static const JSClassOps classOps_;
  static const ClassExtension classExtension_;

  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSPropertySpec staticProperties[];
  static const JSFunctionSpec staticMethods[];

  [[nodiscard]] bool setWithHashableKey(JSContext* cx, const HashableValue& key,
                                        const Value& value);

  [[nodiscard]] bool tryOptimizeCtorWithIterable(JSContext* cx,
                                                 const Value& iterableVal,
                                                 bool* optimized);

  static bool finishInit(JSContext* cx, HandleObject ctor, HandleObject proto);

  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);

  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  static bool is(HandleValue v);
  static bool is(HandleObject o);

  [[nodiscard]] static bool iterator_impl(JSContext* cx, const CallArgs& args,
                                          IteratorKind kind);

  [[nodiscard]] static bool size_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool size(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool get_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool has_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool set_impl(JSContext* cx, const CallArgs& args);
#ifdef NIGHTLY_BUILD
  [[nodiscard]] static bool getOrInsert(JSContext* cx, unsigned argc,
                                        Value* vp);
  [[nodiscard]] static bool getOrInsert_impl(JSContext* cx,
                                             const CallArgs& args);
#endif
  [[nodiscard]] static bool delete_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool delete_(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool keys_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool keys(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool values_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool values(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool entries_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool clear_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool clear(JSContext* cx, unsigned argc, Value* vp);
};

class MapIteratorObject : public TableIteratorObject {
 public:
  static const JSClass class_;

  static const JSFunctionSpec methods[];
  static MapIteratorObject* create(JSContext* cx, Handle<MapObject*> mapobj,
                                   Kind kind);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);

  [[nodiscard]] static bool next(MapIteratorObject* mapIterator,
                                 ArrayObject* resultPairObj);

  static JSObject* createResultPair(JSContext* cx);

 private:
  MapObject* target() const;
};

class SetObject : public OrderedHashSetObject {
 public:
  using Table =
      OrderedHashSetImpl<PreBarriered<HashableValue>, HashableValueHasher>;
  using UnbarrieredTable = OrderedHashSetImpl<Value, UnbarrieredHashPolicy>;

  friend class OrderedHashTableRef<SetObject>;

  enum {
    NurseryKeysSlot = Table::SlotCount,
    RegisteredNurseryIteratorsSlot,
    SlotCount
  };

  using IteratorKind = TableIteratorObject::Kind;

  static const JSClass class_;
  static const JSClass protoClass_;

  [[nodiscard]] bool keys(JS::MutableHandle<GCVector<JS::Value>> keys);
  [[nodiscard]] static bool values(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] bool add(JSContext* cx, const Value& key);

  // Publicly exposed Set calls for JSAPI access (webidl maplike/setlike
  // interfaces, etc.)
  static SetObject* createWithProto(JSContext* cx, HandleObject proto,
                                    NewObjectKind newKind);
  static SetObject* create(JSContext* cx, HandleObject proto = nullptr);
  static SetObject* createFromIterable(
      JSContext* cx, Handle<JSObject*> proto, Handle<Value> iterable,
      Handle<SetObject*> allocatedFromJit = nullptr);

  uint32_t size();
  [[nodiscard]] static bool size(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool add(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool has(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] bool has(JSContext* cx, const Value& key, bool* rval);
  void clear(JSContext* cx);
  [[nodiscard]] static bool iterator(JSContext* cx, IteratorKind kind,
                                     Handle<SetObject*> obj,
                                     MutableHandleValue iter);
  [[nodiscard]] static bool delete_(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] bool delete_(JSContext* cx, const Value& key, bool* rval);

  [[nodiscard]] static bool copy(JSContext* cx, unsigned argc, Value* vp);

  void clearNurseryIteratorsBeforeMinorGC();

  // Sweeps a set that had nursery memory associated with it after a minor
  // GC. This may finalize the set if it was in the nursery and has died.
  //
  // Returns a pointer to the set if it still has nursery memory associated with
  // it, or nullptr.
  static SetObject* sweepAfterMinorGC(JS::GCContext* gcx, SetObject* setobj);

  size_t sizeOfData(mozilla::MallocSizeOf mallocSizeOf);

  static bool isOriginalSizeGetter(Native native) {
    return native == static_cast<Native>(SetObject::size);
  }

 private:
  static const ClassSpec classSpec_;
  static const JSClassOps classOps_;
  static const ClassExtension classExtension_;

  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSPropertySpec staticProperties[];

  [[nodiscard]] bool addHashableValue(JSContext* cx,
                                      const HashableValue& value);

  [[nodiscard]] bool tryOptimizeCtorWithIterable(JSContext* cx,
                                                 const Value& iterableVal,
                                                 bool* optimized);

  static bool finishInit(JSContext* cx, HandleObject ctor, HandleObject proto);

  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);

  static bool construct(JSContext* cx, unsigned argc, Value* vp);

  static bool is(HandleValue v);
  static bool is(HandleObject o);

  [[nodiscard]] static bool iterator_impl(JSContext* cx, const CallArgs& args,
                                          IteratorKind kind);

  [[nodiscard]] static bool size_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool has_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool add_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool delete_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool values_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool entries_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool entries(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool clear_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool clear(JSContext* cx, unsigned argc, Value* vp);
};

class SetIteratorObject : public TableIteratorObject {
 public:
  static const JSClass class_;

  static const JSFunctionSpec methods[];
  static SetIteratorObject* create(JSContext* cx, Handle<SetObject*> setobj,
                                   Kind kind);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);

  [[nodiscard]] static bool next(SetIteratorObject* setIterator,
                                 ArrayObject* resultObj);

  static JSObject* createResult(JSContext* cx);

 private:
  SetObject* target() const;
};

} /* namespace js */

#endif /* builtin_MapObject_h */

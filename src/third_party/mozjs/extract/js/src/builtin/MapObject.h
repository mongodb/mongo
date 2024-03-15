/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_MapObject_h
#define builtin_MapObject_h

#include "mozilla/MemoryReporting.h"

#include "builtin/SelfHostingDefines.h"
#include "vm/JSObject.h"
#include "vm/NativeObject.h"
#include "vm/PIC.h"

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

  [[nodiscard]] bool setValue(JSContext* cx, HandleValue v);
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

using ValueMap = OrderedHashMap<PreBarriered<HashableValue>, HeapPtr<Value>,
                                HashableValueHasher, CellAllocPolicy>;

using ValueSet = OrderedHashSet<PreBarriered<HashableValue>,
                                HashableValueHasher, CellAllocPolicy>;

template <typename ObjectT>
class OrderedHashTableRef;

struct UnbarrieredHashPolicy;

class MapObject : public NativeObject {
 public:
  enum IteratorKind { Keys, Values, Entries };
  static_assert(
      Keys == ITEM_KIND_KEY,
      "IteratorKind Keys must match self-hosting define for item kind key.");
  static_assert(Values == ITEM_KIND_VALUE,
                "IteratorKind Values must match self-hosting define for item "
                "kind value.");
  static_assert(
      Entries == ITEM_KIND_KEY_AND_VALUE,
      "IteratorKind Entries must match self-hosting define for item kind "
      "key-and-value.");

  static const JSClass class_;
  static const JSClass protoClass_;

  enum { DataSlot, NurseryKeysSlot, HasNurseryMemorySlot, SlotCount };

  [[nodiscard]] static bool getKeysAndValuesInterleaved(
      HandleObject obj, JS::MutableHandle<GCVector<JS::Value>> entries);
  [[nodiscard]] static bool entries(JSContext* cx, unsigned argc, Value* vp);
  static MapObject* create(JSContext* cx, HandleObject proto = nullptr);

  // Publicly exposed Map calls for JSAPI access (webidl maplike/setlike
  // interfaces, etc.)
  static uint32_t size(JSContext* cx, HandleObject obj);
  [[nodiscard]] static bool get(JSContext* cx, HandleObject obj,
                                HandleValue key, MutableHandleValue rval);
  [[nodiscard]] static bool has(JSContext* cx, HandleObject obj,
                                HandleValue key, bool* rval);
  [[nodiscard]] static bool delete_(JSContext* cx, HandleObject obj,
                                    HandleValue key, bool* rval);

  // Set call for public JSAPI exposure. Does not actually return map object
  // as stated in spec, expects caller to return a value. for instance, with
  // webidl maplike/setlike, should return interface object.
  [[nodiscard]] static bool set(JSContext* cx, HandleObject obj,
                                HandleValue key, HandleValue val);
  [[nodiscard]] static bool clear(JSContext* cx, HandleObject obj);
  [[nodiscard]] static bool iterator(JSContext* cx, IteratorKind kind,
                                     HandleObject obj, MutableHandleValue iter);

  // OrderedHashMap with the same memory layout as ValueMap but without wrappers
  // that perform post barriers. Used when the owning JS object is in the
  // nursery.
  using PreBarrieredTable =
      OrderedHashMap<PreBarriered<HashableValue>, PreBarriered<Value>,
                     HashableValueHasher, CellAllocPolicy>;

  // OrderedHashMap with the same memory layout as ValueMap but without any
  // wrappers that perform barriers. Used when updating the nursery allocated
  // keys map during minor GC.
  using UnbarrieredTable =
      OrderedHashMap<Value, Value, UnbarrieredHashPolicy, CellAllocPolicy>;
  friend class OrderedHashTableRef<MapObject>;

  static void sweepAfterMinorGC(JS::GCContext* gcx, MapObject* mapobj);

  size_t sizeOfData(mozilla::MallocSizeOf mallocSizeOf);

  static constexpr size_t getDataSlotOffset() {
    return getFixedSlotOffset(DataSlot);
  }

  const ValueMap* getData() { return getTableUnchecked(); }

  [[nodiscard]] static bool get(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool set(JSContext* cx, unsigned argc, Value* vp);

  static bool isOriginalSizeGetter(Native native) {
    return native == static_cast<Native>(MapObject::size);
  }

 private:
  static const ClassSpec classSpec_;
  static const JSClassOps classOps_;

  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSPropertySpec staticProperties[];

  PreBarrieredTable* nurseryTable() {
    MOZ_ASSERT(IsInsideNursery(this));
    return maybePtrFromReservedSlot<PreBarrieredTable>(DataSlot);
  }
  ValueMap* tenuredTable() {
    MOZ_ASSERT(!IsInsideNursery(this));
    return getTableUnchecked();
  }
  ValueMap* getTableUnchecked() {
    return maybePtrFromReservedSlot<ValueMap>(DataSlot);
  }

  static inline bool setWithHashableKey(JSContext* cx, MapObject* obj,
                                        Handle<HashableValue> key,
                                        Handle<Value> value);

  static bool finishInit(JSContext* cx, HandleObject ctor, HandleObject proto);

  static const ValueMap& extract(HandleObject o);
  static const ValueMap& extract(const CallArgs& args);
  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  static bool is(HandleValue v);
  static bool is(HandleObject o);

  [[nodiscard]] static bool iterator_impl(JSContext* cx, const CallArgs& args,
                                          IteratorKind kind);

  [[nodiscard]] static bool size_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool size(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool get_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool has_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool has(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool set_impl(JSContext* cx, const CallArgs& args);
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

class MapIteratorObject : public NativeObject {
 public:
  static const JSClass class_;

  enum { TargetSlot, RangeSlot, KindSlot, SlotCount };

  static_assert(
      TargetSlot == ITERATOR_SLOT_TARGET,
      "TargetSlot must match self-hosting define for iterated object slot.");
  static_assert(
      RangeSlot == ITERATOR_SLOT_RANGE,
      "RangeSlot must match self-hosting define for range or index slot.");
  static_assert(KindSlot == ITERATOR_SLOT_ITEM_KIND,
                "KindSlot must match self-hosting define for item kind slot.");

  static const JSFunctionSpec methods[];
  static MapIteratorObject* create(JSContext* cx, HandleObject mapobj,
                                   const ValueMap* data,
                                   MapObject::IteratorKind kind);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);

  void init(MapObject* mapObj, MapObject::IteratorKind kind) {
    initFixedSlot(TargetSlot, JS::ObjectValue(*mapObj));
    initFixedSlot(RangeSlot, JS::PrivateValue(nullptr));
    initFixedSlot(KindSlot, JS::Int32Value(int32_t(kind)));
  }

  [[nodiscard]] static bool next(MapIteratorObject* mapIterator,
                                 ArrayObject* resultPairObj);

  static JSObject* createResultPair(JSContext* cx);

 private:
  inline MapObject::IteratorKind kind() const;
};

class SetObject : public NativeObject {
 public:
  enum IteratorKind { Keys, Values, Entries };

  static_assert(
      Keys == ITEM_KIND_KEY,
      "IteratorKind Keys must match self-hosting define for item kind key.");
  static_assert(Values == ITEM_KIND_VALUE,
                "IteratorKind Values must match self-hosting define for item "
                "kind value.");
  static_assert(
      Entries == ITEM_KIND_KEY_AND_VALUE,
      "IteratorKind Entries must match self-hosting define for item kind "
      "key-and-value.");

  static const JSClass class_;
  static const JSClass protoClass_;

  enum { DataSlot, NurseryKeysSlot, HasNurseryMemorySlot, SlotCount };

  [[nodiscard]] static bool keys(JSContext* cx, HandleObject obj,
                                 JS::MutableHandle<GCVector<JS::Value>> keys);
  [[nodiscard]] static bool values(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool add(JSContext* cx, HandleObject obj,
                                HandleValue key);

  // Publicly exposed Set calls for JSAPI access (webidl maplike/setlike
  // interfaces, etc.)
  static SetObject* create(JSContext* cx, HandleObject proto = nullptr);
  static uint32_t size(JSContext* cx, HandleObject obj);
  [[nodiscard]] static bool add(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool has(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool has(JSContext* cx, HandleObject obj,
                                HandleValue key, bool* rval);
  [[nodiscard]] static bool clear(JSContext* cx, HandleObject obj);
  [[nodiscard]] static bool iterator(JSContext* cx, IteratorKind kind,
                                     HandleObject obj, MutableHandleValue iter);
  [[nodiscard]] static bool delete_(JSContext* cx, HandleObject obj,
                                    HandleValue key, bool* rval);

  using UnbarrieredTable =
      OrderedHashSet<Value, UnbarrieredHashPolicy, CellAllocPolicy>;
  friend class OrderedHashTableRef<SetObject>;

  static void sweepAfterMinorGC(JS::GCContext* gcx, SetObject* setobj);

  size_t sizeOfData(mozilla::MallocSizeOf mallocSizeOf);

  static constexpr size_t getDataSlotOffset() {
    return getFixedSlotOffset(DataSlot);
  }

  ValueSet* getData() { return getTableUnchecked(); }

  static bool isOriginalSizeGetter(Native native) {
    return native == static_cast<Native>(SetObject::size);
  }

 private:
  static const ClassSpec classSpec_;
  static const JSClassOps classOps_;

  static const JSPropertySpec properties[];
  static const JSFunctionSpec methods[];
  static const JSPropertySpec staticProperties[];

  ValueSet* getTableUnchecked() {
    return maybePtrFromReservedSlot<ValueSet>(DataSlot);
  }

  static bool finishInit(JSContext* cx, HandleObject ctor, HandleObject proto);

  static ValueSet& extract(HandleObject o);
  static ValueSet& extract(const CallArgs& args);
  static void trace(JSTracer* trc, JSObject* obj);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static bool construct(JSContext* cx, unsigned argc, Value* vp);

  static bool is(HandleValue v);
  static bool is(HandleObject o);

  static bool isBuiltinAdd(HandleValue add);

  [[nodiscard]] static bool iterator_impl(JSContext* cx, const CallArgs& args,
                                          IteratorKind kind);

  [[nodiscard]] static bool size_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool size(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool has_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool add_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool delete_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool delete_(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool values_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool entries_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool entries(JSContext* cx, unsigned argc, Value* vp);
  [[nodiscard]] static bool clear_impl(JSContext* cx, const CallArgs& args);
  [[nodiscard]] static bool clear(JSContext* cx, unsigned argc, Value* vp);
};

class SetIteratorObject : public NativeObject {
 public:
  static const JSClass class_;

  enum { TargetSlot, RangeSlot, KindSlot, SlotCount };

  static_assert(
      TargetSlot == ITERATOR_SLOT_TARGET,
      "TargetSlot must match self-hosting define for iterated object slot.");
  static_assert(
      RangeSlot == ITERATOR_SLOT_RANGE,
      "RangeSlot must match self-hosting define for range or index slot.");
  static_assert(KindSlot == ITERATOR_SLOT_ITEM_KIND,
                "KindSlot must match self-hosting define for item kind slot.");

  static const JSFunctionSpec methods[];
  static SetIteratorObject* create(JSContext* cx, HandleObject setobj,
                                   ValueSet* data,
                                   SetObject::IteratorKind kind);
  static void finalize(JS::GCContext* gcx, JSObject* obj);
  static size_t objectMoved(JSObject* obj, JSObject* old);

  void init(SetObject* setObj, SetObject::IteratorKind kind) {
    initFixedSlot(TargetSlot, JS::ObjectValue(*setObj));
    initFixedSlot(RangeSlot, JS::PrivateValue(nullptr));
    initFixedSlot(KindSlot, JS::Int32Value(int32_t(kind)));
  }

  [[nodiscard]] static bool next(SetIteratorObject* setIterator,
                                 ArrayObject* resultObj);

  static JSObject* createResult(JSContext* cx);

 private:
  inline SetObject::IteratorKind kind() const;
};

using SetInitGetPrototypeOp = NativeObject* (*)(JSContext*,
                                                Handle<GlobalObject*>);
using SetInitIsBuiltinOp = bool (*)(HandleValue);

template <SetInitGetPrototypeOp getPrototypeOp, SetInitIsBuiltinOp isBuiltinOp>
[[nodiscard]] static bool IsOptimizableInitForSet(JSContext* cx,
                                                  HandleObject setObject,
                                                  HandleValue iterable,
                                                  bool* optimized) {
  MOZ_ASSERT(!*optimized);

  if (!iterable.isObject()) {
    return true;
  }

  RootedObject array(cx, &iterable.toObject());
  if (!IsPackedArray(array)) {
    return true;
  }

  // Get the canonical prototype object.
  Rooted<NativeObject*> setProto(cx, getPrototypeOp(cx, cx->global()));
  if (!setProto) {
    return false;
  }

  // Ensures setObject's prototype is the canonical prototype.
  if (setObject->staticPrototype() != setProto) {
    return true;
  }

  // Look up the 'add' value on the prototype object.
  mozilla::Maybe<PropertyInfo> addProp = setProto->lookup(cx, cx->names().add);
  if (addProp.isNothing() || !addProp->isDataProperty()) {
    return true;
  }

  // Get the referred value, ensure it holds the canonical add function.
  RootedValue add(cx, setProto->getSlot(addProp->slot()));
  if (!isBuiltinOp(add)) {
    return true;
  }

  ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
  if (!stubChain) {
    return false;
  }

  return stubChain->tryOptimizeArray(cx, array.as<ArrayObject>(), optimized);
}

} /* namespace js */

#endif /* builtin_MapObject_h */

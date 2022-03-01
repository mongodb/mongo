/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSObject_inl_h
#define vm_JSObject_inl_h

#include "vm/JSObject.h"

#include "js/Object.h"  // JS::GetBuiltinClass
#include "vm/ArrayObject.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSFunction.h"
#include "vm/Probes.h"
#include "vm/PropertyResult.h"
#include "vm/TypedArrayObject.h"

#include "gc/FreeOp-inl.h"
#include "gc/Marking-inl.h"
#include "gc/ObjectKind-inl.h"
#include "vm/ObjectOperations-inl.h"  // js::MaybeHasInterestingSymbolProperty
#include "vm/Realm-inl.h"

namespace js {

// Get the GC kind to use for scripted 'new', empty object literals ({}), and
// the |Object| constructor.
static inline gc::AllocKind NewObjectGCKind() { return gc::AllocKind::OBJECT4; }

}  // namespace js

MOZ_ALWAYS_INLINE uint32_t js::NativeObject::numDynamicSlots() const {
  uint32_t slots = getSlotsHeader()->capacity();
  MOZ_ASSERT(slots == calculateDynamicSlots());
  MOZ_ASSERT_IF(hasDynamicSlots(), slots != 0);

  return slots;
}

MOZ_ALWAYS_INLINE uint32_t js::NativeObject::calculateDynamicSlots() const {
  return calculateDynamicSlots(numFixedSlots(), slotSpan(), getClass());
}

/* static */ MOZ_ALWAYS_INLINE uint32_t js::NativeObject::calculateDynamicSlots(
    uint32_t nfixed, uint32_t span, const JSClass* clasp) {
  if (span <= nfixed) {
    return 0;
  }

  uint32_t ndynamic = span - nfixed;

  // Increase the slots to SLOT_CAPACITY_MIN to decrease the likelihood
  // the dynamic slots need to get increased again. ArrayObjects ignore
  // this because slots are uncommon in that case.
  if (clasp != &ArrayObject::class_ && ndynamic <= SLOT_CAPACITY_MIN) {
    return SLOT_CAPACITY_MIN;
  }

  uint32_t count =
      mozilla::RoundUpPow2(ndynamic + ObjectSlots::VALUES_PER_HEADER);

  uint32_t slots = count - ObjectSlots::VALUES_PER_HEADER;
  MOZ_ASSERT(slots >= ndynamic);
  return slots;
}

/* static */ MOZ_ALWAYS_INLINE uint32_t
js::NativeObject::calculateDynamicSlots(Shape* shape) {
  return calculateDynamicSlots(shape->numFixedSlots(), shape->slotSpan(),
                               shape->getObjectClass());
}

inline void JSObject::finalize(JSFreeOp* fop) {
  js::probes::FinalizeObject(this);

#ifdef DEBUG
  MOZ_ASSERT(isTenured());
  if (!IsBackgroundFinalized(asTenured().getAllocKind())) {
    /* Assert we're on the main thread. */
    MOZ_ASSERT(CurrentThreadCanAccessZone(zone()));
  }
#endif

  const JSClass* clasp = getClass();
  js::NativeObject* nobj =
      clasp->isNativeObject() ? &as<js::NativeObject>() : nullptr;

  if (clasp->hasFinalize()) {
    clasp->doFinalize(fop, this);
  }

  if (!nobj) {
    return;
  }

  if (nobj->hasDynamicSlots()) {
    js::ObjectSlots* slotsHeader = nobj->getSlotsHeader();
    size_t size = js::ObjectSlots::allocSize(slotsHeader->capacity());
    fop->free_(this, slotsHeader, size, js::MemoryUse::ObjectSlots);
  }

  if (nobj->hasDynamicElements()) {
    js::ObjectElements* elements = nobj->getElementsHeader();
    size_t size = elements->numAllocatedElements() * sizeof(js::HeapSlot);
    fop->free_(this, nobj->getUnshiftedElementsHeader(), size,
               js::MemoryUse::ObjectElements);
  }
}

inline bool JSObject::isQualifiedVarObj() const {
  if (is<js::DebugEnvironmentProxy>()) {
    return as<js::DebugEnvironmentProxy>().environment().isQualifiedVarObj();
  }
  bool rv = hasFlag(js::ObjectFlag::QualifiedVarObj);
  MOZ_ASSERT_IF(rv, is<js::GlobalObject>() || is<js::CallObject>() ||
                        is<js::VarEnvironmentObject>() ||
                        is<js::ModuleEnvironmentObject>() ||
                        is<js::NonSyntacticVariablesObject>() ||
                        (is<js::WithEnvironmentObject>() &&
                         !as<js::WithEnvironmentObject>().isSyntactic()));
  return rv;
}

inline bool JSObject::isUnqualifiedVarObj() const {
  if (is<js::DebugEnvironmentProxy>()) {
    return as<js::DebugEnvironmentProxy>().environment().isUnqualifiedVarObj();
  }
  return is<js::GlobalObject>() || is<js::NonSyntacticVariablesObject>();
}

namespace js {

inline bool ClassCanHaveFixedData(const JSClass* clasp) {
  // Normally, the number of fixed slots given an object is the maximum
  // permitted for its size class. For array buffers and non-shared typed
  // arrays we only use enough to cover the class reserved slots, so that
  // the remaining space in the object's allocation is available for the
  // buffer's data.
  return !clasp->isNativeObject() || clasp == &js::ArrayBufferObject::class_ ||
         js::IsTypedArrayClass(clasp);
}

class MOZ_RAII AutoSuppressAllocationMetadataBuilder {
  JS::Zone* zone;
  bool saved;

 public:
  explicit AutoSuppressAllocationMetadataBuilder(JSContext* cx)
      : zone(cx->zone()), saved(zone->suppressAllocationMetadataBuilder) {
    zone->suppressAllocationMetadataBuilder = true;
  }

  ~AutoSuppressAllocationMetadataBuilder() {
    zone->suppressAllocationMetadataBuilder = saved;
  }
};

// This function is meant to be called from allocation fast paths.
//
// If we do have an allocation metadata builder, it can cause a GC, so the
// object must be rooted. The usual way to do this would be to make our callers
// pass a HandleObject, but that would require them to pay the cost of rooting
// the object unconditionally, even though collecting metadata is rare. Instead,
// SetNewObjectMetadata's contract is that the caller must use the pointer
// returned in place of the pointer passed. If a GC occurs, the returned pointer
// may be the passed pointer, relocated by GC. If no GC could occur, it's just
// passed through. We root nothing unless necessary.
template <typename T>
[[nodiscard]] static MOZ_ALWAYS_INLINE T* SetNewObjectMetadata(JSContext* cx,
                                                               T* obj) {
  MOZ_ASSERT(!cx->realm()->hasObjectPendingMetadata());

  // The metadata builder is invoked for each object created on the active
  // thread, except when analysis/compilation is active, to avoid recursion.
  if (!cx->isHelperThreadContext()) {
    if (MOZ_UNLIKELY(cx->realm()->hasAllocationMetadataBuilder()) &&
        !cx->zone()->suppressAllocationMetadataBuilder) {
      // Don't collect metadata on objects that represent metadata.
      AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

      Rooted<T*> rooted(cx, obj);
      cx->realm()->setNewObjectMetadata(cx, rooted);
      return rooted;
    }
  }

  return obj;
}

}  // namespace js

inline js::GlobalObject& JSObject::nonCCWGlobal() const {
  /*
   * The global is read-barriered so that it is kept live by access through
   * the Realm. When accessed through a JSObject, however, the global will be
   * already kept live by the black JSObject's group pointer, so does not
   * need to be read-barriered.
   */
  return *nonCCWRealm()->unsafeUnbarrieredMaybeGlobal();
}

inline bool JSObject::nonProxyIsExtensible() const {
  MOZ_ASSERT(!uninlinedIsProxyObject());

  // [[Extensible]] for ordinary non-proxy objects is an object flag.
  return !hasFlag(js::ObjectFlag::NotExtensible);
}

inline bool JSObject::isBoundFunction() const {
  return is<JSFunction>() && as<JSFunction>().isBoundFunction();
}

inline bool JSObject::hasUncacheableProto() const {
  return hasFlag(js::ObjectFlag::UncacheableProto);
}

MOZ_ALWAYS_INLINE bool JSObject::maybeHasInterestingSymbolProperty() const {
  if (is<js::NativeObject>()) {
    return as<js::NativeObject>().hasInterestingSymbol();
  }
  return true;
}

inline bool JSObject::staticPrototypeIsImmutable() const {
  MOZ_ASSERT(hasStaticPrototype());
  return hasFlag(js::ObjectFlag::ImmutablePrototype);
}

namespace js {

static MOZ_ALWAYS_INLINE bool IsFunctionObject(const js::Value& v) {
  return v.isObject() && v.toObject().is<JSFunction>();
}

static MOZ_ALWAYS_INLINE bool IsFunctionObject(const js::Value& v,
                                               JSFunction** fun) {
  if (v.isObject() && v.toObject().is<JSFunction>()) {
    *fun = &v.toObject().as<JSFunction>();
    return true;
  }
  return false;
}

static MOZ_ALWAYS_INLINE bool IsNativeFunction(const js::Value& v,
                                               JSNative native) {
  JSFunction* fun;
  return IsFunctionObject(v, &fun) && fun->maybeNative() == native;
}

static MOZ_ALWAYS_INLINE bool IsNativeFunction(const JSObject* obj,
                                               JSNative native) {
  return obj->is<JSFunction>() && obj->as<JSFunction>().maybeNative() == native;
}

// Return whether looking up a method on 'obj' definitely resolves to the
// original specified native function. The method may conservatively return
// 'false' in the case of proxies or other non-native objects.
static MOZ_ALWAYS_INLINE bool HasNativeMethodPure(JSObject* obj,
                                                  PropertyName* name,
                                                  JSNative native,
                                                  JSContext* cx) {
  Value v;
  if (!GetPropertyPure(cx, obj, NameToId(name), &v)) {
    return false;
  }

  return IsNativeFunction(v, native);
}

// Return whether 'obj' definitely has no @@toPrimitive method.
static MOZ_ALWAYS_INLINE bool HasNoToPrimitiveMethodPure(JSObject* obj,
                                                         JSContext* cx) {
  JS::Symbol* toPrimitive = cx->wellKnownSymbols().toPrimitive;
  JSObject* holder;
  if (!MaybeHasInterestingSymbolProperty(cx, obj, toPrimitive, &holder)) {
#ifdef DEBUG
    NativeObject* pobj;
    PropertyResult prop;
    MOZ_ASSERT(
        LookupPropertyPure(cx, obj, SYMBOL_TO_JSID(toPrimitive), &pobj, &prop));
    MOZ_ASSERT(prop.isNotFound());
#endif
    return true;
  }

  NativeObject* pobj;
  PropertyResult prop;
  if (!LookupPropertyPure(cx, holder, SYMBOL_TO_JSID(toPrimitive), &pobj,
                          &prop)) {
    return false;
  }

  return prop.isNotFound();
}

extern bool ToPropertyKeySlow(JSContext* cx, HandleValue argument,
                              MutableHandleId result);

/* ES6 draft rev 28 (2014 Oct 14) 7.1.14 */
MOZ_ALWAYS_INLINE bool ToPropertyKey(JSContext* cx, HandleValue argument,
                                     MutableHandleId result) {
  if (MOZ_LIKELY(argument.isPrimitive())) {
    return PrimitiveValueToId<CanGC>(cx, argument, result);
  }

  return ToPropertyKeySlow(cx, argument, result);
}

/*
 * Return true if this is a compiler-created internal function accessed by
 * its own object. Such a function object must not be accessible to script
 * or embedding code.
 */
inline bool IsInternalFunctionObject(JSObject& funobj) {
  JSFunction& fun = funobj.as<JSFunction>();
  return fun.isInterpreted() && !fun.environment();
}

inline gc::InitialHeap GetInitialHeap(NewObjectKind newKind,
                                      const JSClass* clasp,
                                      gc::AllocSite* site = nullptr) {
  if (newKind != GenericObject) {
    return gc::TenuredHeap;
  }
  if (clasp->hasFinalize() && !CanNurseryAllocateFinalizedClass(clasp)) {
    return gc::TenuredHeap;
  }
  if (site) {
    return site->initialHeap();
  }
  return gc::DefaultHeap;
}

/*
 * Make an object with the specified prototype. If parent is null, it will
 * default to the prototype's global if the prototype is non-null.
 */
JSObject* NewObjectWithGivenTaggedProto(JSContext* cx, const JSClass* clasp,
                                        Handle<TaggedProto> proto,
                                        gc::AllocKind allocKind,
                                        NewObjectKind newKind,
                                        ObjectFlags objectFlags = {});

template <NewObjectKind NewKind>
inline JSObject* NewObjectWithGivenTaggedProto(JSContext* cx,
                                               const JSClass* clasp,
                                               Handle<TaggedProto> proto) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);
  return NewObjectWithGivenTaggedProto(cx, clasp, proto, allocKind, NewKind);
}

namespace detail {

template <typename T, NewObjectKind NewKind>
inline T* NewObjectWithGivenTaggedProtoForKind(JSContext* cx,
                                               Handle<TaggedProto> proto) {
  JSObject* obj = NewObjectWithGivenTaggedProto<NewKind>(cx, &T::class_, proto);
  return obj ? &obj->as<T>() : nullptr;
}

}  // namespace detail

template <typename T>
inline T* NewObjectWithGivenTaggedProto(JSContext* cx,
                                        Handle<TaggedProto> proto) {
  return detail::NewObjectWithGivenTaggedProtoForKind<T, GenericObject>(cx,
                                                                        proto);
}

inline JSObject* NewObjectWithGivenProto(
    JSContext* cx, const JSClass* clasp, HandleObject proto,
    gc::AllocKind allocKind, NewObjectKind newKind = GenericObject) {
  return NewObjectWithGivenTaggedProto(cx, clasp, AsTaggedProto(proto),
                                       allocKind, newKind);
}

inline JSObject* NewObjectWithGivenProto(JSContext* cx, const JSClass* clasp,
                                         HandleObject proto) {
  return NewObjectWithGivenTaggedProto<GenericObject>(cx, clasp,
                                                      AsTaggedProto(proto));
}

inline JSObject* NewTenuredObjectWithGivenProto(JSContext* cx,
                                                const JSClass* clasp,
                                                HandleObject proto) {
  return NewObjectWithGivenTaggedProto<TenuredObject>(cx, clasp,
                                                      AsTaggedProto(proto));
}

template <typename T>
inline T* NewObjectWithGivenProto(JSContext* cx, HandleObject proto) {
  return detail::NewObjectWithGivenTaggedProtoForKind<T, GenericObject>(
      cx, AsTaggedProto(proto));
}

template <typename T>
inline T* NewTenuredObjectWithGivenProto(JSContext* cx, HandleObject proto) {
  return detail::NewObjectWithGivenTaggedProtoForKind<T, TenuredObject>(
      cx, AsTaggedProto(proto));
}

template <typename T>
inline T* NewObjectWithGivenProtoAndKinds(JSContext* cx, HandleObject proto,
                                          gc::AllocKind allocKind,
                                          NewObjectKind newKind) {
  JSObject* obj = NewObjectWithGivenTaggedProto(
      cx, &T::class_, AsTaggedProto(proto), allocKind, newKind);
  return obj ? &obj->as<T>() : nullptr;
}

// Make an object with the prototype set according to the cached prototype or
// Object.prototype.
JSObject* NewObjectWithClassProto(JSContext* cx, const JSClass* clasp,
                                  HandleObject proto, gc::AllocKind allocKind,
                                  NewObjectKind newKind = GenericObject);

inline JSObject* NewObjectWithClassProto(
    JSContext* cx, const JSClass* clasp, HandleObject proto,
    NewObjectKind newKind = GenericObject) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);
  return NewObjectWithClassProto(cx, clasp, proto, allocKind, newKind);
}

template <class T>
inline T* NewObjectWithClassProto(JSContext* cx, HandleObject proto) {
  JSObject* obj = NewObjectWithClassProto(cx, &T::class_, proto, GenericObject);
  return obj ? &obj->as<T>() : nullptr;
}

template <class T>
inline T* NewObjectWithClassProtoAndKind(JSContext* cx, HandleObject proto,
                                         NewObjectKind newKind) {
  JSObject* obj = NewObjectWithClassProto(cx, &T::class_, proto, newKind);
  return obj ? &obj->as<T>() : nullptr;
}

template <class T>
inline T* NewObjectWithClassProto(JSContext* cx, HandleObject proto,
                                  gc::AllocKind allocKind,
                                  NewObjectKind newKind = GenericObject) {
  JSObject* obj =
      NewObjectWithClassProto(cx, &T::class_, proto, allocKind, newKind);
  return obj ? &obj->as<T>() : nullptr;
}

/*
 * Create a native instance of the given class with parent and proto set
 * according to the context's active global.
 */
inline JSObject* NewBuiltinClassInstance(
    JSContext* cx, const JSClass* clasp, gc::AllocKind allocKind,
    NewObjectKind newKind = GenericObject) {
  return NewObjectWithClassProto(cx, clasp, nullptr, allocKind, newKind);
}

inline JSObject* NewBuiltinClassInstance(
    JSContext* cx, const JSClass* clasp,
    NewObjectKind newKind = GenericObject) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);
  return NewBuiltinClassInstance(cx, clasp, allocKind, newKind);
}

template <typename T>
inline T* NewBuiltinClassInstance(JSContext* cx) {
  JSObject* obj = NewBuiltinClassInstance(cx, &T::class_, GenericObject);
  return obj ? &obj->as<T>() : nullptr;
}

template <typename T>
inline T* NewTenuredBuiltinClassInstance(JSContext* cx) {
  JSObject* obj = NewBuiltinClassInstance(cx, &T::class_, TenuredObject);
  return obj ? &obj->as<T>() : nullptr;
}

template <typename T>
inline T* NewBuiltinClassInstanceWithKind(JSContext* cx,
                                          NewObjectKind newKind) {
  JSObject* obj = NewBuiltinClassInstance(cx, &T::class_, newKind);
  return obj ? &obj->as<T>() : nullptr;
}

template <typename T>
inline T* NewBuiltinClassInstance(JSContext* cx, gc::AllocKind allocKind,
                                  NewObjectKind newKind = GenericObject) {
  JSObject* obj = NewBuiltinClassInstance(cx, &T::class_, allocKind, newKind);
  return obj ? &obj->as<T>() : nullptr;
}

// Used to optimize calls to (new Object())
bool NewObjectScriptedCall(JSContext* cx, MutableHandleObject obj);

/*
 * As for gc::GetGCObjectKind, where numElements is a guess at the final size of
 * the object, zero if the final size is unknown. This should only be used for
 * objects that do not require any fixed slots.
 */
static inline gc::AllocKind GuessObjectGCKind(size_t numElements) {
  if (numElements) {
    return gc::GetGCObjectKind(numElements);
  }
  return gc::AllocKind::OBJECT4;
}

static inline gc::AllocKind GuessArrayGCKind(size_t numElements) {
  if (numElements) {
    return gc::GetGCArrayKind(numElements);
  }
  return gc::AllocKind::OBJECT8;
}

// Returns ESClass::Other if the value isn't an object, or if the object
// isn't of one of the enumerated classes.  Otherwise returns the appropriate
// class.
inline bool GetClassOfValue(JSContext* cx, HandleValue v, ESClass* cls) {
  if (!v.isObject()) {
    *cls = ESClass::Other;
    return true;
  }

  RootedObject obj(cx, &v.toObject());
  return JS::GetBuiltinClass(cx, obj, cls);
}

extern NativeObject* InitClass(JSContext* cx, HandleObject obj,
                               HandleObject parent_proto, const JSClass* clasp,
                               JSNative constructor, unsigned nargs,
                               const JSPropertySpec* ps,
                               const JSFunctionSpec* fs,
                               const JSPropertySpec* static_ps,
                               const JSFunctionSpec* static_fs,
                               NativeObject** ctorp = nullptr);

MOZ_ALWAYS_INLINE const char* GetObjectClassName(JSContext* cx,
                                                 HandleObject obj) {
  if (obj->is<ProxyObject>()) {
    return Proxy::className(cx, obj);
  }

  return obj->getClass()->name;
}

inline bool IsCallable(const Value& v) {
  return v.isObject() && v.toObject().isCallable();
}

// ES6 rev 24 (2014 April 27) 7.2.5 IsConstructor
inline bool IsConstructor(const Value& v) {
  return v.isObject() && v.toObject().isConstructor();
}

static inline bool MaybePreserveDOMWrapper(JSContext* cx, HandleObject obj) {
  if (!obj->getClass()->isDOMClass()) {
    return true;
  }

  MOZ_ASSERT(cx->runtime()->preserveWrapperCallback);
  return cx->runtime()->preserveWrapperCallback(cx, obj);
}

} /* namespace js */

MOZ_ALWAYS_INLINE bool JSObject::isCallable() const {
  if (is<JSFunction>()) {
    return true;
  }
  if (is<js::ProxyObject>()) {
    const js::ProxyObject& p = as<js::ProxyObject>();
    return p.handler()->isCallable(const_cast<JSObject*>(this));
  }
  return callHook() != nullptr;
}

MOZ_ALWAYS_INLINE bool JSObject::isConstructor() const {
  if (is<JSFunction>()) {
    const JSFunction& fun = as<JSFunction>();
    return fun.isConstructor();
  }
  if (is<js::ProxyObject>()) {
    const js::ProxyObject& p = as<js::ProxyObject>();
    return p.handler()->isConstructor(const_cast<JSObject*>(this));
  }
  return constructHook() != nullptr;
}

MOZ_ALWAYS_INLINE JSNative JSObject::callHook() const {
  return getClass()->getCall();
}

MOZ_ALWAYS_INLINE JSNative JSObject::constructHook() const {
  return getClass()->getConstruct();
}

#endif /* vm_JSObject_inl_h */

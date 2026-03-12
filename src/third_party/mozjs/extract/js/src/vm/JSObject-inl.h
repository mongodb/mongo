/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSObject_inl_h
#define vm_JSObject_inl_h

#include "vm/JSObject.h"

#include "gc/Allocator.h"
#include "gc/Zone.h"
#include "js/Object.h"  // JS::GetBuiltinClass
#include "vm/ArrayObject.h"
#include "vm/BoundFunctionObject.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSFunction.h"
#include "vm/PropertyResult.h"
#include "vm/TypedArrayObject.h"
#include "gc/BufferAllocator-inl.h"
#include "gc/GCContext-inl.h"
#include "gc/ObjectKind-inl.h"
#include "vm/ObjectOperations-inl.h"  // js::MaybeHasInterestingSymbolProperty

namespace js {

// Get the GC kind to use for scripted 'new', empty object literals ({}), and
// the |Object| constructor.
static inline gc::AllocKind NewObjectGCKind() { return gc::AllocKind::OBJECT4; }

}  // namespace js

MOZ_ALWAYS_INLINE uint32_t js::NativeObject::numDynamicSlots() const {
  uint32_t slots = getSlotsHeader()->capacity();
  MOZ_ASSERT(slots == calculateDynamicSlots());
  MOZ_ASSERT_IF(hasDynamicSlots() && !hasUniqueId(), slots != 0);

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
#ifdef DEBUG
    size_t count = SLOT_CAPACITY_MIN + ObjectSlots::VALUES_PER_HEADER;
    MOZ_ASSERT(count == gc::GetGoodPower2ElementCount(count, sizeof(Value)));
#endif
    return SLOT_CAPACITY_MIN;
  }

  uint32_t count = gc::GetGoodPower2ElementCount(
      ndynamic + ObjectSlots::VALUES_PER_HEADER, sizeof(Value));

  uint32_t slots = count - ObjectSlots::VALUES_PER_HEADER;
  MOZ_ASSERT(slots >= ndynamic);
  return slots;
}

/* static */ MOZ_ALWAYS_INLINE uint32_t
js::NativeObject::calculateDynamicSlots(SharedShape* shape) {
  return calculateDynamicSlots(shape->numFixedSlots(), shape->slotSpan(),
                               shape->getObjectClass());
}

inline void JSObject::finalize(JS::GCContext* gcx) {
#ifdef DEBUG
  MOZ_ASSERT(isTenured());
  js::gc::AllocKind kind = asTenured().getAllocKind();
  MOZ_ASSERT(IsFinalizedKind(kind));
  MOZ_ASSERT_IF(IsForegroundFinalized(kind),
                js::CurrentThreadCanAccessZone(zoneFromAnyThread()));
#endif

  const JSClass* clasp = shape()->getObjectClass();
  MOZ_ASSERT(clasp->hasFinalize());
  clasp->doFinalize(gcx, this);
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

inline bool JSObject::setQualifiedVarObj(
    JSContext* cx, JS::Handle<js::WithEnvironmentObject*> obj) {
  MOZ_ASSERT(!obj->isSyntactic());
  return setFlag(cx, obj, js::ObjectFlag::QualifiedVarObj);
}

inline bool JSObject::canHaveFixedElements() const {
  return is<js::ArrayObject>();
}

namespace js {

#ifdef DEBUG
inline bool ClassCanHaveFixedData(const JSClass* clasp) {
  // Normally, the number of fixed slots given an object is the maximum
  // permitted for its size class. For array buffers and typed arrays we only
  // use enough to cover the class reserved slots, so that the remaining space
  // in the object's allocation is available for the buffer's data.
  return !clasp->isNativeObject() ||
         clasp == &js::FixedLengthArrayBufferObject::class_ ||
         clasp == &js::ResizableArrayBufferObject::class_ ||
         js::IsTypedArrayClass(clasp);
}
#endif

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
[[nodiscard]] static inline T* SetNewObjectMetadata(JSContext* cx, T* obj) {
  MOZ_ASSERT(cx->realm()->hasAllocationMetadataBuilder());
  MOZ_ASSERT(!cx->realm()->hasObjectPendingMetadata());

  // The metadata builder is invoked for each object created on the main thread,
  // except when it's suppressed or we're throwing over-recursion error.
  if (!cx->zone()->suppressAllocationMetadataBuilder &&
      !cx->isThrowingOverRecursed()) {
    // Don't collect metadata on objects that represent metadata, to avoid
    // recursion.
    AutoSuppressAllocationMetadataBuilder suppressMetadata(cx);

    Rooted<T*> rooted(cx, obj);
    cx->realm()->setNewObjectMetadata(cx, rooted);
    return rooted;
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

inline bool JSObject::hasInvalidatedTeleporting() const {
  return hasFlag(js::ObjectFlag::InvalidatedTeleporting);
}

inline bool JSObject::needsProxyGetSetResultValidation() const {
  return hasFlag(js::ObjectFlag::NeedsProxyGetSetResultValidation);
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
    MOZ_ASSERT(LookupPropertyPure(cx, obj, PropertyKey::Symbol(toPrimitive),
                                  &pobj, &prop));
    MOZ_ASSERT(prop.isNotFound());
#endif
    return true;
  }

  NativeObject* pobj;
  PropertyResult prop;
  if (!LookupPropertyPure(cx, holder, PropertyKey::Symbol(toPrimitive), &pobj,
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

inline gc::Heap GetInitialHeap(NewObjectKind newKind, const JSClass* clasp,
                               gc::AllocSite* site = nullptr) {
  if (newKind != GenericObject) {
    return gc::Heap::Tenured;
  }
  if (clasp->hasFinalize() && !CanNurseryAllocateFinalizedClass(clasp)) {
    return gc::Heap::Tenured;
  }
  if (site) {
    return site->initialHeap();
  }
  return gc::Heap::Default;
}

/*
 * Make an object with the specified prototype. If parent is null, it will
 * default to the prototype's global if the prototype is non-null.
 */
NativeObject* NewObjectWithGivenTaggedProto(JSContext* cx, const JSClass* clasp,
                                            Handle<TaggedProto> proto,
                                            gc::AllocKind allocKind,
                                            NewObjectKind newKind,
                                            ObjectFlags objFlags);

NativeObject* NewObjectWithGivenTaggedProtoAndAllocSite(
    JSContext* cx, const JSClass* clasp, Handle<TaggedProto> proto,
    gc::AllocKind allocKind, NewObjectKind newKind, ObjectFlags objFlags,
    gc::AllocSite* site);

template <NewObjectKind NewKind>
inline NativeObject* NewObjectWithGivenTaggedProto(JSContext* cx,
                                                   const JSClass* clasp,
                                                   Handle<TaggedProto> proto,
                                                   ObjectFlags objFlags) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);
  return NewObjectWithGivenTaggedProto(cx, clasp, proto, allocKind, NewKind,
                                       objFlags);
}

template <NewObjectKind NewKind>
inline NativeObject* NewObjectWithGivenTaggedProtoAndAllocSite(
    JSContext* cx, const JSClass* clasp, Handle<TaggedProto> proto,
    ObjectFlags objFlags, gc::AllocSite* site) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);
  return NewObjectWithGivenTaggedProtoAndAllocSite(cx, clasp, proto, allocKind,
                                                   NewKind, objFlags, site);
}

namespace detail {

template <typename T, NewObjectKind NewKind>
inline T* NewObjectWithGivenTaggedProtoForKind(JSContext* cx,
                                               Handle<TaggedProto> proto) {
  JSObject* obj = NewObjectWithGivenTaggedProto<NewKind>(cx, &T::class_, proto,
                                                         ObjectFlags());
  return obj ? &obj->as<T>() : nullptr;
}

}  // namespace detail

template <typename T>
inline T* NewObjectWithGivenTaggedProto(JSContext* cx,
                                        Handle<TaggedProto> proto) {
  return detail::NewObjectWithGivenTaggedProtoForKind<T, GenericObject>(cx,
                                                                        proto);
}

inline NativeObject* NewObjectWithGivenProto(JSContext* cx,
                                             const JSClass* clasp,
                                             HandleObject proto) {
  return NewObjectWithGivenTaggedProto<GenericObject>(
      cx, clasp, AsTaggedProto(proto), ObjectFlags());
}

inline NativeObject* NewObjectWithGivenProtoAndAllocSite(
    JSContext* cx, const JSClass* clasp, HandleObject proto,
    js::gc::AllocSite* site) {
  return NewObjectWithGivenTaggedProtoAndAllocSite<GenericObject>(
      cx, clasp, AsTaggedProto(proto), ObjectFlags(), site);
}

inline NativeObject* NewTenuredObjectWithGivenProto(
    JSContext* cx, const JSClass* clasp, HandleObject proto,
    ObjectFlags objFlags = ObjectFlags()) {
  return NewObjectWithGivenTaggedProto<TenuredObject>(
      cx, clasp, AsTaggedProto(proto), objFlags);
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
      cx, &T::class_, AsTaggedProto(proto), allocKind, newKind, ObjectFlags());
  return obj ? &obj->as<T>() : nullptr;
}

// Make an object with the prototype set according to the cached prototype or
// Object.prototype.
NativeObject* NewObjectWithClassProto(JSContext* cx, const JSClass* clasp,
                                      HandleObject proto,
                                      gc::AllocKind allocKind,
                                      NewObjectKind newKind = GenericObject,
                                      ObjectFlags objFlags = ObjectFlags());

inline NativeObject* NewObjectWithClassProto(
    JSContext* cx, const JSClass* clasp, HandleObject proto,
    NewObjectKind newKind = GenericObject,
    ObjectFlags objFlags = ObjectFlags()) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);
  return NewObjectWithClassProto(cx, clasp, proto, allocKind, newKind,
                                 objFlags);
}

template <class T>
inline T* NewObjectWithClassProto(JSContext* cx, HandleObject proto) {
  JSObject* obj = NewObjectWithClassProto(cx, &T::class_, proto, GenericObject);
  return obj ? &obj->as<T>() : nullptr;
}

template <class T>
inline T* NewObjectWithClassProtoAndKind(JSContext* cx, HandleObject proto,
                                         NewObjectKind newKind,
                                         ObjectFlags objFlags = ObjectFlags()) {
  JSObject* obj =
      NewObjectWithClassProto(cx, &T::class_, proto, newKind, objFlags);
  return obj ? &obj->as<T>() : nullptr;
}

template <class T>
inline T* NewObjectWithClassProto(JSContext* cx, HandleObject proto,
                                  gc::AllocKind allocKind,
                                  NewObjectKind newKind = GenericObject) {
  NativeObject* obj =
      NewObjectWithClassProto(cx, &T::class_, proto, allocKind, newKind);
  return obj ? &obj->as<T>() : nullptr;
}

/*
 * Create a native instance of the given class with parent and proto set
 * according to the context's active global.
 */
inline NativeObject* NewBuiltinClassInstance(
    JSContext* cx, const JSClass* clasp, gc::AllocKind allocKind,
    NewObjectKind newKind = GenericObject) {
  return NewObjectWithClassProto(cx, clasp, nullptr, allocKind, newKind);
}

inline NativeObject* NewBuiltinClassInstance(
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

static constexpr gc::AllocKind GuessArrayGCKind(size_t numElements) {
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

extern NativeObject* InitClass(
    JSContext* cx, HandleObject obj, const JSClass* protoClass,
    HandleObject protoProto, const char* name, JSNative constructor,
    unsigned nargs, const JSPropertySpec* ps, const JSFunctionSpec* fs,
    const JSPropertySpec* static_ps, const JSFunctionSpec* static_fs,
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
  if (is<js::BoundFunctionObject>()) {
    const js::BoundFunctionObject& bound = as<js::BoundFunctionObject>();
    return bound.isConstructor();
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

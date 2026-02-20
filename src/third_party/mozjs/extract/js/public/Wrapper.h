/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Wrapper_h
#define js_Wrapper_h

#include "mozilla/Attributes.h"

#include "js/Proxy.h"

namespace js {
struct CompartmentFilter;

/*
 * Helper for Wrapper::New default options.
 *
 * Callers of Wrapper::New() who wish to specify a prototype for the created
 * Wrapper, *MUST* construct a WrapperOptions with a JSContext.
 */
class MOZ_STACK_CLASS WrapperOptions : public ProxyOptions {
 public:
  WrapperOptions() : ProxyOptions(false), proto_() {}

  explicit WrapperOptions(JSContext* cx) : ProxyOptions(false), proto_() {
    proto_.emplace(cx);
  }

  inline JSObject* proto() const;
  WrapperOptions& setProto(JSObject* protoArg) {
    MOZ_ASSERT(proto_);
    *proto_ = protoArg;
    return *this;
  }

 private:
  mozilla::Maybe<JS::RootedObject> proto_;
};

// Base class for proxy handlers that want to forward all operations to an
// object stored in the proxy's private slot.
class JS_PUBLIC_API ForwardingProxyHandler : public BaseProxyHandler {
 public:
  using BaseProxyHandler::BaseProxyHandler;

  /* Standard internal methods. */
  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc)
      const override;
  virtual bool defineProperty(JSContext* cx, JS::HandleObject proxy,
                              JS::HandleId id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool ownPropertyKeys(JSContext* cx, JS::HandleObject proxy,
                               JS::MutableHandleIdVector props) const override;
  virtual bool delete_(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                       JS::ObjectOpResult& result) const override;
  virtual bool enumerate(JSContext* cx, JS::HandleObject proxy,
                         JS::MutableHandleIdVector props) const override;
  virtual bool getPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::MutableHandleObject protop) const override;
  virtual bool setPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::HandleObject proto,
                            JS::ObjectOpResult& result) const override;
  virtual bool getPrototypeIfOrdinary(
      JSContext* cx, JS::HandleObject proxy, bool* isOrdinary,
      JS::MutableHandleObject protop) const override;
  virtual bool setImmutablePrototype(JSContext* cx, JS::HandleObject proxy,
                                     bool* succeeded) const override;
  virtual bool preventExtensions(JSContext* cx, JS::HandleObject proxy,
                                 JS::ObjectOpResult& result) const override;
  virtual bool isExtensible(JSContext* cx, JS::HandleObject proxy,
                            bool* extensible) const override;
  virtual bool has(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                   bool* bp) const override;
  virtual bool get(JSContext* cx, JS::HandleObject proxy,
                   JS::HandleValue receiver, JS::HandleId id,
                   JS::MutableHandleValue vp) const override;
  virtual bool set(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                   JS::HandleValue v, JS::HandleValue receiver,
                   JS::ObjectOpResult& result) const override;
  virtual bool call(JSContext* cx, JS::HandleObject proxy,
                    const JS::CallArgs& args) const override;
  virtual bool construct(JSContext* cx, JS::HandleObject proxy,
                         const JS::CallArgs& args) const override;

  /* SpiderMonkey extensions. */
  virtual bool hasOwn(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                      bool* bp) const override;
  virtual bool getOwnEnumerablePropertyKeys(
      JSContext* cx, JS::HandleObject proxy,
      JS::MutableHandleIdVector props) const override;
  virtual bool nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                          JS::NativeImpl impl,
                          const JS::CallArgs& args) const override;
  virtual bool getBuiltinClass(JSContext* cx, JS::HandleObject proxy,
                               ESClass* cls) const override;
  virtual bool isArray(JSContext* cx, JS::HandleObject proxy,
                       JS::IsArrayAnswer* answer) const override;
  virtual const char* className(JSContext* cx,
                                JS::HandleObject proxy) const override;
  virtual JSString* fun_toString(JSContext* cx, JS::HandleObject proxy,
                                 bool isToSource) const override;
  virtual RegExpShared* regexp_toShared(JSContext* cx,
                                        JS::HandleObject proxy) const override;
  virtual bool boxedValue_unbox(JSContext* cx, JS::HandleObject proxy,
                                JS::MutableHandleValue vp) const override;
  virtual bool isCallable(JSObject* obj) const override;
  virtual bool isConstructor(JSObject* obj) const override;

  // Use the target object for private fields.
  virtual bool useProxyExpandoObjectForPrivateFields() const override {
    return false;
  }
};

/*
 * A wrapper is a proxy with a target object to which it generally forwards
 * operations, but may restrict access to certain operations or augment those
 * operations in various ways.
 *
 * A wrapper can be "unwrapped" in C++, exposing the underlying object.
 * Callers should be careful to avoid unwrapping security wrappers in the wrong
 * context.
 *
 * Important: If you add a method implementation here, you probably also need
 * to add an override in CrossCompartmentWrapper. If you don't, you risk
 * compartment mismatches. See bug 945826 comment 0.
 */
class JS_PUBLIC_API Wrapper : public ForwardingProxyHandler {
  unsigned mFlags;

 public:
  explicit constexpr Wrapper(unsigned aFlags, bool aHasPrototype = false,
                             bool aHasSecurityPolicy = false)
      : ForwardingProxyHandler(&family, aHasPrototype, aHasSecurityPolicy),
        mFlags(aFlags) {}

  virtual bool finalizeInBackground(const JS::Value& priv) const override;

  /**
   * A hook subclasses can override to implement CheckedUnwrapDynamic
   * behavior.  The JSContext represents the "who is trying to unwrap?" Realm.
   * The JSObject is the wrapper that the caller is trying to unwrap.
   */
  virtual bool dynamicCheckedUnwrapAllowed(JS::HandleObject obj,
                                           JSContext* cx) const {
    MOZ_ASSERT(hasSecurityPolicy(), "Why are you asking?");
    return false;
  }

  using BaseProxyHandler::Action;

  enum Flags { CROSS_COMPARTMENT = 1 << 0, LAST_USED_FLAG = CROSS_COMPARTMENT };

  static JSObject* New(JSContext* cx, JSObject* obj, const Wrapper* handler,
                       const WrapperOptions& options = WrapperOptions());

  static JSObject* Renew(JSObject* existing, JSObject* obj,
                         const Wrapper* handler);

  static inline const Wrapper* wrapperHandler(const JSObject* wrapper);

  static JSObject* wrappedObject(JSObject* wrapper);

  unsigned flags() const { return mFlags; }

  bool isCrossCompartmentWrapper() const {
    return !!(mFlags & CROSS_COMPARTMENT);
  }

  static const char family;
  static const Wrapper singleton;
  static const Wrapper singletonWithPrototype;

  static JSObject* const defaultProto;
};

inline JSObject* WrapperOptions::proto() const {
  return proto_ ? *proto_ : Wrapper::defaultProto;
}

/* Base class for all cross compartment wrapper handlers. */
class JS_PUBLIC_API CrossCompartmentWrapper : public Wrapper {
 public:
  explicit constexpr CrossCompartmentWrapper(unsigned aFlags,
                                             bool aHasPrototype = false,
                                             bool aHasSecurityPolicy = false)
      : Wrapper(CROSS_COMPARTMENT | aFlags, aHasPrototype, aHasSecurityPolicy) {
  }

  /* Standard internal methods. */
  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc)
      const override;
  virtual bool defineProperty(JSContext* cx, JS::HandleObject wrapper,
                              JS::HandleId id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool ownPropertyKeys(JSContext* cx, JS::HandleObject wrapper,
                               JS::MutableHandleIdVector props) const override;
  virtual bool delete_(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                       JS::ObjectOpResult& result) const override;
  virtual bool enumerate(JSContext* cx, JS::HandleObject proxy,
                         JS::MutableHandleIdVector props) const override;
  virtual bool getPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::MutableHandleObject protop) const override;
  virtual bool setPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::HandleObject proto,
                            JS::ObjectOpResult& result) const override;

  virtual bool getPrototypeIfOrdinary(
      JSContext* cx, JS::HandleObject proxy, bool* isOrdinary,
      JS::MutableHandleObject protop) const override;
  virtual bool setImmutablePrototype(JSContext* cx, JS::HandleObject proxy,
                                     bool* succeeded) const override;
  virtual bool preventExtensions(JSContext* cx, JS::HandleObject wrapper,
                                 JS::ObjectOpResult& result) const override;
  virtual bool isExtensible(JSContext* cx, JS::HandleObject wrapper,
                            bool* extensible) const override;
  virtual bool has(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                   bool* bp) const override;
  virtual bool get(JSContext* cx, JS::HandleObject wrapper,
                   JS::HandleValue receiver, JS::HandleId id,
                   JS::MutableHandleValue vp) const override;
  virtual bool set(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                   JS::HandleValue v, JS::HandleValue receiver,
                   JS::ObjectOpResult& result) const override;
  virtual bool call(JSContext* cx, JS::HandleObject wrapper,
                    const JS::CallArgs& args) const override;
  virtual bool construct(JSContext* cx, JS::HandleObject wrapper,
                         const JS::CallArgs& args) const override;

  /* SpiderMonkey extensions. */
  virtual bool hasOwn(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                      bool* bp) const override;
  virtual bool getOwnEnumerablePropertyKeys(
      JSContext* cx, JS::HandleObject wrapper,
      JS::MutableHandleIdVector props) const override;
  virtual bool nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                          JS::NativeImpl impl,
                          const JS::CallArgs& args) const override;
  virtual const char* className(JSContext* cx,
                                JS::HandleObject proxy) const override;
  virtual JSString* fun_toString(JSContext* cx, JS::HandleObject wrapper,
                                 bool isToSource) const override;
  virtual RegExpShared* regexp_toShared(JSContext* cx,
                                        JS::HandleObject proxy) const override;
  virtual bool boxedValue_unbox(JSContext* cx, JS::HandleObject proxy,
                                JS::MutableHandleValue vp) const override;

  // Allocate CrossCompartmentWrappers in the nursery.
  virtual bool canNurseryAllocate() const override { return true; }
  void finalize(JS::GCContext* gcx, JSObject* proxy) const final {
    Wrapper::finalize(gcx, proxy);
  }

  static const CrossCompartmentWrapper singleton;
  static const CrossCompartmentWrapper singletonWithPrototype;
};

class JS_PUBLIC_API OpaqueCrossCompartmentWrapper
    : public CrossCompartmentWrapper {
 public:
  explicit constexpr OpaqueCrossCompartmentWrapper()
      : CrossCompartmentWrapper(0) {}

  /* Standard internal methods. */
  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc)
      const override;
  virtual bool defineProperty(JSContext* cx, JS::HandleObject wrapper,
                              JS::HandleId id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool ownPropertyKeys(JSContext* cx, JS::HandleObject wrapper,
                               JS::MutableHandleIdVector props) const override;
  virtual bool delete_(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                       JS::ObjectOpResult& result) const override;
  virtual bool enumerate(JSContext* cx, JS::HandleObject proxy,
                         JS::MutableHandleIdVector props) const override;
  virtual bool getPrototype(JSContext* cx, JS::HandleObject wrapper,
                            JS::MutableHandleObject protop) const override;
  virtual bool setPrototype(JSContext* cx, JS::HandleObject wrapper,
                            JS::HandleObject proto,
                            JS::ObjectOpResult& result) const override;
  virtual bool getPrototypeIfOrdinary(
      JSContext* cx, JS::HandleObject wrapper, bool* isOrdinary,
      JS::MutableHandleObject protop) const override;
  virtual bool setImmutablePrototype(JSContext* cx, JS::HandleObject wrapper,
                                     bool* succeeded) const override;
  virtual bool preventExtensions(JSContext* cx, JS::HandleObject wrapper,
                                 JS::ObjectOpResult& result) const override;
  virtual bool isExtensible(JSContext* cx, JS::HandleObject wrapper,
                            bool* extensible) const override;
  virtual bool has(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                   bool* bp) const override;
  virtual bool get(JSContext* cx, JS::HandleObject wrapper,
                   JS::HandleValue receiver, JS::HandleId id,
                   JS::MutableHandleValue vp) const override;
  virtual bool set(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                   JS::HandleValue v, JS::HandleValue receiver,
                   JS::ObjectOpResult& result) const override;
  virtual bool call(JSContext* cx, JS::HandleObject wrapper,
                    const JS::CallArgs& args) const override;
  virtual bool construct(JSContext* cx, JS::HandleObject wrapper,
                         const JS::CallArgs& args) const override;

  /* SpiderMonkey extensions. */
  virtual bool hasOwn(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                      bool* bp) const override;
  virtual bool getOwnEnumerablePropertyKeys(
      JSContext* cx, JS::HandleObject wrapper,
      JS::MutableHandleIdVector props) const override;
  virtual bool getBuiltinClass(JSContext* cx, JS::HandleObject wrapper,
                               ESClass* cls) const override;
  virtual bool isArray(JSContext* cx, JS::HandleObject obj,
                       JS::IsArrayAnswer* answer) const override;
  virtual const char* className(JSContext* cx,
                                JS::HandleObject wrapper) const override;
  virtual JSString* fun_toString(JSContext* cx, JS::HandleObject proxy,
                                 bool isToSource) const override;

  static const OpaqueCrossCompartmentWrapper singleton;
};

/*
 * Base class for security wrappers. A security wrapper is potentially hiding
 * all or part of some wrapped object thus SecurityWrapper defaults to denying
 * access to the wrappee. This is the opposite of Wrapper which tries to be
 * completely transparent.
 *
 * NB: Currently, only a few ProxyHandler operations are overridden to deny
 * access, relying on derived SecurityWrapper to block access when necessary.
 */
template <class Base>
class JS_PUBLIC_API SecurityWrapper : public Base {
 public:
  explicit constexpr SecurityWrapper(unsigned flags, bool hasPrototype = false)
      : Base(flags, hasPrototype, /* hasSecurityPolicy = */ true) {}

  virtual bool enter(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                     Wrapper::Action act, bool mayThrow,
                     bool* bp) const override;

  virtual bool defineProperty(JSContext* cx, JS::HandleObject wrapper,
                              JS::HandleId id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const override;
  virtual bool isExtensible(JSContext* cx, JS::HandleObject wrapper,
                            bool* extensible) const override;
  virtual bool preventExtensions(JSContext* cx, JS::HandleObject wrapper,
                                 JS::ObjectOpResult& result) const override;
  virtual bool setPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::HandleObject proto,
                            JS::ObjectOpResult& result) const override;
  virtual bool setImmutablePrototype(JSContext* cx, JS::HandleObject proxy,
                                     bool* succeeded) const override;

  virtual bool nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                          JS::NativeImpl impl,
                          const JS::CallArgs& args) const override;
  virtual bool getBuiltinClass(JSContext* cx, JS::HandleObject wrapper,
                               ESClass* cls) const override;
  virtual bool isArray(JSContext* cx, JS::HandleObject wrapper,
                       JS::IsArrayAnswer* answer) const override;
  virtual RegExpShared* regexp_toShared(JSContext* cx,
                                        JS::HandleObject proxy) const override;
  virtual bool boxedValue_unbox(JSContext* cx, JS::HandleObject proxy,
                                JS::MutableHandleValue vp) const override;

  // Allow isCallable and isConstructor. They used to be class-level, and so
  // could not be guarded against.

  /*
   * Allow our subclasses to select the superclass behavior they want without
   * needing to specify an exact superclass.
   */
  typedef Base Permissive;
  typedef SecurityWrapper<Base> Restrictive;
};

typedef SecurityWrapper<CrossCompartmentWrapper>
    CrossCompartmentSecurityWrapper;

extern JSObject* TransparentObjectWrapper(JSContext* cx,
                                          JS::HandleObject existing,
                                          JS::HandleObject obj);

inline bool IsWrapper(const JSObject* obj) {
  return IsProxy(obj) && GetProxyHandler(obj)->family() == &Wrapper::family;
}

inline bool IsCrossCompartmentWrapper(const JSObject* obj) {
  return IsWrapper(obj) &&
         (Wrapper::wrapperHandler(obj)->flags() & Wrapper::CROSS_COMPARTMENT);
}

/* static */ inline const Wrapper* Wrapper::wrapperHandler(
    const JSObject* wrapper) {
  MOZ_ASSERT(IsWrapper(wrapper));
  return static_cast<const Wrapper*>(GetProxyHandler(wrapper));
}

// Given a JSObject, returns that object stripped of wrappers. If
// stopAtWindowProxy is true, then this returns the WindowProxy if it was
// previously wrapped. Otherwise, this returns the first object for which
// JSObject::isWrapper returns false.
//
// ExposeToActiveJS is called on wrapper targets to allow gray marking
// assertions to work while an incremental GC is in progress, but this means
// that this cannot be called from the GC or off the main thread.
JS_PUBLIC_API JSObject* UncheckedUnwrap(JSObject* obj,
                                        bool stopAtWindowProxy = true,
                                        unsigned* flagsp = nullptr);

// Given a JSObject, returns that object stripped of wrappers, except
// WindowProxy wrappers.  At each stage, the wrapper has the opportunity to veto
// the unwrap. Null is returned if there are security wrappers that can't be
// unwrapped.
//
// This does a static-only unwrap check: it basically checks whether _all_
// globals in the wrapper's source compartment should be able to access the
// wrapper target.  This won't necessarily return the right thing for the HTML
// spec's cross-origin objects (WindowProxy and Location), but is fine to use
// when failure to unwrap one of those objects wouldn't be a problem.  For
// example, if you want to test whether your target object is a specific class
// that's not WindowProxy or Location, you can use this.
//
// ExposeToActiveJS is called on wrapper targets to allow gray marking
// assertions to work while an incremental GC is in progress, but this means
// that this cannot be called from the GC or off the main thread.
JS_PUBLIC_API JSObject* CheckedUnwrapStatic(JSObject* obj);

// Unwrap only the outermost security wrapper, with the same semantics as
// above. This is the checked version of Wrapper::wrappedObject.
JS_PUBLIC_API JSObject* UnwrapOneCheckedStatic(JSObject* obj);

// Given a JSObject, returns that object stripped of wrappers. At each stage,
// the security wrapper has the opportunity to veto the unwrap. If
// stopAtWindowProxy is true, then this returns the WindowProxy if it was
// previously wrapped.  Null is returned if there are security wrappers that
// can't be unwrapped.
//
// ExposeToActiveJS is called on wrapper targets to allow gray marking
// assertions to work while an incremental GC is in progress, but this means
// that this cannot be called from the GC or off the main thread.
//
// The JSContext argument will be used for dynamic checks (needed by WindowProxy
// and Location) and should represent the Realm doing the unwrapping.  It is not
// used to throw exceptions; this function never throws.
//
// This function may be able to GC (and the static analysis definitely thinks it
// can), but it still takes a JSObject* argument, because some of its callers
// would actually have a bit of a hard time producing a Rooted.  And it ends up
// having to root internally anyway, because it wants to use the value in a loop
// and you can't assign to a HandleObject.  What this means is that callers who
// plan to use the argument object after they have called this function will
// need to root it to avoid hazard failures, even though this function doesn't
// require a Handle.
JS_PUBLIC_API JSObject* CheckedUnwrapDynamic(JSObject* obj, JSContext* cx,
                                             bool stopAtWindowProxy = true);

// Unwrap only the outermost security wrapper, with the same semantics as
// above. This is the checked version of Wrapper::wrappedObject.
JS_PUBLIC_API JSObject* UnwrapOneCheckedDynamic(JS::HandleObject obj,
                                                JSContext* cx,
                                                bool stopAtWindowProxy = true);

// Given a JSObject, returns that object stripped of wrappers. This returns the
// WindowProxy if it was previously wrapped.
//
// ExposeToActiveJS is not called on wrapper targets so this can be called from
// the GC or off the main thread.
JS_PUBLIC_API JSObject* UncheckedUnwrapWithoutExpose(JSObject* obj);

void ReportAccessDenied(JSContext* cx);

JS_PUBLIC_API void NukeCrossCompartmentWrapper(JSContext* cx,
                                               JSObject* wrapper);

// If a cross-compartment wrapper source => target exists, nuke it.
JS_PUBLIC_API void NukeCrossCompartmentWrapperIfExists(JSContext* cx,
                                                       JS::Compartment* source,
                                                       JSObject* target);

void RemapWrapper(JSContext* cx, JSObject* wobj, JSObject* newTarget);
void RemapDeadWrapper(JSContext* cx, JS::HandleObject wobj,
                      JS::HandleObject newTarget);

JS_PUBLIC_API bool RemapAllWrappersForObject(JSContext* cx,
                                             JS::HandleObject oldTarget,
                                             JS::HandleObject newTarget);

// API to recompute all cross-compartment wrappers whose source and target
// match the given filters.
JS_PUBLIC_API bool RecomputeWrappers(JSContext* cx,
                                     const CompartmentFilter& sourceFilter,
                                     const CompartmentFilter& targetFilter);

} /* namespace js */

#endif /* js_Wrapper_h */

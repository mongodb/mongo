/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Wrapper_h
#define js_Wrapper_h

#include "mozilla/Attributes.h"

#include "js/Proxy.h"

namespace js {

/*
 * Helper for Wrapper::New default options.
 *
 * Callers of Wrapper::New() who wish to specify a prototype for the created
 * Wrapper, *MUST* construct a WrapperOptions with a JSContext.
 */
class MOZ_STACK_CLASS WrapperOptions : public ProxyOptions {
  public:
    WrapperOptions() : ProxyOptions(false),
                       proto_()
    {}

    explicit WrapperOptions(JSContext* cx) : ProxyOptions(false),
                                             proto_()
    {
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
class JS_FRIEND_API(ForwardingProxyHandler) : public BaseProxyHandler
{
  public:
    using BaseProxyHandler::BaseProxyHandler;

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                         ObjectOpResult& result) const override;
    virtual JSObject* enumerate(JSContext* cx, HandleObject proxy) const override;
    virtual bool getPrototype(JSContext* cx, HandleObject proxy,
                              MutableHandleObject protop) const override;
    virtual bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                              ObjectOpResult& result) const override;
    virtual bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy, bool* isOrdinary,
                                        MutableHandleObject protop) const override;
    virtual bool setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                       bool* succeeded) const override;
    virtual bool preventExtensions(JSContext* cx, HandleObject proxy,
                                   ObjectOpResult& result) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const override;
    virtual bool has(JSContext* cx, HandleObject proxy, HandleId id,
                     bool* bp) const override;
    virtual bool get(JSContext* cx, HandleObject proxy, HandleValue receiver,
                     HandleId id, MutableHandleValue vp) const override;
    virtual bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                     HandleValue receiver, ObjectOpResult& result) const override;
    virtual bool call(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;
    virtual bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const override;

    /* SpiderMonkey extensions. */
    virtual bool getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                       MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool hasOwn(JSContext* cx, HandleObject proxy, HandleId id,
                        bool* bp) const override;
    virtual bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                              AutoIdVector& props) const override;
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const override;
    virtual bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                             bool* bp) const override;
    virtual bool getBuiltinClass(JSContext* cx, HandleObject proxy, ESClass* cls) const override;
    virtual bool isArray(JSContext* cx, HandleObject proxy,
                         JS::IsArrayAnswer* answer) const override;
    virtual const char* className(JSContext* cx, HandleObject proxy) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy,
                                   bool isToSource) const override;
    virtual RegExpShared* regexp_toShared(JSContext* cx, HandleObject proxy) const override;
    virtual bool boxedValue_unbox(JSContext* cx, HandleObject proxy,
                                  MutableHandleValue vp) const override;
    virtual bool isCallable(JSObject* obj) const override;
    virtual bool isConstructor(JSObject* obj) const override;
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
class JS_FRIEND_API(Wrapper) : public ForwardingProxyHandler
{
    unsigned mFlags;

  public:
    explicit constexpr Wrapper(unsigned aFlags, bool aHasPrototype = false,
                               bool aHasSecurityPolicy = false)
      : ForwardingProxyHandler(&family, aHasPrototype, aHasSecurityPolicy),
        mFlags(aFlags)
    { }

    virtual bool finalizeInBackground(const Value& priv) const override;
    virtual JSObject* weakmapKeyDelegate(JSObject* proxy) const override;

    using BaseProxyHandler::Action;

    enum Flags {
        CROSS_COMPARTMENT = 1 << 0,
        LAST_USED_FLAG = CROSS_COMPARTMENT
    };

    static JSObject* New(JSContext* cx, JSObject* obj, const Wrapper* handler,
                         const WrapperOptions& options = WrapperOptions());

    static JSObject* Renew(JSObject* existing, JSObject* obj, const Wrapper* handler);

    static const Wrapper* wrapperHandler(JSObject* wrapper);

    static JSObject* wrappedObject(JSObject* wrapper);

    unsigned flags() const {
        return mFlags;
    }

    static const char family;
    static const Wrapper singleton;
    static const Wrapper singletonWithPrototype;

    static JSObject* defaultProto;
};

inline JSObject*
WrapperOptions::proto() const
{
    return proto_ ? *proto_ : Wrapper::defaultProto;
}

/* Base class for all cross compartment wrapper handlers. */
class JS_FRIEND_API(CrossCompartmentWrapper) : public Wrapper
{
  public:
    explicit constexpr CrossCompartmentWrapper(unsigned aFlags, bool aHasPrototype = false,
                                                   bool aHasSecurityPolicy = false)
      : Wrapper(CROSS_COMPARTMENT | aFlags, aHasPrototype, aHasSecurityPolicy)
    { }

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject wrapper, HandleId id,
                         ObjectOpResult& result) const override;
    virtual JSObject* enumerate(JSContext* cx, HandleObject wrapper) const override;
    virtual bool getPrototype(JSContext* cx, HandleObject proxy,
                              MutableHandleObject protop) const override;
    virtual bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                              ObjectOpResult& result) const override;

    virtual bool getPrototypeIfOrdinary(JSContext* cx, HandleObject proxy, bool* isOrdinary,
                                        MutableHandleObject protop) const override;
    virtual bool setImmutablePrototype(JSContext* cx, HandleObject proxy,
                                       bool* succeeded) const override;
    virtual bool preventExtensions(JSContext* cx, HandleObject wrapper,
                                   ObjectOpResult& result) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject wrapper, bool* extensible) const override;
    virtual bool has(JSContext* cx, HandleObject wrapper, HandleId id, bool* bp) const override;
    virtual bool get(JSContext* cx, HandleObject wrapper, HandleValue receiver,
                     HandleId id, MutableHandleValue vp) const override;
    virtual bool set(JSContext* cx, HandleObject wrapper, HandleId id, HandleValue v,
                     HandleValue receiver, ObjectOpResult& result) const override;
    virtual bool call(JSContext* cx, HandleObject wrapper, const CallArgs& args) const override;
    virtual bool construct(JSContext* cx, HandleObject wrapper, const CallArgs& args) const override;

    /* SpiderMonkey extensions. */
    virtual bool getPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                       MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool hasOwn(JSContext* cx, HandleObject wrapper, HandleId id, bool* bp) const override;
    virtual bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject wrapper,
                                              AutoIdVector& props) const override;
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const override;
    virtual bool hasInstance(JSContext* cx, HandleObject wrapper, MutableHandleValue v,
                             bool* bp) const override;
    virtual const char* className(JSContext* cx, HandleObject proxy) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject wrapper,
                                   bool isToSource) const override;
    virtual RegExpShared* regexp_toShared(JSContext* cx, HandleObject proxy) const override;
    virtual bool boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp) const override;

    // Allocate CrossCompartmentWrappers in the nursery.
    virtual bool canNurseryAllocate() const override { return true; }

    static const CrossCompartmentWrapper singleton;
    static const CrossCompartmentWrapper singletonWithPrototype;
};

class JS_FRIEND_API(OpaqueCrossCompartmentWrapper) : public CrossCompartmentWrapper
{
  public:
    explicit constexpr OpaqueCrossCompartmentWrapper() : CrossCompartmentWrapper(0)
    { }

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject wrapper,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject wrapper, HandleId id,
                         ObjectOpResult& result) const override;
    virtual JSObject* enumerate(JSContext* cx, HandleObject wrapper) const override;
    virtual bool getPrototype(JSContext* cx, HandleObject wrapper,
                              MutableHandleObject protop) const override;
    virtual bool setPrototype(JSContext* cx, HandleObject wrapper, HandleObject proto,
                              ObjectOpResult& result) const override;
    virtual bool getPrototypeIfOrdinary(JSContext* cx, HandleObject wrapper, bool* isOrdinary,
                                        MutableHandleObject protop) const override;
    virtual bool setImmutablePrototype(JSContext* cx, HandleObject wrapper,
                                       bool* succeeded) const override;
    virtual bool preventExtensions(JSContext* cx, HandleObject wrapper,
                                   ObjectOpResult& result) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject wrapper, bool* extensible) const override;
    virtual bool has(JSContext* cx, HandleObject wrapper, HandleId id,
                     bool* bp) const override;
    virtual bool get(JSContext* cx, HandleObject wrapper, HandleValue receiver,
                     HandleId id, MutableHandleValue vp) const override;
    virtual bool set(JSContext* cx, HandleObject wrapper, HandleId id, HandleValue v,
                     HandleValue receiver, ObjectOpResult& result) const override;
    virtual bool call(JSContext* cx, HandleObject wrapper, const CallArgs& args) const override;
    virtual bool construct(JSContext* cx, HandleObject wrapper, const CallArgs& args) const override;

    /* SpiderMonkey extensions. */
    virtual bool getPropertyDescriptor(JSContext* cx, HandleObject wrapper, HandleId id,
                                       MutableHandle<PropertyDescriptor> desc) const override;
    virtual bool hasOwn(JSContext* cx, HandleObject wrapper, HandleId id,
                        bool* bp) const override;
    virtual bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject wrapper,
                                              AutoIdVector& props) const override;
    virtual bool getBuiltinClass(JSContext* cx, HandleObject wrapper, ESClass* cls) const override;
    virtual bool isArray(JSContext* cx, HandleObject obj,
                         JS::IsArrayAnswer* answer) const override;
    virtual const char* className(JSContext* cx, HandleObject wrapper) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy,
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
class JS_FRIEND_API(SecurityWrapper) : public Base
{
  public:
    explicit constexpr SecurityWrapper(unsigned flags, bool hasPrototype = false)
      : Base(flags, hasPrototype, /* hasSecurityPolicy = */ true)
    { }

    virtual bool enter(JSContext* cx, HandleObject wrapper, HandleId id, Wrapper::Action act,
                       bool mayThrow, bool* bp) const override;

    virtual bool defineProperty(JSContext* cx, HandleObject wrapper, HandleId id,
                                Handle<PropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool isExtensible(JSContext* cx, HandleObject wrapper, bool* extensible) const override;
    virtual bool preventExtensions(JSContext* cx, HandleObject wrapper,
                                   ObjectOpResult& result) const override;
    virtual bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                              ObjectOpResult& result) const override;
    virtual bool setImmutablePrototype(JSContext* cx, HandleObject proxy, bool* succeeded) const override;

    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const override;
    virtual bool getBuiltinClass(JSContext* cx, HandleObject wrapper, ESClass* cls) const override;
    virtual bool isArray(JSContext* cx, HandleObject wrapper, JS::IsArrayAnswer* answer) const override;
    virtual RegExpShared* regexp_toShared(JSContext* cx, HandleObject proxy) const override;
    virtual bool boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp) const override;

    // Allow isCallable and isConstructor. They used to be class-level, and so could not be guarded
    // against.

    /*
     * Allow our subclasses to select the superclass behavior they want without
     * needing to specify an exact superclass.
     */
    typedef Base Permissive;
    typedef SecurityWrapper<Base> Restrictive;
};

typedef SecurityWrapper<CrossCompartmentWrapper> CrossCompartmentSecurityWrapper;

extern JSObject*
TransparentObjectWrapper(JSContext* cx, HandleObject existing, HandleObject obj);

inline bool
IsWrapper(JSObject* obj)
{
    return IsProxy(obj) && GetProxyHandler(obj)->family() == &Wrapper::family;
}

// Given a JSObject, returns that object stripped of wrappers. If
// stopAtWindowProxy is true, then this returns the WindowProxy if it was
// previously wrapped. Otherwise, this returns the first object for which
// JSObject::isWrapper returns false.
//
// ExposeToActiveJS is called on wrapper targets to allow gray marking
// assertions to work while an incremental GC is in progress, but this means
// that this cannot be called from the GC or off the main thread.
JS_FRIEND_API(JSObject*)
UncheckedUnwrap(JSObject* obj, bool stopAtWindowProxy = true, unsigned* flagsp = nullptr);

// Given a JSObject, returns that object stripped of wrappers. At each stage,
// the security wrapper has the opportunity to veto the unwrap. If
// stopAtWindowProxy is true, then this returns the WindowProxy if it was
// previously wrapped.
//
// ExposeToActiveJS is called on wrapper targets to allow gray marking
// assertions to work while an incremental GC is in progress, but this means
// that this cannot be called from the GC or off the main thread.
JS_FRIEND_API(JSObject*)
CheckedUnwrap(JSObject* obj, bool stopAtWindowProxy = true);

// Unwrap only the outermost security wrapper, with the same semantics as
// above. This is the checked version of Wrapper::wrappedObject.
JS_FRIEND_API(JSObject*)
UnwrapOneChecked(JSObject* obj, bool stopAtWindowProxy = true);

// Given a JSObject, returns that object stripped of wrappers. This returns the
// WindowProxy if it was previously wrapped.
//
// ExposeToActiveJS is not called on wrapper targets so this can be called from
// the GC or off the main thread.
JS_FRIEND_API(JSObject*)
UncheckedUnwrapWithoutExpose(JSObject* obj);

void
ReportAccessDenied(JSContext* cx);

JS_FRIEND_API(bool)
IsCrossCompartmentWrapper(JSObject* obj);

JS_FRIEND_API(void)
NukeCrossCompartmentWrapper(JSContext* cx, JSObject* wrapper);

void
RemapWrapper(JSContext* cx, JSObject* wobj, JSObject* newTarget);

JS_FRIEND_API(bool)
RemapAllWrappersForObject(JSContext* cx, JSObject* oldTarget,
                          JSObject* newTarget);

// API to recompute all cross-compartment wrappers whose source and target
// match the given filters.
JS_FRIEND_API(bool)
RecomputeWrappers(JSContext* cx, const CompartmentFilter& sourceFilter,
                  const CompartmentFilter& targetFilter);

} /* namespace js */

#endif /* js_Wrapper_h */

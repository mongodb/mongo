/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Proxy_h
#define js_Proxy_h

#include "mozilla/Maybe.h"

#include "jsfriendapi.h"

#include "js/CallNonGenericMethod.h"
#include "js/Class.h"

namespace js {

using JS::AutoIdVector;
using JS::CallArgs;
using JS::Handle;
using JS::HandleId;
using JS::HandleObject;
using JS::HandleValue;
using JS::IsAcceptableThis;
using JS::MutableHandle;
using JS::MutableHandleObject;
using JS::MutableHandleValue;
using JS::NativeImpl;
using JS::ObjectOpResult;
using JS::PrivateValue;
using JS::Value;

class RegExpGuard;
class JS_FRIEND_API(Wrapper);

/*
 * A proxy is a JSObject with highly customizable behavior. ES6 specifies a
 * single kind of proxy, but the customization mechanisms we use to implement
 * ES6 Proxy objects are also useful wherever an object with weird behavior is
 * wanted. Proxies are used to implement:
 *
 * -   the scope objects used by the Debugger's frame.eval() method
 *     (see js::GetDebugScopeForFunction)
 *
 * -   the khuey hack, whereby a whole compartment can be blown away
 *     even if other compartments hold references to objects in it
 *     (see js::NukeCrossCompartmentWrappers)
 *
 * -   XPConnect security wrappers, which protect chrome from malicious content
 *     (js/xpconnect/wrappers)
 *
 * -   DOM objects with special property behavior, like named getters
 *     (dom/bindings/Codegen.py generates these proxies from WebIDL)
 *
 * -   semi-transparent use of objects that live in other processes
 *     (CPOWs, implemented in js/ipc)
 *
 * ### Proxies and internal methods
 *
 * ES6 draft rev 27 (24 August 2014) specifies 14 internal methods. The runtime
 * semantics of just about everything a script can do to an object is specified
 * in terms of these internal methods. For example:
 *
 *     JS code                      ES6 internal method that gets called
 *     ---------------------------  --------------------------------
 *     obj.prop                     obj.[[Get]](obj, "prop")
 *     "prop" in obj                obj.[[HasProperty]]("prop")
 *     new obj()                    obj.[[Construct]](<empty argument List>)
 *     for (k in obj) {}            obj.[[Enumerate]]()
 *
 * With regard to the implementation of these internal methods, there are three
 * very different kinds of object in SpiderMonkey.
 *
 * 1.  Native objects' internal methods are implemented in vm/NativeObject.cpp,
 *     with duplicate (but functionally identical) implementations scattered
 *     through the ICs and JITs.
 *
 * 2.  Certain non-native objects have internal methods that are implemented as
 *     magical js::ObjectOps hooks. We're trying to get rid of these.
 *
 * 3.  All other objects are proxies. A proxy's internal methods are
 *     implemented in C++, as the virtual methods of a C++ object stored on the
 *     proxy, known as its handler.
 *
 * This means that just about anything you do to a proxy will end up going
 * through a C++ virtual method call. Possibly several. There's no reason the
 * JITs and ICs can't specialize for particular proxies, based on the handler;
 * but currently we don't do much of this, so the virtual method overhead
 * typically is actually incurred.
 *
 * ### The proxy handler hierarchy
 *
 * A major use case for proxies is to forward each internal method call to
 * another object, known as its target. The target can be an arbitrary JS
 * object. Not every proxy has the notion of a target, however.
 *
 * To minimize code duplication, a set of abstract proxy handler classes is
 * provided, from which other handlers may inherit. These abstract classes are
 * organized in the following hierarchy:
 *
 *     BaseProxyHandler
 *     |
 *     DirectProxyHandler        // has a target
 *     |
 *     Wrapper                   // can be unwrapped, revealing target
 *     |                         // (see js::CheckedUnwrap)
 *     |
 *     CrossCompartmentWrapper   // target is in another compartment;
 *                               // implements membrane between compartments
 *
 * Example: Some DOM objects (including all the arraylike DOM objects) are
 * implemented as proxies. Since these objects don't need to forward operations
 * to any underlying JS object, DOMJSProxyHandler directly subclasses
 * BaseProxyHandler.
 *
 * Gecko's security wrappers are examples of cross-compartment wrappers.
 *
 * ### Proxy prototype chains
 *
 * In addition to the normal methods, there are two models for proxy prototype
 * chains.
 *
 * 1.  Proxies can use the standard prototype mechanism used throughout the
 *     engine. To do so, simply pass a prototype to NewProxyObject() at
 *     creation time. All prototype accesses will then "just work" to treat the
 *     proxy as a "normal" object.
 *
 * 2.  A proxy can implement more complicated prototype semantics (if, for
 *     example, it wants to delegate the prototype lookup to a wrapped object)
 *     by passing Proxy::LazyProto as the prototype at create time. This
 *     guarantees that the getPrototype() handler method will be called every
 *     time the object's prototype chain is accessed.
 *
 *     This system is implemented with two methods: {get,set}Prototype. The
 *     default implementation of setPrototype throws a TypeError. Since it is
 *     not possible to create an object without a sense of prototype chain,
 *     handlers must implement getPrototype if opting in to the dynamic
 *     prototype system.
 */

/*
 * BaseProxyHandler is the most generic kind of proxy handler. It does not make
 * any assumptions about the target. Consequently, it does not provide any
 * default implementation for most methods. As a convenience, a few high-level
 * methods, like get() and set(), are given default implementations that work by
 * calling the low-level methods, like getOwnPropertyDescriptor().
 *
 * Important: If you add a method here, you should probably also add a
 * Proxy::foo entry point with an AutoEnterPolicy. If you don't, you need an
 * explicit override for the method in SecurityWrapper. See bug 945826 comment 0.
 */
class JS_FRIEND_API(BaseProxyHandler)
{
    /*
     * Sometimes it's desirable to designate groups of proxy handlers as "similar".
     * For this, we use the notion of a "family": A consumer-provided opaque pointer
     * that designates the larger group to which this proxy belongs.
     *
     * If it will never be important to differentiate this proxy from others as
     * part of a distinct group, nullptr may be used instead.
     */
    const void* mFamily;

    /*
     * Proxy handlers can use mHasPrototype to request the following special
     * treatment from the JS engine:
     *
     *   - When mHasPrototype is true, the engine never calls these methods:
     *     getPropertyDescriptor, has, set, enumerate, iterate.  Instead, for
     *     these operations, it calls the "own" methods like
     *     getOwnPropertyDescriptor, hasOwn, defineProperty,
     *     getOwnEnumerablePropertyKeys, etc., and consults the prototype chain
     *     if needed.
     *
     *   - When mHasPrototype is true, the engine calls handler->get() only if
     *     handler->hasOwn() says an own property exists on the proxy. If not,
     *     it consults the prototype chain.
     *
     * This is useful because it frees the ProxyHandler from having to implement
     * any behavior having to do with the prototype chain.
     */
    bool mHasPrototype;

    /*
     * All proxies indicate whether they have any sort of interesting security
     * policy that might prevent the caller from doing something it wants to
     * the object. In the case of wrappers, this distinction is used to
     * determine whether the caller may strip off the wrapper if it so desires.
     */
    bool mHasSecurityPolicy;

  public:
    explicit MOZ_CONSTEXPR BaseProxyHandler(const void* aFamily, bool aHasPrototype = false,
                                            bool aHasSecurityPolicy = false)
      : mFamily(aFamily),
        mHasPrototype(aHasPrototype),
        mHasSecurityPolicy(aHasSecurityPolicy)
    { }

    bool hasPrototype() const {
        return mHasPrototype;
    }

    bool hasSecurityPolicy() const {
        return mHasSecurityPolicy;
    }

    inline const void* family() const {
        return mFamily;
    }
    static size_t offsetOfFamily() {
        return offsetof(BaseProxyHandler, mFamily);
    }

    virtual bool finalizeInBackground(Value priv) const {
        /*
         * Called on creation of a proxy to determine whether its finalize
         * method can be finalized on the background thread.
         */
        return true;
    }

    /* Policy enforcement methods.
     *
     * enter() allows the policy to specify whether the caller may perform |act|
     * on the proxy's |id| property. In the case when |act| is CALL, |id| is
     * generally JSID_VOID.
     *
     * The |act| parameter to enter() specifies the action being performed.
     * If |bp| is false, the method suggests that the caller throw (though it
     * may still decide to squelch the error).
     *
     * We make these OR-able so that assertEnteredPolicy can pass a union of them.
     * For example, get{,Own}PropertyDescriptor is invoked by calls to ::get()
     * ::set(), in addition to being invoked on its own, so there are several
     * valid Actions that could have been entered.
     */
    typedef uint32_t Action;
    enum {
        NONE      = 0x00,
        GET       = 0x01,
        SET       = 0x02,
        CALL      = 0x04,
        ENUMERATE = 0x08,
        GET_PROPERTY_DESCRIPTOR = 0x10
    };

    virtual bool enter(JSContext* cx, HandleObject wrapper, HandleId id, Action act,
                       bool* bp) const;

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                          MutableHandle<JSPropertyDescriptor> desc) const = 0;
    virtual bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                Handle<JSPropertyDescriptor> desc,
                                ObjectOpResult& result) const = 0;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                 AutoIdVector& props) const = 0;
    virtual bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                         ObjectOpResult& result) const = 0;

    /*
     * Because [[Enumerate]] is one of the standard traps it should be overridden.
     * However for convenience BaseProxyHandler includes a pure virtual implementation,
     * that turns the properties returned by getOwnEnumerablePropertyKeys (and proto walking)
     * into an Iterator object.
     */
    virtual bool enumerate(JSContext* cx, HandleObject proxy, MutableHandleObject objp) const = 0;

    /*
     * These methods are standard, but the engine does not normally call them.
     * They're opt-in. See "Proxy prototype chains" above.
     *
     * getPrototype() crashes if called. setPrototype() throws a TypeError.
     */
    virtual bool getPrototype(JSContext* cx, HandleObject proxy, MutableHandleObject protop) const;
    virtual bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                              ObjectOpResult& result) const;

    /* Non-standard but conceptual kin to {g,s}etPrototype, so lives here. */
    virtual bool setImmutablePrototype(JSContext* cx, HandleObject proxy, bool* succeeded) const;

    virtual bool preventExtensions(JSContext* cx, HandleObject proxy,
                                   ObjectOpResult& result) const = 0;
    virtual bool isExtensible(JSContext* cx, HandleObject proxy, bool* extensible) const = 0;

    /*
     * These standard internal methods are implemented, as a convenience, so
     * that ProxyHandler subclasses don't have to provide every single method.
     *
     * The base-class implementations work by calling getPropertyDescriptor().
     * They do not follow any standard. When in doubt, override them.
     */
    virtual bool has(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const;
    virtual bool get(JSContext* cx, HandleObject proxy, HandleValue receiver,
                     HandleId id, MutableHandleValue vp) const;
    virtual bool set(JSContext* cx, HandleObject proxy, HandleId id, HandleValue v,
                     HandleValue receiver, ObjectOpResult& result) const;

    /*
     * [[Call]] and [[Construct]] are standard internal methods but according
     * to the spec, they are not present on every object.
     *
     * SpiderMonkey never calls a proxy's call()/construct() internal method
     * unless isCallable()/isConstructor() returns true for that proxy.
     *
     * BaseProxyHandler::isCallable()/isConstructor() always return false, and
     * BaseProxyHandler::call()/construct() crash if called. So if you're
     * creating a kind of that is never callable, you don't have to override
     * anything, but otherwise you probably want to override all four.
     */
    virtual bool call(JSContext* cx, HandleObject proxy, const CallArgs& args) const;
    virtual bool construct(JSContext* cx, HandleObject proxy, const CallArgs& args) const;

    /* SpiderMonkey extensions. */
    virtual bool getPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                       MutableHandle<JSPropertyDescriptor> desc) const;
    virtual bool hasOwn(JSContext* cx, HandleObject proxy, HandleId id, bool* bp) const;
    virtual bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                              AutoIdVector& props) const;
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const;
    virtual bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v, bool* bp) const;
    virtual bool getBuiltinClass(JSContext* cx, HandleObject proxy,
                                 ESClassValue* classValue) const;
    virtual bool isArray(JSContext* cx, HandleObject proxy, JS::IsArrayAnswer* answer) const;
    virtual const char* className(JSContext* cx, HandleObject proxy) const;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy, unsigned indent) const;
    virtual bool regexp_toShared(JSContext* cx, HandleObject proxy, RegExpGuard* g) const;
    virtual bool boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp) const;
    virtual void trace(JSTracer* trc, JSObject* proxy) const;
    virtual void finalize(JSFreeOp* fop, JSObject* proxy) const;
    virtual void objectMoved(JSObject* proxy, const JSObject* old) const;

    // Allow proxies, wrappers in particular, to specify callability at runtime.
    // Note: These do not take const JSObject*, but they do in spirit.
    //       We are not prepared to do this, as there's little const correctness
    //       in the external APIs that handle proxies.
    virtual bool isCallable(JSObject* obj) const;
    virtual bool isConstructor(JSObject* obj) const;

    // These two hooks must be overridden, or not overridden, in tandem -- no
    // overriding just one!
    virtual bool watch(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                       JS::HandleObject callable) const;
    virtual bool unwatch(JSContext* cx, JS::HandleObject proxy, JS::HandleId id) const;

    virtual bool getElements(JSContext* cx, HandleObject proxy, uint32_t begin, uint32_t end,
                             ElementAdder* adder) const;

    /* See comment for weakmapKeyDelegateOp in js/Class.h. */
    virtual JSObject* weakmapKeyDelegate(JSObject* proxy) const;
    virtual bool isScripted() const { return false; }
};

/*
 * DirectProxyHandler includes a notion of a target object. All methods are
 * reimplemented such that they forward their behavior to the target. This
 * allows consumers of this class to forward to another object as transparently
 * and efficiently as possible.
 *
 * Important: If you add a method implementation here, you probably also need
 * to add an override in CrossCompartmentWrapper. If you don't, you risk
 * compartment mismatches. See bug 945826 comment 0.
 */
class JS_FRIEND_API(DirectProxyHandler) : public BaseProxyHandler
{
  public:
    explicit MOZ_CONSTEXPR DirectProxyHandler(const void* aFamily, bool aHasPrototype = false,
                                              bool aHasSecurityPolicy = false)
      : BaseProxyHandler(aFamily, aHasPrototype, aHasSecurityPolicy)
    { }

    /* Standard internal methods. */
    virtual bool getOwnPropertyDescriptor(JSContext* cx, HandleObject proxy, HandleId id,
                                          MutableHandle<JSPropertyDescriptor> desc) const override;
    virtual bool defineProperty(JSContext* cx, HandleObject proxy, HandleId id,
                                Handle<JSPropertyDescriptor> desc,
                                ObjectOpResult& result) const override;
    virtual bool ownPropertyKeys(JSContext* cx, HandleObject proxy,
                                 AutoIdVector& props) const override;
    virtual bool delete_(JSContext* cx, HandleObject proxy, HandleId id,
                         ObjectOpResult& result) const override;
    virtual bool enumerate(JSContext* cx, HandleObject proxy,
                           MutableHandleObject objp) const override;
    virtual bool getPrototype(JSContext* cx, HandleObject proxy,
                              MutableHandleObject protop) const override;
    virtual bool setPrototype(JSContext* cx, HandleObject proxy, HandleObject proto,
                              ObjectOpResult& result) const override;
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
                                       MutableHandle<JSPropertyDescriptor> desc) const override;
    virtual bool hasOwn(JSContext* cx, HandleObject proxy, HandleId id,
                        bool* bp) const override;
    virtual bool getOwnEnumerablePropertyKeys(JSContext* cx, HandleObject proxy,
                                              AutoIdVector& props) const override;
    virtual bool nativeCall(JSContext* cx, IsAcceptableThis test, NativeImpl impl,
                            const CallArgs& args) const override;
    virtual bool hasInstance(JSContext* cx, HandleObject proxy, MutableHandleValue v,
                             bool* bp) const override;
    virtual bool getBuiltinClass(JSContext* cx, HandleObject proxy,
                                 ESClassValue* classValue) const override;
    virtual bool isArray(JSContext* cx, HandleObject proxy,
                         JS::IsArrayAnswer* answer) const override;
    virtual const char* className(JSContext* cx, HandleObject proxy) const override;
    virtual JSString* fun_toString(JSContext* cx, HandleObject proxy,
                                   unsigned indent) const override;
    virtual bool regexp_toShared(JSContext* cx, HandleObject proxy,
                                 RegExpGuard* g) const override;
    virtual bool boxedValue_unbox(JSContext* cx, HandleObject proxy, MutableHandleValue vp) const override;
    virtual bool isCallable(JSObject* obj) const override;
    virtual JSObject* weakmapKeyDelegate(JSObject* proxy) const override;
};

extern JS_FRIEND_DATA(const js::Class* const) ProxyClassPtr;

inline bool IsProxy(const JSObject* obj)
{
    return GetObjectClass(obj)->isProxy();
}

const uint32_t PROXY_EXTRA_SLOTS = 2;

// Layout of the values stored by a proxy. Note that API clients require the
// private slot to be the first slot in the proxy's values, so that the private
// slot can be accessed in the same fashion as the first reserved slot, via
// {Get,Set}ReservedOrProxyPrivateSlot.

struct ProxyValueArray
{
    Value privateSlot;
    Value extraSlots[PROXY_EXTRA_SLOTS];

    ProxyValueArray()
      : privateSlot(JS::UndefinedValue())
    {
        for (size_t i = 0; i < PROXY_EXTRA_SLOTS; i++)
            extraSlots[i] = JS::UndefinedValue();
    }
};

// All proxies share the same data layout. Following the object's shape and
// type, the proxy has a ProxyDataLayout structure with a pointer to an array
// of values and the proxy's handler. This is designed both so that proxies can
// be easily swapped with other objects (via RemapWrapper) and to mimic the
// layout of other objects (proxies and other objects have the same size) so
// that common code can access either type of object.
//
// See GetReservedOrProxyPrivateSlot below.
struct ProxyDataLayout
{
    ProxyValueArray* values;
    const BaseProxyHandler* handler;
};

const uint32_t ProxyDataOffset = 2 * sizeof(void*);

// This method should only be used internally and by the accessors below.
inline ProxyDataLayout*
GetProxyDataLayout(JSObject* obj)
{
    MOZ_ASSERT(IsProxy(obj));
    return reinterpret_cast<ProxyDataLayout*>(reinterpret_cast<uint8_t*>(obj) + ProxyDataOffset);
}

inline const BaseProxyHandler*
GetProxyHandler(JSObject* obj)
{
    return GetProxyDataLayout(obj)->handler;
}

inline const Value&
GetProxyPrivate(JSObject* obj)
{
    return GetProxyDataLayout(obj)->values->privateSlot;
}

inline JSObject*
GetProxyTargetObject(JSObject* obj)
{
    return GetProxyPrivate(obj).toObjectOrNull();
}

inline const Value&
GetProxyExtra(JSObject* obj, size_t n)
{
    MOZ_ASSERT(n < PROXY_EXTRA_SLOTS);
    return GetProxyDataLayout(obj)->values->extraSlots[n];
}

inline void
SetProxyHandler(JSObject* obj, const BaseProxyHandler* handler)
{
    GetProxyDataLayout(obj)->handler = handler;
}

JS_FRIEND_API(void)
SetValueInProxy(Value* slot, const Value& value);

inline void
SetProxyExtra(JSObject* obj, size_t n, const Value& extra)
{
    MOZ_ASSERT(n < PROXY_EXTRA_SLOTS);
    Value* vp = &GetProxyDataLayout(obj)->values->extraSlots[n];

    // Trigger a barrier before writing the slot.
    if (vp->isMarkable() || extra.isMarkable())
        SetValueInProxy(vp, extra);
    else
        *vp = extra;
}

inline bool
IsScriptedProxy(JSObject* obj)
{
    return IsProxy(obj) && GetProxyHandler(obj)->isScripted();
}

inline const Value&
GetReservedOrProxyPrivateSlot(JSObject* obj, size_t slot)
{
    MOZ_ASSERT(slot == 0);
    MOZ_ASSERT(slot < JSCLASS_RESERVED_SLOTS(GetObjectClass(obj)) || IsProxy(obj));
    return reinterpret_cast<const shadow::Object*>(obj)->slotRef(slot);
}

inline void
SetReservedOrProxyPrivateSlot(JSObject* obj, size_t slot, const Value& value)
{
    MOZ_ASSERT(slot == 0);
    MOZ_ASSERT(slot < JSCLASS_RESERVED_SLOTS(GetObjectClass(obj)) || IsProxy(obj));
    shadow::Object* sobj = reinterpret_cast<shadow::Object*>(obj);
    if (sobj->slotRef(slot).isMarkable() || value.isMarkable())
        SetReservedOrProxyPrivateSlotWithBarrier(obj, slot, value);
    else
        sobj->slotRef(slot) = value;
}

class MOZ_STACK_CLASS ProxyOptions {
  protected:
    /* protected constructor for subclass */
    explicit ProxyOptions(bool singletonArg, bool lazyProtoArg = false)
      : singleton_(singletonArg),
        lazyProto_(lazyProtoArg),
        clasp_(ProxyClassPtr)
    {}

  public:
    ProxyOptions() : singleton_(false),
                     lazyProto_(false),
                     clasp_(ProxyClassPtr)
    {}

    bool singleton() const { return singleton_; }
    ProxyOptions& setSingleton(bool flag) {
        singleton_ = flag;
        return *this;
    }

    bool lazyProto() const { return lazyProto_; }
    ProxyOptions& setLazyProto(bool flag) {
        lazyProto_ = flag;
        return *this;
    }

    const Class* clasp() const {
        return clasp_;
    }
    ProxyOptions& setClass(const Class* claspArg) {
        clasp_ = claspArg;
        return *this;
    }

  private:
    bool singleton_;
    bool lazyProto_;
    const Class* clasp_;
};

JS_FRIEND_API(JSObject*)
NewProxyObject(JSContext* cx, const BaseProxyHandler* handler, HandleValue priv,
               JSObject* proto, const ProxyOptions& options = ProxyOptions());

JSObject*
RenewProxyObject(JSContext* cx, JSObject* obj, BaseProxyHandler* handler, Value priv);

class JS_FRIEND_API(AutoEnterPolicy)
{
  public:
    typedef BaseProxyHandler::Action Action;
    AutoEnterPolicy(JSContext* cx, const BaseProxyHandler* handler,
                    HandleObject wrapper, HandleId id, Action act, bool mayThrow)
#ifdef JS_DEBUG
        : context(nullptr)
#endif
    {
        allow = handler->hasSecurityPolicy() ? handler->enter(cx, wrapper, id, act, &rv)
                                             : true;
        recordEnter(cx, wrapper, id, act);
        // We want to throw an exception if all of the following are true:
        // * The policy disallowed access.
        // * The policy set rv to false, indicating that we should throw.
        // * The caller did not instruct us to ignore exceptions.
        // * The policy did not throw itself.
        if (!allow && !rv && mayThrow)
            reportErrorIfExceptionIsNotPending(cx, id);
    }

    virtual ~AutoEnterPolicy() { recordLeave(); }
    inline bool allowed() { return allow; }
    inline bool returnValue() { MOZ_ASSERT(!allowed()); return rv; }

  protected:
    // no-op constructor for subclass
    AutoEnterPolicy()
#ifdef JS_DEBUG
        : context(nullptr)
        , enteredAction(BaseProxyHandler::NONE)
#endif
        {}
    void reportErrorIfExceptionIsNotPending(JSContext* cx, jsid id);
    bool allow;
    bool rv;

#ifdef JS_DEBUG
    JSContext* context;
    mozilla::Maybe<HandleObject> enteredProxy;
    mozilla::Maybe<HandleId> enteredId;
    Action                   enteredAction;

    // NB: We explicitly don't track the entered action here, because sometimes
    // set() methods do an implicit get() during their implementation, leading
    // to spurious assertions.
    AutoEnterPolicy* prev;
    void recordEnter(JSContext* cx, HandleObject proxy, HandleId id, Action act);
    void recordLeave();

    friend JS_FRIEND_API(void) assertEnteredPolicy(JSContext* cx, JSObject* proxy, jsid id, Action act);
#else
    inline void recordEnter(JSContext* cx, JSObject* proxy, jsid id, Action act) {}
    inline void recordLeave() {}
#endif

};

#ifdef JS_DEBUG
class JS_FRIEND_API(AutoWaivePolicy) : public AutoEnterPolicy {
public:
    AutoWaivePolicy(JSContext* cx, HandleObject proxy, HandleId id,
                    BaseProxyHandler::Action act)
    {
        allow = true;
        recordEnter(cx, proxy, id, act);
    }
};
#else
class JS_FRIEND_API(AutoWaivePolicy) {
  public:
    AutoWaivePolicy(JSContext* cx, HandleObject proxy, HandleId id,
                    BaseProxyHandler::Action act)
    {}
};
#endif

#ifdef JS_DEBUG
extern JS_FRIEND_API(void)
assertEnteredPolicy(JSContext* cx, JSObject* obj, jsid id,
                    BaseProxyHandler::Action act);
#else
inline void assertEnteredPolicy(JSContext* cx, JSObject* obj, jsid id,
                                BaseProxyHandler::Action act)
{}
#endif

extern JS_FRIEND_API(JSObject*)
InitProxyClass(JSContext* cx, JS::HandleObject obj);

} /* namespace js */

#endif /* js_Proxy_h */

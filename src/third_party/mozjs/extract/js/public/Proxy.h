/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_Proxy_h
#define js_Proxy_h

#include "mozilla/Maybe.h"

#include "jstypes.h"  // for JS_PUBLIC_API, JS_PUBLIC_DATA

#include "js/Array.h"  // JS::IsArrayAnswer
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/HeapAPI.h"        // for ObjectIsMarkedBlack
#include "js/Id.h"             // for jsid
#include "js/Object.h"         // JS::GetClass
#include "js/RootingAPI.h"     // for Handle, MutableHandle (ptr only)
#include "js/shadow/Object.h"  // JS::shadow::Object
#include "js/TypeDecls.h"  // for HandleObject, HandleId, HandleValue, MutableHandleIdVector, MutableHandleValue, MutableHand...
#include "js/Value.h"  // for Value, AssertValueIsNotGray, UndefinedValue, ObjectOrNullValue

namespace js {

class RegExpShared;

class JS_PUBLIC_API Wrapper;

/*
 * [SMDOC] Proxy Objects
 *
 * A proxy is a JSObject with highly customizable behavior. ES6 specifies a
 * single kind of proxy, but the customization mechanisms we use to implement
 * ES6 Proxy objects are also useful wherever an object with weird behavior is
 * wanted. Proxies are used to implement:
 *
 * -   the scope objects used by the Debugger's frame.eval() method
 *     (see js::GetDebugEnvironment)
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
 * ### Proxies and internal methods
 *
 * ES2019 specifies 13 internal methods. The runtime semantics of just about
 * everything a script can do to an object is specified in terms of these
 * internal methods. For example:
 *
 *     JS code                      ES6 internal method that gets called
 *     ---------------------------  --------------------------------
 *     obj.prop                     obj.[[Get]](obj, "prop")
 *     "prop" in obj                obj.[[HasProperty]]("prop")
 *     new obj()                    obj.[[Construct]](<empty argument List>)
 *
 * With regard to the implementation of these internal methods, there are three
 * very different kinds of object in SpiderMonkey.
 *
 * 1.  Native objects cover most objects and contain both internal slots and
 *     properties. JSClassOps and ObjectOps may be used to override certain
 *     default behaviors.
 *
 * 2.  Proxy objects are composed of internal slots and a ProxyHandler. The
 *     handler contains C++ methods that can implement these standard (and
 *     non-standard) internal methods. JSClassOps and ObjectOps for the base
 *     ProxyObject invoke the handler methods as appropriate.
 *
 * 3.  Objects with custom layouts like TypedObjects. These rely on JSClassOps
 *     and ObjectOps to implement internal methods.
 *
 * Native objects with custom JSClassOps / ObjectOps are used when the object
 * behaves very similar to a normal object such as the ArrayObject and it's
 * length property. Most usages wrapping a C++ or other type should prefer
 * using a Proxy. Using the proxy approach makes it much easier to create an
 * ECMAScript and JIT compatible object, particularly if using an appropriate
 * base class.
 *
 * Just about anything you do to a proxy will end up going through a C++
 * virtual method call. Possibly several. There's no reason the JITs and ICs
 * can't specialize for particular proxies, based on the handler; but currently
 * we don't do much of this, so the virtual method overhead typically is
 * actually incurred.
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
 *     ForwardingProxyHandler    // has a target and forwards internal methods
 *     |
 *     Wrapper                   // can be unwrapped to reveal target
 *     |                         // (see js::CheckedUnwrap)
 *     |
 *     CrossCompartmentWrapper   // target is in another compartment;
 *                               // implements membrane between compartments
 *
 * Example: Some DOM objects (including all the arraylike DOM objects) are
 * implemented as proxies. Since these objects don't need to forward operations
 * to any underlying JS object, BaseDOMProxyHandler directly subclasses
 * BaseProxyHandler.
 *
 * Gecko's security wrappers are examples of cross-compartment wrappers.
 *
 * ### Proxy prototype chains
 *
 * While most ECMAScript internal methods are handled by simply calling the
 * handler method, the [[GetPrototypeOf]] / [[SetPrototypeOf]] behaviors may
 * follow one of two models:
 *
 * 1.  A concrete prototype object (or null) is passed to object construction
 *     and ordinary prototype read and write applies. The prototype-related
 *     handler hooks are never called in this case. The [[Prototype]] slot is
 *     used to store the current prototype value.
 *
 * 2.  TaggedProto::LazyProto is passed to NewProxyObject (or the
 *     ProxyOptions::lazyProto flag is set). Each read or write of the
 *     prototype will invoke the handler. This dynamic prototype behavior may
 *     be useful for wrapper-like objects. If this mode is used the
 *     getPrototype handler at a minimum must be implemented.
 *
 *     NOTE: In this mode the [[Prototype]] internal slot is unavailable and
 *           must be simulated if needed. This is non-standard, but an
 *           appropriate handler can hide this implementation detail.
 *
 * One subtlety here is that ECMAScript has a notion of "ordinary" prototypes.
 * An object that doesn't override [[GetPrototypeOf]] is considered to have an
 * ordinary prototype. The getPrototypeIfOrdinary handler must be implemented
 * by you or your base class. Typically model 1 will be considered "ordinary"
 * and model 2 will not.
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
 * explicit override for the method in SecurityWrapper. See bug 945826 comment
 * 0.
 */
class JS_PUBLIC_API BaseProxyHandler {
  /*
   * Sometimes it's desirable to designate groups of proxy handlers as
   * "similar". For this, we use the notion of a "family": A consumer-provided
   * opaque pointer that designates the larger group to which this proxy
   * belongs.
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
   *     has, set, enumerate, iterate.  Instead, for these operations,
   *     it calls the "own" methods like getOwnPropertyDescriptor, hasOwn,
   *     defineProperty, getOwnEnumerablePropertyKeys, etc.,
   *     and consults the prototype chain if needed.
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
  explicit constexpr BaseProxyHandler(const void* aFamily,
                                      bool aHasPrototype = false,
                                      bool aHasSecurityPolicy = false)
      : mFamily(aFamily),
        mHasPrototype(aHasPrototype),
        mHasSecurityPolicy(aHasSecurityPolicy) {}

  bool hasPrototype() const { return mHasPrototype; }

  bool hasSecurityPolicy() const { return mHasSecurityPolicy; }

  inline const void* family() const { return mFamily; }
  static size_t offsetOfFamily() { return offsetof(BaseProxyHandler, mFamily); }

  virtual bool finalizeInBackground(const JS::Value& priv) const {
    /*
     * Called on creation of a proxy to determine whether its finalize
     * method can be finalized on the background thread.
     */
    return true;
  }

  virtual bool canNurseryAllocate() const {
    /*
     * Nursery allocation is allowed if and only if it is safe to not
     * run |finalize| when the ProxyObject dies.
     */
    return false;
  }

  /* Policy enforcement methods.
   *
   * enter() allows the policy to specify whether the caller may perform |act|
   * on the proxy's |id| property. In the case when |act| is CALL, |id| is
   * generally JS::PropertyKey::isVoid.  The |mayThrow| parameter indicates
   * whether a handler that wants to throw custom exceptions when denying
   * should do so or not.
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
    NONE = 0x00,
    GET = 0x01,
    SET = 0x02,
    CALL = 0x04,
    ENUMERATE = 0x08,
    GET_PROPERTY_DESCRIPTOR = 0x10
  };

  virtual bool enter(JSContext* cx, JS::HandleObject wrapper, JS::HandleId id,
                     Action act, bool mayThrow, bool* bp) const;

  /* Standard internal methods. */
  virtual bool getOwnPropertyDescriptor(
      JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
      JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc) const = 0;
  virtual bool defineProperty(JSContext* cx, JS::HandleObject proxy,
                              JS::HandleId id,
                              JS::Handle<JS::PropertyDescriptor> desc,
                              JS::ObjectOpResult& result) const = 0;
  virtual bool ownPropertyKeys(JSContext* cx, JS::HandleObject proxy,
                               JS::MutableHandleIdVector props) const = 0;
  virtual bool delete_(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                       JS::ObjectOpResult& result) const = 0;

  /*
   * These methods are standard, but the engine does not normally call them.
   * They're opt-in. See "Proxy prototype chains" above.
   *
   * getPrototype() crashes if called. setPrototype() throws a TypeError.
   */
  virtual bool getPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::MutableHandleObject protop) const;
  virtual bool setPrototype(JSContext* cx, JS::HandleObject proxy,
                            JS::HandleObject proto,
                            JS::ObjectOpResult& result) const;

  /* Non-standard but conceptual kin to {g,s}etPrototype, so these live here. */
  virtual bool getPrototypeIfOrdinary(JSContext* cx, JS::HandleObject proxy,
                                      bool* isOrdinary,
                                      JS::MutableHandleObject protop) const = 0;
  virtual bool setImmutablePrototype(JSContext* cx, JS::HandleObject proxy,
                                     bool* succeeded) const;

  virtual bool preventExtensions(JSContext* cx, JS::HandleObject proxy,
                                 JS::ObjectOpResult& result) const = 0;
  virtual bool isExtensible(JSContext* cx, JS::HandleObject proxy,
                            bool* extensible) const = 0;

  /*
   * These standard internal methods are implemented, as a convenience, so
   * that ProxyHandler subclasses don't have to provide every single method.
   *
   * The base-class implementations work by calling getOwnPropertyDescriptor()
   * and going up the [[Prototype]] chain if necessary. The algorithm for this
   * follows what is defined for Ordinary Objects in the ES spec.
   * They do not follow any standard. When in doubt, override them.
   */
  virtual bool has(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                   bool* bp) const;
  virtual bool get(JSContext* cx, JS::HandleObject proxy,
                   JS::HandleValue receiver, JS::HandleId id,
                   JS::MutableHandleValue vp) const;
  virtual bool set(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                   JS::HandleValue v, JS::HandleValue receiver,
                   JS::ObjectOpResult& result) const;

  // Use the ProxyExpando object for private fields, rather than taking the
  // normal get/set/defineField paths.
  virtual bool useProxyExpandoObjectForPrivateFields() const { return true; }

  // For some exotic objects (WindowProxy, Location), we want to be able to
  // throw rather than allow private fields on these objects.
  //
  // As a simplfying assumption, if throwOnPrivateFields returns true,
  // we should also return true to useProxyExpandoObjectForPrivateFields.
  virtual bool throwOnPrivateField() const { return false; }

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
  virtual bool call(JSContext* cx, JS::HandleObject proxy,
                    const JS::CallArgs& args) const;
  virtual bool construct(JSContext* cx, JS::HandleObject proxy,
                         const JS::CallArgs& args) const;

  /* SpiderMonkey extensions. */
  virtual bool enumerate(JSContext* cx, JS::HandleObject proxy,
                         JS::MutableHandleIdVector props) const;
  virtual bool hasOwn(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                      bool* bp) const;
  virtual bool getOwnEnumerablePropertyKeys(
      JSContext* cx, JS::HandleObject proxy,
      JS::MutableHandleIdVector props) const;
  virtual bool nativeCall(JSContext* cx, JS::IsAcceptableThis test,
                          JS::NativeImpl impl, const JS::CallArgs& args) const;
  virtual bool getBuiltinClass(JSContext* cx, JS::HandleObject proxy,
                               ESClass* cls) const;
  virtual bool isArray(JSContext* cx, JS::HandleObject proxy,
                       JS::IsArrayAnswer* answer) const;
  virtual const char* className(JSContext* cx, JS::HandleObject proxy) const;
  virtual JSString* fun_toString(JSContext* cx, JS::HandleObject proxy,
                                 bool isToSource) const;
  virtual RegExpShared* regexp_toShared(JSContext* cx,
                                        JS::HandleObject proxy) const;
  virtual bool boxedValue_unbox(JSContext* cx, JS::HandleObject proxy,
                                JS::MutableHandleValue vp) const;
  virtual void trace(JSTracer* trc, JSObject* proxy) const;
  virtual void finalize(JS::GCContext* gcx, JSObject* proxy) const;
  virtual size_t objectMoved(JSObject* proxy, JSObject* old) const;

  // Allow proxies, wrappers in particular, to specify callability at runtime.
  // Note: These do not take const JSObject*, but they do in spirit.
  //       We are not prepared to do this, as there's little const correctness
  //       in the external APIs that handle proxies.
  virtual bool isCallable(JSObject* obj) const;
  virtual bool isConstructor(JSObject* obj) const;

  virtual bool getElements(JSContext* cx, JS::HandleObject proxy,
                           uint32_t begin, uint32_t end,
                           ElementAdder* adder) const;

  virtual bool isScripted() const { return false; }
};

extern JS_PUBLIC_DATA const JSClass ProxyClass;

inline bool IsProxy(const JSObject* obj) {
  return reinterpret_cast<const JS::shadow::Object*>(obj)->shape->isProxy();
}

namespace detail {

// Proxy slot layout
// -----------------
//
// Every proxy has a ProxyValueArray that contains the following Values:
//
// - The expando slot. This is used to hold private fields should they be
//   stamped into a non-forwarding proxy type.
// - The private slot.
// - The reserved slots. The number of slots is determined by the proxy's Class.
//
// Proxy objects store a pointer to the reserved slots (ProxyReservedSlots*).
// The ProxyValueArray and the private slot can be accessed using
// ProxyValueArray::fromReservedSlots or ProxyDataLayout::values.
//
// Storing a pointer to ProxyReservedSlots instead of ProxyValueArray has a
// number of advantages. In particular, it means JS::GetReservedSlot and
// JS::SetReservedSlot can be used with both proxies and native objects. This
// works because the ProxyReservedSlots* pointer is stored where native objects
// store their dynamic slots pointer.

struct ProxyReservedSlots {
  JS::Value slots[1];

  static constexpr ptrdiff_t offsetOfPrivateSlot();

  static inline int offsetOfSlot(size_t slot) {
    return offsetof(ProxyReservedSlots, slots[0]) + slot * sizeof(JS::Value);
  }

  void init(size_t nreserved) {
    for (size_t i = 0; i < nreserved; i++) {
      slots[i] = JS::UndefinedValue();
    }
  }

  ProxyReservedSlots(const ProxyReservedSlots&) = delete;
  void operator=(const ProxyReservedSlots&) = delete;
};

struct ProxyValueArray {
  JS::Value expandoSlot;
  JS::Value privateSlot;
  ProxyReservedSlots reservedSlots;

  void init(size_t nreserved) {
    expandoSlot = JS::ObjectOrNullValue(nullptr);
    privateSlot = JS::UndefinedValue();
    reservedSlots.init(nreserved);
  }

  static MOZ_ALWAYS_INLINE ProxyValueArray* fromReservedSlots(
      ProxyReservedSlots* slots) {
    uintptr_t p = reinterpret_cast<uintptr_t>(slots);
    return reinterpret_cast<ProxyValueArray*>(p - offsetOfReservedSlots());
  }
  static constexpr size_t offsetOfReservedSlots() {
    return offsetof(ProxyValueArray, reservedSlots);
  }

  static size_t allocCount(size_t nreserved) {
    static_assert(offsetOfReservedSlots() % sizeof(JS::Value) == 0);
    return offsetOfReservedSlots() / sizeof(JS::Value) + nreserved;
  }
  static size_t sizeOf(size_t nreserved) {
    return allocCount(nreserved) * sizeof(JS::Value);
  }

  ProxyValueArray(const ProxyValueArray&) = delete;
  void operator=(const ProxyValueArray&) = delete;
};

/* static */
constexpr ptrdiff_t ProxyReservedSlots::offsetOfPrivateSlot() {
  return -ptrdiff_t(ProxyValueArray::offsetOfReservedSlots()) +
         offsetof(ProxyValueArray, privateSlot);
}

// All proxies share the same data layout. Following the object's shape and
// type, the proxy has a ProxyDataLayout structure with a pointer to an array
// of values and the proxy's handler. This is designed both so that proxies can
// be easily swapped with other objects (via RemapWrapper) and to mimic the
// layout of other objects (proxies and other objects have the same size) so
// that common code can access either type of object.
//
// See GetReservedOrProxyPrivateSlot below.
struct ProxyDataLayout {
  ProxyReservedSlots* reservedSlots;
  const BaseProxyHandler* handler;

  MOZ_ALWAYS_INLINE ProxyValueArray* values() const {
    return ProxyValueArray::fromReservedSlots(reservedSlots);
  }
};

#ifdef JS_64BIT
constexpr uint32_t ProxyDataOffset = 1 * sizeof(void*);
#else
constexpr uint32_t ProxyDataOffset = 2 * sizeof(void*);
#endif

inline ProxyDataLayout* GetProxyDataLayout(JSObject* obj) {
  MOZ_ASSERT(IsProxy(obj));
  return reinterpret_cast<ProxyDataLayout*>(reinterpret_cast<uint8_t*>(obj) +
                                            ProxyDataOffset);
}

inline const ProxyDataLayout* GetProxyDataLayout(const JSObject* obj) {
  MOZ_ASSERT(IsProxy(obj));
  return reinterpret_cast<const ProxyDataLayout*>(
      reinterpret_cast<const uint8_t*>(obj) + ProxyDataOffset);
}

JS_PUBLIC_API void SetValueInProxy(JS::Value* slot, const JS::Value& value);

inline void SetProxyReservedSlotUnchecked(JSObject* obj, size_t n,
                                          const JS::Value& extra) {
  MOZ_ASSERT(n < JSCLASS_RESERVED_SLOTS(JS::GetClass(obj)));

  JS::Value* vp = &GetProxyDataLayout(obj)->reservedSlots->slots[n];

  // Trigger a barrier before writing the slot.
  if (vp->isGCThing() || extra.isGCThing()) {
    SetValueInProxy(vp, extra);
  } else {
    *vp = extra;
  }
}

}  // namespace detail

inline const BaseProxyHandler* GetProxyHandler(const JSObject* obj) {
  return detail::GetProxyDataLayout(obj)->handler;
}

inline const JS::Value& GetProxyPrivate(const JSObject* obj) {
  return detail::GetProxyDataLayout(obj)->values()->privateSlot;
}

inline const JS::Value& GetProxyExpando(const JSObject* obj) {
  return detail::GetProxyDataLayout(obj)->values()->expandoSlot;
}

inline JSObject* GetProxyTargetObject(const JSObject* obj) {
  return GetProxyPrivate(obj).toObjectOrNull();
}

inline const JS::Value& GetProxyReservedSlot(const JSObject* obj, size_t n) {
  MOZ_ASSERT(n < JSCLASS_RESERVED_SLOTS(JS::GetClass(obj)));
  return detail::GetProxyDataLayout(obj)->reservedSlots->slots[n];
}

inline void SetProxyHandler(JSObject* obj, const BaseProxyHandler* handler) {
  detail::GetProxyDataLayout(obj)->handler = handler;
}

inline void SetProxyReservedSlot(JSObject* obj, size_t n,
                                 const JS::Value& extra) {
#ifdef DEBUG
  if (gc::detail::ObjectIsMarkedBlack(obj)) {
    JS::AssertValueIsNotGray(extra);
  }
#endif

  detail::SetProxyReservedSlotUnchecked(obj, n, extra);
}

inline void SetProxyPrivate(JSObject* obj, const JS::Value& value) {
#ifdef DEBUG
  if (gc::detail::ObjectIsMarkedBlack(obj)) {
    JS::AssertValueIsNotGray(value);
  }
#endif

  JS::Value* vp = &detail::GetProxyDataLayout(obj)->values()->privateSlot;

  // Trigger a barrier before writing the slot.
  if (vp->isGCThing() || value.isGCThing()) {
    detail::SetValueInProxy(vp, value);
  } else {
    *vp = value;
  }
}

inline bool IsScriptedProxy(const JSObject* obj) {
  return IsProxy(obj) && GetProxyHandler(obj)->isScripted();
}

class MOZ_STACK_CLASS ProxyOptions {
 protected:
  /* protected constructor for subclass */
  explicit ProxyOptions(bool lazyProtoArg)
      : lazyProto_(lazyProtoArg), clasp_(&ProxyClass) {}

 public:
  ProxyOptions() : ProxyOptions(false) {}

  bool lazyProto() const { return lazyProto_; }
  ProxyOptions& setLazyProto(bool flag) {
    lazyProto_ = flag;
    return *this;
  }

  const JSClass* clasp() const { return clasp_; }
  ProxyOptions& setClass(const JSClass* claspArg) {
    clasp_ = claspArg;
    return *this;
  }

 private:
  bool lazyProto_;
  const JSClass* clasp_;
};

JS_PUBLIC_API JSObject* NewProxyObject(
    JSContext* cx, const BaseProxyHandler* handler, JS::HandleValue priv,
    JSObject* proto, const ProxyOptions& options = ProxyOptions());

JSObject* RenewProxyObject(JSContext* cx, JSObject* obj,
                           BaseProxyHandler* handler, const JS::Value& priv);

class JS_PUBLIC_API AutoEnterPolicy {
 public:
  typedef BaseProxyHandler::Action Action;
  AutoEnterPolicy(JSContext* cx, const BaseProxyHandler* handler,
                  JS::HandleObject wrapper, JS::HandleId id, Action act,
                  bool mayThrow)
#ifdef JS_DEBUG
      : context(nullptr)
#endif
  {
    allow = handler->hasSecurityPolicy()
                ? handler->enter(cx, wrapper, id, act, mayThrow, &rv)
                : true;
    recordEnter(cx, wrapper, id, act);
    // We want to throw an exception if all of the following are true:
    // * The policy disallowed access.
    // * The policy set rv to false, indicating that we should throw.
    // * The caller did not instruct us to ignore exceptions.
    // * The policy did not throw itself.
    if (!allow && !rv && mayThrow) {
      reportErrorIfExceptionIsNotPending(cx, id);
    }
  }

  virtual ~AutoEnterPolicy() { recordLeave(); }
  inline bool allowed() { return allow; }
  inline bool returnValue() {
    MOZ_ASSERT(!allowed());
    return rv;
  }

 protected:
  // no-op constructor for subclass
  AutoEnterPolicy()
#ifdef JS_DEBUG
      : context(nullptr),
        enteredAction(BaseProxyHandler::NONE)
#endif
  {
  }
  void reportErrorIfExceptionIsNotPending(JSContext* cx, JS::HandleId id);
  bool allow;
  bool rv;

#ifdef JS_DEBUG
  JSContext* context;
  mozilla::Maybe<JS::HandleObject> enteredProxy;
  mozilla::Maybe<JS::HandleId> enteredId;
  Action enteredAction;

  // NB: We explicitly don't track the entered action here, because sometimes
  // set() methods do an implicit get() during their implementation, leading
  // to spurious assertions.
  AutoEnterPolicy* prev;
  void recordEnter(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                   Action act);
  void recordLeave();

  friend JS_PUBLIC_API void assertEnteredPolicy(JSContext* cx, JSObject* proxy,
                                                jsid id, Action act);
#else
  inline void recordEnter(JSContext* cx, JSObject* proxy, jsid id, Action act) {
  }
  inline void recordLeave() {}
#endif

 private:
  // This operator needs to be deleted explicitly, otherwise Visual C++ will
  // create it automatically when it is part of the export JS API. In that
  // case, compile would fail because HandleId is not allowed to be assigned
  // and consequently instantiation of assign operator of mozilla::Maybe
  // would fail. See bug 1325351 comment 16. Copy constructor is removed at
  // the same time for consistency.
  AutoEnterPolicy(const AutoEnterPolicy&) = delete;
  AutoEnterPolicy& operator=(const AutoEnterPolicy&) = delete;
};

#ifdef JS_DEBUG
class JS_PUBLIC_API AutoWaivePolicy : public AutoEnterPolicy {
 public:
  AutoWaivePolicy(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                  BaseProxyHandler::Action act) {
    allow = true;
    recordEnter(cx, proxy, id, act);
  }
};
#else
class JS_PUBLIC_API AutoWaivePolicy {
 public:
  AutoWaivePolicy(JSContext* cx, JS::HandleObject proxy, JS::HandleId id,
                  BaseProxyHandler::Action act) {}
};
#endif

#ifdef JS_DEBUG
extern JS_PUBLIC_API void assertEnteredPolicy(JSContext* cx, JSObject* obj,
                                              jsid id,
                                              BaseProxyHandler::Action act);
#else
inline void assertEnteredPolicy(JSContext* cx, JSObject* obj, jsid id,
                                BaseProxyHandler::Action act) {}
#endif

extern JS_PUBLIC_DATA const JSClassOps ProxyClassOps;
extern JS_PUBLIC_DATA const js::ClassExtension ProxyClassExtension;
extern JS_PUBLIC_DATA const js::ObjectOps ProxyObjectOps;

template <unsigned Flags>
constexpr unsigned CheckProxyFlags() {
  constexpr size_t reservedSlots =
      (Flags >> JSCLASS_RESERVED_SLOTS_SHIFT) & JSCLASS_RESERVED_SLOTS_MASK;

  // For now assert each Proxy Class has at least 1 reserved slot. This is
  // not a hard requirement, but helps catch Classes that need an explicit
  // JSCLASS_HAS_RESERVED_SLOTS since bug 1360523.
  static_assert(reservedSlots > 0,
                "Proxy Classes must have at least 1 reserved slot");

  constexpr size_t numSlots =
      offsetof(js::detail::ProxyValueArray, reservedSlots) / sizeof(JS::Value);

  // ProxyValueArray must fit inline in the object, so assert the number of
  // slots does not exceed MAX_FIXED_SLOTS.
  static_assert(numSlots + reservedSlots <= JS::shadow::Object::MAX_FIXED_SLOTS,
                "ProxyValueArray size must not exceed max JSObject size");

  // Proxies must not have the JSCLASS_SKIP_NURSERY_FINALIZE flag set: they
  // always have finalizers, and whether they can be nursery allocated is
  // controlled by the canNurseryAllocate() method on the proxy handler.
  static_assert(!(Flags & JSCLASS_SKIP_NURSERY_FINALIZE),
                "Proxies must not use JSCLASS_SKIP_NURSERY_FINALIZE; use "
                "the canNurseryAllocate() proxy handler method instead.");
  return Flags;
}

#define PROXY_CLASS_DEF_WITH_CLASS_SPEC(name, flags, classSpec)            \
  {                                                                        \
    name,                                                                  \
        JSClass::NON_NATIVE | JSCLASS_IS_PROXY |                           \
            JSCLASS_DELAY_METADATA_BUILDER | js::CheckProxyFlags<flags>(), \
        &js::ProxyClassOps, classSpec, &js::ProxyClassExtension,           \
        &js::ProxyObjectOps                                                \
  }

#define PROXY_CLASS_DEF(name, flags) \
  PROXY_CLASS_DEF_WITH_CLASS_SPEC(name, flags, JS_NULL_CLASS_SPEC)

// Converts a proxy into a DeadObjectProxy that will throw exceptions on all
// access. This will run the proxy's finalizer to perform clean-up before the
// conversion happens.
JS_PUBLIC_API void NukeNonCCWProxy(JSContext* cx, JS::HandleObject proxy);

// This is a variant of js::NukeNonCCWProxy() for CCWs. It should only be called
// on CCWs that have been removed from CCW tables.
JS_PUBLIC_API void NukeRemovedCrossCompartmentWrapper(JSContext* cx,
                                                      JSObject* wrapper);

} /* namespace js */

#endif /* js_Proxy_h */

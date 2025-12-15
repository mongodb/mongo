/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JSClass definition and its component types, plus related interfaces. */

#ifndef js_Class_h
#define js_Class_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include "jstypes.h"

#include "js/CallArgs.h"
#include "js/HeapAPI.h"
#include "js/Id.h"
#include "js/TypeDecls.h"

/*
 * A JSClass acts as a vtable for JS objects that allows JSAPI clients to
 * control various aspects of the behavior of an object like property lookup.
 * It contains some engine-private extensions that allows more control over
 * object behavior and, e.g., allows custom slow layout.
 */

struct JSAtomState;
struct JSFunctionSpec;

namespace js {

class PropertyResult;

// These are equal to js::FunctionClass / js::ExtendedFunctionClass.
extern JS_PUBLIC_DATA const JSClass* const FunctionClassPtr;
extern JS_PUBLIC_DATA const JSClass* const FunctionExtendedClassPtr;

}  // namespace js

namespace JS {

/**
 * Per ES6, the [[DefineOwnProperty]] internal method has three different
 * possible outcomes:
 *
 * -   It can throw an exception (which we indicate by returning false).
 *
 * -   It can return true, indicating unvarnished success.
 *
 * -   It can return false, indicating "strict failure". The property could
 *     not be defined. It's an error, but no exception was thrown.
 *
 * It's not just [[DefineOwnProperty]]: all the mutating internal methods have
 * the same three outcomes. (The other affected internal methods are [[Set]],
 * [[Delete]], [[SetPrototypeOf]], and [[PreventExtensions]].)
 *
 * If you think this design is awful, you're not alone.  But as it's the
 * standard, we must represent these boolean "success" values somehow.
 * ObjectOpSuccess is the class for this. It's like a bool, but when it's false
 * it also stores an error code.
 *
 * Typical usage:
 *
 *     ObjectOpResult result;
 *     if (!DefineProperty(cx, obj, id, ..., result)) {
 *         return false;
 *     }
 *     if (!result) {
 *         return result.reportError(cx, obj, id);
 *     }
 *
 * Users don't have to call `result.report()`; another possible ending is:
 *
 *     argv.rval().setBoolean(result.ok());
 *     return true;
 */
class ObjectOpResult {
 private:
  /**
   * code_ is either one of the special codes OkCode or Uninitialized, or an
   * error code. For now the error codes are JS friend API and are defined in
   * js/public/friend/ErrorNumbers.msg.
   *
   * code_ is uintptr_t (rather than uint32_t) for the convenience of the
   * JITs, which would otherwise have to deal with either padding or stack
   * alignment on 64-bit platforms.
   */
  uintptr_t code_;

 public:
  enum SpecialCodes : uintptr_t { OkCode = 0, Uninitialized = uintptr_t(-1) };

  ObjectOpResult() : code_(Uninitialized) {}

  /* Return true if succeed() was called. */
  bool ok() const {
    MOZ_ASSERT(code_ != Uninitialized);
    return code_ == OkCode;
  }

  explicit operator bool() const { return ok(); }

  /* Set this ObjectOpResult to true and return true. */
  bool succeed() {
    code_ = OkCode;
    return true;
  }

  /*
   * Set this ObjectOpResult to false with an error code.
   *
   * Always returns true, as a convenience. Typical usage will be:
   *
   *     if (funny condition) {
   *         return result.fail(JSMSG_CANT_DO_THE_THINGS);
   *     }
   *
   * The true return value indicates that no exception is pending, and it
   * would be OK to ignore the failure and continue.
   */
  bool fail(uint32_t msg) {
    MOZ_ASSERT(msg != OkCode);
    code_ = msg;
    return true;
  }

  JS_PUBLIC_API bool failCantRedefineProp();
  JS_PUBLIC_API bool failReadOnly();
  JS_PUBLIC_API bool failGetterOnly();
  JS_PUBLIC_API bool failCantDelete();

  JS_PUBLIC_API bool failCantSetInterposed();
  JS_PUBLIC_API bool failCantDefineWindowElement();
  JS_PUBLIC_API bool failCantDeleteWindowElement();
  JS_PUBLIC_API bool failCantDefineWindowNamedProperty();
  JS_PUBLIC_API bool failCantDeleteWindowNamedProperty();
  JS_PUBLIC_API bool failCantPreventExtensions();
  JS_PUBLIC_API bool failCantSetProto();
  JS_PUBLIC_API bool failNoNamedSetter();
  JS_PUBLIC_API bool failNoIndexedSetter();
  JS_PUBLIC_API bool failNotDataDescriptor();
  JS_PUBLIC_API bool failInvalidDescriptor();

  // Careful: This case has special handling in Object.defineProperty.
  JS_PUBLIC_API bool failCantDefineWindowNonConfigurable();

  JS_PUBLIC_API bool failBadArrayLength();
  JS_PUBLIC_API bool failBadIndex();

  uint32_t failureCode() const {
    MOZ_ASSERT(!ok());
    return uint32_t(code_);
  }

  /*
   * Report an error if necessary; return true to proceed and
   * false if an error was reported.
   *
   * The precise rules are like this:
   *
   * -   If ok(), then we succeeded. Do nothing and return true.
   * -   Otherwise, if |strict| is true, throw a TypeError and return false.
   * -   Otherwise, do nothing and return true.
   */
  bool checkStrictModeError(JSContext* cx, HandleObject obj, HandleId id,
                            bool strict) {
    if (ok() || !strict) {
      return true;
    }
    return reportError(cx, obj, id);
  }

  /*
   * The same as checkStrictModeError(cx, id, strict), except the
   * operation is not associated with a particular property id. This is
   * used for [[PreventExtensions]] and [[SetPrototypeOf]]. failureCode()
   * must not be an error that has "{0}" in the error message.
   */
  bool checkStrictModeError(JSContext* cx, HandleObject obj, bool strict) {
    if (ok() || !strict) {
      return true;
    }
    return reportError(cx, obj);
  }

  /* Throw a TypeError. Call this only if !ok(). */
  bool reportError(JSContext* cx, HandleObject obj, HandleId id);

  /*
   * The same as reportError(cx, obj, id), except the operation is not
   * associated with a particular property id.
   */
  bool reportError(JSContext* cx, HandleObject obj);

  // Convenience method. Return true if ok(); otherwise throw a TypeError
  // and return false.
  bool checkStrict(JSContext* cx, HandleObject obj, HandleId id) {
    return checkStrictModeError(cx, obj, id, true);
  }

  // Convenience method. The same as checkStrict(cx, obj, id), except the
  // operation is not associated with a particular property id.
  bool checkStrict(JSContext* cx, HandleObject obj) {
    return checkStrictModeError(cx, obj, true);
  }
};

}  // namespace JS

// JSClass operation signatures.

/** Add a property named by id to obj. */
typedef bool (*JSAddPropertyOp)(JSContext* cx, JS::HandleObject obj,
                                JS::HandleId id, JS::HandleValue v);

/**
 * Delete a property named by id in obj.
 *
 * If an error occurred, return false as per normal JSAPI error practice.
 *
 * If no error occurred, but the deletion attempt wasn't allowed (perhaps
 * because the property was non-configurable), call result.fail() and
 * return true.  This will cause |delete obj[id]| to evaluate to false in
 * non-strict mode code, and to throw a TypeError in strict mode code.
 *
 * If no error occurred and the deletion wasn't disallowed (this is *not* the
 * same as saying that a deletion actually occurred -- deleting a non-existent
 * property, or an inherited property, is allowed -- it's just pointless),
 * call result.succeed() and return true.
 */
typedef bool (*JSDeletePropertyOp)(JSContext* cx, JS::HandleObject obj,
                                   JS::HandleId id, JS::ObjectOpResult& result);

/**
 * The type of ObjectOps::enumerate. This callback overrides a portion of
 * SpiderMonkey's default [[Enumerate]] internal method. When an ordinary object
 * is enumerated, that object and each object on its prototype chain is tested
 * for an enumerate op, and those ops are called in order. The properties each
 * op adds to the 'properties' vector are added to the set of values the for-in
 * loop will iterate over. All of this is nonstandard.
 *
 * An object is "enumerated" when it's the target of a for-in loop or
 * JS_Enumerate(). The callback's job is to populate 'properties' with the
 * object's property keys. If `enumerableOnly` is true, the callback should only
 * add enumerable properties.
 */
typedef bool (*JSNewEnumerateOp)(JSContext* cx, JS::HandleObject obj,
                                 JS::MutableHandleIdVector properties,
                                 bool enumerableOnly);

/**
 * The old-style JSClass.enumerate op should define all lazy properties not
 * yet reflected in obj.
 */
typedef bool (*JSEnumerateOp)(JSContext* cx, JS::HandleObject obj);

/**
 * The type of ObjectOps::funToString.  This callback allows an object to
 * provide a custom string to use when Function.prototype.toString is invoked on
 * that object.  A null return value means OOM.
 */
typedef JSString* (*JSFunToStringOp)(JSContext* cx, JS::HandleObject obj,
                                     bool isToSource);

/**
 * Resolve a lazy property named by id in obj by defining it directly in obj.
 * Lazy properties are those reflected from some peer native property space
 * (e.g., the DOM attributes for a given node reflected as obj) on demand.
 *
 * JS looks for a property in an object, and if not found, tries to resolve
 * the given id. *resolvedp should be set to true iff the property was defined
 * on |obj|.
 *
 * See JS::dbg::ShouldAvoidSideEffects in Debug.h if this function has any
 * other side-effect than just resolving the property.
 */
typedef bool (*JSResolveOp)(JSContext* cx, JS::HandleObject obj,
                            JS::HandleId id, bool* resolvedp);

/**
 * A class with a resolve hook can optionally have a mayResolve hook. This hook
 * must have no side effects and must return true for a given id if the resolve
 * hook may resolve this id. This is useful when we're doing a "pure" lookup: if
 * mayResolve returns false, we know we don't have to call the effectful resolve
 * hook.
 *
 * maybeObj, if non-null, is the object on which we're doing the lookup. This
 * can be nullptr: during JIT compilation we sometimes know the Class but not
 * the object.
 */
typedef bool (*JSMayResolveOp)(const JSAtomState& names, jsid id,
                               JSObject* maybeObj);

/**
 * Finalize obj, which the garbage collector has determined to be unreachable
 * from other live objects or from GC roots.  Obviously, finalizers must never
 * store a reference to obj.
 */
typedef void (*JSFinalizeOp)(JS::GCContext* gcx, JSObject* obj);

/**
 * Function type for trace operation of the class called to enumerate all
 * traceable things reachable from obj's private data structure. For each such
 * thing, a trace implementation must call JS::TraceEdge on the thing's
 * location.
 *
 * JSTraceOp implementation can assume that no other threads mutates object
 * state. It must not change state of the object or corresponding native
 * structures. The only exception for this rule is the case when the embedding
 * needs a tight integration with GC. In that case the embedding can check if
 * the traversal is a part of the marking phase through calling
 * JS_IsGCMarkingTracer and apply a special code like emptying caches or
 * marking its native structures.
 */
typedef void (*JSTraceOp)(JSTracer* trc, JSObject* obj);

typedef size_t (*JSObjectMovedOp)(JSObject* obj, JSObject* old);

namespace js {

/* Internal / friend API operation signatures. */

typedef bool (*LookupPropertyOp)(JSContext* cx, JS::HandleObject obj,
                                 JS::HandleId id, JS::MutableHandleObject objp,
                                 PropertyResult* propp);
typedef bool (*DefinePropertyOp)(JSContext* cx, JS::HandleObject obj,
                                 JS::HandleId id,
                                 JS::Handle<JS::PropertyDescriptor> desc,
                                 JS::ObjectOpResult& result);
typedef bool (*HasPropertyOp)(JSContext* cx, JS::HandleObject obj,
                              JS::HandleId id, bool* foundp);
typedef bool (*GetPropertyOp)(JSContext* cx, JS::HandleObject obj,
                              JS::HandleValue receiver, JS::HandleId id,
                              JS::MutableHandleValue vp);
typedef bool (*SetPropertyOp)(JSContext* cx, JS::HandleObject obj,
                              JS::HandleId id, JS::HandleValue v,
                              JS::HandleValue receiver,
                              JS::ObjectOpResult& result);
typedef bool (*GetOwnPropertyOp)(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id,
    JS::MutableHandle<mozilla::Maybe<JS::PropertyDescriptor>> desc);
typedef bool (*DeletePropertyOp)(JSContext* cx, JS::HandleObject obj,
                                 JS::HandleId id, JS::ObjectOpResult& result);

class JS_PUBLIC_API ElementAdder {
 public:
  enum GetBehavior {
    // Check if the element exists before performing the Get and preserve
    // holes.
    CheckHasElemPreserveHoles,

    // Perform a Get operation, like obj[index] in JS.
    GetElement
  };

 private:
  // Only one of these is used.
  JS::RootedObject resObj_;
  JS::Value* vp_;

  uint32_t index_;
#ifdef DEBUG
  uint32_t length_;
#endif
  GetBehavior getBehavior_;

 public:
  ElementAdder(JSContext* cx, JSObject* obj, uint32_t length,
               GetBehavior behavior)
      : resObj_(cx, obj),
        vp_(nullptr),
        index_(0),
#ifdef DEBUG
        length_(length),
#endif
        getBehavior_(behavior) {
  }
  ElementAdder(JSContext* cx, JS::Value* vp, uint32_t length,
               GetBehavior behavior)
      : resObj_(cx),
        vp_(vp),
        index_(0),
#ifdef DEBUG
        length_(length),
#endif
        getBehavior_(behavior) {
  }

  GetBehavior getBehavior() const { return getBehavior_; }

  bool append(JSContext* cx, JS::HandleValue v);
  void appendHole();
};

typedef bool (*GetElementsOp)(JSContext* cx, JS::HandleObject obj,
                              uint32_t begin, uint32_t end,
                              ElementAdder* adder);

/** Callback for the creation of constructor and prototype objects. */
typedef JSObject* (*ClassObjectCreationOp)(JSContext* cx, JSProtoKey key);

/**
 * Callback for custom post-processing after class initialization via
 * ClassSpec.
 */
typedef bool (*FinishClassInitOp)(JSContext* cx, JS::HandleObject ctor,
                                  JS::HandleObject proto);

const size_t JSCLASS_CACHED_PROTO_WIDTH = 7;

struct MOZ_STATIC_CLASS ClassSpec {
  ClassObjectCreationOp createConstructor;
  ClassObjectCreationOp createPrototype;
  const JSFunctionSpec* constructorFunctions;
  const JSPropertySpec* constructorProperties;
  const JSFunctionSpec* prototypeFunctions;
  const JSPropertySpec* prototypeProperties;
  FinishClassInitOp finishInit;
  uintptr_t flags;

  static const size_t ProtoKeyWidth = JSCLASS_CACHED_PROTO_WIDTH;

  static const uintptr_t ProtoKeyMask = (1 << ProtoKeyWidth) - 1;
  static const uintptr_t DontDefineConstructor = 1 << ProtoKeyWidth;

  bool defined() const { return !!createConstructor; }

  // The ProtoKey this class inherits from.
  JSProtoKey inheritanceProtoKey() const {
    MOZ_ASSERT(defined());
    static_assert(JSProto_Null == 0, "zeroed key must be null");

    // Default: Inherit from Object.
    if (!(flags & ProtoKeyMask)) {
      return JSProto_Object;
    }

    return JSProtoKey(flags & ProtoKeyMask);
  }

  bool shouldDefineConstructor() const {
    MOZ_ASSERT(defined());
    return !(flags & DontDefineConstructor);
  }
};

struct MOZ_STATIC_CLASS ClassExtension {
  /**
   * Optional hook called when an object is moved by generational or
   * compacting GC.
   *
   * There may exist weak pointers to an object that are not traced through
   * when the normal trace APIs are used, for example objects in the wrapper
   * cache. This hook allows these pointers to be updated.
   *
   * Note that this hook can be called before JS_NewObject() returns if a GC
   * is triggered during construction of the object. This can happen for
   * global objects for example.
   *
   * The function should return the difference between nursery bytes used and
   * tenured bytes used, which may be nonzero e.g. if some nursery-allocated
   * data beyond the actual GC thing is moved into malloced memory.
   *
   * This is used to compute the nursery promotion rate.
   */
  JSObjectMovedOp objectMovedOp;
};

struct MOZ_STATIC_CLASS ObjectOps {
  LookupPropertyOp lookupProperty;
  DefinePropertyOp defineProperty;
  HasPropertyOp hasProperty;
  GetPropertyOp getProperty;
  SetPropertyOp setProperty;
  GetOwnPropertyOp getOwnPropertyDescriptor;
  DeletePropertyOp deleteProperty;
  GetElementsOp getElements;
  JSFunToStringOp funToString;
};

}  // namespace js

static constexpr const js::ClassSpec* JS_NULL_CLASS_SPEC = nullptr;
static constexpr const js::ClassExtension* JS_NULL_CLASS_EXT = nullptr;

static constexpr const js::ObjectOps* JS_NULL_OBJECT_OPS = nullptr;

// Classes, objects, and properties.

// (1 << 0 is unused)

// Class's initialization code will call `SetNewObjectMetadata` itself.
static const uint32_t JSCLASS_DELAY_METADATA_BUILDER = 1 << 1;

// Class is an XPCWrappedNative. WeakMaps use this to override the wrapper
// disposal mechanism.
static const uint32_t JSCLASS_IS_WRAPPED_NATIVE = 1 << 2;

// First reserved slot is `PrivateValue(nsISupports*)` or `UndefinedValue`.
static constexpr uint32_t JSCLASS_SLOT0_IS_NSISUPPORTS = 1 << 3;

// Objects are DOM.
static const uint32_t JSCLASS_IS_DOMJSCLASS = 1 << 4;

// If wrapped by an xray wrapper, the builtin class's constructor won't be
// unwrapped and invoked. Instead, the constructor is resolved in the caller's
// compartment and invoked with a wrapped newTarget. The constructor has to
// detect and handle this situation. See PromiseConstructor for details.
static const uint32_t JSCLASS_HAS_XRAYED_CONSTRUCTOR = 1 << 5;

// Objects of this class act like the value undefined, in some contexts.
static const uint32_t JSCLASS_EMULATES_UNDEFINED = 1 << 6;

// Reserved for embeddings.
static const uint32_t JSCLASS_USERBIT1 = 1 << 7;

// To reserve slots fetched and stored via JS_Get/SetReservedSlot, bitwise-or
// JSCLASS_HAS_RESERVED_SLOTS(n) into the initializer for JSClass.flags, where n
// is a constant in [1, 255]. Reserved slots are indexed from 0 to n-1.

// Room for 8 flags below ...
static const uintptr_t JSCLASS_RESERVED_SLOTS_SHIFT = 8;
// ... and 16 above this field.
static const uint32_t JSCLASS_RESERVED_SLOTS_WIDTH = 8;

static const uint32_t JSCLASS_RESERVED_SLOTS_MASK =
    js::BitMask(JSCLASS_RESERVED_SLOTS_WIDTH);

static constexpr uint32_t JSCLASS_HAS_RESERVED_SLOTS(uint32_t n) {
  return (n & JSCLASS_RESERVED_SLOTS_MASK) << JSCLASS_RESERVED_SLOTS_SHIFT;
}

static constexpr uint32_t JSCLASS_HIGH_FLAGS_SHIFT =
    JSCLASS_RESERVED_SLOTS_SHIFT + JSCLASS_RESERVED_SLOTS_WIDTH;

static const uint32_t JSCLASS_INTERNAL_FLAG1 =
    1 << (JSCLASS_HIGH_FLAGS_SHIFT + 0);
static const uint32_t JSCLASS_IS_GLOBAL = 1 << (JSCLASS_HIGH_FLAGS_SHIFT + 1);
static const uint32_t JSCLASS_INTERNAL_FLAG2 =
    1 << (JSCLASS_HIGH_FLAGS_SHIFT + 2);
static const uint32_t JSCLASS_IS_PROXY = 1 << (JSCLASS_HIGH_FLAGS_SHIFT + 3);
static const uint32_t JSCLASS_SKIP_NURSERY_FINALIZE =
    1 << (JSCLASS_HIGH_FLAGS_SHIFT + 4);

// Reserved for embeddings.
static const uint32_t JSCLASS_USERBIT2 = 1 << (JSCLASS_HIGH_FLAGS_SHIFT + 5);
static const uint32_t JSCLASS_USERBIT3 = 1 << (JSCLASS_HIGH_FLAGS_SHIFT + 6);

static const uint32_t JSCLASS_BACKGROUND_FINALIZE =
    1 << (JSCLASS_HIGH_FLAGS_SHIFT + 7);
static const uint32_t JSCLASS_FOREGROUND_FINALIZE =
    1 << (JSCLASS_HIGH_FLAGS_SHIFT + 8);

// Bits 25 through 31 are reserved for the CACHED_PROTO_KEY mechanism, see
// below.

// ECMA-262 requires that most constructors used internally create objects
// with "the original Foo.prototype value" as their [[Prototype]] (__proto__)
// member initial value.  The "original ... value" verbiage is there because
// in ECMA-262, global properties naming class objects are read/write and
// deleteable, for the most part.
//
// Implementing this efficiently requires that global objects have classes
// with the following flags. Failure to use JSCLASS_GLOBAL_FLAGS was
// previously allowed, but is now an ES5 violation and thus unsupported.
//
// JSCLASS_GLOBAL_APPLICATION_SLOTS is the number of slots reserved at
// the beginning of every global object's slots for use by the
// application.
static const uint32_t JSCLASS_GLOBAL_APPLICATION_SLOTS = 5;
static const uint32_t JSCLASS_GLOBAL_SLOT_COUNT =
    JSCLASS_GLOBAL_APPLICATION_SLOTS + 1;

static constexpr uint32_t JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(uint32_t n) {
  return JSCLASS_IS_GLOBAL |
         JSCLASS_HAS_RESERVED_SLOTS(JSCLASS_GLOBAL_SLOT_COUNT + n);
}

static constexpr uint32_t JSCLASS_GLOBAL_FLAGS =
    JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(0);

// Fast access to the original value of each standard class's prototype.
static const uint32_t JSCLASS_CACHED_PROTO_SHIFT = JSCLASS_HIGH_FLAGS_SHIFT + 9;
static const uint32_t JSCLASS_CACHED_PROTO_MASK =
    js::BitMask(js::JSCLASS_CACHED_PROTO_WIDTH);

static_assert(JSProto_LIMIT <= (JSCLASS_CACHED_PROTO_MASK + 1),
              "JSProtoKey must not exceed the maximum cacheable proto-mask");

static constexpr uint32_t JSCLASS_HAS_CACHED_PROTO(JSProtoKey key) {
  return uint32_t(key) << JSCLASS_CACHED_PROTO_SHIFT;
}

struct MOZ_STATIC_CLASS JSClassOps {
  /* Function pointer members (may be null). */
  JSAddPropertyOp addProperty;
  JSDeletePropertyOp delProperty;
  JSEnumerateOp enumerate;
  JSNewEnumerateOp newEnumerate;
  JSResolveOp resolve;
  JSMayResolveOp mayResolve;
  JSFinalizeOp finalize;
  JSNative call;
  JSNative construct;
  JSTraceOp trace;
};

static constexpr const JSClassOps* JS_NULL_CLASS_OPS = nullptr;

// Note: This is a MOZ_STATIC_CLASS, as having a non-static JSClass
// can lead to bizarre behaviour, however the annotation
// is at the bottom to handle some incompatibility with GCC
// annotation processing.
struct alignas(js::gc::JSClassAlignBytes) JSClass {
  const char* name;
  uint32_t flags;
  const JSClassOps* cOps;

  const js::ClassSpec* spec;
  const js::ClassExtension* ext;
  const js::ObjectOps* oOps;

  // Public accessors:

  JSAddPropertyOp getAddProperty() const {
    return cOps ? cOps->addProperty : nullptr;
  }
  JSDeletePropertyOp getDelProperty() const {
    return cOps ? cOps->delProperty : nullptr;
  }
  JSEnumerateOp getEnumerate() const {
    return cOps ? cOps->enumerate : nullptr;
  }
  JSNewEnumerateOp getNewEnumerate() const {
    return cOps ? cOps->newEnumerate : nullptr;
  }
  JSResolveOp getResolve() const { return cOps ? cOps->resolve : nullptr; }
  JSMayResolveOp getMayResolve() const {
    return cOps ? cOps->mayResolve : nullptr;
  }
  JSNative getCall() const { return cOps ? cOps->call : nullptr; }
  JSNative getConstruct() const { return cOps ? cOps->construct : nullptr; }

  bool hasFinalize() const { return cOps && cOps->finalize; }
  bool hasTrace() const { return cOps && cOps->trace; }

  bool isTrace(JSTraceOp trace) const { return cOps && cOps->trace == trace; }

  // The special treatment of |finalize| and |trace| is necessary because if we
  // assign either of those hooks to a local variable and then call it -- as is
  // done with the other hooks -- the GC hazard analysis gets confused.
  void doFinalize(JS::GCContext* gcx, JSObject* obj) const {
    MOZ_ASSERT(cOps && cOps->finalize);
    cOps->finalize(gcx, obj);
  }
  void doTrace(JSTracer* trc, JSObject* obj) const {
    MOZ_ASSERT(cOps && cOps->trace);
    cOps->trace(trc, obj);
  }

  /*
   * Objects of this class aren't native objects. They don't have Shapes that
   * describe their properties and layout. Classes using this flag must
   * provide their own property behavior, either by being proxy classes (do
   * this) or by overriding all the ObjectOps except getElements
   * (don't do this).
   */
  static const uint32_t NON_NATIVE = JSCLASS_INTERNAL_FLAG2;

  // A JSObject created from a JSClass extends from one of:
  //  - js::NativeObject
  //  - js::ProxyObject
  //
  // While it is possible to introduce new families of objects, it is strongly
  // discouraged. The JITs would be entirely unable to optimize them and testing
  // coverage is low. The existing NativeObject and ProxyObject are extremely
  // flexible and are able to represent the entire Gecko embedding requirements.
  //
  // NOTE: Internal to SpiderMonkey, there is an experimental js::TypedObject
  //       object family for future WASM features.
  bool isNativeObject() const { return !(flags & NON_NATIVE); }
  bool isProxyObject() const { return flags & JSCLASS_IS_PROXY; }

  bool emulatesUndefined() const { return flags & JSCLASS_EMULATES_UNDEFINED; }

  bool isJSFunction() const {
    return this == js::FunctionClassPtr || this == js::FunctionExtendedClassPtr;
  }

  bool nonProxyCallable() const {
    MOZ_ASSERT(!isProxyObject());
    return isJSFunction() || getCall();
  }

  bool isGlobal() const { return flags & JSCLASS_IS_GLOBAL; }

  bool isDOMClass() const { return flags & JSCLASS_IS_DOMJSCLASS; }

  bool shouldDelayMetadataBuilder() const {
    return flags & JSCLASS_DELAY_METADATA_BUILDER;
  }

  bool isWrappedNative() const { return flags & JSCLASS_IS_WRAPPED_NATIVE; }

  bool slot0IsISupports() const { return flags & JSCLASS_SLOT0_IS_NSISUPPORTS; }

  static size_t offsetOfFlags() { return offsetof(JSClass, flags); }

  // Internal / friend API accessors:

  bool specDefined() const { return spec ? spec->defined() : false; }
  JSProtoKey specInheritanceProtoKey() const {
    return spec ? spec->inheritanceProtoKey() : JSProto_Null;
  }
  bool specShouldDefineConstructor() const {
    return spec ? spec->shouldDefineConstructor() : true;
  }
  js::ClassObjectCreationOp specCreateConstructorHook() const {
    return spec ? spec->createConstructor : nullptr;
  }
  js::ClassObjectCreationOp specCreatePrototypeHook() const {
    return spec ? spec->createPrototype : nullptr;
  }
  const JSFunctionSpec* specConstructorFunctions() const {
    return spec ? spec->constructorFunctions : nullptr;
  }
  const JSPropertySpec* specConstructorProperties() const {
    return spec ? spec->constructorProperties : nullptr;
  }
  const JSFunctionSpec* specPrototypeFunctions() const {
    return spec ? spec->prototypeFunctions : nullptr;
  }
  const JSPropertySpec* specPrototypeProperties() const {
    return spec ? spec->prototypeProperties : nullptr;
  }
  js::FinishClassInitOp specFinishInitHook() const {
    return spec ? spec->finishInit : nullptr;
  }

  JSObjectMovedOp extObjectMovedOp() const {
    return ext ? ext->objectMovedOp : nullptr;
  }

  js::LookupPropertyOp getOpsLookupProperty() const {
    return oOps ? oOps->lookupProperty : nullptr;
  }
  js::DefinePropertyOp getOpsDefineProperty() const {
    return oOps ? oOps->defineProperty : nullptr;
  }
  js::HasPropertyOp getOpsHasProperty() const {
    return oOps ? oOps->hasProperty : nullptr;
  }
  js::GetPropertyOp getOpsGetProperty() const {
    return oOps ? oOps->getProperty : nullptr;
  }
  js::SetPropertyOp getOpsSetProperty() const {
    return oOps ? oOps->setProperty : nullptr;
  }
  js::GetOwnPropertyOp getOpsGetOwnPropertyDescriptor() const {
    return oOps ? oOps->getOwnPropertyDescriptor : nullptr;
  }
  js::DeletePropertyOp getOpsDeleteProperty() const {
    return oOps ? oOps->deleteProperty : nullptr;
  }
  js::GetElementsOp getOpsGetElements() const {
    return oOps ? oOps->getElements : nullptr;
  }
  JSFunToStringOp getOpsFunToString() const {
    return oOps ? oOps->funToString : nullptr;
  }
} MOZ_STATIC_CLASS;

static constexpr uint32_t JSCLASS_RESERVED_SLOTS(const JSClass* clasp) {
  return (clasp->flags >> JSCLASS_RESERVED_SLOTS_SHIFT) &
         JSCLASS_RESERVED_SLOTS_MASK;
}

static constexpr bool JSCLASS_HAS_GLOBAL_FLAG_AND_SLOTS(const JSClass* clasp) {
  return (clasp->flags & JSCLASS_IS_GLOBAL) &&
         JSCLASS_RESERVED_SLOTS(clasp) >= JSCLASS_GLOBAL_SLOT_COUNT;
}

static constexpr JSProtoKey JSCLASS_CACHED_PROTO_KEY(const JSClass* clasp) {
  return JSProtoKey((clasp->flags >> JSCLASS_CACHED_PROTO_SHIFT) &
                    JSCLASS_CACHED_PROTO_MASK);
}

namespace js {

/**
 * Enumeration describing possible values of the [[Class]] internal property
 * value of objects.
 */
enum class ESClass {
  Object,
  Array,
  Number,
  String,
  Boolean,
  RegExp,
  ArrayBuffer,
  SharedArrayBuffer,
  Date,
  Set,
  Map,
  Promise,
  MapIterator,
  SetIterator,
  Arguments,
  Error,
  BigInt,
  Function,  // Note: Only JSFunction objects.

  /** None of the above. */
  Other
};

/* Fills |vp| with the unboxed value for boxed types, or undefined otherwise. */
bool Unbox(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue vp);

// Classes with JSCLASS_SKIP_NURSERY_FINALIZE or Wrapper classes with
// CROSS_COMPARTMENT flags will not have their finalizer called if they are
// nursery allocated and not promoted to the tenured heap. The finalizers for
// these classes must do nothing except free data which was allocated via
// Nursery::allocateBuffer.
inline bool CanNurseryAllocateFinalizedClass(const JSClass* const clasp) {
  MOZ_ASSERT(clasp->hasFinalize());
  return clasp->flags & JSCLASS_SKIP_NURSERY_FINALIZE;
}

#ifdef DEBUG
JS_PUBLIC_API bool HasObjectMovedOp(JSObject* obj);
#endif

} /* namespace js */

#endif /* js_Class_h */

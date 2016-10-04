/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JSClass definition and its component types, plus related interfaces. */

#ifndef js_Class_h
#define js_Class_h

#include "mozilla/DebugOnly.h"

#include "jstypes.h"

#include "js/CallArgs.h"
#include "js/Id.h"
#include "js/TypeDecls.h"

/*
 * A JSClass acts as a vtable for JS objects that allows JSAPI clients to
 * control various aspects of the behavior of an object like property lookup.
 * js::Class is an engine-private extension that allows more control over
 * object behavior and, e.g., allows custom slow layout.
 */

struct JSAtomState;
struct JSFreeOp;
struct JSFunctionSpec;

namespace js {

struct Class;
class FreeOp;
class Shape;

// This is equal to JSFunction::class_.  Use it in places where you don't want
// to #include jsfun.h.
extern JS_FRIEND_DATA(const js::Class* const) FunctionClassPtr;

} // namespace js

namespace JS {

template <typename T>
class AutoVectorRooter;
typedef AutoVectorRooter<jsid> AutoIdVector;

/**
 * The answer to a successful query as to whether an object is an Array per
 * ES6's internal |IsArray| operation (as exposed by |Array.isArray|).
 */
enum class IsArrayAnswer
{
    Array,
    NotArray,
    RevokedProxy
};

/**
 * ES6 7.2.2.
 *
 * Returns false on failure, otherwise returns true and sets |*isArray|
 * indicating whether the object passes ECMAScript's IsArray test.  This is the
 * same test performed by |Array.isArray|.
 *
 * This is NOT the same as asking whether |obj| is an Array or a wrapper around
 * one.  If |obj| is a proxy created by |Proxy.revocable()| and has been
 * revoked, or if |obj| is a proxy whose target (at any number of hops) is a
 * revoked proxy, this method throws a TypeError and returns false.
 */
extern JS_PUBLIC_API(bool)
IsArray(JSContext* cx, HandleObject obj, bool* isArray);

/**
 * Identical to IsArray above, but the nature of the object (if successfully
 * determined) is communicated via |*answer|.  In particular this method
 * returns true and sets |*answer = IsArrayAnswer::RevokedProxy| when called on
 * a revoked proxy.
 *
 * Most users will want the overload above, not this one.
 */
extern JS_PUBLIC_API(bool)
IsArray(JSContext* cx, HandleObject obj, IsArrayAnswer* answer);

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
 *     if (!DefineProperty(cx, obj, id, ..., result))
 *         return false;
 *     if (!result)
 *         return result.reportError(cx, obj, id);
 *
 * Users don't have to call `result.report()`; another possible ending is:
 *
 *     argv.rval().setBoolean(bool(result));
 *     return true;
 */
class ObjectOpResult
{
  private:
    /**
     * code_ is either one of the special codes OkCode or Uninitialized, or
     * an error code. For now the error codes are private to the JS engine;
     * they're defined in js/src/js.msg.
     *
     * code_ is uintptr_t (rather than uint32_t) for the convenience of the
     * JITs, which would otherwise have to deal with either padding or stack
     * alignment on 64-bit platforms.
     */
    uintptr_t code_;

  public:
    enum SpecialCodes : uintptr_t {
        OkCode = 0,
        Uninitialized = uintptr_t(-1)
    };

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
     *     if (funny condition)
     *         return result.fail(JSMSG_CANT_DO_THE_THINGS);
     *
     * The true return value indicates that no exception is pending, and it
     * would be OK to ignore the failure and continue.
     */
    bool fail(uint32_t msg) {
        MOZ_ASSERT(msg != OkCode);
        code_ = msg;
        return true;
    }

    JS_PUBLIC_API(bool) failCantRedefineProp();
    JS_PUBLIC_API(bool) failReadOnly();
    JS_PUBLIC_API(bool) failGetterOnly();
    JS_PUBLIC_API(bool) failCantDelete();

    JS_PUBLIC_API(bool) failCantSetInterposed();
    JS_PUBLIC_API(bool) failCantDefineWindowElement();
    JS_PUBLIC_API(bool) failCantDeleteWindowElement();
    JS_PUBLIC_API(bool) failCantDeleteWindowNamedProperty();
    JS_PUBLIC_API(bool) failCantPreventExtensions();
    JS_PUBLIC_API(bool) failCantSetProto();
    JS_PUBLIC_API(bool) failNoNamedSetter();
    JS_PUBLIC_API(bool) failNoIndexedSetter();

    uint32_t failureCode() const {
        MOZ_ASSERT(!ok());
        return uint32_t(code_);
    }

    /*
     * Report an error or warning if necessary; return true to proceed and
     * false if an error was reported. Call this when failure should cause
     * a warning if extraWarnings are enabled.
     *
     * The precise rules are like this:
     *
     * -   If ok(), then we succeeded. Do nothing and return true.
     * -   Otherwise, if |strict| is true, or if cx has both extraWarnings and
     *     werrorOption enabled, throw a TypeError and return false.
     * -   Otherwise, if cx has extraWarnings enabled, emit a warning and
     *     return true.
     * -   Otherwise, do nothing and return true.
     */
    bool checkStrictErrorOrWarning(JSContext* cx, HandleObject obj, HandleId id, bool strict) {
        if (ok())
            return true;
        return reportStrictErrorOrWarning(cx, obj, id, strict);
    }

    /*
     * The same as checkStrictErrorOrWarning(cx, id, strict), except the
     * operation is not associated with a particular property id. This is
     * used for [[PreventExtensions]] and [[SetPrototypeOf]]. failureCode()
     * must not be an error that has "{0}" in the error message.
     */
    bool checkStrictErrorOrWarning(JSContext* cx, HandleObject obj, bool strict) {
        return ok() || reportStrictErrorOrWarning(cx, obj, strict);
    }

    /* Throw a TypeError. Call this only if !ok(). */
    bool reportError(JSContext* cx, HandleObject obj, HandleId id) {
        return reportStrictErrorOrWarning(cx, obj, id, true);
    }

    /*
     * The same as reportError(cx, obj, id), except the operation is not
     * associated with a particular property id.
     */
    bool reportError(JSContext* cx, HandleObject obj) {
        return reportStrictErrorOrWarning(cx, obj, true);
    }

    /* Helper function for checkStrictErrorOrWarning's slow path. */
    JS_PUBLIC_API(bool) reportStrictErrorOrWarning(JSContext* cx, HandleObject obj, HandleId id, bool strict);
    JS_PUBLIC_API(bool) reportStrictErrorOrWarning(JSContext* cx, HandleObject obj, bool strict);

    /*
     * Convenience method. Return true if ok() or if strict is false; otherwise
     * throw a TypeError and return false.
     */
    bool checkStrict(JSContext* cx, HandleObject obj, HandleId id) {
        return checkStrictErrorOrWarning(cx, obj, id, true);
    }

    /*
     * Convenience method. The same as checkStrict(cx, id), except the
     * operation is not associated with a particular property id.
     */
    bool checkStrict(JSContext* cx, HandleObject obj) {
        return checkStrictErrorOrWarning(cx, obj, true);
    }
};

} // namespace JS

// JSClass operation signatures.

/**
 * Get a property named by id in obj.  Note the jsid id type -- id may
 * be a string (Unicode property identifier) or an int (element index).  The
 * *vp out parameter, on success, is the new property value after the action.
 */
typedef bool
(* JSGetterOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
               JS::MutableHandleValue vp);

/** Add a property named by id to obj. */
typedef bool
(* JSAddPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v);

/**
 * Set a property named by id in obj, treating the assignment as strict
 * mode code if strict is true. Note the jsid id type -- id may be a string
 * (Unicode property identifier) or an int (element index). The *vp out
 * parameter, on success, is the new property value after the
 * set.
 */
typedef bool
(* JSSetterOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
               JS::MutableHandleValue vp, JS::ObjectOpResult& result);

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
typedef bool
(* JSDeletePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                       JS::ObjectOpResult& result);

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
typedef bool
(* JSNewEnumerateOp)(JSContext* cx, JS::HandleObject obj, JS::AutoIdVector& properties,
                     bool enumerableOnly);

/**
 * The old-style JSClass.enumerate op should define all lazy properties not
 * yet reflected in obj.
 */
typedef bool
(* JSEnumerateOp)(JSContext* cx, JS::HandleObject obj);

/**
 * The type of ObjectOps::funToString.  This callback allows an object to
 * provide a custom string to use when Function.prototype.toString is invoked on
 * that object.  A null return value means OOM.
 */
typedef JSString*
(* JSFunToStringOp)(JSContext* cx, JS::HandleObject obj, unsigned indent);

/**
 * Resolve a lazy property named by id in obj by defining it directly in obj.
 * Lazy properties are those reflected from some peer native property space
 * (e.g., the DOM attributes for a given node reflected as obj) on demand.
 *
 * JS looks for a property in an object, and if not found, tries to resolve
 * the given id. *resolvedp should be set to true iff the property was
 * was defined on |obj|.
 */
typedef bool
(* JSResolveOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                bool* resolvedp);

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
typedef bool
(* JSMayResolveOp)(const JSAtomState& names, jsid id, JSObject* maybeObj);

/**
 * Finalize obj, which the garbage collector has determined to be unreachable
 * from other live objects or from GC roots.  Obviously, finalizers must never
 * store a reference to obj.
 */
typedef void
(* JSFinalizeOp)(JSFreeOp* fop, JSObject* obj);

/** Finalizes external strings created by JS_NewExternalString. */
struct JSStringFinalizer {
    void (*finalize)(const JSStringFinalizer* fin, char16_t* chars);
};

/**
 * Check whether v is an instance of obj.  Return false on error or exception,
 * true on success with true in *bp if v is an instance of obj, false in
 * *bp otherwise.
 */
typedef bool
(* JSHasInstanceOp)(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue vp,
                    bool* bp);

/**
 * Function type for trace operation of the class called to enumerate all
 * traceable things reachable from obj's private data structure. For each such
 * thing, a trace implementation must call one of the JS_Call*Tracer variants
 * on the thing.
 *
 * JSTraceOp implementation can assume that no other threads mutates object
 * state. It must not change state of the object or corresponding native
 * structures. The only exception for this rule is the case when the embedding
 * needs a tight integration with GC. In that case the embedding can check if
 * the traversal is a part of the marking phase through calling
 * JS_IsGCMarkingTracer and apply a special code like emptying caches or
 * marking its native structures.
 */
typedef void
(* JSTraceOp)(JSTracer* trc, JSObject* obj);

typedef JSObject*
(* JSWeakmapKeyDelegateOp)(JSObject* obj);

typedef void
(* JSObjectMovedOp)(JSObject* obj, const JSObject* old);

/* js::Class operation signatures. */

namespace js {

typedef bool
(* LookupPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                     JS::MutableHandleObject objp, JS::MutableHandle<Shape*> propp);
typedef bool
(* DefinePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                     JS::Handle<JSPropertyDescriptor> desc,
                     JS::ObjectOpResult& result);
typedef bool
(* HasPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* foundp);
typedef bool
(* GetPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleValue receiver, JS::HandleId id,
                  JS::MutableHandleValue vp);
typedef bool
(* SetPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue v,
                  JS::HandleValue receiver, JS::ObjectOpResult& result);
typedef bool
(* GetOwnPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                     JS::MutableHandle<JSPropertyDescriptor> desc);
typedef bool
(* DeletePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                     JS::ObjectOpResult& result);

typedef bool
(* WatchOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleObject callable);

typedef bool
(* UnwatchOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id);

class JS_FRIEND_API(ElementAdder)
{
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
    mozilla::DebugOnly<uint32_t> length_;
    GetBehavior getBehavior_;

  public:
    ElementAdder(JSContext* cx, JSObject* obj, uint32_t length, GetBehavior behavior)
      : resObj_(cx, obj), vp_(nullptr), index_(0), length_(length), getBehavior_(behavior)
    {}
    ElementAdder(JSContext* cx, JS::Value* vp, uint32_t length, GetBehavior behavior)
      : resObj_(cx), vp_(vp), index_(0), length_(length), getBehavior_(behavior)
    {}

    GetBehavior getBehavior() const { return getBehavior_; }

    bool append(JSContext* cx, JS::HandleValue v);
    void appendHole();
};

typedef bool
(* GetElementsOp)(JSContext* cx, JS::HandleObject obj, uint32_t begin, uint32_t end,
                  ElementAdder* adder);

typedef void
(* FinalizeOp)(FreeOp* fop, JSObject* obj);

#define JS_CLASS_MEMBERS(FinalizeOpType)                                      \
    const char*         name;                                                \
    uint32_t            flags;                                                \
                                                                              \
    /* Function pointer members (may be null). */                             \
    JSAddPropertyOp     addProperty;                                          \
    JSDeletePropertyOp  delProperty;                                          \
    JSGetterOp          getProperty;                                          \
    JSSetterOp          setProperty;                                          \
    JSEnumerateOp       enumerate;                                            \
    JSResolveOp         resolve;                                              \
    JSMayResolveOp      mayResolve;                                           \
    FinalizeOpType      finalize;                                             \
    JSNative            call;                                                 \
    JSHasInstanceOp     hasInstance;                                          \
    JSNative            construct;                                            \
    JSTraceOp           trace

/** Callback for the creation of constructor and prototype objects. */
typedef JSObject* (*ClassObjectCreationOp)(JSContext* cx, JSProtoKey key);

/** Callback for custom post-processing after class initialization via ClassSpec. */
typedef bool (*FinishClassInitOp)(JSContext* cx, JS::HandleObject ctor,
                                  JS::HandleObject proto);

const size_t JSCLASS_CACHED_PROTO_WIDTH = 6;

struct ClassSpec
{
    // All properties except flags should be accessed through accessor.
    ClassObjectCreationOp createConstructor_;
    ClassObjectCreationOp createPrototype_;
    const JSFunctionSpec* constructorFunctions_;
    const JSPropertySpec* constructorProperties_;
    const JSFunctionSpec* prototypeFunctions_;
    const JSPropertySpec* prototypeProperties_;
    FinishClassInitOp finishInit_;
    uintptr_t flags;

    static const size_t ParentKeyWidth = JSCLASS_CACHED_PROTO_WIDTH;

    static const uintptr_t ParentKeyMask = (1 << ParentKeyWidth) - 1;
    static const uintptr_t DontDefineConstructor = 1 << ParentKeyWidth;
    static const uintptr_t IsDelegated = 1 << (ParentKeyWidth + 1);

    bool defined() const { return !!createConstructor_; }

    bool delegated() const {
        return (flags & IsDelegated);
    }

    bool dependent() const {
        MOZ_ASSERT(defined());
        return (flags & ParentKeyMask);
    }

    JSProtoKey parentKey() const {
        static_assert(JSProto_Null == 0, "zeroed key must be null");
        return JSProtoKey(flags & ParentKeyMask);
    }

    bool shouldDefineConstructor() const {
        MOZ_ASSERT(defined());
        return !(flags & DontDefineConstructor);
    }

    const ClassSpec* delegatedClassSpec() const {
        MOZ_ASSERT(delegated());
        return reinterpret_cast<ClassSpec*>(createConstructor_);
    }

    ClassObjectCreationOp createConstructorHook() const {
        if (delegated())
            return delegatedClassSpec()->createConstructorHook();
        return createConstructor_;
    }
    ClassObjectCreationOp createPrototypeHook() const {
        if (delegated())
            return delegatedClassSpec()->createPrototypeHook();
        return createPrototype_;
    }
    const JSFunctionSpec* constructorFunctions() const {
        if (delegated())
            return delegatedClassSpec()->constructorFunctions();
        return constructorFunctions_;
    }
    const JSPropertySpec* constructorProperties() const {
        if (delegated())
            return delegatedClassSpec()->constructorProperties();
        return constructorProperties_;
    }
    const JSFunctionSpec* prototypeFunctions() const {
        if (delegated())
            return delegatedClassSpec()->prototypeFunctions();
        return prototypeFunctions_;
    }
    const JSPropertySpec* prototypeProperties() const {
        if (delegated())
            return delegatedClassSpec()->prototypeProperties();
        return prototypeProperties_;
    }
    FinishClassInitOp finishInitHook() const {
        if (delegated())
            return delegatedClassSpec()->finishInitHook();
        return finishInit_;
    }
};

struct ClassExtension
{
    /**
     * isWrappedNative is true only if the class is an XPCWrappedNative.
     * WeakMaps use this to override the wrapper disposal optimization.
     */
    bool                isWrappedNative;

    /**
     * If an object is used as a key in a weakmap, it may be desirable for the
     * garbage collector to keep that object around longer than it otherwise
     * would. A common case is when the key is a wrapper around an object in
     * another compartment, and we want to avoid collecting the wrapper (and
     * removing the weakmap entry) as long as the wrapped object is alive. In
     * that case, the wrapped object is returned by the wrapper's
     * weakmapKeyDelegateOp hook. As long as the wrapper is used as a weakmap
     * key, it will not be collected (and remain in the weakmap) until the
     * wrapped object is collected.
     */
    JSWeakmapKeyDelegateOp weakmapKeyDelegateOp;

    /**
     * Optional hook called when an object is moved by a compacting GC.
     *
     * There may exist weak pointers to an object that are not traced through
     * when the normal trace APIs are used, for example objects in the wrapper
     * cache. This hook allows these pointers to be updated.
     *
     * Note that this hook can be called before JS_NewObject() returns if a GC
     * is triggered during construction of the object. This can happen for
     * global objects for example.
     */
    JSObjectMovedOp objectMovedOp;
};

inline ClassObjectCreationOp DELEGATED_CLASSSPEC(const ClassSpec* spec) {
    return reinterpret_cast<ClassObjectCreationOp>(const_cast<ClassSpec*>(spec));
}

#define JS_NULL_CLASS_SPEC  {nullptr,nullptr,nullptr,nullptr,nullptr,nullptr}
#define JS_NULL_CLASS_EXT   {false,nullptr}

struct ObjectOps
{
    LookupPropertyOp    lookupProperty;
    DefinePropertyOp    defineProperty;
    HasPropertyOp       hasProperty;
    GetPropertyOp       getProperty;
    SetPropertyOp       setProperty;
    GetOwnPropertyOp    getOwnPropertyDescriptor;
    DeletePropertyOp    deleteProperty;
    WatchOp             watch;
    UnwatchOp           unwatch;
    GetElementsOp       getElements;
    JSNewEnumerateOp    enumerate;
    JSFunToStringOp     funToString;
};

#define JS_NULL_OBJECT_OPS                                                    \
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  \
     nullptr, nullptr, nullptr, nullptr}

} // namespace js

// Classes, objects, and properties.

typedef void (*JSClassInternal)();

struct JSClass {
    JS_CLASS_MEMBERS(JSFinalizeOp);

    void*               reserved[23];
};

#define JSCLASS_HAS_PRIVATE             (1<<0)  // objects have private slot
#define JSCLASS_DELAY_METADATA_CALLBACK (1<<1)  // class's initialization code
                                                // will call
                                                // SetNewObjectMetadata itself
#define JSCLASS_PRIVATE_IS_NSISUPPORTS  (1<<3)  // private is (nsISupports*)
#define JSCLASS_IS_DOMJSCLASS           (1<<4)  // objects are DOM
// Bit 5 is unused.
#define JSCLASS_EMULATES_UNDEFINED      (1<<6)  // objects of this class act
                                                // like the value undefined,
                                                // in some contexts
#define JSCLASS_USERBIT1                (1<<7)  // Reserved for embeddings.

// To reserve slots fetched and stored via JS_Get/SetReservedSlot, bitwise-or
// JSCLASS_HAS_RESERVED_SLOTS(n) into the initializer for JSClass.flags, where
// n is a constant in [1, 255].  Reserved slots are indexed from 0 to n-1.
#define JSCLASS_RESERVED_SLOTS_SHIFT    8       // room for 8 flags below */
#define JSCLASS_RESERVED_SLOTS_WIDTH    8       // and 16 above this field */
#define JSCLASS_RESERVED_SLOTS_MASK     JS_BITMASK(JSCLASS_RESERVED_SLOTS_WIDTH)
#define JSCLASS_HAS_RESERVED_SLOTS(n)   (((n) & JSCLASS_RESERVED_SLOTS_MASK)  \
                                         << JSCLASS_RESERVED_SLOTS_SHIFT)
#define JSCLASS_RESERVED_SLOTS(clasp)   (((clasp)->flags                      \
                                          >> JSCLASS_RESERVED_SLOTS_SHIFT)    \
                                         & JSCLASS_RESERVED_SLOTS_MASK)

#define JSCLASS_HIGH_FLAGS_SHIFT        (JSCLASS_RESERVED_SLOTS_SHIFT +       \
                                         JSCLASS_RESERVED_SLOTS_WIDTH)

#define JSCLASS_IS_ANONYMOUS            (1<<(JSCLASS_HIGH_FLAGS_SHIFT+0))
#define JSCLASS_IS_GLOBAL               (1<<(JSCLASS_HIGH_FLAGS_SHIFT+1))
#define JSCLASS_INTERNAL_FLAG2          (1<<(JSCLASS_HIGH_FLAGS_SHIFT+2))
#define JSCLASS_INTERNAL_FLAG3          (1<<(JSCLASS_HIGH_FLAGS_SHIFT+3))

#define JSCLASS_IS_PROXY                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+4))

#define JSCLASS_SKIP_NURSERY_FINALIZE   (1<<(JSCLASS_HIGH_FLAGS_SHIFT+5))

// Reserved for embeddings.
#define JSCLASS_USERBIT2                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+6))
#define JSCLASS_USERBIT3                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+7))

#define JSCLASS_BACKGROUND_FINALIZE     (1<<(JSCLASS_HIGH_FLAGS_SHIFT+8))

// Bits 26 through 31 are reserved for the CACHED_PROTO_KEY mechanism, see
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
#define JSCLASS_GLOBAL_APPLICATION_SLOTS 5
#define JSCLASS_GLOBAL_SLOT_COUNT                                             \
    (JSCLASS_GLOBAL_APPLICATION_SLOTS + JSProto_LIMIT * 3 + 36)
#define JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(n)                                    \
    (JSCLASS_IS_GLOBAL | JSCLASS_HAS_RESERVED_SLOTS(JSCLASS_GLOBAL_SLOT_COUNT + (n)))
#define JSCLASS_GLOBAL_FLAGS                                                  \
    JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(0)
#define JSCLASS_HAS_GLOBAL_FLAG_AND_SLOTS(clasp)                              \
  (((clasp)->flags & JSCLASS_IS_GLOBAL)                                       \
   && JSCLASS_RESERVED_SLOTS(clasp) >= JSCLASS_GLOBAL_SLOT_COUNT)

// Fast access to the original value of each standard class's prototype.
#define JSCLASS_CACHED_PROTO_SHIFT      (JSCLASS_HIGH_FLAGS_SHIFT + 10)
#define JSCLASS_CACHED_PROTO_MASK       JS_BITMASK(js::JSCLASS_CACHED_PROTO_WIDTH)
#define JSCLASS_HAS_CACHED_PROTO(key)   (uint32_t(key) << JSCLASS_CACHED_PROTO_SHIFT)
#define JSCLASS_CACHED_PROTO_KEY(clasp) ((JSProtoKey)                         \
                                         (((clasp)->flags                     \
                                           >> JSCLASS_CACHED_PROTO_SHIFT)     \
                                          & JSCLASS_CACHED_PROTO_MASK))

// Initializer for unused members of statically initialized JSClass structs.
#define JSCLASS_NO_INTERNAL_MEMBERS     {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
#define JSCLASS_NO_OPTIONAL_MEMBERS     0,0,0,0,0,JSCLASS_NO_INTERNAL_MEMBERS

namespace js {

struct Class
{
    JS_CLASS_MEMBERS(FinalizeOp);
    ClassSpec          spec;
    ClassExtension      ext;
    ObjectOps           ops;

    /*
     * Objects of this class aren't native objects. They don't have Shapes that
     * describe their properties and layout. Classes using this flag must
     * provide their own property behavior, either by being proxy classes (do
     * this) or by overriding all the ObjectOps except getElements, watch and
     * unwatch (don't do this).
     */
    static const uint32_t NON_NATIVE = JSCLASS_INTERNAL_FLAG2;

    bool isNative() const {
        return !(flags & NON_NATIVE);
    }

    bool hasPrivate() const {
        return !!(flags & JSCLASS_HAS_PRIVATE);
    }

    bool emulatesUndefined() const {
        return flags & JSCLASS_EMULATES_UNDEFINED;
    }

    bool isJSFunction() const {
        return this == js::FunctionClassPtr;
    }

    bool nonProxyCallable() const {
        MOZ_ASSERT(!isProxy());
        return isJSFunction() || call;
    }

    bool isProxy() const {
        return flags & JSCLASS_IS_PROXY;
    }

    bool isDOMClass() const {
        return flags & JSCLASS_IS_DOMJSCLASS;
    }

    bool shouldDelayMetadataCallback() const {
        return flags & JSCLASS_DELAY_METADATA_CALLBACK;
    }

    static size_t offsetOfFlags() { return offsetof(Class, flags); }
};

static_assert(offsetof(JSClass, name) == offsetof(Class, name),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, flags) == offsetof(Class, flags),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, addProperty) == offsetof(Class, addProperty),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, delProperty) == offsetof(Class, delProperty),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, getProperty) == offsetof(Class, getProperty),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, setProperty) == offsetof(Class, setProperty),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, enumerate) == offsetof(Class, enumerate),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, resolve) == offsetof(Class, resolve),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, mayResolve) == offsetof(Class, mayResolve),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, finalize) == offsetof(Class, finalize),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, call) == offsetof(Class, call),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, construct) == offsetof(Class, construct),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, hasInstance) == offsetof(Class, hasInstance),
              "Class and JSClass must be consistent");
static_assert(offsetof(JSClass, trace) == offsetof(Class, trace),
              "Class and JSClass must be consistent");
static_assert(sizeof(JSClass) == sizeof(Class),
              "Class and JSClass must be consistent");

static MOZ_ALWAYS_INLINE const JSClass*
Jsvalify(const Class* c)
{
    return (const JSClass*)c;
}

static MOZ_ALWAYS_INLINE const Class*
Valueify(const JSClass* c)
{
    return (const Class*)c;
}

/**
 * Enumeration describing possible values of the [[Class]] internal property
 * value of objects.
 */
enum ESClassValue {
    ESClass_Object, ESClass_Array, ESClass_Number, ESClass_String,
    ESClass_Boolean, ESClass_RegExp, ESClass_ArrayBuffer, ESClass_SharedArrayBuffer,
    ESClass_Date, ESClass_Set, ESClass_Map,

    /** None of the above. */
    ESClass_Other
};

/* Fills |vp| with the unboxed value for boxed types, or undefined otherwise. */
inline bool
Unbox(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue vp);

#ifdef DEBUG
JS_FRIEND_API(bool)
HasObjectMovedOp(JSObject* obj);
#endif

}  /* namespace js */

#endif  /* js_Class_h */

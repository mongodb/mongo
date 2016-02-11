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

struct JSFreeOp;
struct JSFunctionSpec;

namespace js {

struct Class;
class FreeOp;
class PropertyName;
class Shape;

// This is equal to JSFunction::class_.  Use it in places where you don't want
// to #include jsfun.h.
extern JS_FRIEND_DATA(const js::Class* const) FunctionClassPtr;

} // namespace js

namespace JS {

class AutoIdVector;

}

// JSClass operation signatures.

// Add or get a property named by id in obj.  Note the jsid id type -- id may
// be a string (Unicode property identifier) or an int (element index).  The
// *vp out parameter, on success, is the new property value after the action.
typedef bool
(* JSPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                 JS::MutableHandleValue vp);

// Set a property named by id in obj, treating the assignment as strict
// mode code if strict is true. Note the jsid id type -- id may be a string
// (Unicode property identifier) or an int (element index). The *vp out
// parameter, on success, is the new property value after the
// set.
typedef bool
(* JSStrictPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                       bool strict, JS::MutableHandleValue vp);

// Delete a property named by id in obj.
//
// If an error occurred, return false as per normal JSAPI error practice.
//
// If no error occurred, but the deletion attempt wasn't allowed (perhaps
// because the property was non-configurable), set *succeeded to false and
// return true.  This will cause |delete obj[id]| to evaluate to false in
// non-strict mode code, and to throw a TypeError in strict mode code.
//
// If no error occurred and the deletion wasn't disallowed (this is *not* the
// same as saying that a deletion actually occurred -- deleting a non-existent
// property, or an inherited property, is allowed -- it's just pointless),
// set *succeeded to true and return true.
typedef bool
(* JSDeletePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                       bool* succeeded);

// The type of ObjectOps::enumerate. This callback overrides a portion of SpiderMonkey's default
// [[Enumerate]] internal method. When an ordinary object is enumerated, that object and each object
// on its prototype chain is tested for an enumerate op, and those ops are called in order.
// The properties each op adds to the 'properties' vector are added to the set of values the
// for-in loop will iterate over. All of this is nonstandard.
//
// An object is "enumerated" when it's the target of a for-in loop or JS_Enumerate().
// All other property inspection, including Object.keys(obj), goes through [[OwnKeys]].
//
// The callback's job is to populate 'properties' with all property keys that the for-in loop
// should visit.
typedef bool
(* JSNewEnumerateOp)(JSContext* cx, JS::HandleObject obj, JS::AutoIdVector& properties);

// The old-style JSClass.enumerate op should define all lazy properties not
// yet reflected in obj.
typedef bool
(* JSEnumerateOp)(JSContext* cx, JS::HandleObject obj);

// Resolve a lazy property named by id in obj by defining it directly in obj.
// Lazy properties are those reflected from some peer native property space
// (e.g., the DOM attributes for a given node reflected as obj) on demand.
//
// JS looks for a property in an object, and if not found, tries to resolve
// the given id. *resolvedp should be set to true iff the property was
// was defined on |obj|.
//
typedef bool
(* JSResolveOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                bool* resolvedp);

// Convert obj to the given type, returning true with the resulting value in
// *vp on success, and returning false on error or exception.
typedef bool
(* JSConvertOp)(JSContext* cx, JS::HandleObject obj, JSType type,
                JS::MutableHandleValue vp);

// Finalize obj, which the garbage collector has determined to be unreachable
// from other live objects or from GC roots.  Obviously, finalizers must never
// store a reference to obj.
typedef void
(* JSFinalizeOp)(JSFreeOp* fop, JSObject* obj);

// Finalizes external strings created by JS_NewExternalString.
struct JSStringFinalizer {
    void (*finalize)(const JSStringFinalizer* fin, char16_t* chars);
};

// Check whether v is an instance of obj.  Return false on error or exception,
// true on success with true in *bp if v is an instance of obj, false in
// *bp otherwise.
typedef bool
(* JSHasInstanceOp)(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue vp,
                    bool* bp);

// Function type for trace operation of the class called to enumerate all
// traceable things reachable from obj's private data structure. For each such
// thing, a trace implementation must call one of the JS_Call*Tracer variants
// on the thing.
//
// JSTraceOp implementation can assume that no other threads mutates object
// state. It must not change state of the object or corresponding native
// structures. The only exception for this rule is the case when the embedding
// needs a tight integration with GC. In that case the embedding can check if
// the traversal is a part of the marking phase through calling
// JS_IsGCMarkingTracer and apply a special code like emptying caches or
// marking its native structures.
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
(* DefinePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, JS::HandleValue value,
                     JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);
typedef bool
(* HasPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* foundp);
typedef bool
(* GetPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleObject receiver, JS::HandleId id,
                  JS::MutableHandleValue vp);
typedef bool
(* SetPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleObject receiver, JS::HandleId id,
                  JS::MutableHandleValue vp, bool strict);
typedef bool
(* GetOwnPropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id,
                     JS::MutableHandle<JSPropertyDescriptor> desc);
typedef bool
(* DeletePropertyOp)(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* succeeded);

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

    void append(JSContext* cx, JS::HandleValue v);
    void appendHole();
};

typedef bool
(* GetElementsOp)(JSContext* cx, JS::HandleObject obj, uint32_t begin, uint32_t end,
                  ElementAdder* adder);

// A generic type for functions mapping an object to another object, or null
// if an error or exception was thrown on cx.
typedef JSObject*
(* ObjectOp)(JSContext* cx, JS::HandleObject obj);

// Hook to map an object to its inner object. Infallible.
typedef JSObject*
(* InnerObjectOp)(JSObject* obj);

typedef void
(* FinalizeOp)(FreeOp* fop, JSObject* obj);

#define JS_CLASS_MEMBERS(FinalizeOpType)                                      \
    const char*         name;                                                \
    uint32_t            flags;                                                \
                                                                              \
    /* Function pointer members (may be null). */                             \
    JSPropertyOp        addProperty;                                          \
    JSDeletePropertyOp  delProperty;                                          \
    JSPropertyOp        getProperty;                                          \
    JSStrictPropertyOp  setProperty;                                          \
    JSEnumerateOp       enumerate;                                            \
    JSResolveOp         resolve;                                              \
    JSConvertOp         convert;                                              \
    FinalizeOpType      finalize;                                             \
    JSNative            call;                                                 \
    JSHasInstanceOp     hasInstance;                                          \
    JSNative            construct;                                            \
    JSTraceOp           trace

// Callback for the creation of constructor and prototype objects.
typedef JSObject* (*ClassObjectCreationOp)(JSContext* cx, JSProtoKey key);

// Callback for custom post-processing after class initialization via ClassSpec.
typedef bool (*FinishClassInitOp)(JSContext* cx, JS::HandleObject ctor,
                                  JS::HandleObject proto);

const size_t JSCLASS_CACHED_PROTO_WIDTH = 6;

struct ClassSpec
{
    ClassObjectCreationOp createConstructor;
    ClassObjectCreationOp createPrototype;
    const JSFunctionSpec* constructorFunctions;
    const JSFunctionSpec* prototypeFunctions;
    const JSPropertySpec* prototypeProperties;
    FinishClassInitOp finishInit;
    uintptr_t flags;

    static const size_t ParentKeyWidth = JSCLASS_CACHED_PROTO_WIDTH;

    static const uintptr_t ParentKeyMask = (1 << ParentKeyWidth) - 1;
    static const uintptr_t DontDefineConstructor = 1 << ParentKeyWidth;

    bool defined() const { return !!createConstructor; }

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
};

struct ClassExtension
{
    ObjectOp            outerObject;
    InnerObjectOp       innerObject;

    /*
     * isWrappedNative is true only if the class is an XPCWrappedNative.
     * WeakMaps use this to override the wrapper disposal optimization.
     */
    bool                isWrappedNative;

    /*
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

    /*
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

#define JS_NULL_CLASS_SPEC  {nullptr,nullptr,nullptr,nullptr,nullptr,nullptr}
#define JS_NULL_CLASS_EXT   {nullptr,nullptr,false,nullptr,nullptr}

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
    ObjectOp            thisObject;
};

#define JS_NULL_OBJECT_OPS                                                    \
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,  \
     nullptr, nullptr, nullptr, nullptr}

} // namespace js

// Classes, objects, and properties.

typedef void (*JSClassInternal)();

struct JSClass {
    JS_CLASS_MEMBERS(JSFinalizeOp);

    void*               reserved[24];
};

#define JSCLASS_HAS_PRIVATE             (1<<0)  // objects have private slot
#define JSCLASS_PRIVATE_IS_NSISUPPORTS  (1<<3)  // private is (nsISupports*)
#define JSCLASS_IS_DOMJSCLASS           (1<<4)  // objects are DOM
#define JSCLASS_IMPLEMENTS_BARRIERS     (1<<5)  // Correctly implements GC read
                                                // and write barriers
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

#define JSCLASS_FINALIZE_FROM_NURSERY   (1<<(JSCLASS_HIGH_FLAGS_SHIFT+5))

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
#define JSCLASS_GLOBAL_APPLICATION_SLOTS 4
#define JSCLASS_GLOBAL_SLOT_COUNT      (JSCLASS_GLOBAL_APPLICATION_SLOTS + JSProto_LIMIT * 3 + 31)
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
     * this) or by overriding all the ObjectOps except getElements, watch,
     * unwatch, and thisObject (don't do this).
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
static_assert(offsetof(JSClass, convert) == offsetof(Class, convert),
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

/*
 * Enumeration describing possible values of the [[Class]] internal property
 * value of objects.
 */
enum ESClassValue {
    ESClass_Object, ESClass_Array, ESClass_Number, ESClass_String,
    ESClass_Boolean, ESClass_RegExp, ESClass_ArrayBuffer, ESClass_SharedArrayBuffer,
    ESClass_Date, ESClass_Set, ESClass_Map,

    // Special snowflake for the ES6 IsArray method.
    // Please don't use it without calling that function.
    ESClass_IsArray
};

/*
 * Return whether the given object has the given [[Class]] internal property
 * value. Beware, this query says nothing about the js::Class of the JSObject
 * so the caller must not assume anything about obj's representation (e.g., obj
 * may be a proxy).
 */
inline bool
ObjectClassIs(JSObject& obj, ESClassValue classValue, JSContext* cx);

/* Just a helper that checks v.isObject before calling ObjectClassIs. */
inline bool
IsObjectWithClass(const JS::Value& v, ESClassValue classValue, JSContext* cx);

/* Fills |vp| with the unboxed value for boxed types, or undefined otherwise. */
inline bool
Unbox(JSContext* cx, JS::HandleObject obj, JS::MutableHandleValue vp);

#ifdef DEBUG
JS_FRIEND_API(bool)
HasObjectMovedOp(JSObject* obj);
#endif

}  /* namespace js */

#endif  /* js_Class_h */

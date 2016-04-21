/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsobj_h
#define jsobj_h

/*
 * JS object definitions.
 *
 * A JS object consists of a possibly-shared object descriptor containing
 * ordered property names, called the map; and a dense vector of property
 * values, called slots.  The map/slot pointer pair is GC'ed, while the map
 * is reference counted and the slot vector is malloc'ed.
 */

#include "mozilla/MemoryReporting.h"

#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "js/Conversions.h"
#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/TraceableVector.h"
#include "vm/Shape.h"
#include "vm/String.h"
#include "vm/Xdr.h"

namespace JS {
struct ClassInfo;
} // namespace JS

namespace js {

using PropertyDescriptorVector = TraceableVector<PropertyDescriptor>;
class GCMarker;
class Nursery;

namespace gc {
class RelocationOverlay;
} // namespace gc

inline JSObject*
CastAsObject(GetterOp op)
{
    return JS_FUNC_TO_DATA_PTR(JSObject*, op);
}

inline JSObject*
CastAsObject(SetterOp op)
{
    return JS_FUNC_TO_DATA_PTR(JSObject*, op);
}

inline Value
CastAsObjectJsval(GetterOp op)
{
    return ObjectOrNullValue(CastAsObject(op));
}

inline Value
CastAsObjectJsval(SetterOp op)
{
    return ObjectOrNullValue(CastAsObject(op));
}

/******************************************************************************/

extern const Class IntlClass;
extern const Class JSONClass;
extern const Class MathClass;

class GlobalObject;
class NewObjectCache;

// Forward declarations, required for later friend declarations.
bool PreventExtensions(JSContext* cx, JS::HandleObject obj, JS::ObjectOpResult& result);
bool SetImmutablePrototype(js::ExclusiveContext* cx, JS::HandleObject obj, bool* succeeded);

}  /* namespace js */

/*
 * A JavaScript object. The members common to all objects are as follows:
 *
 * - The |group_| member stores the group of the object, which contains its
 *   prototype object, its class and the possible types of its properties.
 *
 * Subclasses of JSObject --- mainly NativeObject and JSFunction --- add more
 * members. Notable among these is the object's shape, which stores flags and
 * some other state, and, for native objects, the layout of all its properties.
 * The second word of a JSObject generally stores its shape; if the second word
 * stores anything else, the value stored cannot be a valid Shape* pointer, so
 * that shape guards can be performed on objects without regard to the specific
 * layout in use.
 */
class JSObject : public js::gc::Cell
{
  protected:
    js::HeapPtrObjectGroup group_;

  private:
    friend class js::Shape;
    friend class js::GCMarker;
    friend class js::NewObjectCache;
    friend class js::Nursery;
    friend class js::gc::RelocationOverlay;
    friend bool js::PreventExtensions(JSContext* cx, JS::HandleObject obj, JS::ObjectOpResult& result);
    friend bool js::SetImmutablePrototype(js::ExclusiveContext* cx, JS::HandleObject obj,
                                          bool* succeeded);

    // Make a new group to use for a singleton object.
    static js::ObjectGroup* makeLazyGroup(JSContext* cx, js::HandleObject obj);

  public:
    bool isNative() const {
        return getClass()->isNative();
    }

    const js::Class* getClass() const {
        return group_->clasp();
    }
    const JSClass* getJSClass() const {
        return Jsvalify(getClass());
    }
    bool hasClass(const js::Class* c) const {
        return getClass() == c;
    }
    const js::ObjectOps* getOps() const {
        return &getClass()->ops;
    }

    js::ObjectGroup* group() const {
        MOZ_ASSERT(!hasLazyGroup());
        return groupRaw();
    }

    js::ObjectGroup* groupRaw() const {
        return group_;
    }

    /*
     * Whether this is the only object which has its specified group. This
     * object will have its group constructed lazily as needed by analysis.
     */
    bool isSingleton() const {
        return group_->singleton();
    }

    /*
     * Whether the object's group has not been constructed yet. If an object
     * might have a lazy group, use getGroup() below, otherwise group().
     */
    bool hasLazyGroup() const {
        return group_->lazy();
    }

    JSCompartment* compartment() const { return group_->compartment(); }
    JSCompartment* maybeCompartment() const { return compartment(); }

    inline js::Shape* maybeShape() const;
    inline js::Shape* ensureShape(js::ExclusiveContext* cx);

    /*
     * Make a non-array object with the specified initial state. This method
     * takes ownership of any extantSlots it is passed.
     */
    static inline JSObject* create(js::ExclusiveContext* cx,
                                   js::gc::AllocKind kind,
                                   js::gc::InitialHeap heap,
                                   js::HandleShape shape,
                                   js::HandleObjectGroup group);

    // Set the shape of an object. This pointer is valid for native objects and
    // some non-native objects. After creating an object, the objects for which
    // the shape pointer is invalid need to overwrite this pointer before a GC
    // can occur.
    inline void setInitialShapeMaybeNonNative(js::Shape* shape);
    inline void setShapeMaybeNonNative(js::Shape* shape);

    // Set the initial slots and elements of an object. These pointers are only
    // valid for native objects, but during initialization are set for all
    // objects. For non-native objects, these must not be dynamically allocated
    // pointers which leak when the non-native object finishes initialization.
    inline void setInitialSlotsMaybeNonNative(js::HeapSlot* slots);
    inline void setInitialElementsMaybeNonNative(js::HeapSlot* elements);

    enum GenerateShape {
        GENERATE_NONE,
        GENERATE_SHAPE
    };

    bool setFlags(js::ExclusiveContext* cx, js::BaseShape::Flag flags,
                  GenerateShape generateShape = GENERATE_NONE);
    inline bool hasAllFlags(js::BaseShape::Flag flags) const;

    /*
     * An object is a delegate if it is on another object's prototype or scope
     * chain, and therefore the delegate might be asked implicitly to get or
     * set a property on behalf of another object. Delegates may be accessed
     * directly too, as may any object, but only those objects linked after the
     * head of any prototype or scope chain are flagged as delegates. This
     * definition helps to optimize shape-based property cache invalidation
     * (see Purge{Scope,Proto}Chain in jsobj.cpp).
     */
    inline bool isDelegate() const;
    bool setDelegate(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::DELEGATE, GENERATE_SHAPE);
    }

    inline bool isBoundFunction() const;
    inline bool hasSpecialEquality() const;

    inline bool watched() const;
    bool setWatched(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::WATCHED, GENERATE_SHAPE);
    }

    // A "qualified" varobj is the object on which "qualified" variable
    // declarations (i.e., those defined with "var") are kept.
    //
    // Conceptually, when a var binding is defined, it is defined on the
    // innermost qualified varobj on the scope chain.
    //
    // Function scopes (CallObjects) are qualified varobjs, and there can be
    // no other qualified varobj that is more inner for var bindings in that
    // function. As such, all references to local var bindings in a function
    // may be statically bound to the function scope. This is subject to
    // further optimization. Unaliased bindings inside functions reside
    // entirely on the frame, not in CallObjects.
    //
    // Global scopes are also qualified varobjs. It is possible to statically
    // know, for a given script, that are no more inner qualified varobjs, so
    // free variable references can be statically bound to the global.
    //
    // Finally, there are non-syntactic qualified varobjs used by embedders
    // (e.g., Gecko and XPConnect), as they often wish to run scripts under a
    // scope that captures var bindings.
    inline bool isQualifiedVarObj() const;
    bool setQualifiedVarObj(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::QUALIFIED_VAROBJ);
    }

    // An "unqualified" varobj is the object on which "unqualified"
    // assignments (i.e., bareword assignments for which the LHS does not
    // exist on the scope chain) are kept.
    inline bool isUnqualifiedVarObj() const;

    /*
     * Objects with an uncacheable proto can have their prototype mutated
     * without inducing a shape change on the object. Property cache entries
     * and JIT inline caches should not be filled for lookups across prototype
     * lookups on the object.
     */
    inline bool hasUncacheableProto() const;
    bool setUncacheableProto(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::UNCACHEABLE_PROTO, GENERATE_SHAPE);
    }

    /*
     * Whether SETLELEM was used to access this object. See also the comment near
     * PropertyTree::MAX_HEIGHT.
     */
    inline bool hadElementsAccess() const;
    bool setHadElementsAccess(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::HAD_ELEMENTS_ACCESS);
    }

    /*
     * Whether there may be indexed properties on this object, excluding any in
     * the object's elements.
     */
    inline bool isIndexed() const;

    /*
     * If this object was instantiated with `new Ctor`, return the constructor's
     * display atom. Otherwise, return nullptr.
     */
    bool constructorDisplayAtom(JSContext* cx, js::MutableHandleAtom name);

    /*
     * The same as constructorDisplayAtom above, however if this object has a
     * lazy group, nullptr is returned. This allows for use in situations that
     * cannot GC and where having some information, even if it is inconsistently
     * available, is better than no information.
     */
    JSAtom* maybeConstructorDisplayAtom() const;

    /* GC support. */

    void traceChildren(JSTracer* trc);

    void fixupAfterMovingGC();

    static js::ThingRootKind rootKind() { return js::THING_ROOT_OBJECT; }
    static const size_t MaxTagBits = 3;
    static bool isNullLike(const JSObject* obj) { return uintptr_t(obj) < (1 << MaxTagBits); }

    MOZ_ALWAYS_INLINE JS::Zone* zone() const {
        return group_->zone();
    }
    MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZone() const {
        return JS::shadow::Zone::asShadowZone(zone());
    }
    MOZ_ALWAYS_INLINE JS::Zone* zoneFromAnyThread() const {
        return group_->zoneFromAnyThread();
    }
    MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZoneFromAnyThread() const {
        return JS::shadow::Zone::asShadowZone(zoneFromAnyThread());
    }
    static MOZ_ALWAYS_INLINE void readBarrier(JSObject* obj);
    static MOZ_ALWAYS_INLINE void writeBarrierPre(JSObject* obj);
    static MOZ_ALWAYS_INLINE void writeBarrierPost(void* cellp, JSObject* prev, JSObject* next);

    /* Return the allocKind we would use if we were to tenure this object. */
    js::gc::AllocKind allocKindForTenure(const js::Nursery& nursery) const;

    size_t tenuredSizeOfThis() const {
        MOZ_ASSERT(isTenured());
        return js::gc::Arena::thingSize(asTenured().getAllocKind());
    }

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::ClassInfo* info);

    // We can only use addSizeOfExcludingThis on tenured objects: it assumes it
    // can apply mallocSizeOf to bits and pieces of the object, whereas objects
    // in the nursery may have those bits and pieces allocated in the nursery
    // along with them, and are not each their own malloc blocks.
    size_t sizeOfIncludingThisInNursery() const;

    // Marks this object as having a singleton group, and leave the group lazy.
    // Constructs a new, unique shape for the object. This should only be
    // called for an object that was just created.
    static inline bool setSingleton(js::ExclusiveContext* cx, js::HandleObject obj);

    // Change an existing object to have a singleton group.
    static bool changeToSingleton(JSContext* cx, js::HandleObject obj);

    inline js::ObjectGroup* getGroup(JSContext* cx);

    const js::HeapPtrObjectGroup& groupFromGC() const {
        /* Direct field access for use by GC. */
        return group_;
    }

    /*
     * We allow the prototype of an object to be lazily computed if the object
     * is a proxy. In the lazy case, we store (JSObject*)0x1 in the proto field
     * of the object's group. We offer three ways of getting the prototype:
     *
     * 1. obj->getProto() returns the prototype, but asserts if obj is a proxy
     *    with a relevant getPrototype() handler.
     * 2. obj->getTaggedProto() returns a TaggedProto, which can be tested to
     *    check if the proto is an object, nullptr, or lazily computed.
     * 3. js::GetPrototype(cx, obj, &proto) computes the proto of an object.
     *    If obj is a proxy and the proto is lazy, this code may allocate or
     *    GC in order to compute the proto. Currently, it will not run JS code.
     */

    js::TaggedProto getTaggedProto() const {
        return group_->proto();
    }

    bool hasTenuredProto() const;

    bool uninlinedIsProxy() const;

    JSObject* getProto() const {
        MOZ_ASSERT(!hasLazyPrototype());
        return getTaggedProto().toObjectOrNull();
    }

    // Normal objects and a subset of proxies have uninteresting [[Prototype]].
    // For such objects the [[Prototype]] is just a value returned when needed
    // for accesses, or modified in response to requests.  These objects store
    // the [[Prototype]] directly within |obj->type_|.
    //
    // Proxies that don't have such a simple [[Prototype]] instead have a
    // "lazy" [[Prototype]].  Accessing the [[Prototype]] of such an object
    // requires going through the proxy handler {get,set}Prototype and
    // setImmutablePrototype methods.  This is most commonly useful for proxies
    // that are wrappers around other objects.  If the [[Prototype]] of the
    // underlying object changes, the [[Prototype]] of the wrapper must also
    // simultaneously change.  We implement this by having the handler methods
    // simply delegate to the wrapped object, forwarding its response to the
    // caller.
    //
    // This method returns true if this object has a non-simple [[Prototype]]
    // as described above, or false otherwise.
    bool hasLazyPrototype() const {
        bool lazy = getTaggedProto().isLazy();
        MOZ_ASSERT_IF(lazy, uninlinedIsProxy());
        return lazy;
    }

    // True iff this object's [[Prototype]] is immutable.  Must not be called
    // on proxies with lazy [[Prototype]]!
    inline bool nonLazyPrototypeIsImmutable() const;

    inline void setGroup(js::ObjectGroup* group);

    /*
     * Mark an object that has been iterated over and is a singleton. We need
     * to recover this information in the object's type information after it
     * is purged on GC.
     */
    inline bool isIteratedSingleton() const;
    bool setIteratedSingleton(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::ITERATED_SINGLETON);
    }

    /*
     * Mark an object as requiring its default 'new' type to have unknown
     * properties.
     */
    inline bool isNewGroupUnknown() const;
    static bool setNewGroupUnknown(JSContext* cx, const js::Class* clasp, JS::HandleObject obj);

    // Mark an object as having its 'new' script information cleared.
    inline bool wasNewScriptCleared() const;
    bool setNewScriptCleared(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::NEW_SCRIPT_CLEARED);
    }

    /* Set a new prototype for an object with a singleton type. */
    bool splicePrototype(JSContext* cx, const js::Class* clasp, js::Handle<js::TaggedProto> proto);

    /*
     * For bootstrapping, whether to splice a prototype for Function.prototype
     * or the global object.
     */
    bool shouldSplicePrototype(JSContext* cx);

    /*
     * Scope chains.
     *
     * The scope chain of an object is the link in the search path when a script
     * does a name lookup on a scope object. For JS internal scope objects ---
     * Call, DeclEnv, Block, and With --- the chain is stored in the first fixed
     * slot of the object.  For other scope objects, the chain goes directly to
     * the global.
     *
     * In code which is not marked hasNonSyntacticScope, scope chains can
     * contain only syntactic scope objects (see IsSyntacticScope) with a global
     * object at the root as the scope of the outermost non-function script. In
     * hasNonSyntacticScope code, the scope of the outermost non-function
     * script might not be a global object, and can have a mix of other objects
     * above it before the global object is reached.
     */

    /*
     * Get the enclosing scope of an object. When called on non-scope object,
     * this will just be the global (the name "enclosing scope" still applies
     * in this situation because non-scope objects can be on the scope chain).
     */
    inline JSObject* enclosingScope();

    inline js::GlobalObject& global() const;
    inline bool isOwnGlobal() const;

    /*
     * ES5 meta-object properties and operations.
     */

  public:
    // Indicates whether a non-proxy is extensible.  Don't call on proxies!
    // This method really shouldn't exist -- but there are a few internal
    // places that want it (JITs and the like), and it'd be a pain to mark them
    // all as friends.
    inline bool nonProxyIsExtensible() const;

  public:
    /*
     * Iterator-specific getters and setters.
     */

    static const uint32_t ITER_CLASS_NFIXED_SLOTS = 1;

    /*
     * Back to generic stuff.
     */
    bool isCallable() const;
    bool isConstructor() const;
    JSNative callHook() const;
    JSNative constructHook() const;

    MOZ_ALWAYS_INLINE void finalize(js::FreeOp* fop);

  public:
    static bool reportReadOnly(JSContext* cx, jsid id, unsigned report = JSREPORT_ERROR);
    bool reportNotConfigurable(JSContext* cx, jsid id, unsigned report = JSREPORT_ERROR);
    bool reportNotExtensible(JSContext* cx, unsigned report = JSREPORT_ERROR);

    /*
     * Get the property with the given id, then call it as a function with the
     * given arguments, providing this object as |this|. If the property isn't
     * callable a TypeError will be thrown. On success the value returned by
     * the call is stored in *vp.
     */
    bool callMethod(JSContext* cx, js::HandleId id, unsigned argc, js::Value* argv,
                    js::MutableHandleValue vp);

    static bool nonNativeSetProperty(JSContext* cx, js::HandleObject obj, js::HandleId id,
                                     js::HandleValue v, js::HandleValue receiver,
                                     JS::ObjectOpResult& result);
    static bool nonNativeSetElement(JSContext* cx, js::HandleObject obj, uint32_t index,
                                    js::HandleValue v, js::HandleValue receiver,
                                    JS::ObjectOpResult& result);

    static bool swap(JSContext* cx, JS::HandleObject a, JS::HandleObject b);

  private:
    void fixDictionaryShapeAfterSwap();

  public:
    inline void initArrayClass();

    /*
     * In addition to the generic object interface provided by JSObject,
     * specific types of objects may provide additional operations. To access,
     * these addition operations, callers should use the pattern:
     *
     *   if (obj.is<XObject>()) {
     *     XObject& x = obj.as<XObject>();
     *     x.foo();
     *   }
     *
     * These XObject classes form a hierarchy. For example, for a cloned block
     * object, the following predicates are true: is<ClonedBlockObject>,
     * is<BlockObject>, is<NestedScopeObject> and is<ScopeObject>. Each of
     * these has a respective class that derives and adds operations.
     *
     * A class XObject is defined in a vm/XObject{.h, .cpp, -inl.h} file
     * triplet (along with any class YObject that derives XObject).
     *
     * Note that X represents a low-level representation and does not query the
     * [[Class]] property of object defined by the spec (for this, see
     * js::GetBuiltinClass).
     */

    template <class T>
    inline bool is() const { return getClass() == &T::class_; }

    template <class T>
    T& as() {
        MOZ_ASSERT(this->is<T>());
        return *static_cast<T*>(this);
    }

    template <class T>
    const T& as() const {
        MOZ_ASSERT(this->is<T>());
        return *static_cast<const T*>(this);
    }

#ifdef DEBUG
    void dump();
#endif

    /* JIT Accessors */

    static size_t offsetOfGroup() { return offsetof(JSObject, group_); }
    static size_t offsetOfShape() { return sizeof(JSObject); }

    // Maximum size in bytes of a JSObject.
    static const size_t MAX_BYTE_SIZE = 4 * sizeof(void*) + 16 * sizeof(JS::Value);

  private:
    JSObject() = delete;
    JSObject(const JSObject& other) = delete;
    void operator=(const JSObject& other) = delete;
};

template <class U>
MOZ_ALWAYS_INLINE JS::Handle<U*>
js::RootedBase<JSObject*>::as() const
{
    const JS::Rooted<JSObject*>& self = *static_cast<const JS::Rooted<JSObject*>*>(this);
    MOZ_ASSERT(self->is<U>());
    return Handle<U*>::fromMarkedLocation(reinterpret_cast<U* const*>(self.address()));
}

template <class U>
MOZ_ALWAYS_INLINE JS::Handle<U*>
js::HandleBase<JSObject*>::as() const
{
    const JS::Handle<JSObject*>& self = *static_cast<const JS::Handle<JSObject*>*>(this);
    MOZ_ASSERT(self->is<U>());
    return Handle<U*>::fromMarkedLocation(reinterpret_cast<U* const*>(self.address()));
}

/*
 * The only sensible way to compare JSObject with == is by identity. We use
 * const& instead of * as a syntactic way to assert non-null. This leads to an
 * abundance of address-of operators to identity. Hence this overload.
 */
static MOZ_ALWAYS_INLINE bool
operator==(const JSObject& lhs, const JSObject& rhs)
{
    return &lhs == &rhs;
}

static MOZ_ALWAYS_INLINE bool
operator!=(const JSObject& lhs, const JSObject& rhs)
{
    return &lhs != &rhs;
}

// Size of the various GC thing allocation sizes used for objects.
struct JSObject_Slots0 : JSObject { void* data[3]; };
struct JSObject_Slots2 : JSObject { void* data[3]; js::Value fslots[2]; };
struct JSObject_Slots4 : JSObject { void* data[3]; js::Value fslots[4]; };
struct JSObject_Slots8 : JSObject { void* data[3]; js::Value fslots[8]; };
struct JSObject_Slots12 : JSObject { void* data[3]; js::Value fslots[12]; };
struct JSObject_Slots16 : JSObject { void* data[3]; js::Value fslots[16]; };

/* static */ MOZ_ALWAYS_INLINE void
JSObject::readBarrier(JSObject* obj)
{
    MOZ_ASSERT_IF(obj, !isNullLike(obj));
    if (obj && obj->isTenured())
        obj->asTenured().readBarrier(&obj->asTenured());
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPre(JSObject* obj)
{
    MOZ_ASSERT_IF(obj, !isNullLike(obj));
    if (obj && obj->isTenured())
        obj->asTenured().writeBarrierPre(&obj->asTenured());
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPost(void* cellp, JSObject* prev, JSObject* next)
{
    MOZ_ASSERT(cellp);
    MOZ_ASSERT_IF(next, !IsNullTaggedPointer(next));
    MOZ_ASSERT_IF(prev, !IsNullTaggedPointer(prev));

    // If the target needs an entry, add it.
    js::gc::StoreBuffer* buffer;
    if (next && (buffer = next->storeBuffer())) {
        // If we know that the prev has already inserted an entry, we can skip
        // doing the lookup to add the new entry. Note that we cannot safely
        // assert the presence of the entry because it may have been added
        // via a different store buffer.
        if (prev && prev->storeBuffer())
            return;
        buffer->putCell(static_cast<js::gc::Cell**>(cellp));
        return;
    }

    // Remove the prev entry if the new value does not need it.
    if (prev && (buffer = prev->storeBuffer()))
        buffer->unputCell(static_cast<js::gc::Cell**>(cellp));
}

namespace js {

inline bool
IsCallable(const Value& v)
{
    return v.isObject() && v.toObject().isCallable();
}

// ES6 rev 24 (2014 April 27) 7.2.5 IsConstructor
inline bool
IsConstructor(const Value& v)
{
    return v.isObject() && v.toObject().isConstructor();
}

} /* namespace js */

class JSValueArray {
  public:
    const js::Value* array;
    size_t length;

    JSValueArray(const js::Value* v, size_t c) : array(v), length(c) {}
};

class ValueArray {
  public:
    js::Value* array;
    size_t length;

    ValueArray(js::Value* v, size_t c) : array(v), length(c) {}
};

namespace js {

/*** Standard internal methods ********************************************************************
 *
 * The functions below are the fundamental operations on objects. See the
 * comment about "Standard internal methods" in jsapi.h.
 */

/*
 * ES6 [[GetPrototypeOf]]. Get obj's prototype, storing it in protop.
 *
 * If obj is definitely not a proxy, the infallible obj->getProto() can be used
 * instead. See the comment on JSObject::getTaggedProto().
 */
inline bool
GetPrototype(JSContext* cx, HandleObject obj, MutableHandleObject protop);

/*
 * ES6 [[SetPrototypeOf]]. Change obj's prototype to proto.
 *
 * Returns false on error, success of operation in *result. For example, if
 * obj is not extensible, its prototype is fixed. js::SetPrototype will return
 * true, because no exception is thrown for this; but *result will be false.
 */
extern bool
SetPrototype(JSContext* cx, HandleObject obj, HandleObject proto,
             ObjectOpResult& result);

/* Convenience function: like the above, but throw on failure. */
extern bool
SetPrototype(JSContext* cx, HandleObject obj, HandleObject proto);

/*
 * ES6 [[IsExtensible]]. Extensible objects can have new properties defined on
 * them. Inextensible objects can't, and their [[Prototype]] slot is fixed as
 * well.
 */
inline bool
IsExtensible(ExclusiveContext* cx, HandleObject obj, bool* extensible);

/*
 * ES6 [[PreventExtensions]]. Attempt to change the [[Extensible]] bit on |obj|
 * to false.  Indicate success or failure through the |result| outparam, or
 * actual error through the return value.
 */
extern bool
PreventExtensions(JSContext* cx, HandleObject obj, ObjectOpResult& result);

/* Convenience function. As above, but throw on failure. */
extern bool
PreventExtensions(JSContext* cx, HandleObject obj);

/*
 * ES6 [[GetOwnProperty]]. Get a description of one of obj's own properties.
 *
 * If no such property exists on obj, return true with desc.object() set to
 * null.
 */
extern bool
GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                         MutableHandle<PropertyDescriptor> desc);

/* ES6 [[DefineOwnProperty]]. Define a property on obj. */
extern bool
DefineProperty(JSContext* cx, HandleObject obj, HandleId id,
               Handle<PropertyDescriptor> desc, ObjectOpResult& result);

extern bool
DefineProperty(ExclusiveContext* cx, HandleObject obj, HandleId id, HandleValue value,
               JSGetterOp getter, JSSetterOp setter, unsigned attrs, ObjectOpResult& result);

extern bool
DefineProperty(ExclusiveContext* cx, HandleObject obj, PropertyName* name, HandleValue value,
               JSGetterOp getter, JSSetterOp setter, unsigned attrs, ObjectOpResult& result);

extern bool
DefineElement(ExclusiveContext* cx, HandleObject obj, uint32_t index, HandleValue value,
              JSGetterOp getter, JSSetterOp setter, unsigned attrs, ObjectOpResult& result);

/*
 * When the 'result' out-param is omitted, the behavior is the same as above, except
 * that any failure results in a TypeError.
 */
extern bool
DefineProperty(JSContext* cx, HandleObject obj, HandleId id, Handle<PropertyDescriptor> desc);

extern bool
DefineProperty(ExclusiveContext* cx, HandleObject obj, HandleId id, HandleValue value,
               JSGetterOp getter = nullptr,
               JSSetterOp setter = nullptr,
               unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineProperty(ExclusiveContext* cx, HandleObject obj, PropertyName* name, HandleValue value,
               JSGetterOp getter = nullptr,
               JSSetterOp setter = nullptr,
               unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineElement(ExclusiveContext* cx, HandleObject obj, uint32_t index, HandleValue value,
              JSGetterOp getter = nullptr,
              JSSetterOp setter = nullptr,
              unsigned attrs = JSPROP_ENUMERATE);

/*
 * ES6 [[Has]]. Set *foundp to true if `id in obj` (that is, if obj has an own
 * or inherited property obj[id]), false otherwise.
 */
inline bool
HasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);

inline bool
HasProperty(JSContext* cx, HandleObject obj, PropertyName* name, bool* foundp);

/*
 * ES6 [[Get]]. Get the value of the property `obj[id]`, or undefined if no
 * such property exists.
 *
 * Typically obj == receiver; if obj != receiver then the caller is most likely
 * a proxy using GetProperty to finish a property get that started out as
 * `receiver[id]`, and we've already searched the prototype chain up to `obj`.
 */
inline bool
GetProperty(JSContext* cx, HandleObject obj, HandleValue receiver, HandleId id,
            MutableHandleValue vp);

inline bool
GetProperty(JSContext* cx, HandleObject obj, HandleValue receiver, PropertyName* name,
            MutableHandleValue vp)
{
    RootedId id(cx, NameToId(name));
    return GetProperty(cx, obj, receiver, id, vp);
}

inline bool
GetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, HandleId id,
            MutableHandleValue vp)
{
    RootedValue receiverValue(cx, ObjectValue(*receiver));
    return GetProperty(cx, obj, receiverValue, id, vp);
}

inline bool
GetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, PropertyName* name,
            MutableHandleValue vp)
{
    RootedValue receiverValue(cx, ObjectValue(*receiver));
    return GetProperty(cx, obj, receiverValue, name, vp);
}

inline bool
GetElement(JSContext* cx, HandleObject obj, HandleValue receiver, uint32_t index,
           MutableHandleValue vp);

inline bool
GetElement(JSContext* cx, HandleObject obj, HandleObject receiver, uint32_t index,
           MutableHandleValue vp);

inline bool
GetPropertyNoGC(JSContext* cx, JSObject* obj, const Value& receiver, jsid id, Value* vp);

inline bool
GetPropertyNoGC(JSContext* cx, JSObject* obj, JSObject* receiver, jsid id, Value* vp)
{
    return GetPropertyNoGC(cx, obj, ObjectValue(*receiver), id, vp);
}

inline bool
GetPropertyNoGC(JSContext* cx, JSObject* obj, const Value& receiver, PropertyName* name, Value* vp)
{
    return GetPropertyNoGC(cx, obj, receiver, NameToId(name), vp);
}

inline bool
GetPropertyNoGC(JSContext* cx, JSObject* obj, JSObject* receiver, PropertyName* name, Value* vp)
{
    return GetPropertyNoGC(cx, obj, ObjectValue(*receiver), name, vp);
}

inline bool
GetElementNoGC(JSContext* cx, JSObject* obj, const Value& receiver, uint32_t index, Value* vp);

inline bool
GetElementNoGC(JSContext* cx, JSObject* obj, JSObject* receiver, uint32_t index, Value* vp);

/*
 * ES6 [[Set]]. Carry out the assignment `obj[id] = v`.
 *
 * The `receiver` argument has to do with how [[Set]] interacts with the
 * prototype chain and proxies. It's hard to explain and ES6 doesn't really
 * try. Long story short, if you just want bog-standard assignment, pass
 * `ObjectValue(*obj)` as receiver. Or better, use one of the signatures that
 * doesn't have a receiver parameter.
 *
 * Callers pass obj != receiver e.g. when a proxy is involved, obj is the
 * proxy's target, and the proxy is using SetProperty to finish an assignment
 * that started out as `receiver[id] = v`, by delegating it to obj.
 */
inline bool
SetProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
            HandleValue receiver, ObjectOpResult& result);

inline bool
SetProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v)
{
    RootedValue receiver(cx, ObjectValue(*obj));
    ObjectOpResult result;
    return SetProperty(cx, obj, id, v, receiver, result) &&
           result.checkStrict(cx, obj, id);
}

inline bool
SetProperty(JSContext* cx, HandleObject obj, PropertyName* name, HandleValue v,
            HandleValue receiver, ObjectOpResult& result)
{
    RootedId id(cx, NameToId(name));
    return SetProperty(cx, obj, id, v, receiver, result);
}

inline bool
SetProperty(JSContext* cx, HandleObject obj, PropertyName* name, HandleValue v)
{
    RootedId id(cx, NameToId(name));
    RootedValue receiver(cx, ObjectValue(*obj));
    ObjectOpResult result;
    return SetProperty(cx, obj, id, v, receiver, result) &&
           result.checkStrict(cx, obj, id);
}

inline bool
SetElement(JSContext* cx, HandleObject obj, uint32_t index, HandleValue v,
           HandleValue receiver, ObjectOpResult& result);

/*
 * ES6 draft rev 31 (15 Jan 2015) 7.3.3 Put (O, P, V, Throw), except that on
 * success, the spec says this is supposed to return a boolean value, which we
 * don't bother doing.
 */
inline bool
PutProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v, bool strict)
{
    RootedValue receiver(cx, ObjectValue(*obj));
    ObjectOpResult result;
    return SetProperty(cx, obj, id, v, receiver, result) &&
           result.checkStrictErrorOrWarning(cx, obj, id, strict);
}

/*
 * ES6 [[Delete]]. Equivalent to the JS code `delete obj[id]`.
 */
inline bool
DeleteProperty(JSContext* cx, HandleObject obj, HandleId id, ObjectOpResult& result);

inline bool
DeleteElement(JSContext* cx, HandleObject obj, uint32_t index, ObjectOpResult& result);


/*** SpiderMonkey nonstandard internal methods ***************************************************/

/*
 * Attempt to make |obj|'s [[Prototype]] immutable, such that subsequently
 * trying to change it will not work.  If an internal error occurred,
 * returns false.  Otherwise, |*succeeded| is set to true iff |obj|'s
 * [[Prototype]] is now immutable.
 */
extern bool
SetImmutablePrototype(js::ExclusiveContext* cx, JS::HandleObject obj, bool* succeeded);

extern bool
GetPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                      MutableHandle<PropertyDescriptor> desc);

/*
 * Deprecated. A version of HasProperty that also returns the object on which
 * the property was found (but that information is unreliable for proxies), and
 * the Shape of the property, if native.
 */
extern bool
LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
               MutableHandleObject objp, MutableHandleShape propp);

inline bool
LookupProperty(JSContext* cx, HandleObject obj, PropertyName* name,
               MutableHandleObject objp, MutableHandleShape propp)
{
    RootedId id(cx, NameToId(name));
    return LookupProperty(cx, obj, id, objp, propp);
}

/* Set *result to tell whether obj has an own property with the given id. */
extern bool
HasOwnProperty(JSContext* cx, HandleObject obj, HandleId id, bool* result);

/*
 * Set a watchpoint: a synchronous callback when the given property of the
 * given object is set.
 *
 * Watchpoints are nonstandard and do not fit in well with the way ES6
 * specifies [[Set]]. They are also insufficient for implementing
 * Object.observe.
 */
extern bool
WatchProperty(JSContext* cx, HandleObject obj, HandleId id, HandleObject callable);

/* Clear a watchpoint. */
extern bool
UnwatchProperty(JSContext* cx, HandleObject obj, HandleId id);

/* ES6 draft rev 36 (2015 March 17) 7.1.1 ToPrimitive(vp[, preferredType]) */
extern bool
ToPrimitiveSlow(JSContext* cx, JSType hint, MutableHandleValue vp);

inline bool
ToPrimitive(JSContext* cx, MutableHandleValue vp)
{
    if (vp.isPrimitive())
        return true;
    return ToPrimitiveSlow(cx, JSTYPE_VOID, vp);
}

inline bool
ToPrimitive(JSContext* cx, JSType preferredType, MutableHandleValue vp)
{
    if (vp.isPrimitive())
        return true;
    return ToPrimitiveSlow(cx, preferredType, vp);
}

/*
 * toString support. (This isn't called GetClassName because there's a macro in
 * <windows.h> with that name.)
 */
extern const char*
GetObjectClassName(JSContext* cx, HandleObject obj);

/*
 * Return an object that may be used as `this` in place of obj. For most
 * objects this just returns obj.
 *
 * Some JSObjects shouldn't be exposed directly to script. This includes (at
 * least) DynamicWithObjects and Window objects. However, since both of those
 * can be on scope chains, we sometimes would expose those as `this` if we
 * were not so vigilant about calling GetThisValue where appropriate.
 *
 * See comments at ComputeImplicitThis.
 */
Value
GetThisValue(JSObject* obj);

/* * */

typedef JSObject* (*ClassInitializerOp)(JSContext* cx, JS::HandleObject obj);

/* Fast access to builtin constructors and prototypes. */
bool
GetBuiltinConstructor(ExclusiveContext* cx, JSProtoKey key, MutableHandleObject objp);

bool
GetBuiltinPrototype(ExclusiveContext* cx, JSProtoKey key, MutableHandleObject objp);

JSObject*
GetBuiltinPrototypePure(GlobalObject* global, JSProtoKey protoKey);

extern bool
SetClassAndProto(JSContext* cx, HandleObject obj,
                 const Class* clasp, Handle<TaggedProto> proto);

extern bool
IsStandardPrototype(JSObject* obj, JSProtoKey key);

} /* namespace js */

/*
 * Select Object.prototype method names shared between jsapi.cpp and jsobj.cpp.
 */
extern const char js_watch_str[];
extern const char js_unwatch_str[];
extern const char js_hasOwnProperty_str[];
extern const char js_isPrototypeOf_str[];
extern const char js_propertyIsEnumerable_str[];

#ifdef JS_OLD_GETTER_SETTER_METHODS
extern const char js_defineGetter_str[];
extern const char js_defineSetter_str[];
extern const char js_lookupGetter_str[];
extern const char js_lookupSetter_str[];
#endif

namespace js {

inline gc::InitialHeap
GetInitialHeap(NewObjectKind newKind, const Class* clasp)
{
    if (newKind != GenericObject)
        return gc::TenuredHeap;
    if (clasp->finalize && !(clasp->flags & JSCLASS_SKIP_NURSERY_FINALIZE))
        return gc::TenuredHeap;
    return gc::DefaultHeap;
}

bool
NewObjectWithTaggedProtoIsCachable(ExclusiveContext* cxArg, Handle<TaggedProto> proto,
                                   NewObjectKind newKind, const Class* clasp);

// ES6 9.1.15 GetPrototypeFromConstructor.
extern bool
GetPrototypeFromConstructor(JSContext* cx, js::HandleObject newTarget, js::MutableHandleObject proto);

extern bool
GetPrototypeFromCallableConstructor(JSContext* cx, const CallArgs& args, js::MutableHandleObject proto);

// Specialized call for constructing |this| with a known function callee,
// and a known prototype.
extern JSObject*
CreateThisForFunctionWithProto(JSContext* cx, js::HandleObject callee, HandleObject newTarget,
                               HandleObject proto, NewObjectKind newKind = GenericObject);

// Specialized call for constructing |this| with a known function callee.
extern JSObject*
CreateThisForFunction(JSContext* cx, js::HandleObject callee, js::HandleObject newTarget,
                      NewObjectKind newKind);

// Generic call for constructing |this|.
extern JSObject*
CreateThis(JSContext* cx, const js::Class* clasp, js::HandleObject callee);

extern JSObject*
CloneObject(JSContext* cx, HandleObject obj, Handle<js::TaggedProto> proto);

extern JSObject*
DeepCloneObjectLiteral(JSContext* cx, HandleObject obj, NewObjectKind newKind = GenericObject);

extern bool
DefineProperties(JSContext* cx, HandleObject obj, HandleObject props);

inline JSGetterOp
CastAsGetterOp(JSObject* object)
{
    return JS_DATA_TO_FUNC_PTR(JSGetterOp, object);
}

inline JSSetterOp
CastAsSetterOp(JSObject* object)
{
    return JS_DATA_TO_FUNC_PTR(JSSetterOp, object);
}

/* ES6 draft rev 32 (2015 Feb 2) 6.2.4.5 ToPropertyDescriptor(Obj) */
bool
ToPropertyDescriptor(JSContext* cx, HandleValue descval, bool checkAccessors,
                     MutableHandle<PropertyDescriptor> desc);

/*
 * Throw a TypeError if desc.getterObject() or setterObject() is not
 * callable. This performs exactly the checks omitted by ToPropertyDescriptor
 * when checkAccessors is false.
 */
bool
CheckPropertyDescriptorAccessors(JSContext* cx, Handle<PropertyDescriptor> desc);

void
CompletePropertyDescriptor(MutableHandle<PropertyDescriptor> desc);

/*
 * Read property descriptors from props, as for Object.defineProperties. See
 * ES5 15.2.3.7 steps 3-5.
 */
extern bool
ReadPropertyDescriptors(JSContext* cx, HandleObject props, bool checkAccessors,
                        AutoIdVector* ids, MutableHandle<PropertyDescriptorVector> descs);

/* Read the name using a dynamic lookup on the scopeChain. */
extern bool
LookupName(JSContext* cx, HandlePropertyName name, HandleObject scopeChain,
           MutableHandleObject objp, MutableHandleObject pobjp, MutableHandleShape propp);

extern bool
LookupNameNoGC(JSContext* cx, PropertyName* name, JSObject* scopeChain,
               JSObject** objp, JSObject** pobjp, Shape** propp);

/*
 * Like LookupName except returns the global object if 'name' is not found in
 * any preceding scope.
 *
 * Additionally, pobjp and propp are not needed by callers so they are not
 * returned.
 */
extern bool
LookupNameWithGlobalDefault(JSContext* cx, HandlePropertyName name, HandleObject scopeChain,
                            MutableHandleObject objp);

/*
 * Like LookupName except returns the unqualified var object if 'name' is not
 * found in any preceding scope. Normally the unqualified var object is the
 * global. If the value for the name in the looked-up scope is an
 * uninitialized lexical, an UninitializedLexicalObject is returned.
 *
 * Additionally, pobjp is not needed by callers so it is not returned.
 */
extern bool
LookupNameUnqualified(JSContext* cx, HandlePropertyName name, HandleObject scopeChain,
                      MutableHandleObject objp);

} // namespace js

namespace js {

extern JSObject*
FindVariableScope(JSContext* cx, JSFunction** funp);

bool
LookupPropertyPure(ExclusiveContext* cx, JSObject* obj, jsid id, JSObject** objp,
                   Shape** propp);

bool
GetPropertyPure(ExclusiveContext* cx, JSObject* obj, jsid id, Value* vp);

bool
GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                         MutableHandle<PropertyDescriptor> desc);

bool
GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id, MutableHandleValue vp);

/*
 * ES6 draft rev 32 (2015 Feb 2) 6.2.4.4 FromPropertyDescriptor(Desc).
 *
 * If desc.object() is null, then vp is set to undefined.
 */
extern bool
FromPropertyDescriptor(JSContext* cx, Handle<PropertyDescriptor> desc, MutableHandleValue vp);

/*
 * Like FromPropertyDescriptor, but ignore desc.object() and always set vp
 * to an object on success.
 *
 * Use FromPropertyDescriptor for getOwnPropertyDescriptor, since desc.object()
 * is used to indicate whether a result was found or not.  Use this instead for
 * defineProperty: it would be senseless to define a "missing" property.
 */
extern bool
FromPropertyDescriptorToObject(JSContext* cx, Handle<PropertyDescriptor> desc,
                               MutableHandleValue vp);

extern bool
IsDelegate(JSContext* cx, HandleObject obj, const Value& v, bool* result);

// obj is a JSObject*, but we root it immediately up front. We do it
// that way because we need a Rooted temporary in this method anyway.
extern bool
IsDelegateOfObject(JSContext* cx, HandleObject protoObj, JSObject* obj, bool* result);

/* Wrap boolean, number or string as Boolean, Number or String object. */
extern JSObject*
PrimitiveToObject(JSContext* cx, const Value& v);

} /* namespace js */

namespace js {

/* For converting stack values to objects. */
MOZ_ALWAYS_INLINE JSObject*
ToObjectFromStack(JSContext* cx, HandleValue vp)
{
    if (vp.isObject())
        return &vp.toObject();
    return js::ToObjectSlow(cx, vp, true);
}

template<XDRMode mode>
bool
XDRObjectLiteral(XDRState<mode>* xdr, MutableHandleObject obj);

extern bool
ReportGetterOnlyAssignment(JSContext* cx, bool strict);

/*
 * Report a TypeError: "so-and-so is not an object".
 * Using NotNullObject is usually less code.
 */
extern void
ReportNotObject(JSContext* cx, const Value& v);

inline JSObject*
NonNullObject(JSContext* cx, const Value& v)
{
    if (v.isObject())
        return &v.toObject();
    ReportNotObject(cx, v);
    return nullptr;
}

extern const char*
InformalValueTypeName(const Value& v);

extern bool
GetFirstArgumentAsObject(JSContext* cx, const CallArgs& args, const char* method,
                         MutableHandleObject objp);

/* Helpers for throwing. These always return false. */
extern bool
Throw(JSContext* cx, jsid id, unsigned errorNumber);

extern bool
Throw(JSContext* cx, JSObject* obj, unsigned errorNumber);

enum class IntegrityLevel {
    Sealed,
    Frozen
};

/*
 * ES6 rev 29 (6 Dec 2014) 7.3.13. Mark obj as non-extensible, and adjust each
 * of obj's own properties' attributes appropriately: each property becomes
 * non-configurable, and if level == Frozen, data properties become
 * non-writable as well.
 */
extern bool
SetIntegrityLevel(JSContext* cx, HandleObject obj, IntegrityLevel level);

inline bool
FreezeObject(JSContext* cx, HandleObject obj)
{
    return SetIntegrityLevel(cx, obj, IntegrityLevel::Frozen);
}

/*
 * ES6 rev 29 (6 Dec 2014) 7.3.14. Code shared by Object.isSealed and
 * Object.isFrozen.
 */
extern bool
TestIntegrityLevel(JSContext* cx, HandleObject obj, IntegrityLevel level, bool* resultp);

}  /* namespace js */

#endif /* jsobj_h */

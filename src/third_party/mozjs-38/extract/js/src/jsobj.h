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
#include "vm/Shape.h"
#include "vm/String.h"
#include "vm/Xdr.h"

namespace JS {
struct ClassInfo;
}

namespace js {

class AutoPropDescVector;
class GCMarker;
struct NativeIterator;
class Nursery;
class ObjectElements;
struct StackShape;

namespace gc {
class RelocationOverlay;
}

inline JSObject*
CastAsObject(PropertyOp op)
{
    return JS_FUNC_TO_DATA_PTR(JSObject*, op);
}

inline JSObject*
CastAsObject(StrictPropertyOp op)
{
    return JS_FUNC_TO_DATA_PTR(JSObject*, op);
}

inline Value
CastAsObjectJsval(PropertyOp op)
{
    return ObjectOrNullValue(CastAsObject(op));
}

inline Value
CastAsObjectJsval(StrictPropertyOp op)
{
    return ObjectOrNullValue(CastAsObject(op));
}

/******************************************************************************/

typedef Vector<PropDesc, 1> PropDescArray;

extern const Class IntlClass;
extern const Class JSONClass;
extern const Class MathClass;

class GlobalObject;
class MapObject;
class NewObjectCache;
class NormalArgumentsObject;
class SetObject;
class StrictArgumentsObject;

// Forward declarations, required for later friend declarations.
bool PreventExtensions(JSContext* cx, JS::HandleObject obj, bool* succeeded);
bool SetImmutablePrototype(js::ExclusiveContext* cx, JS::HandleObject obj, bool* succeeded);

}  /* namespace js */

/*
 * A JavaScript object. The members common to all objects are as follows:
 *
 * - The |shape_| member stores the shape of the object, which includes the
 *   object's class and the layout of all its properties.
 *
 * - The |group_| member stores the group of the object, which contains its
 *   prototype object and the possible types of its properties.
 *
 * Subclasses of JSObject --- mainly NativeObject and JSFunction --- add more
 * members.
 */
class JSObject : public js::gc::Cell
{
  protected:
    js::HeapPtrShape shape_;
    js::HeapPtrObjectGroup group_;

  private:
    friend class js::Shape;
    friend class js::GCMarker;
    friend class js::NewObjectCache;
    friend class js::Nursery;
    friend class js::gc::RelocationOverlay;
    friend bool js::PreventExtensions(JSContext* cx, JS::HandleObject obj, bool* succeeded);
    friend bool js::SetImmutablePrototype(js::ExclusiveContext* cx, JS::HandleObject obj,
                                          bool* succeeded);

    // Make a new group to use for a singleton object.
    static js::ObjectGroup* makeLazyGroup(JSContext* cx, js::HandleObject obj);

  public:
    js::Shape * lastProperty() const {
        MOZ_ASSERT(shape_);
        return shape_;
    }

    bool isNative() const {
        return lastProperty()->isNative();
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
     * Whether this is the only object which has its specified gbroup. This
     * object will have its group constructed lazily as needed by analysis.
     */
    bool isSingleton() const {
        return !!group_->singleton();
    }

    /*
     * Whether the object's group has not been constructed yet. If an object
     * might have a lazy group, use getGroup() below, otherwise group().
     */
    bool hasLazyGroup() const {
        return group_->lazy();
    }

    JSCompartment* compartment() const {
        return lastProperty()->base()->compartment();
    }

    /*
     * Make a non-array object with the specified initial state. This method
     * takes ownership of any extantSlots it is passed.
     */
    static inline JSObject* create(js::ExclusiveContext* cx,
                                   js::gc::AllocKind kind,
                                   js::gc::InitialHeap heap,
                                   js::HandleShape shape,
                                   js::HandleObjectGroup group);

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

    bool setFlags(js::ExclusiveContext* cx, /*BaseShape::Flag*/ uint32_t flags,
                  GenerateShape generateShape = GENERATE_NONE);

    /*
     * An object is a delegate if it is on another object's prototype or scope
     * chain, and therefore the delegate might be asked implicitly to get or
     * set a property on behalf of another object. Delegates may be accessed
     * directly too, as may any object, but only those objects linked after the
     * head of any prototype or scope chain are flagged as delegates. This
     * definition helps to optimize shape-based property cache invalidation
     * (see Purge{Scope,Proto}Chain in jsobj.cpp).
     */
    bool isDelegate() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::DELEGATE);
    }

    bool setDelegate(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::DELEGATE, GENERATE_SHAPE);
    }

    bool isBoundFunction() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::BOUND_FUNCTION);
    }

    inline bool hasSpecialEquality() const;

    bool watched() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::WATCHED);
    }
    bool setWatched(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::WATCHED, GENERATE_SHAPE);
    }

    /* See InterpreterFrame::varObj. */
    inline bool isQualifiedVarObj();
    bool setQualifiedVarObj(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::QUALIFIED_VAROBJ);
    }

    inline bool isUnqualifiedVarObj();
    bool setUnqualifiedVarObj(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::UNQUALIFIED_VAROBJ);
    }

    /*
     * Objects with an uncacheable proto can have their prototype mutated
     * without inducing a shape change on the object. Property cache entries
     * and JIT inline caches should not be filled for lookups across prototype
     * lookups on the object.
     */
    bool hasUncacheableProto() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::UNCACHEABLE_PROTO);
    }
    bool setUncacheableProto(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::UNCACHEABLE_PROTO, GENERATE_SHAPE);
    }

    /*
     * Whether SETLELEM was used to access this object. See also the comment near
     * PropertyTree::MAX_HEIGHT.
     */
    bool hadElementsAccess() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::HAD_ELEMENTS_ACCESS);
    }
    bool setHadElementsAccess(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::HAD_ELEMENTS_ACCESS);
    }

    /*
     * Whether there may be indexed properties on this object, excluding any in
     * the object's elements.
     */
    bool isIndexed() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::INDEXED);
    }

    uint32_t propertyCount() const {
        return lastProperty()->entryCount();
    }

    bool hasShapeTable() const {
        return lastProperty()->hasTable();
    }

    /* GC support. */

    void markChildren(JSTracer* trc);

    void fixupAfterMovingGC();

    static js::ThingRootKind rootKind() { return js::THING_ROOT_OBJECT; }
    static const size_t MaxTagBits = 3;
    static bool isNullLike(const JSObject* obj) { return uintptr_t(obj) < (1 << MaxTagBits); }

    MOZ_ALWAYS_INLINE JS::Zone* zone() const {
        return shape_->zone();
    }
    MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZone() const {
        return JS::shadow::Zone::asShadowZone(zone());
    }
    MOZ_ALWAYS_INLINE JS::Zone* zoneFromAnyThread() const {
        return shape_->zoneFromAnyThread();
    }
    MOZ_ALWAYS_INLINE JS::shadow::Zone* shadowZoneFromAnyThread() const {
        return JS::shadow::Zone::asShadowZone(zoneFromAnyThread());
    }
    static MOZ_ALWAYS_INLINE void readBarrier(JSObject* obj);
    static MOZ_ALWAYS_INLINE void writeBarrierPre(JSObject* obj);
    static MOZ_ALWAYS_INLINE void writeBarrierPost(JSObject* obj, void* cellp);
    static MOZ_ALWAYS_INLINE void writeBarrierPostRelocate(JSObject* obj, void* cellp);
    static MOZ_ALWAYS_INLINE void writeBarrierPostRemove(JSObject* obj, void* cellp);

    size_t tenuredSizeOfThis() const {
        MOZ_ASSERT(isTenured());
        return js::gc::Arena::thingSize(asTenured().getAllocKind());
    }

    void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf, JS::ClassInfo* info);

    bool hasIdempotentProtoChain() const;

    /*
     * Marks this object as having a singleton type, and leave the group lazy.
     * Constructs a new, unique shape for the object.
     */
    static inline bool setSingleton(js::ExclusiveContext* cx, js::HandleObject obj);

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
     * 1. obj->getProto() returns the prototype, but asserts if obj is a proxy.
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
        MOZ_ASSERT(!uninlinedIsProxy());
        return getTaggedProto().toObjectOrNull();
    }

    // Normal objects and a subset of proxies have uninteresting [[Prototype]].
    // For such objects the [[Prototype]] is just a value returned when needed
    // for accesses, or modified in response to requests.  These objects store
    // the [[Prototype]] directly within |obj->type_|.
    //
    // Proxies that don't have such a simple [[Prototype]] instead have a
    // "lazy" [[Prototype]].  Accessing the [[Prototype]] of such an object
    // requires going through the proxy handler {get,set}PrototypeOf and
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
    bool nonLazyPrototypeIsImmutable() const {
        MOZ_ASSERT(!hasLazyPrototype());
        return lastProperty()->hasObjectFlag(js::BaseShape::IMMUTABLE_PROTOTYPE);
    }

    inline void setGroup(js::ObjectGroup* group);

    /*
     * Mark an object that has been iterated over and is a singleton. We need
     * to recover this information in the object's type information after it
     * is purged on GC.
     */
    bool isIteratedSingleton() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::ITERATED_SINGLETON);
    }
    bool setIteratedSingleton(js::ExclusiveContext* cx) {
        return setFlags(cx, js::BaseShape::ITERATED_SINGLETON);
    }

    /*
     * Mark an object as requiring its default 'new' type to have unknown
     * properties.
     */
    bool isNewGroupUnknown() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::NEW_GROUP_UNKNOWN);
    }
    static bool setNewGroupUnknown(JSContext* cx, const js::Class* clasp, JS::HandleObject obj);

    // Mark an object as having its 'new' script information cleared.
    bool wasNewScriptCleared() const {
        return lastProperty()->hasObjectFlag(js::BaseShape::NEW_SCRIPT_CLEARED);
    }
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
     * Parents and scope chains.
     *
     * All script-accessible objects with a nullptr parent are global objects,
     * and all global objects have a nullptr parent. Some builtin objects
     * which are not script-accessible also have a nullptr parent, such as
     * parser created functions for non-compileAndGo scripts.
     *
     * Except for the non-script-accessible builtins, the global with which an
     * object is associated can be reached by following parent links to that
     * global (see global()).
     *
     * The scope chain of an object is the link in the search path when a
     * script does a name lookup on a scope object. For JS internal scope
     * objects --- Call, DeclEnv and Block --- the chain is stored in
     * the first fixed slot of the object, and the object's parent is the
     * associated global. For other scope objects, the chain is stored in the
     * object's parent.
     *
     * In compileAndGo code, scope chains can contain only internal scope
     * objects with a global object at the root as the scope of the outermost
     * non-function script. In non-compileAndGo code, the scope of the
     * outermost non-function script might not be a global object, and can have
     * a mix of other objects above it before the global object is reached.
     */

    /* Access the parent link of an object. */
    JSObject* getParent() const {
        return lastProperty()->getObjectParent();
    }
    static bool setParent(JSContext* cx, js::HandleObject obj, js::HandleObject newParent);

    /*
     * Get the enclosing scope of an object. When called on non-scope object,
     * this will just be the global (the name "enclosing scope" still applies
     * in this situation because non-scope objects can be on the scope chain).
     */
    inline JSObject* enclosingScope();

    /* Access the metadata on an object. */
    inline JSObject* getMetadata() const {
        return lastProperty()->getObjectMetadata();
    }
    static bool setMetadata(JSContext* cx, js::HandleObject obj, js::HandleObject newMetadata);

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
    bool nonProxyIsExtensible() const {
        MOZ_ASSERT(!uninlinedIsProxy());

        // [[Extensible]] for ordinary non-proxy objects is an object flag.
        return !lastProperty()->hasObjectFlag(js::BaseShape::NOT_EXTENSIBLE);
    }

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

    static bool nonNativeSetProperty(JSContext* cx, js::HandleObject obj,
                                     js::HandleObject receiver, js::HandleId id,
                                     js::MutableHandleValue vp, bool strict);
    static bool nonNativeSetElement(JSContext* cx, js::HandleObject obj,
                                    js::HandleObject receiver, uint32_t index,
                                    js::MutableHandleValue vp, bool strict);

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
     * js::ObjectClassIs).
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

    static size_t offsetOfShape() { return offsetof(JSObject, shape_); }
    static size_t offsetOfGroup() { return offsetof(JSObject, group_); }

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
struct JSObject_Slots0 : JSObject { void* data[2]; };
struct JSObject_Slots2 : JSObject { void* data[2]; js::Value fslots[2]; };
struct JSObject_Slots4 : JSObject { void* data[2]; js::Value fslots[4]; };
struct JSObject_Slots8 : JSObject { void* data[2]; js::Value fslots[8]; };
struct JSObject_Slots12 : JSObject { void* data[2]; js::Value fslots[12]; };
struct JSObject_Slots16 : JSObject { void* data[2]; js::Value fslots[16]; };

/* static */ MOZ_ALWAYS_INLINE void
JSObject::readBarrier(JSObject* obj)
{
    if (!isNullLike(obj) && obj->isTenured())
        obj->asTenured().readBarrier(&obj->asTenured());
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPre(JSObject* obj)
{
    if (!isNullLike(obj) && obj->isTenured())
        obj->asTenured().writeBarrierPre(&obj->asTenured());
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPost(JSObject* obj, void* cellp)
{
    MOZ_ASSERT(cellp);
    if (IsNullTaggedPointer(obj))
        return;
    MOZ_ASSERT(obj == *static_cast<JSObject**>(cellp));
    js::gc::StoreBuffer* storeBuffer = obj->storeBuffer();
    if (storeBuffer)
        storeBuffer->putCellFromAnyThread(static_cast<js::gc::Cell**>(cellp));
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPostRelocate(JSObject* obj, void* cellp)
{
    MOZ_ASSERT(cellp);
    MOZ_ASSERT(obj);
    MOZ_ASSERT(obj == *static_cast<JSObject**>(cellp));
    js::gc::StoreBuffer* storeBuffer = obj->storeBuffer();
    if (storeBuffer)
        storeBuffer->putRelocatableCellFromAnyThread(static_cast<js::gc::Cell**>(cellp));
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPostRemove(JSObject* obj, void* cellp)
{
    MOZ_ASSERT(cellp);
    MOZ_ASSERT(obj);
    MOZ_ASSERT(obj == *static_cast<JSObject**>(cellp));
    obj->shadowRuntimeFromAnyThread()->gcStoreBufferPtr()->removeRelocatableCellFromAnyThread(
        static_cast<js::gc::Cell**>(cellp));
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
    const jsval* array;
    size_t length;

    JSValueArray(const jsval* v, size_t c) : array(v), length(c) {}
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
 * The functions below are the fundamental operations on objects.
 *
 * ES6 specifies 14 internal methods that define how objects behave.  The spec
 * is actually quite good on this topic, though you may have to read it a few
 * times. See ES6 draft rev 29 (6 Dec 2014) 6.1.7.2 and 6.1.7.3.
 *
 * When 'obj' is an ordinary object, these functions have boring standard
 * behavior as specified by ES6 draft rev 29 section 9.1; see the section about
 * internal methods in vm/NativeObject.h.
 *
 * Proxies override the behavior of internal methods. So when 'obj' is a proxy,
 * any one of the functions below could do just about anything. See js/Proxy.h.
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
 * Returns false on error, success of operation in outparam. For example, if
 * obj is not extensible, its prototype is fixed. js::SetPrototype will return
 * true, because no exception is thrown for this; but *succeeded will be false.
 */
extern bool
SetPrototype(JSContext* cx, HandleObject obj, HandleObject proto, bool* succeeded);

/*
 * ES6 [[IsExtensible]]. Extensible objects can have new properties defined on
 * them. Inextensible objects can't, and their [[Prototype]] slot is fixed as
 * well.
 */
inline bool
IsExtensible(ExclusiveContext* cx, HandleObject obj, bool* extensible);

/*
 * ES6 [[PreventExtensions]]. Attempt to change the [[Extensible]] bit on |obj|
 * to false.  Indicate success or failure through the |*succeeded| outparam, or
 * actual error through the return value.
 */
extern bool
PreventExtensions(JSContext* cx, HandleObject obj, bool* succeeded);

/*
 * ES6 [[GetOwnPropertyDescriptor]]. Get a description of one of obj's own
 * properties.
 *
 * If no such property exists on obj, return true with desc.object() set to
 * null.
 */
extern bool
GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                         MutableHandle<PropertyDescriptor> desc);

/*
 * ES6 [[DefineOwnProperty]]. Define a property on obj.
 *
 * If obj is an array, this follows ES5 15.4.5.1.
 * If obj is any other native object, this follows ES5 8.12.9.
 * If obj is a proxy, this calls the proxy handler's defineProperty method.
 * Otherwise, this reports an error and returns false.
 *
 * Both StandardDefineProperty functions hew close to the ES5 spec. Note that
 * the DefineProperty functions do not enforce some invariants mandated by ES6.
 */
extern bool
StandardDefineProperty(JSContext* cx, HandleObject obj, HandleId id,
                       const PropDesc& desc, bool throwError, bool* rval);

extern bool
StandardDefineProperty(JSContext* cx, HandleObject obj, HandleId id,
                       Handle<PropertyDescriptor> descriptor, bool* bp);

extern bool
DefineProperty(ExclusiveContext* cx, HandleObject obj, HandleId id, HandleValue value,
               JSPropertyOp getter = nullptr,
               JSStrictPropertyOp setter = nullptr,
               unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineProperty(ExclusiveContext* cx, HandleObject obj, PropertyName* name, HandleValue value,
               JSPropertyOp getter = nullptr,
               JSStrictPropertyOp setter = nullptr,
               unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineElement(ExclusiveContext* cx, HandleObject obj, uint32_t index, HandleValue value,
              JSPropertyOp getter = nullptr,
              JSStrictPropertyOp setter = nullptr,
              unsigned attrs = JSPROP_ENUMERATE);

/*
 * ES6 [[HasProperty]]. Set *foundp to true if `id in obj` (that is, if obj has
 * an own or inherited property obj[id]), false otherwise.
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
GetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, HandleId id,
            MutableHandleValue vp);

inline bool
GetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, PropertyName* name,
            MutableHandleValue vp)
{
    RootedId id(cx, NameToId(name));
    return GetProperty(cx, obj, receiver, id, vp);
}

inline bool
GetElement(JSContext* cx, HandleObject obj, HandleObject receiver, uint32_t index,
           MutableHandleValue vp);

inline bool
GetPropertyNoGC(JSContext* cx, JSObject* obj, JSObject* receiver, jsid id, Value* vp);

inline bool
GetPropertyNoGC(JSContext* cx, JSObject* obj, JSObject* receiver, PropertyName* name, Value* vp)
{
    return GetPropertyNoGC(cx, obj, receiver, NameToId(name), vp);
}

inline bool
GetElementNoGC(JSContext* cx, JSObject* obj, JSObject* receiver, uint32_t index, Value* vp);

/*
 * ES6 [[Set]]. Carry out the assignment `obj[id] = vp`.
 *
 * The `receiver` argument has to do with how [[Set]] interacts with the
 * prototype chain and proxies. It's hard to explain and ES6 doesn't really
 * try. Long story short, if you just want bog-standard assignment, pass the
 * same object as both obj and receiver.
 *
 * When obj != receiver, it's a reasonable guess that a proxy is involved, obj
 * is the proxy's target, and the proxy is using SetProperty to finish an
 * assignment that started out as `receiver[id] = vp`, by delegating it to obj.
 *
 * Strict errors: ES6 specifies that this method returns a boolean value
 * indicating whether assignment "succeeded". We currently take a `strict`
 * argument instead, but this has to change. See bug 1113369.
 */
inline bool
SetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, HandleId id,
            MutableHandleValue vp, bool strict);

inline bool
SetProperty(JSContext* cx, HandleObject obj, HandleObject receiver, PropertyName* name,
            MutableHandleValue vp, bool strict)
{
    RootedId id(cx, NameToId(name));
    return SetProperty(cx, obj, receiver, id, vp, strict);
}

inline bool
SetElement(JSContext* cx, HandleObject obj, HandleObject receiver, uint32_t index,
           MutableHandleValue vp, bool strict);

/*
 * ES6 [[Delete]]. Equivalent to the JS code `delete obj[id]`.
 */
inline bool
DeleteProperty(JSContext* cx, js::HandleObject obj, js::HandleId id, bool* succeeded);

inline bool
DeleteElement(JSContext* cx, js::HandleObject obj, uint32_t index, bool* succeeded);


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

/*
 * ToPrimitive support, currently implemented like an internal method (JSClass::convert).
 * In ES6 this is just a method, @@toPrimitive. See bug 1054756.
 */
extern bool
ToPrimitive(JSContext* cx, HandleObject obj, JSType hint, MutableHandleValue vp);

MOZ_ALWAYS_INLINE bool
ToPrimitive(JSContext* cx, MutableHandleValue vp);

MOZ_ALWAYS_INLINE bool
ToPrimitive(JSContext* cx, JSType preferredType, MutableHandleValue vp);

/*
 * toString support. (This isn't called GetClassName because there's a macro in
 * <windows.h> with that name.)
 */
extern const char*
GetObjectClassName(JSContext* cx, HandleObject obj);

/*
 * Inner and outer objects
 *
 * GetInnerObject and GetOuterObject (and also GetThisObject, somewhat) have to
 * do with Windows and WindowProxies. There's a screwy invariant that actual
 * Window objects (the global objects of web pages) are never directly exposed
 * to script. Instead we often substitute a WindowProxy.
 *
 * As a result, we have calls to these three "substitute-this-object-for-that-
 * object" functions sprinkled at apparently arbitrary (but actually *very*
 * carefully and nervously selected) places throughout the engine and indeed
 * the universe.
 */

/*
 * If obj a WindowProxy, return its current inner Window. Otherwise return obj.
 *
 * GetInnerObject is called when we need a scope chain; you never want a
 * WindowProxy on a scope chain.
 *
 * It's also called in a few places where an object comes in from script, and
 * the user probably intends to operate on the Window, not the
 * WindowProxy. Object.prototype.watch and various Debugger features do
 * this. (Users can't simply pass the Window, because the Window isn't exposed
 * to scripts.)
 */
inline JSObject*
GetInnerObject(JSObject* obj)
{
    if (InnerObjectOp op = obj->getClass()->ext.innerObject) {
        JS::AutoSuppressGCAnalysis nogc;
        return op(obj);
    }
    return obj;
}

/*
 * If obj is a Window object, return the WindowProxy. Otherwise return obj.
 *
 * This must be called before passing an object to script, if the object might
 * be a Window. (But usually those cases involve scope objects, and for those,
 * it is better to call GetThisObject instead.)
 */
inline JSObject*
GetOuterObject(JSContext* cx, HandleObject obj)
{
    if (ObjectOp op = obj->getClass()->ext.outerObject)
        return op(cx, obj);
    return obj;
}

/*
 * Return an object that may be used as `this` in place of obj. For most
 * objects this just returns obj.
 *
 * Some JSObjects shouldn't be exposed directly to script. This includes (at
 * least) DynamicWithObjects and Window objects. However, since both of those
 * can be on scope chains, we sometimes would expose those as `this` if we
 * were not so vigilant about calling GetThisObject where appropriate.
 *
 * See comments at ComputeImplicitThis.
 */
inline JSObject*
GetThisObject(JSContext* cx, HandleObject obj)
{
    if (ObjectOp op = obj->getOps()->thisObject)
        return op(cx, obj);
    return obj;
}


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

/*
 * The NewObjectKind allows an allocation site to specify the type properties
 * and lifetime requirements that must be fixed at allocation time.
 */
enum NewObjectKind {
    /* This is the default. Most objects are generic. */
    GenericObject,

    /*
     * Singleton objects are treated specially by the type system. This flag
     * ensures that the new object is automatically set up correctly as a
     * singleton and is allocated in the correct heap.
     */
    SingletonObject,

    /*
     * Objects which may be marked as a singleton after allocation must still
     * be allocated on the correct heap, but are not automatically setup as a
     * singleton after allocation.
     */
    MaybeSingletonObject,

    /*
     * Objects which will not benefit from being allocated in the nursery
     * (e.g. because they are known to have a long lifetime) may be allocated
     * with this kind to place them immediately into the tenured generation.
     */
    TenuredObject
};

inline gc::InitialHeap
GetInitialHeap(NewObjectKind newKind, const Class* clasp)
{
    if (newKind != GenericObject)
        return gc::TenuredHeap;
    if (clasp->finalize && !(clasp->flags & JSCLASS_FINALIZE_FROM_NURSERY))
        return gc::TenuredHeap;
    return gc::DefaultHeap;
}

// Specialized call for constructing |this| with a known function callee,
// and a known prototype.
extern JSObject*
CreateThisForFunctionWithProto(JSContext* cx, js::HandleObject callee, HandleObject proto,
                               NewObjectKind newKind = GenericObject);

// Specialized call for constructing |this| with a known function callee.
extern JSObject*
CreateThisForFunction(JSContext* cx, js::HandleObject callee, NewObjectKind newKind);

// Generic call for constructing |this|.
extern JSObject*
CreateThis(JSContext* cx, const js::Class* clasp, js::HandleObject callee);

extern JSObject*
CloneObject(JSContext* cx, HandleObject obj, Handle<js::TaggedProto> proto, HandleObject parent);

extern NativeObject*
DeepCloneObjectLiteral(JSContext* cx, HandleNativeObject obj, NewObjectKind newKind = GenericObject);

extern bool
DefineProperties(JSContext* cx, HandleObject obj, HandleObject props);

/*
 * Read property descriptors from props, as for Object.defineProperties. See
 * ES5 15.2.3.7 steps 3-5.
 */
extern bool
ReadPropertyDescriptors(JSContext* cx, HandleObject props, bool checkAccessors,
                        AutoIdVector* ids, AutoPropDescVector* descs);

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

}

extern JSObject*
js_FindVariableScope(JSContext* cx, JSFunction** funp);


namespace js {

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

bool
NewPropertyDescriptorObject(JSContext* cx, Handle<PropertyDescriptor> desc, MutableHandleValue vp);

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
XDRObjectLiteral(XDRState<mode>* xdr, MutableHandleNativeObject obj);

extern JSObject*
CloneObjectLiteral(JSContext* cx, HandleObject parent, HandleObject srcObj);

} /* namespace js */

extern void
js_GetObjectSlotName(JSTracer* trc, char* buf, size_t bufsize);

extern bool
js_ReportGetterOnlyAssignment(JSContext* cx, bool strict);


namespace js {

extern JSObject*
NonNullObject(JSContext* cx, const Value& v);

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

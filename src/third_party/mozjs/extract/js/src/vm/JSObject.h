/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSObject_h
#define vm_JSObject_h

#include "mozilla/MemoryReporting.h"

#include "gc/Barrier.h"
#include "js/Conversions.h"
#include "js/GCVector.h"
#include "js/HeapAPI.h"
#include "vm/Printer.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/Xdr.h"

namespace JS {
struct ClassInfo;
} // namespace JS

namespace js {

using PropertyDescriptorVector = JS::GCVector<JS::PropertyDescriptor>;
class GCMarker;
class Nursery;

namespace gc {
class RelocationOverlay;
} // namespace gc

/******************************************************************************/

extern const Class IntlClass;
extern const Class JSONClass;
extern const Class MathClass;

class GlobalObject;
class NewObjectCache;

enum class IntegrityLevel {
    Sealed,
    Frozen
};

// Forward declarations, required for later friend declarations.
bool PreventExtensions(JSContext* cx, JS::HandleObject obj, JS::ObjectOpResult& result, IntegrityLevel level = IntegrityLevel::Sealed);
bool SetImmutablePrototype(JSContext* cx, JS::HandleObject obj, bool* succeeded);

}  /* namespace js */

/*
 * A JavaScript object.
 *
 * This is the base class for all objects exposed to JS script (as well as some
 * objects that are only accessed indirectly). Subclasses add additional fields
 * and execution semantics. The runtime class of an arbitrary JSObject is
 * identified by JSObject::getClass().
 *
 * The members common to all objects are as follows:
 *
 * - The |group_| member stores the group of the object, which contains its
 *   prototype object, its class and the possible types of its properties.
 *
 * - The |shapeOrExpando_| member points to (an optional) guard object that JIT
 *   may use to optimize. The pointed-to object dictates the constraints
 *   imposed on the JSObject:
 *      nullptr
 *          - Safe value if this field is not needed.
 *      js::Shape
 *          - All objects that might point |shapeOrExpando_| to a js::Shape
 *            must follow the rules specified on js::ShapedObject.
 *      JSObject
 *          - Implies nothing about the current object or target object. Either
 *            of which may mutate in place. Store a JSObject* only to save
 *            space, not to guard on.
 *
 * NOTE: The JIT may check |shapeOrExpando_| pointer value without ever
 *       inspecting |group_| or the class.
 *
 * NOTE: Some operations can change the contents of an object (including class)
 *       in-place so avoid assuming an object with same pointer has same class
 *       as before.
 *       - JSObject::swap()
 *       - UnboxedPlainObject::convertToNative()
 *
 * NOTE: UnboxedObjects may change class without changing |group_|.
 *       - js::TryConvertToUnboxedLayout
 */
class JSObject : public js::gc::Cell
{
  protected:
    js::GCPtrObjectGroup group_;
    void* shapeOrExpando_;

  private:
    friend class js::Shape;
    friend class js::GCMarker;
    friend class js::NewObjectCache;
    friend class js::Nursery;
    friend class js::gc::RelocationOverlay;
    friend bool js::PreventExtensions(JSContext* cx, JS::HandleObject obj, JS::ObjectOpResult& result, js::IntegrityLevel level);
    friend bool js::SetImmutablePrototype(JSContext* cx, JS::HandleObject obj,
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

    js::LookupPropertyOp getOpsLookupProperty() const { return getClass()->getOpsLookupProperty(); }
    js::DefinePropertyOp getOpsDefineProperty() const { return getClass()->getOpsDefineProperty(); }
    js::HasPropertyOp    getOpsHasProperty()    const { return getClass()->getOpsHasProperty(); }
    js::GetPropertyOp    getOpsGetProperty()    const { return getClass()->getOpsGetProperty(); }
    js::SetPropertyOp    getOpsSetProperty()    const { return getClass()->getOpsSetProperty(); }
    js::GetOwnPropertyOp getOpsGetOwnPropertyDescriptor()
                                                const { return getClass()->getOpsGetOwnPropertyDescriptor(); }
    js::DeletePropertyOp getOpsDeleteProperty() const { return getClass()->getOpsDeleteProperty(); }
    js::GetElementsOp    getOpsGetElements()    const { return getClass()->getOpsGetElements(); }
    JSFunToStringOp      getOpsFunToString()    const { return getClass()->getOpsFunToString(); }

    js::ObjectGroup* group() const {
        MOZ_ASSERT(!hasLazyGroup());
        return groupRaw();
    }

    js::ObjectGroup* groupRaw() const {
        return group_;
    }

    void initGroup(js::ObjectGroup* group) {
        group_.init(group);
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
    inline js::Shape* ensureShape(JSContext* cx);

    enum GenerateShape {
        GENERATE_NONE,
        GENERATE_SHAPE
    };

    static bool setFlags(JSContext* cx, JS::HandleObject obj, js::BaseShape::Flag flags,
                         GenerateShape generateShape = GENERATE_NONE);
    inline bool hasAllFlags(js::BaseShape::Flag flags) const;

    // An object is a delegate if it is on another object's prototype or
    // environment chain. Optimization heuristics will make use of this flag.
    // See: ReshapeForProtoMutation, ReshapeForShadowedProp
    inline bool isDelegate() const;
    static bool setDelegate(JSContext* cx, JS::HandleObject obj) {
        return setFlags(cx, obj, js::BaseShape::DELEGATE, GENERATE_SHAPE);
    }

    inline bool isBoundFunction() const;
    inline bool hasSpecialEquality() const;

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
    static bool setQualifiedVarObj(JSContext* cx, JS::HandleObject obj) {
        return setFlags(cx, obj, js::BaseShape::QUALIFIED_VAROBJ);
    }

    // An "unqualified" varobj is the object on which "unqualified"
    // assignments (i.e., bareword assignments for which the LHS does not
    // exist on the scope chain) are kept.
    inline bool isUnqualifiedVarObj() const;

    // Objects with an uncacheable proto can have their prototype mutated
    // without inducing a shape change on the object. JIT inline caches should
    // do an explicit group guard to guard against this. Singletons always
    // generate a new shape when their prototype changes, regardless of this
    // hasUncacheableProto flag.
    inline bool hasUncacheableProto() const;
    static bool setUncacheableProto(JSContext* cx, JS::HandleObject obj) {
        MOZ_ASSERT(obj->hasStaticPrototype(),
                   "uncacheability as a concept is only applicable to static "
                   "(not dynamically-computed) prototypes");
        return setFlags(cx, obj, js::BaseShape::UNCACHEABLE_PROTO, GENERATE_SHAPE);
    }

    /*
     * Whether there may be "interesting symbol" properties on this object. An
     * interesting symbol is a symbol for which symbol->isInterestingSymbol()
     * returns true.
     */
    MOZ_ALWAYS_INLINE bool maybeHasInterestingSymbolProperty() const;

    /*
     * If this object was instantiated with `new Ctor`, return the constructor's
     * display atom. Otherwise, return nullptr.
     */
    static bool constructorDisplayAtom(JSContext* cx, js::HandleObject obj,
                                       js::MutableHandleAtom name);

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

    static const JS::TraceKind TraceKind = JS::TraceKind::Object;
    static const size_t MaxTagBits = 3;

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
    static inline bool setSingleton(JSContext* cx, js::HandleObject obj);

    // Change an existing object to have a singleton group.
    static bool changeToSingleton(JSContext* cx, js::HandleObject obj);

    static inline js::ObjectGroup* getGroup(JSContext* cx, js::HandleObject obj);

    const js::GCPtrObjectGroup& groupFromGC() const {
        /* Direct field access for use by GC. */
        return group_;
    }

#ifdef DEBUG
    static void debugCheckNewObject(js::ObjectGroup* group, js::Shape* shape,
                                    js::gc::AllocKind allocKind, js::gc::InitialHeap heap);
#else
    static void debugCheckNewObject(js::ObjectGroup* group, js::Shape* shape,
                                    js::gc::AllocKind allocKind, js::gc::InitialHeap heap)
    {}
#endif

    /*
     * We permit proxies to dynamically compute their prototype if desired.
     * (Not all proxies will so desire: in particular, most DOM proxies can
     * track their prototype with a single, nullable JSObject*.)  If a proxy
     * so desires, we store (JSObject*)0x1 in the proto field of the object's
     * group.
     *
     * We offer three ways to get an object's prototype:
     *
     * 1. obj->staticPrototype() returns the prototype, but it asserts if obj
     *    is a proxy, and the proxy has opted to dynamically compute its
     *    prototype using a getPrototype() handler.
     * 2. obj->taggedProto() returns a TaggedProto, which can be tested to
     *    check if the proto is an object, nullptr, or lazily computed.
     * 3. js::GetPrototype(cx, obj, &proto) computes the proto of an object.
     *    If obj is a proxy with dynamically-computed prototype, this code may
     *    perform arbitrary behavior (allocation, GC, run JS) while computing
     *    the proto.
     */

    js::TaggedProto taggedProto() const {
        return group_->proto();
    }

    bool hasTenuredProto() const;

    bool uninlinedIsProxy() const;

    JSObject* staticPrototype() const {
        MOZ_ASSERT(hasStaticPrototype());
        return taggedProto().toObjectOrNull();
    }

    // Normal objects and a subset of proxies have an uninteresting, static
    // (albeit perhaps mutable) [[Prototype]].  For such objects the
    // [[Prototype]] is just a value returned when needed for accesses, or
    // modified in response to requests.  These objects store the
    // [[Prototype]] directly within |obj->group_|.
    bool hasStaticPrototype() const {
        return !hasDynamicPrototype();
    }

    // The remaining proxies have a [[Prototype]] requiring dynamic computation
    // for every access, going through the proxy handler {get,set}Prototype and
    // setImmutablePrototype methods.  (Wrappers particularly use this to keep
    // the wrapper/wrappee [[Prototype]]s consistent.)
    bool hasDynamicPrototype() const {
        bool dynamic = taggedProto().isDynamic();
        MOZ_ASSERT_IF(dynamic, uninlinedIsProxy());
        MOZ_ASSERT_IF(dynamic, !isNative());
        return dynamic;
    }

    // True iff this object's [[Prototype]] is immutable.  Must be called only
    // on objects with a static [[Prototype]]!
    inline bool staticPrototypeIsImmutable() const;

    inline void setGroup(js::ObjectGroup* group);

    /*
     * Mark an object that has been iterated over and is a singleton. We need
     * to recover this information in the object's type information after it
     * is purged on GC.
     */
    inline bool isIteratedSingleton() const;
    static bool setIteratedSingleton(JSContext* cx, JS::HandleObject obj) {
        return setFlags(cx, obj, js::BaseShape::ITERATED_SINGLETON);
    }

    /*
     * Mark an object as requiring its default 'new' type to have unknown
     * properties.
     */
    inline bool isNewGroupUnknown() const;
    static bool setNewGroupUnknown(JSContext* cx, const js::Class* clasp, JS::HandleObject obj);

    /* Set a new prototype for an object with a singleton type. */
    static bool splicePrototype(JSContext* cx, js::HandleObject obj, const js::Class* clasp,
                                js::Handle<js::TaggedProto> proto);

    /*
     * For bootstrapping, whether to splice a prototype for Function.prototype
     * or the global object.
     */
    bool shouldSplicePrototype();

    /*
     * Environment chains.
     *
     * The environment chain of an object is the link in the search path when
     * a script does a name lookup on an environment object. For JS internal
     * environment objects --- Call, LexicalEnvironment, and WithEnvironment
     * --- the chain is stored in the first fixed slot of the object.  For
     * other environment objects, the chain goes directly to the global.
     *
     * In code which is not marked hasNonSyntacticScope, environment chains
     * can contain only syntactic environment objects (see
     * IsSyntacticEnvironment) with a global object at the root as the
     * environment of the outermost non-function script. In
     * hasNonSyntacticScope code, the environment of the outermost
     * non-function script might not be a global object, and can have a mix of
     * other objects above it before the global object is reached.
     */

    /*
     * Get the enclosing environment of an object. When called on a
     * non-EnvironmentObject, this will just be the global (the name
     * "enclosing environment" still applies in this situation because
     * non-EnvironmentObjects can be on the environment chain).
     */
    inline JSObject* enclosingEnvironment() const;

    inline js::GlobalObject& global() const;

    // In some rare cases the global object's compartment's global may not be
    // the same global object. For this reason, we need to take extra care when
    // tracing.
    //
    // These cases are:
    //  1) The off-thread parsing task uses a dummy global since it cannot
    //     share with the actual global being used concurrently on the active
    //     thread.
    //  2) A GC may occur when creating the GlobalObject, in which case the
    //     compartment global pointer may not yet be set. In this case there is
    //     nothing interesting to trace in the compartment.
    inline bool isOwnGlobal(JSTracer*) const;
    inline js::GlobalObject* globalForTracing(JSTracer*) const;

    /*
     * ES5 meta-object properties and operations.
     */

  public:
    // Indicates whether a non-proxy is extensible.  Don't call on proxies!
    // This method really shouldn't exist -- but there are a few internal
    // places that want it (JITs and the like), and it'd be a pain to mark them
    // all as friends.
    inline bool nonProxyIsExtensible() const;
    bool uninlinedNonProxyIsExtensible() const;

  public:
    /*
     * Iterator-specific getters and setters.
     */

    static const uint32_t ITER_CLASS_NFIXED_SLOTS = 1;

    /*
     * Back to generic stuff.
     */
    MOZ_ALWAYS_INLINE bool isCallable() const;
    MOZ_ALWAYS_INLINE bool isConstructor() const;
    MOZ_ALWAYS_INLINE JSNative callHook() const;
    MOZ_ALWAYS_INLINE JSNative constructHook() const;

    MOZ_ALWAYS_INLINE void finalize(js::FreeOp* fop);

  public:
    static bool reportReadOnly(JSContext* cx, jsid id, unsigned report = JSREPORT_ERROR);
    static bool reportNotConfigurable(JSContext* cx, jsid id, unsigned report = JSREPORT_ERROR);
    static bool reportNotExtensible(JSContext* cx, js::HandleObject obj,
                                    unsigned report = JSREPORT_ERROR);

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
     * is<NestedScopeObject> and is<ScopeObject>. Each of these has a
     * respective class that derives and adds operations.
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
    void dump(js::GenericPrinter& fp) const;
    void dump() const;
#endif

    // Maximum size in bytes of a JSObject.
    static const size_t MAX_BYTE_SIZE = 4 * sizeof(void*) + 16 * sizeof(JS::Value);

  protected:
    // JIT Accessors.
    //
    // To help avoid writing Spectre-unsafe code, we only allow MacroAssembler
    // to call the method below.
    friend class js::jit::MacroAssembler;

    static constexpr size_t offsetOfGroup() {
        return offsetof(JSObject, group_);
    }
    static constexpr size_t offsetOfShapeOrExpando() {
        return offsetof(JSObject, shapeOrExpando_);
    }

  private:
    JSObject() = delete;
    JSObject(const JSObject& other) = delete;
    void operator=(const JSObject& other) = delete;
};

template <typename Wrapper>
template <typename U>
MOZ_ALWAYS_INLINE JS::Handle<U*>
js::RootedBase<JSObject*, Wrapper>::as() const
{
    const Wrapper& self = *static_cast<const Wrapper*>(this);
    MOZ_ASSERT(self->template is<U>());
    return Handle<U*>::fromMarkedLocation(reinterpret_cast<U* const*>(self.address()));
}

template <typename Wrapper>
template <class U>
MOZ_ALWAYS_INLINE JS::Handle<U*>
js::HandleBase<JSObject*, Wrapper>::as() const
{
    const JS::Handle<JSObject*>& self = *static_cast<const JS::Handle<JSObject*>*>(this);
    MOZ_ASSERT(self->template is<U>());
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
    if (obj && obj->isTenured())
        obj->asTenured().readBarrier(&obj->asTenured());
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPre(JSObject* obj)
{
    if (obj && obj->isTenured())
        obj->asTenured().writeBarrierPre(&obj->asTenured());
}

/* static */ MOZ_ALWAYS_INLINE void
JSObject::writeBarrierPost(void* cellp, JSObject* prev, JSObject* next)
{
    MOZ_ASSERT(cellp);

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

    // Remove the prev entry if the new value does not need it. There will only
    // be a prev entry if the prev value was in the nursery.
    if (prev && (buffer = prev->storeBuffer()))
        buffer->unputCell(static_cast<js::gc::Cell**>(cellp));
}

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
IsExtensible(JSContext* cx, HandleObject obj, bool* extensible);

/*
 * ES6 [[PreventExtensions]]. Attempt to change the [[Extensible]] bit on |obj|
 * to false.  Indicate success or failure through the |result| outparam, or
 * actual error through the return value.
 *
 * The `level` argument is SM-specific. `obj` should have an integrity level of
 * at least `level`.
 */
extern bool
PreventExtensions(JSContext* cx, HandleObject obj, ObjectOpResult& result, IntegrityLevel level);

/* Convenience function. As above, but throw on failure. */
extern bool
PreventExtensions(JSContext* cx, HandleObject obj, IntegrityLevel level = IntegrityLevel::Sealed);

/*
 * ES6 [[GetOwnProperty]]. Get a description of one of obj's own properties.
 *
 * If no such property exists on obj, return true with desc.object() set to
 * null.
 */
extern bool
GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                         MutableHandle<JS::PropertyDescriptor> desc);

/* ES6 [[DefineOwnProperty]]. Define a property on obj. */
extern bool
DefineProperty(JSContext* cx, HandleObject obj, HandleId id,
               Handle<JS::PropertyDescriptor> desc, ObjectOpResult& result);

extern bool
DefineAccessorProperty(JSContext* cx, HandleObject obj, HandleId id,
                       JSGetterOp getter, JSSetterOp setter, unsigned attrs,
                       ObjectOpResult& result);

extern bool
DefineDataProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue value,
                   unsigned attrs, ObjectOpResult& result);

extern bool
DefineAccessorProperty(JSContext* cx, HandleObject obj, PropertyName* name,
                       JSGetterOp getter, JSSetterOp setter, unsigned attrs,
                       ObjectOpResult& result);

extern bool
DefineDataProperty(JSContext* cx, HandleObject obj, PropertyName* name, HandleValue value,
                   unsigned attrs, ObjectOpResult& result);

extern bool
DefineAccessorElement(JSContext* cx, HandleObject obj, uint32_t index,
                      JSGetterOp getter, JSSetterOp setter, unsigned attrs,
                      ObjectOpResult& result);

extern bool
DefineDataElement(JSContext* cx, HandleObject obj, uint32_t index, HandleValue value,
                  unsigned attrs, ObjectOpResult& result);

/*
 * When the 'result' out-param is omitted, the behavior is the same as above, except
 * that any failure results in a TypeError.
 */
extern bool
DefineProperty(JSContext* cx, HandleObject obj, HandleId id, Handle<JS::PropertyDescriptor> desc);

extern bool
DefineAccessorProperty(JSContext* cx, HandleObject obj, HandleId id,
                       JSGetterOp getter, JSSetterOp setter, unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineDataProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue value,
                   unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineAccessorProperty(JSContext* cx, HandleObject obj, PropertyName* name,
                       JSGetterOp getter, JSSetterOp setter, unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineDataProperty(JSContext* cx, HandleObject obj, PropertyName* name, HandleValue value,
                   unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineAccessorElement(JSContext* cx, HandleObject obj, uint32_t index,
                      JSGetterOp getter, JSSetterOp setter, unsigned attrs = JSPROP_ENUMERATE);

extern bool
DefineDataElement(JSContext* cx, HandleObject obj, uint32_t index, HandleValue value,
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
GetPropertyNoGC(JSContext* cx, JSObject* obj, const Value& receiver, PropertyName* name, Value* vp)
{
    return GetPropertyNoGC(cx, obj, receiver, NameToId(name), vp);
}

inline bool
GetElementNoGC(JSContext* cx, JSObject* obj, const Value& receiver, uint32_t index, Value* vp);

// Returns whether |obj| or an object on its proto chain may have an interesting
// symbol property (see JSObject::hasInterestingSymbolProperty). If it returns
// true, *holder is set to the object that may have this property.
MOZ_ALWAYS_INLINE bool
MaybeHasInterestingSymbolProperty(JSContext* cx, JSObject* obj, Symbol* symbol,
                                  JSObject** holder = nullptr);

// Like GetProperty but optimized for interesting symbol properties like
// @@toStringTag.
MOZ_ALWAYS_INLINE bool
GetInterestingSymbolProperty(JSContext* cx, HandleObject obj, Symbol* sym, MutableHandleValue vp);

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

/**
 * If |obj| (underneath any functionally-transparent wrapper proxies) has as
 * its [[GetPrototypeOf]] trap the ordinary [[GetPrototypeOf]] behavior defined
 * for ordinary objects, set |*isOrdinary = true| and store |obj|'s prototype
 * in |result|.  Otherwise set |*isOrdinary = false|.  In case of error, both
 * outparams have unspecified value.
 */
extern bool
GetPrototypeIfOrdinary(JSContext* cx, HandleObject obj, bool* isOrdinary,
                       MutableHandleObject protop);

/*
 * Attempt to make |obj|'s [[Prototype]] immutable, such that subsequently
 * trying to change it will not work.  If an internal error occurred,
 * returns false.  Otherwise, |*succeeded| is set to true iff |obj|'s
 * [[Prototype]] is now immutable.
 */
extern bool
SetImmutablePrototype(JSContext* cx, JS::HandleObject obj, bool* succeeded);

extern bool
GetPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                      MutableHandle<JS::PropertyDescriptor> desc);

/*
 * Deprecated. A version of HasProperty that also returns the object on which
 * the property was found (but that information is unreliable for proxies), and
 * the Shape of the property, if native.
 */
extern bool
LookupProperty(JSContext* cx, HandleObject obj, HandleId id,
               MutableHandleObject objp, MutableHandle<PropertyResult> propp);

inline bool
LookupProperty(JSContext* cx, HandleObject obj, PropertyName* name,
               MutableHandleObject objp, MutableHandle<PropertyResult> propp)
{
    RootedId id(cx, NameToId(name));
    return LookupProperty(cx, obj, id, objp, propp);
}

/* Set *result to tell whether obj has an own property with the given id. */
extern bool
HasOwnProperty(JSContext* cx, HandleObject obj, HandleId id, bool* result);

/**
 * This enum is used to select whether the defined functions should be marked as
 * builtin native instrinsics for self-hosted code.
 */
enum DefineAsIntrinsic {
    NotIntrinsic,
    AsIntrinsic
};

extern bool
DefineFunctions(JSContext* cx, HandleObject obj, const JSFunctionSpec* fs,
                DefineAsIntrinsic intrinsic);

/* ES6 draft rev 36 (2015 March 17) 7.1.1 ToPrimitive(vp[, preferredType]) */
extern bool
ToPrimitiveSlow(JSContext* cx, JSType hint, MutableHandleValue vp);

inline bool
ToPrimitive(JSContext* cx, MutableHandleValue vp)
{
    if (vp.isPrimitive())
        return true;
    return ToPrimitiveSlow(cx, JSTYPE_UNDEFINED, vp);
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
MOZ_ALWAYS_INLINE const char*
GetObjectClassName(JSContext* cx, HandleObject obj);

/*
 * Prepare a |this| value to be returned to script. This includes replacing
 * Windows with their corresponding WindowProxy.
 *
 * Helpers are also provided to first extract the |this| from specific
 * types of environment.
 */
Value
GetThisValue(JSObject* obj);

Value
GetThisValueOfLexical(JSObject* env);

Value
GetThisValueOfWith(JSObject* env);

/* * */

typedef JSObject* (*ClassInitializerOp)(JSContext* cx, JS::HandleObject obj);

} /* namespace js */

namespace js {

inline gc::InitialHeap
GetInitialHeap(NewObjectKind newKind, const Class* clasp)
{
    if (newKind == NurseryAllocatedProxy) {
        MOZ_ASSERT(clasp->isProxy());
        MOZ_ASSERT(clasp->hasFinalize());
        MOZ_ASSERT(!CanNurseryAllocateFinalizedClass(clasp));
        return gc::DefaultHeap;
    }
    if (newKind != GenericObject)
        return gc::TenuredHeap;
    if (clasp->hasFinalize() && !CanNurseryAllocateFinalizedClass(clasp))
        return gc::TenuredHeap;
    return gc::DefaultHeap;
}

bool
NewObjectWithTaggedProtoIsCachable(JSContext* cx, Handle<TaggedProto> proto,
                                   NewObjectKind newKind, const Class* clasp);

// ES6 9.1.15 GetPrototypeFromConstructor.
extern bool
GetPrototypeFromConstructor(JSContext* cx, js::HandleObject newTarget, js::MutableHandleObject proto);

MOZ_ALWAYS_INLINE bool
GetPrototypeFromBuiltinConstructor(JSContext* cx, const CallArgs& args, js::MutableHandleObject proto)
{
    // When proto is set to nullptr, the caller is expected to select the
    // correct default built-in prototype for this constructor.
    if (!args.isConstructing() || &args.newTarget().toObject() == &args.callee()) {
        proto.set(nullptr);
        return true;
    }

    // We're calling this constructor from a derived class, retrieve the
    // actual prototype from newTarget.
    RootedObject newTarget(cx, &args.newTarget().toObject());
    return GetPrototypeFromConstructor(cx, newTarget, proto);
}

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
                     MutableHandle<JS::PropertyDescriptor> desc);

/*
 * Throw a TypeError if desc.getterObject() or setterObject() is not
 * callable. This performs exactly the checks omitted by ToPropertyDescriptor
 * when checkAccessors is false.
 */
Result<>
CheckPropertyDescriptorAccessors(JSContext* cx, Handle<JS::PropertyDescriptor> desc);

void
CompletePropertyDescriptor(MutableHandle<JS::PropertyDescriptor> desc);

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
           MutableHandleObject objp, MutableHandleObject pobjp, MutableHandle<PropertyResult> propp);

extern bool
LookupNameNoGC(JSContext* cx, PropertyName* name, JSObject* scopeChain,
               JSObject** objp, JSObject** pobjp, PropertyResult* propp);

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

bool
LookupPropertyPure(JSContext* cx, JSObject* obj, jsid id, JSObject** objp,
                   PropertyResult* propp);

bool
LookupOwnPropertyPure(JSContext* cx, JSObject* obj, jsid id, PropertyResult* propp,
                      bool* isTypedArrayOutOfRange = nullptr);

bool
GetPropertyPure(JSContext* cx, JSObject* obj, jsid id, Value* vp);

bool
GetOwnPropertyPure(JSContext* cx, JSObject* obj, jsid id, Value* vp);

bool
GetGetterPure(JSContext* cx, JSObject* obj, jsid id, JSFunction** fp);

bool
GetOwnGetterPure(JSContext* cx, JSObject* obj, jsid id, JSFunction** fp);

bool
GetOwnNativeGetterPure(JSContext* cx, JSObject* obj, jsid id, JSNative* native);

bool
HasOwnDataPropertyPure(JSContext* cx, JSObject* obj, jsid id, bool* result);

bool
GetOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                         MutableHandle<JS::PropertyDescriptor> desc);

/*
 * Like JS::FromPropertyDescriptor, but ignore desc.object() and always set vp
 * to an object on success.
 *
 * Use JS::FromPropertyDescriptor for getOwnPropertyDescriptor, since desc.object()
 * is used to indicate whether a result was found or not.  Use this instead for
 * defineProperty: it would be senseless to define a "missing" property.
 */
extern bool
FromPropertyDescriptorToObject(JSContext* cx, Handle<JS::PropertyDescriptor> desc,
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


/*
 * Report a TypeError: "N-th argument of FUN must be an object, got VALUE".
 * Using NotNullObjectArg is usually less code.
 */
extern void
ReportNotObjectArg(JSContext* cx, const char* nth, const char* fun, HandleValue v);

inline JSObject*
NonNullObjectArg(JSContext* cx, const char* nth, const char* fun, HandleValue v)
{
    if (v.isObject())
        return &v.toObject();
    ReportNotObjectArg(cx, nth, fun, v);
    return nullptr;
}

/*
 * Report a TypeError: "SOMETHING must be an object, got VALUE".
 * Using NotNullObjectWithName is usually less code.
 */
extern void
ReportNotObjectWithName(JSContext* cx, const char* name, HandleValue v);

inline JSObject*
NonNullObjectWithName(JSContext* cx, const char* name, HandleValue v)
{
    if (v.isObject())
        return &v.toObject();
    ReportNotObjectWithName(cx, name, v);
    return nullptr;
}


extern bool
GetFirstArgumentAsObject(JSContext* cx, const CallArgs& args, const char* method,
                         MutableHandleObject objp);

/* Helper for throwing, always returns false. */
extern bool
Throw(JSContext* cx, jsid id, unsigned errorNumber, const char* details = nullptr);

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

extern MOZ_MUST_USE JSObject*
SpeciesConstructor(JSContext* cx, HandleObject obj, HandleObject defaultCtor,
                   bool (*isDefaultSpecies)(JSContext*, JSFunction*));

extern MOZ_MUST_USE JSObject*
SpeciesConstructor(JSContext* cx, HandleObject obj, JSProtoKey ctorKey,
                   bool (*isDefaultSpecies)(JSContext*, JSFunction*));

extern bool
GetObjectFromIncumbentGlobal(JSContext* cx, MutableHandleObject obj);


#ifdef DEBUG
inline bool
IsObjectValueInCompartment(const Value& v, JSCompartment* comp)
{
    if (!v.isObject())
        return true;
    return v.toObject().compartment() == comp;
}
#endif

}  /* namespace js */

#endif /* vm_JSObject_h */

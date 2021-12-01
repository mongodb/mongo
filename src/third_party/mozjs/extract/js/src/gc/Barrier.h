/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Barrier_h
#define gc_Barrier_h

#include "NamespaceImports.h"

#include "gc/Cell.h"
#include "gc/StoreBuffer.h"
#include "js/HeapAPI.h"
#include "js/Id.h"
#include "js/RootingAPI.h"
#include "js/Value.h"

/*
 * A write barrier is a mechanism used by incremental or generation GCs to
 * ensure that every value that needs to be marked is marked. In general, the
 * write barrier should be invoked whenever a write can cause the set of things
 * traced through by the GC to change. This includes:
 *   - writes to object properties
 *   - writes to array slots
 *   - writes to fields like JSObject::shape_ that we trace through
 *   - writes to fields in private data
 *   - writes to non-markable fields like JSObject::private that point to
 *     markable data
 * The last category is the trickiest. Even though the private pointers does not
 * point to a GC thing, changing the private pointer may change the set of
 * objects that are traced by the GC. Therefore it needs a write barrier.
 *
 * Every barriered write should have the following form:
 *   <pre-barrier>
 *   obj->field = value; // do the actual write
 *   <post-barrier>
 * The pre-barrier is used for incremental GC and the post-barrier is for
 * generational GC.
 *
 *                               PRE-BARRIER
 *
 * To understand the pre-barrier, let's consider how incremental GC works. The
 * GC itself is divided into "slices". Between each slice, JS code is allowed to
 * run. Each slice should be short so that the user doesn't notice the
 * interruptions. In our GC, the structure of the slices is as follows:
 *
 * 1. ... JS work, which leads to a request to do GC ...
 * 2. [first GC slice, which performs all root marking and possibly more marking]
 * 3. ... more JS work is allowed to run ...
 * 4. [GC mark slice, which runs entirely in drainMarkStack]
 * 5. ... more JS work ...
 * 6. [GC mark slice, which runs entirely in drainMarkStack]
 * 7. ... more JS work ...
 * 8. [GC marking finishes; sweeping done non-incrementally; GC is done]
 * 9. ... JS continues uninterrupted now that GC is finishes ...
 *
 * Of course, there may be a different number of slices depending on how much
 * marking is to be done.
 *
 * The danger inherent in this scheme is that the JS code in steps 3, 5, and 7
 * might change the heap in a way that causes the GC to collect an object that
 * is actually reachable. The write barrier prevents this from happening. We use
 * a variant of incremental GC called "snapshot at the beginning." This approach
 * guarantees the invariant that if an object is reachable in step 2, then we
 * will mark it eventually. The name comes from the idea that we take a
 * theoretical "snapshot" of all reachable objects in step 2; all objects in
 * that snapshot should eventually be marked. (Note that the write barrier
 * verifier code takes an actual snapshot.)
 *
 * The basic correctness invariant of a snapshot-at-the-beginning collector is
 * that any object reachable at the end of the GC (step 9) must either:
 *   (1) have been reachable at the beginning (step 2) and thus in the snapshot
 *   (2) or must have been newly allocated, in steps 3, 5, or 7.
 * To deal with case (2), any objects allocated during an incremental GC are
 * automatically marked black.
 *
 * This strategy is actually somewhat conservative: if an object becomes
 * unreachable between steps 2 and 8, it would be safe to collect it. We won't,
 * mainly for simplicity. (Also, note that the snapshot is entirely
 * theoretical. We don't actually do anything special in step 2 that we wouldn't
 * do in a non-incremental GC.
 *
 * It's the pre-barrier's job to maintain the snapshot invariant. Consider the
 * write "obj->field = value". Let the prior value of obj->field be
 * value0. Since it's possible that value0 may have been what obj->field
 * contained in step 2, when the snapshot was taken, the barrier marks
 * value0. Note that it only does this if we're in the middle of an incremental
 * GC. Since this is rare, the cost of the write barrier is usually just an
 * extra branch.
 *
 * In practice, we implement the pre-barrier differently based on the type of
 * value0. E.g., see JSObject::writeBarrierPre, which is used if obj->field is
 * a JSObject*. It takes value0 as a parameter.
 *
 *                                READ-BARRIER
 *
 * Incremental GC requires that weak pointers have read barriers. The problem
 * happens when, during an incremental GC, some code reads a weak pointer and
 * writes it somewhere on the heap that has been marked black in a previous
 * slice. Since the weak pointer will not otherwise be marked and will be swept
 * and finalized in the last slice, this will leave the pointer just written
 * dangling after the GC. To solve this, we immediately mark black all weak
 * pointers that get read between slices so that it is safe to store them in an
 * already marked part of the heap, e.g. in Rooted.
 *
 *                                POST-BARRIER
 *
 * For generational GC, we want to be able to quickly collect the nursery in a
 * minor collection.  Part of the way this is achieved is to only mark the
 * nursery itself; tenured things, which may form the majority of the heap, are
 * not traced through or marked.  This leads to the problem of what to do about
 * tenured objects that have pointers into the nursery: if such things are not
 * marked, they may be discarded while there are still live objects which
 * reference them. The solution is to maintain information about these pointers,
 * and mark their targets when we start a minor collection.
 *
 * The pointers can be thought of as edges in object graph, and the set of edges
 * from the tenured generation into the nursery is know as the remembered set.
 * Post barriers are used to track this remembered set.
 *
 * Whenever a slot which could contain such a pointer is written, we use a write
 * barrier to check if the edge created is in the remembered set, and if so we
 * insert it into the store buffer, which is the collector's representation of
 * the remembered set.  This means than when we come to do a minor collection we
 * can examine the contents of the store buffer and mark any edge targets that
 * are in the nursery.
 *
 *                            IMPLEMENTATION DETAILS
 *
 * Since it would be awkward to change every write to memory into a function
 * call, this file contains a bunch of C++ classes and templates that use
 * operator overloading to take care of barriers automatically. In many cases,
 * all that's necessary to make some field be barriered is to replace
 *     Type* field;
 * with
 *     GCPtr<Type> field;
 *
 * One additional note: not all object writes need to be pre-barriered. Writes
 * to newly allocated objects do not need a pre-barrier. In these cases, we use
 * the "obj->field.init(value)" method instead of "obj->field = value". We use
 * the init naming idiom in many places to signify that a field is being
 * assigned for the first time.
 *
 * This file implements four classes, illustrated here:
 *
 * BarrieredBase             base class of all barriers
 *  |  |
 *  | WriteBarrieredBase     base class which provides common write operations
 *  |  |  |  |  |
 *  |  |  |  | PreBarriered  provides pre-barriers only
 *  |  |  |  |
 *  |  |  | GCPtr            provides pre- and post-barriers
 *  |  |  |
 *  |  | HeapPtr             provides pre- and post-barriers; is relocatable
 *  |  |                     and deletable for use inside C++ managed memory
 *  |  |
 *  | HeapSlot               similar to GCPtr, but tailored to slots storage
 *  |
 * ReadBarrieredBase         base class which provides common read operations
 *  |
 * ReadBarriered             provides read barriers only
 *
 *
 * The implementation of the barrier logic is implemented on T::writeBarrier.*,
 * via:
 *
 * WriteBarrieredBase<T>::pre
 *  -> InternalBarrierMethods<T*>::preBarrier
 *      -> T::writeBarrierPre
 *  -> InternalBarrierMethods<Value>::preBarrier
 *  -> InternalBarrierMethods<jsid>::preBarrier
 *      -> InternalBarrierMethods<T*>::preBarrier
 *          -> T::writeBarrierPre
 *
 * GCPtr<T>::post and HeapPtr<T>::post
 *  -> InternalBarrierMethods<T*>::postBarrier
 *      -> T::writeBarrierPost
 *  -> InternalBarrierMethods<Value>::postBarrier
 *      -> StoreBuffer::put
 *
 * These classes are designed to be used by the internals of the JS engine.
 * Barriers designed to be used externally are provided in js/RootingAPI.h.
 * These external barriers call into the same post-barrier implementations at
 * InternalBarrierMethods<T>::post via an indirect call to Heap(.+)Barrier.
 *
 * These clases are designed to be used to wrap GC thing pointers or values that
 * act like them (i.e. JS::Value and jsid).  It is possible to use them for
 * other types by supplying the necessary barrier implementations but this
 * is not usually necessary and should be done with caution.
 */

class JSFlatString;
class JSLinearString;

namespace js {

class AccessorShape;
class ArrayObject;
class ArgumentsObject;
class ArrayBufferObjectMaybeShared;
class ArrayBufferObject;
class ArrayBufferViewObject;
class SharedArrayBufferObject;
class BaseShape;
class DebugEnvironmentProxy;
class GlobalObject;
class LazyScript;
class ModuleObject;
class ModuleEnvironmentObject;
class ModuleNamespaceObject;
class NativeObject;
class PlainObject;
class PropertyName;
class SavedFrame;
class EnvironmentObject;
class ScriptSourceObject;
class Shape;
class UnownedBaseShape;
class ObjectGroup;

namespace jit {
class JitCode;
} // namespace jit

#ifdef DEBUG

// Barriers can't be triggered during backend Ion compilation, which may run on
// a helper thread.
bool
CurrentThreadIsIonCompiling();

bool
CurrentThreadIsIonCompilingSafeForMinorGC();

bool
CurrentThreadIsGCSweeping();

bool
IsMarkedBlack(JSObject* obj);

bool
CurrentThreadIsTouchingGrayThings();

#endif

struct MOZ_RAII AutoTouchingGrayThings
{
#ifdef DEBUG
    AutoTouchingGrayThings();
    ~AutoTouchingGrayThings();
#else
    AutoTouchingGrayThings() {}
#endif
};

template <typename T>
struct InternalBarrierMethods {};

template <typename T>
struct InternalBarrierMethods<T*>
{
    static bool isMarkable(T* v) { return v != nullptr; }

    static void preBarrier(T* v) { T::writeBarrierPre(v); }

    static void postBarrier(T** vp, T* prev, T* next) { T::writeBarrierPost(vp, prev, next); }

    static void readBarrier(T* v) { T::readBarrier(v); }

#ifdef DEBUG
    static bool thingIsNotGray(T* v) { return T::thingIsNotGray(v); }
#endif
};

template <typename S> struct PreBarrierFunctor : public VoidDefaultAdaptor<S> {
    template <typename T> void operator()(T* t);
};

template <typename S> struct ReadBarrierFunctor : public VoidDefaultAdaptor<S> {
    template <typename T> void operator()(T* t);
};

template <>
struct InternalBarrierMethods<Value>
{
    static bool isMarkable(const Value& v) { return v.isGCThing(); }

    static void preBarrier(const Value& v) {
        DispatchTyped(PreBarrierFunctor<Value>(), v);
    }

    static MOZ_ALWAYS_INLINE void postBarrier(Value* vp, const Value& prev, const Value& next) {
        MOZ_ASSERT(!CurrentThreadIsIonCompiling());
        MOZ_ASSERT(vp);

        // If the target needs an entry, add it.
        js::gc::StoreBuffer* sb;
        if ((next.isObject() || next.isString()) && (sb = next.toGCThing()->storeBuffer())) {
            // If we know that the prev has already inserted an entry, we can
            // skip doing the lookup to add the new entry. Note that we cannot
            // safely assert the presence of the entry because it may have been
            // added via a different store buffer.
            if ((prev.isObject() || prev.isString()) && prev.toGCThing()->storeBuffer())
                return;
            sb->putValue(vp);
            return;
        }
        // Remove the prev entry if the new value does not need it.
        if ((prev.isObject() || prev.isString()) && (sb = prev.toGCThing()->storeBuffer()))
            sb->unputValue(vp);
    }

    static void readBarrier(const Value& v) {
        DispatchTyped(ReadBarrierFunctor<Value>(), v);
    }

#ifdef DEBUG
    static bool thingIsNotGray(const Value& v) { return JS::ValueIsNotGray(v); }
#endif
};

template <>
struct InternalBarrierMethods<jsid>
{
    static bool isMarkable(jsid id) { return JSID_IS_GCTHING(id); }
    static void preBarrier(jsid id) { DispatchTyped(PreBarrierFunctor<jsid>(), id); }
    static void postBarrier(jsid* idp, jsid prev, jsid next) {}
#ifdef DEBUG
    static bool thingIsNotGray(jsid id) { return JS::IdIsNotGray(id); }
#endif
};

template <typename T>
static inline void
CheckTargetIsNotGray(const T& v)
{
    MOZ_ASSERT(InternalBarrierMethods<T>::thingIsNotGray(v) ||
               CurrentThreadIsTouchingGrayThings());
}

// Base class of all barrier types.
//
// This is marked non-memmovable since post barriers added by derived classes
// can add pointers to class instances to the store buffer.
template <typename T>
class MOZ_NON_MEMMOVABLE BarrieredBase
{
  protected:
    // BarrieredBase is not directly instantiable.
    explicit BarrieredBase(const T& v) : value(v) {}

    // BarrieredBase subclasses cannot be copy constructed by default.
    BarrieredBase(const BarrieredBase<T>& other) = default;

    // Storage for all barrier classes. |value| must be a GC thing reference
    // type: either a direct pointer to a GC thing or a supported tagged
    // pointer that can reference GC things, such as JS::Value or jsid. Nested
    // barrier types are NOT supported. See assertTypeConstraints.
    T value;

  public:
    // Note: this is public because C++ cannot friend to a specific template instantiation.
    // Friending to the generic template leads to a number of unintended consequences, including
    // template resolution ambiguity and a circular dependency with Tracing.h.
    T* unsafeUnbarrieredForTracing() { return &value; }
};

// Base class for barriered pointer types that intercept only writes.
template <class T>
class WriteBarrieredBase : public BarrieredBase<T>,
                           public WrappedPtrOperations<T, WriteBarrieredBase<T>>
{
  protected:
    using BarrieredBase<T>::value;

    // WriteBarrieredBase is not directly instantiable.
    explicit WriteBarrieredBase(const T& v) : BarrieredBase<T>(v) {}

  public:
    using ElementType = T;

    DECLARE_POINTER_CONSTREF_OPS(T);

    // Use this if the automatic coercion to T isn't working.
    const T& get() const { return this->value; }

    // Use this if you want to change the value without invoking barriers.
    // Obviously this is dangerous unless you know the barrier is not needed.
    void unsafeSet(const T& v) { this->value = v; }

    // For users who need to manually barrier the raw types.
    static void writeBarrierPre(const T& v) { InternalBarrierMethods<T>::preBarrier(v); }

  protected:
    void pre() { InternalBarrierMethods<T>::preBarrier(this->value); }
    MOZ_ALWAYS_INLINE void post(const T& prev, const T& next) {
        InternalBarrierMethods<T>::postBarrier(&this->value, prev, next);
    }
};

/*
 * PreBarriered only automatically handles pre-barriers. Post-barriers must be
 * manually implemented when using this class. GCPtr and HeapPtr should be used
 * in all cases that do not require explicit low-level control of moving
 * behavior, e.g. for HashMap keys.
 */
template <class T>
class PreBarriered : public WriteBarrieredBase<T>
{
  public:
    PreBarriered() : WriteBarrieredBase<T>(JS::GCPolicy<T>::initial()) {}
    /*
     * Allow implicit construction for use in generic contexts, such as
     * DebuggerWeakMap::markKeys.
     */
    MOZ_IMPLICIT PreBarriered(const T& v) : WriteBarrieredBase<T>(v) {}
    explicit PreBarriered(const PreBarriered<T>& v) : WriteBarrieredBase<T>(v.value) {}
    ~PreBarriered() { this->pre(); }

    void init(const T& v) {
        this->value = v;
    }

    /* Use to set the pointer to nullptr. */
    void clear() {
        this->pre();
        this->value = nullptr;
    }

    DECLARE_POINTER_ASSIGN_OPS(PreBarriered, T);

  private:
    void set(const T& v) {
        CheckTargetIsNotGray(v);
        this->pre();
        this->value = v;
    }
};

/*
 * A pre- and post-barriered heap pointer, for use inside the JS engine.
 *
 * It must only be stored in memory that has GC lifetime. GCPtr must not be
 * used in contexts where it may be implicitly moved or deleted, e.g. most
 * containers.
 *
 * The post-barriers implemented by this class are faster than those
 * implemented by js::HeapPtr<T> or JS::Heap<T> at the cost of not
 * automatically handling deletion or movement.
 */
template <class T>
class GCPtr : public WriteBarrieredBase<T>
{
  public:
    GCPtr() : WriteBarrieredBase<T>(JS::GCPolicy<T>::initial()) {}
    explicit GCPtr(const T& v) : WriteBarrieredBase<T>(v) {
        this->post(JS::GCPolicy<T>::initial(), v);
    }
    explicit GCPtr(const GCPtr<T>& v) : WriteBarrieredBase<T>(v) {
        this->post(JS::GCPolicy<T>::initial(), v);
    }
#ifdef DEBUG
    ~GCPtr() {
        // No barriers are necessary as this only happens when we are sweeping
        // or when after GCManagedDeletePolicy has triggered the barriers for us
        // and cleared the pointer.
        //
        // If you get a crash here, you may need to make the containing object
        // use GCManagedDeletePolicy and use JS::DeletePolicy to destroy it.
        //
        // Note that when sweeping the wrapped pointer may already have been
        // freed by this point.
        MOZ_ASSERT(CurrentThreadIsGCSweeping() || this->value == JS::GCPolicy<T>::initial());
        Poison(this, JS_FREED_HEAP_PTR_PATTERN, sizeof(*this));
    }
#endif

    void init(const T& v) {
        CheckTargetIsNotGray(v);
        this->value = v;
        this->post(JS::GCPolicy<T>::initial(), v);
    }

    DECLARE_POINTER_ASSIGN_OPS(GCPtr, T);

  private:
    void set(const T& v) {
        CheckTargetIsNotGray(v);
        this->pre();
        T tmp = this->value;
        this->value = v;
        this->post(tmp, this->value);
    }

    /*
     * Unlike HeapPtr<T>, GCPtr<T> must be managed with GC lifetimes.
     * Specifically, the memory used by the pointer itself must be live until
     * at least the next minor GC. For that reason, move semantics are invalid
     * and are deleted here. Please note that not all containers support move
     * semantics, so this does not completely prevent invalid uses.
     */
    GCPtr(GCPtr<T>&&) = delete;
    GCPtr<T>& operator=(GCPtr<T>&&) = delete;
};

/*
 * A pre- and post-barriered heap pointer, for use inside the JS engine. These
 * heap pointers can be stored in C++ containers like GCVector and GCHashMap.
 *
 * The GC sometimes keeps pointers to pointers to GC things --- for example, to
 * track references into the nursery. However, C++ containers like GCVector and
 * GCHashMap usually reserve the right to relocate their elements any time
 * they're modified, invalidating all pointers to the elements. HeapPtr
 * has a move constructor which knows how to keep the GC up to date if it is
 * moved to a new location.
 *
 * However, because of this additional communication with the GC, HeapPtr
 * is somewhat slower, so it should only be used in contexts where this ability
 * is necessary.
 *
 * Obviously, JSObjects, JSStrings, and the like get tenured and compacted, so
 * whatever pointers they contain get relocated, in the sense used here.
 * However, since the GC itself is moving those values, it takes care of its
 * internal pointers to those pointers itself. HeapPtr is only necessary
 * when the relocation would otherwise occur without the GC's knowledge.
 */
template <class T>
class HeapPtr : public WriteBarrieredBase<T>
{
  public:
    HeapPtr() : WriteBarrieredBase<T>(JS::GCPolicy<T>::initial()) {}

    // Implicitly adding barriers is a reasonable default.
    MOZ_IMPLICIT HeapPtr(const T& v) : WriteBarrieredBase<T>(v) {
        this->post(JS::GCPolicy<T>::initial(), this->value);
    }

    /*
     * For HeapPtr, move semantics are equivalent to copy semantics. In
     * C++, a copy constructor taking const-ref is the way to get a single
     * function that will be used for both lvalue and rvalue copies, so we can
     * simply omit the rvalue variant.
     */
    MOZ_IMPLICIT HeapPtr(const HeapPtr<T>& v) : WriteBarrieredBase<T>(v) {
        this->post(JS::GCPolicy<T>::initial(), this->value);
    }

    ~HeapPtr() {
        this->pre();
        this->post(this->value, JS::GCPolicy<T>::initial());
    }

    void init(const T& v) {
        CheckTargetIsNotGray(v);
        this->value = v;
        this->post(JS::GCPolicy<T>::initial(), this->value);
    }

    DECLARE_POINTER_ASSIGN_OPS(HeapPtr, T);

    /* Make this friend so it can access pre() and post(). */
    template <class T1, class T2>
    friend inline void
    BarrieredSetPair(Zone* zone,
                     HeapPtr<T1*>& v1, T1* val1,
                     HeapPtr<T2*>& v2, T2* val2);

  protected:
    void set(const T& v) {
        CheckTargetIsNotGray(v);
        this->pre();
        postBarrieredSet(v);
    }

    void postBarrieredSet(const T& v) {
        CheckTargetIsNotGray(v);
        T tmp = this->value;
        this->value = v;
        this->post(tmp, this->value);
    }
};

// Base class for barriered pointer types that intercept reads and writes.
template <typename T>
class ReadBarrieredBase : public BarrieredBase<T>
{
  protected:
    // ReadBarrieredBase is not directly instantiable.
    explicit ReadBarrieredBase(const T& v) : BarrieredBase<T>(v) {}

  protected:
    void read() const { InternalBarrierMethods<T>::readBarrier(this->value); }
    void post(const T& prev, const T& next) {
        InternalBarrierMethods<T>::postBarrier(&this->value, prev, next);
    }
};

// Incremental GC requires that weak pointers have read barriers. See the block
// comment at the top of Barrier.h for a complete discussion of why.
//
// Note that this class also has post-barriers, so is safe to use with nursery
// pointers. However, when used as a hashtable key, care must still be taken to
// insert manual post-barriers on the table for rekeying if the key is based in
// any way on the address of the object.
template <typename T>
class ReadBarriered : public ReadBarrieredBase<T>,
                      public WrappedPtrOperations<T, ReadBarriered<T>>
{
  protected:
    using ReadBarrieredBase<T>::value;

  public:
    ReadBarriered() : ReadBarrieredBase<T>(JS::GCPolicy<T>::initial()) {}

    // It is okay to add barriers implicitly.
    MOZ_IMPLICIT ReadBarriered(const T& v) : ReadBarrieredBase<T>(v) {
        this->post(JS::GCPolicy<T>::initial(), v);
    }

    // The copy constructor creates a new weak edge but the wrapped pointer does
    // not escape, so no read barrier is necessary.
    explicit ReadBarriered(const ReadBarriered& v) : ReadBarrieredBase<T>(v) {
        this->post(JS::GCPolicy<T>::initial(), v.unbarrieredGet());
    }

    // Move retains the lifetime status of the source edge, so does not fire
    // the read barrier of the defunct edge.
    ReadBarriered(ReadBarriered&& v)
      : ReadBarrieredBase<T>(mozilla::Move(v))
    {
        this->post(JS::GCPolicy<T>::initial(), v.value);
    }

    ~ReadBarriered() {
        this->post(this->value, JS::GCPolicy<T>::initial());
    }

    ReadBarriered& operator=(const ReadBarriered& v) {
        CheckTargetIsNotGray(v.value);
        T prior = this->value;
        this->value = v.value;
        this->post(prior, v.value);
        return *this;
    }

    const T& get() const {
        if (InternalBarrierMethods<T>::isMarkable(this->value))
            this->read();
        return this->value;
    }

    const T& unbarrieredGet() const {
        return this->value;
    }

    explicit operator bool() const {
        return bool(this->value);
    }

    operator const T&() const { return get(); }

    const T& operator->() const { return get(); }

    T* unsafeGet() { return &this->value; }
    T const* unsafeGet() const { return &this->value; }

    void set(const T& v)
    {
        CheckTargetIsNotGray(v);
        T tmp = this->value;
        this->value = v;
        this->post(tmp, v);
    }
};

// A WeakRef pointer does not hold its target live and is automatically nulled
// out when the GC discovers that it is not reachable from any other path.
template <typename T>
using WeakRef = ReadBarriered<T>;

// A pre- and post-barriered Value that is specialized to be aware that it
// resides in a slots or elements vector. This allows it to be relocated in
// memory, but with substantially less overhead than a HeapPtr.
class HeapSlot : public WriteBarrieredBase<Value>
{
  public:
    enum Kind {
        Slot = 0,
        Element = 1
    };

    void init(NativeObject* owner, Kind kind, uint32_t slot, const Value& v) {
        value = v;
        post(owner, kind, slot, v);
    }

    void destroy() {
        pre();
    }

#ifdef DEBUG
    bool preconditionForSet(NativeObject* owner, Kind kind, uint32_t slot) const;
    void assertPreconditionForWriteBarrierPost(NativeObject* obj, Kind kind, uint32_t slot,
                                               const Value& target) const;
#endif

    MOZ_ALWAYS_INLINE void set(NativeObject* owner, Kind kind, uint32_t slot, const Value& v) {
        MOZ_ASSERT(preconditionForSet(owner, kind, slot));
        pre();
        value = v;
        post(owner, kind, slot, v);
    }

  private:
    void post(NativeObject* owner, Kind kind, uint32_t slot, const Value& target) {
#ifdef DEBUG
        assertPreconditionForWriteBarrierPost(owner, kind, slot, target);
#endif
        if (this->value.isObject() || this->value.isString()) {
            gc::Cell* cell = this->value.toGCThing();
            if (cell->storeBuffer())
                cell->storeBuffer()->putSlot(owner, kind, slot, 1);
        }
    }
};

class HeapSlotArray
{
    HeapSlot* array;

    // Whether writes may be performed to the slots in this array. This helps
    // to control how object elements which may be copy on write are used.
#ifdef DEBUG
    bool allowWrite_;
#endif

  public:
    explicit HeapSlotArray(HeapSlot* array, bool allowWrite)
      : array(array)
#ifdef DEBUG
      , allowWrite_(allowWrite)
#endif
    {}

    operator const Value*() const {
        JS_STATIC_ASSERT(sizeof(GCPtr<Value>) == sizeof(Value));
        JS_STATIC_ASSERT(sizeof(HeapSlot) == sizeof(Value));
        return reinterpret_cast<const Value*>(array);
    }
    operator HeapSlot*() const { MOZ_ASSERT(allowWrite()); return array; }

    HeapSlotArray operator +(int offset) const { return HeapSlotArray(array + offset, allowWrite()); }
    HeapSlotArray operator +(uint32_t offset) const { return HeapSlotArray(array + offset, allowWrite()); }

  private:
    bool allowWrite() const {
#ifdef DEBUG
        return allowWrite_;
#else
        return true;
#endif
    }
};

/*
 * This is a hack for RegExpStatics::updateFromMatch. It allows us to do two
 * barriers with only one branch to check if we're in an incremental GC.
 */
template <class T1, class T2>
static inline void
BarrieredSetPair(Zone* zone,
                 HeapPtr<T1*>& v1, T1* val1,
                 HeapPtr<T2*>& v2, T2* val2)
{
    if (T1::needWriteBarrierPre(zone)) {
        v1.pre();
        v2.pre();
    }
    v1.postBarrieredSet(val1);
    v2.postBarrieredSet(val2);
}

/*
 * ImmutableTenuredPtr is designed for one very narrow case: replacing
 * immutable raw pointers to GC-managed things, implicitly converting to a
 * handle type for ease of use. Pointers encapsulated by this type must:
 *
 *   be immutable (no incremental write barriers),
 *   never point into the nursery (no generational write barriers), and
 *   be traced via MarkRuntime (we use fromMarkedLocation).
 *
 * In short: you *really* need to know what you're doing before you use this
 * class!
 */
template <typename T>
class ImmutableTenuredPtr
{
    T value;

  public:
    operator T() const { return value; }
    T operator->() const { return value; }

    operator Handle<T>() const {
        return Handle<T>::fromMarkedLocation(&value);
    }

    void init(T ptr) {
        MOZ_ASSERT(ptr->isTenured());
        CheckTargetIsNotGray(ptr);
        value = ptr;
    }

    T get() const { return value; }
    const T* address() { return &value; }
};

template <typename T>
struct MovableCellHasher<PreBarriered<T>>
{
    using Key = PreBarriered<T>;
    using Lookup = T;

    static bool hasHash(const Lookup& l) { return MovableCellHasher<T>::hasHash(l); }
    static bool ensureHash(const Lookup& l) { return MovableCellHasher<T>::ensureHash(l); }
    static HashNumber hash(const Lookup& l) { return MovableCellHasher<T>::hash(l); }
    static bool match(const Key& k, const Lookup& l) { return MovableCellHasher<T>::match(k, l); }
    static void rekey(Key& k, const Key& newKey) { k.unsafeSet(newKey); }
};

template <typename T>
struct MovableCellHasher<HeapPtr<T>>
{
    using Key = HeapPtr<T>;
    using Lookup = T;

    static bool hasHash(const Lookup& l) { return MovableCellHasher<T>::hasHash(l); }
    static bool ensureHash(const Lookup& l) { return MovableCellHasher<T>::ensureHash(l); }
    static HashNumber hash(const Lookup& l) { return MovableCellHasher<T>::hash(l); }
    static bool match(const Key& k, const Lookup& l) { return MovableCellHasher<T>::match(k, l); }
    static void rekey(Key& k, const Key& newKey) { k.unsafeSet(newKey); }
};

template <typename T>
struct MovableCellHasher<ReadBarriered<T>>
{
    using Key = ReadBarriered<T>;
    using Lookup = T;

    static bool hasHash(const Lookup& l) { return MovableCellHasher<T>::hasHash(l); }
    static bool ensureHash(const Lookup& l) { return MovableCellHasher<T>::ensureHash(l); }
    static HashNumber hash(const Lookup& l) { return MovableCellHasher<T>::hash(l); }
    static bool match(const Key& k, const Lookup& l) {
        return MovableCellHasher<T>::match(k.unbarrieredGet(), l);
    }
    static void rekey(Key& k, const Key& newKey) { k.unsafeSet(newKey); }
};

/* Useful for hashtables with a GCPtr as key. */
template <class T>
struct GCPtrHasher
{
    typedef GCPtr<T> Key;
    typedef T Lookup;

    static HashNumber hash(Lookup obj) { return DefaultHasher<T>::hash(obj); }
    static bool match(const Key& k, Lookup l) { return k.get() == l; }
    static void rekey(Key& k, const Key& newKey) { k.unsafeSet(newKey); }
};

/* Specialized hashing policy for GCPtrs. */
template <class T>
struct DefaultHasher<GCPtr<T>> : GCPtrHasher<T> {};

template <class T>
struct PreBarrieredHasher
{
    typedef PreBarriered<T> Key;
    typedef T Lookup;

    static HashNumber hash(Lookup obj) { return DefaultHasher<T>::hash(obj); }
    static bool match(const Key& k, Lookup l) { return k.get() == l; }
    static void rekey(Key& k, const Key& newKey) { k.unsafeSet(newKey); }
};

template <class T>
struct DefaultHasher<PreBarriered<T>> : PreBarrieredHasher<T> { };

/* Useful for hashtables with a ReadBarriered as key. */
template <class T>
struct ReadBarrieredHasher
{
    typedef ReadBarriered<T> Key;
    typedef T Lookup;

    static HashNumber hash(Lookup obj) { return DefaultHasher<T>::hash(obj); }
    static bool match(const Key& k, Lookup l) { return k.unbarrieredGet() == l; }
    static void rekey(Key& k, const Key& newKey) { k.set(newKey.unbarrieredGet()); }
};

/* Specialized hashing policy for ReadBarriereds. */
template <class T>
struct DefaultHasher<ReadBarriered<T>> : ReadBarrieredHasher<T> { };

class ArrayObject;
class ArrayBufferObject;
class GlobalObject;
class Scope;
class ScriptSourceObject;
class Shape;
class BaseShape;
class UnownedBaseShape;
class WasmInstanceObject;
class WasmTableObject;
namespace jit {
class JitCode;
} // namespace jit

typedef PreBarriered<JSObject*> PreBarrieredObject;
typedef PreBarriered<JSScript*> PreBarrieredScript;
typedef PreBarriered<jit::JitCode*> PreBarrieredJitCode;
typedef PreBarriered<JSString*> PreBarrieredString;
typedef PreBarriered<JSAtom*> PreBarrieredAtom;

typedef GCPtr<NativeObject*> GCPtrNativeObject;
typedef GCPtr<ArrayObject*> GCPtrArrayObject;
typedef GCPtr<ArrayBufferObjectMaybeShared*> GCPtrArrayBufferObjectMaybeShared;
typedef GCPtr<ArrayBufferObject*> GCPtrArrayBufferObject;
typedef GCPtr<BaseShape*> GCPtrBaseShape;
typedef GCPtr<JSAtom*> GCPtrAtom;
typedef GCPtr<JSFlatString*> GCPtrFlatString;
typedef GCPtr<JSFunction*> GCPtrFunction;
typedef GCPtr<JSLinearString*> GCPtrLinearString;
typedef GCPtr<JSObject*> GCPtrObject;
typedef GCPtr<JSScript*> GCPtrScript;
typedef GCPtr<JSString*> GCPtrString;
typedef GCPtr<ModuleObject*> GCPtrModuleObject;
typedef GCPtr<ModuleEnvironmentObject*> GCPtrModuleEnvironmentObject;
typedef GCPtr<ModuleNamespaceObject*> GCPtrModuleNamespaceObject;
typedef GCPtr<PlainObject*> GCPtrPlainObject;
typedef GCPtr<PropertyName*> GCPtrPropertyName;
typedef GCPtr<Shape*> GCPtrShape;
typedef GCPtr<UnownedBaseShape*> GCPtrUnownedBaseShape;
typedef GCPtr<jit::JitCode*> GCPtrJitCode;
typedef GCPtr<ObjectGroup*> GCPtrObjectGroup;
typedef GCPtr<Scope*> GCPtrScope;

typedef PreBarriered<Value> PreBarrieredValue;
typedef GCPtr<Value> GCPtrValue;

typedef PreBarriered<jsid> PreBarrieredId;
typedef GCPtr<jsid> GCPtrId;

typedef ImmutableTenuredPtr<PropertyName*> ImmutablePropertyNamePtr;
typedef ImmutableTenuredPtr<JS::Symbol*> ImmutableSymbolPtr;

typedef ReadBarriered<DebugEnvironmentProxy*> ReadBarrieredDebugEnvironmentProxy;
typedef ReadBarriered<GlobalObject*> ReadBarrieredGlobalObject;
typedef ReadBarriered<JSObject*> ReadBarrieredObject;
typedef ReadBarriered<JSFunction*> ReadBarrieredFunction;
typedef ReadBarriered<JSScript*> ReadBarrieredScript;
typedef ReadBarriered<ScriptSourceObject*> ReadBarrieredScriptSourceObject;
typedef ReadBarriered<Shape*> ReadBarrieredShape;
typedef ReadBarriered<jit::JitCode*> ReadBarrieredJitCode;
typedef ReadBarriered<ObjectGroup*> ReadBarrieredObjectGroup;
typedef ReadBarriered<JS::Symbol*> ReadBarrieredSymbol;
typedef ReadBarriered<WasmInstanceObject*> ReadBarrieredWasmInstanceObject;
typedef ReadBarriered<WasmTableObject*> ReadBarrieredWasmTableObject;

typedef ReadBarriered<Value> ReadBarrieredValue;

namespace detail {

template <typename T>
struct DefineComparisonOps<PreBarriered<T>> : mozilla::TrueType {
    static const T& get(const PreBarriered<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<GCPtr<T>> : mozilla::TrueType {
    static const T& get(const GCPtr<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<HeapPtr<T>> : mozilla::TrueType {
    static const T& get(const HeapPtr<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<ReadBarriered<T>> : mozilla::TrueType {
    static const T& get(const ReadBarriered<T>& v) { return v.unbarrieredGet(); }
};

template <>
struct DefineComparisonOps<HeapSlot> : mozilla::TrueType {
    static const Value& get(const HeapSlot& v) { return v.get(); }
};

} /* namespace detail */

} /* namespace js */

#endif /* gc_Barrier_h */

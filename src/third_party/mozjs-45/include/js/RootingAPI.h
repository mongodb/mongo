/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_RootingAPI_h
#define js_RootingAPI_h

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Move.h"
#include "mozilla/TypeTraits.h"

#include "jspubtd.h"

#include "js/GCAPI.h"
#include "js/HeapAPI.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"

/*
 * Moving GC Stack Rooting
 *
 * A moving GC may change the physical location of GC allocated things, even
 * when they are rooted, updating all pointers to the thing to refer to its new
 * location. The GC must therefore know about all live pointers to a thing,
 * not just one of them, in order to behave correctly.
 *
 * The |Rooted| and |Handle| classes below are used to root stack locations
 * whose value may be held live across a call that can trigger GC. For a
 * code fragment such as:
 *
 * JSObject* obj = NewObject(cx);
 * DoSomething(cx);
 * ... = obj->lastProperty();
 *
 * If |DoSomething()| can trigger a GC, the stack location of |obj| must be
 * rooted to ensure that the GC does not move the JSObject referred to by
 * |obj| without updating |obj|'s location itself. This rooting must happen
 * regardless of whether there are other roots which ensure that the object
 * itself will not be collected.
 *
 * If |DoSomething()| cannot trigger a GC, and the same holds for all other
 * calls made between |obj|'s definitions and its last uses, then no rooting
 * is required.
 *
 * SpiderMonkey can trigger a GC at almost any time and in ways that are not
 * always clear. For example, the following innocuous-looking actions can
 * cause a GC: allocation of any new GC thing; JSObject::hasProperty;
 * JS_ReportError and friends; and ToNumber, among many others. The following
 * dangerous-looking actions cannot trigger a GC: js_malloc, cx->malloc_,
 * rt->malloc_, and friends and JS_ReportOutOfMemory.
 *
 * The following family of three classes will exactly root a stack location.
 * Incorrect usage of these classes will result in a compile error in almost
 * all cases. Therefore, it is very hard to be incorrectly rooted if you use
 * these classes exclusively. These classes are all templated on the type T of
 * the value being rooted.
 *
 * - Rooted<T> declares a variable of type T, whose value is always rooted.
 *   Rooted<T> may be automatically coerced to a Handle<T>, below. Rooted<T>
 *   should be used whenever a local variable's value may be held live across a
 *   call which can trigger a GC.
 *
 * - Handle<T> is a const reference to a Rooted<T>. Functions which take GC
 *   things or values as arguments and need to root those arguments should
 *   generally use handles for those arguments and avoid any explicit rooting.
 *   This has two benefits. First, when several such functions call each other
 *   then redundant rooting of multiple copies of the GC thing can be avoided.
 *   Second, if the caller does not pass a rooted value a compile error will be
 *   generated, which is quicker and easier to fix than when relying on a
 *   separate rooting analysis.
 *
 * - MutableHandle<T> is a non-const reference to Rooted<T>. It is used in the
 *   same way as Handle<T> and includes a |set(const T& v)| method to allow
 *   updating the value of the referenced Rooted<T>. A MutableHandle<T> can be
 *   created with an implicit cast from a Rooted<T>*.
 *
 * In some cases the small performance overhead of exact rooting (measured to
 * be a few nanoseconds on desktop) is too much. In these cases, try the
 * following:
 *
 * - Move all Rooted<T> above inner loops: this allows you to re-use the root
 *   on each iteration of the loop.
 *
 * - Pass Handle<T> through your hot call stack to avoid re-rooting costs at
 *   every invocation.
 *
 * The following diagram explains the list of supported, implicit type
 * conversions between classes of this family:
 *
 *  Rooted<T> ----> Handle<T>
 *     |               ^
 *     |               |
 *     |               |
 *     +---> MutableHandle<T>
 *     (via &)
 *
 * All of these types have an implicit conversion to raw pointers.
 */

namespace js {

template <typename T>
struct GCMethods {
    static T initial() { return T(); }
};

template <typename T>
class RootedBase {};

template <typename T>
class HandleBase {};

template <typename T>
class MutableHandleBase {};

template <typename T>
class HeapBase {};

template <typename T>
class PersistentRootedBase {};

static void* const ConstNullValue = nullptr;

namespace gc {
struct Cell;
template<typename T>
struct PersistentRootedMarker;
} /* namespace gc */

#define DECLARE_POINTER_COMPARISON_OPS(T)                                                \
    bool operator==(const T& other) const { return get() == other; }                              \
    bool operator!=(const T& other) const { return get() != other; }

// Important: Return a reference so passing a Rooted<T>, etc. to
// something that takes a |const T&| is not a GC hazard.
#define DECLARE_POINTER_CONSTREF_OPS(T)                                                  \
    operator const T&() const { return get(); }                                                  \
    const T& operator->() const { return get(); }

// Assignment operators on a base class are hidden by the implicitly defined
// operator= on the derived class. Thus, define the operator= directly on the
// class as we would need to manually pass it through anyway.
#define DECLARE_POINTER_ASSIGN_OPS(Wrapper, T)                                                    \
    Wrapper<T>& operator=(const T& p) {                                                           \
        set(p);                                                                                   \
        return *this;                                                                             \
    }                                                                                             \
    Wrapper<T>& operator=(const Wrapper<T>& other) {                                              \
        set(other.get());                                                                         \
        return *this;                                                                             \
    }                                                                                             \

#define DELETE_ASSIGNMENT_OPS(Wrapper, T)                                                 \
    template <typename S> Wrapper<T>& operator=(S) = delete;                                      \
    Wrapper<T>& operator=(const Wrapper<T>&) = delete;

#define DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr)                                                  \
    const T* address() const { return &(ptr); }                                                   \
    const T& get() const { return (ptr); }                                                        \

#define DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(ptr)                                          \
    T* address() { return &(ptr); }                                                               \
    T& get() { return (ptr); }                                                                    \

} /* namespace js */

namespace JS {

template <typename T> class Rooted;
template <typename T> class PersistentRooted;

/* This is exposing internal state of the GC for inlining purposes. */
JS_FRIEND_API(bool) isGCEnabled();

JS_FRIEND_API(void) HeapObjectPostBarrier(JSObject** objp, JSObject* prev, JSObject* next);

#ifdef JS_DEBUG
/**
 * For generational GC, assert that an object is in the tenured generation as
 * opposed to being in the nursery.
 */
extern JS_FRIEND_API(void)
AssertGCThingMustBeTenured(JSObject* obj);
extern JS_FRIEND_API(void)
AssertGCThingIsNotAnObjectSubclass(js::gc::Cell* cell);
#else
inline void
AssertGCThingMustBeTenured(JSObject* obj) {}
inline void
AssertGCThingIsNotAnObjectSubclass(js::gc::Cell* cell) {}
#endif

/**
 * The Heap<T> class is a heap-stored reference to a JS GC thing. All members of
 * heap classes that refer to GC things should use Heap<T> (or possibly
 * TenuredHeap<T>, described below).
 *
 * Heap<T> is an abstraction that hides some of the complexity required to
 * maintain GC invariants for the contained reference. It uses operator
 * overloading to provide a normal pointer interface, but notifies the GC every
 * time the value it contains is updated. This is necessary for generational GC,
 * which keeps track of all pointers into the nursery.
 *
 * Heap<T> instances must be traced when their containing object is traced to
 * keep the pointed-to GC thing alive.
 *
 * Heap<T> objects should only be used on the heap. GC references stored on the
 * C/C++ stack must use Rooted/Handle/MutableHandle instead.
 *
 * Type T must be one of: JS::Value, jsid, JSObject*, JSString*, JSScript*
 */
template <typename T>
class Heap : public js::HeapBase<T>
{
  public:
    Heap() {
        static_assert(sizeof(T) == sizeof(Heap<T>),
                      "Heap<T> must be binary compatible with T.");
        init(js::GCMethods<T>::initial());
    }
    explicit Heap(T p) { init(p); }

    /*
     * For Heap, move semantics are equivalent to copy semantics. In C++, a
     * copy constructor taking const-ref is the way to get a single function
     * that will be used for both lvalue and rvalue copies, so we can simply
     * omit the rvalue variant.
     */
    explicit Heap(const Heap<T>& p) { init(p.ptr); }

    ~Heap() {
        post(ptr, js::GCMethods<T>::initial());
    }

    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_POINTER_ASSIGN_OPS(Heap, T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr);

    T* unsafeGet() { return &ptr; }

    /*
     * Set the pointer to a value which will cause a crash if it is
     * dereferenced.
     */
    void setToCrashOnTouch() {
        ptr = reinterpret_cast<T>(crashOnTouchPointer);
    }

    bool isSetToCrashOnTouch() {
        return ptr == crashOnTouchPointer;
    }

  private:
    void init(T newPtr) {
        ptr = newPtr;
        post(js::GCMethods<T>::initial(), ptr);
    }

    void set(T newPtr) {
        T tmp = ptr;
        ptr = newPtr;
        post(tmp, ptr);
    }

    void post(const T& prev, const T& next) {
        js::GCMethods<T>::postBarrier(&ptr, prev, next);
    }

    enum {
        crashOnTouchPointer = 1
    };

    T ptr;
};

/**
 * The TenuredHeap<T> class is similar to the Heap<T> class above in that it
 * encapsulates the GC concerns of an on-heap reference to a JS object. However,
 * it has two important differences:
 *
 *  1) Pointers which are statically known to only reference "tenured" objects
 *     can avoid the extra overhead of SpiderMonkey's write barriers.
 *
 *  2) Objects in the "tenured" heap have stronger alignment restrictions than
 *     those in the "nursery", so it is possible to store flags in the lower
 *     bits of pointers known to be tenured. TenuredHeap wraps a normal tagged
 *     pointer with a nice API for accessing the flag bits and adds various
 *     assertions to ensure that it is not mis-used.
 *
 * GC things are said to be "tenured" when they are located in the long-lived
 * heap: e.g. they have gained tenure as an object by surviving past at least
 * one GC. For performance, SpiderMonkey allocates some things which are known
 * to normally be long lived directly into the tenured generation; for example,
 * global objects. Additionally, SpiderMonkey does not visit individual objects
 * when deleting non-tenured objects, so object with finalizers are also always
 * tenured; for instance, this includes most DOM objects.
 *
 * The considerations to keep in mind when using a TenuredHeap<T> vs a normal
 * Heap<T> are:
 *
 *  - It is invalid for a TenuredHeap<T> to refer to a non-tenured thing.
 *  - It is however valid for a Heap<T> to refer to a tenured thing.
 *  - It is not possible to store flag bits in a Heap<T>.
 */
template <typename T>
class TenuredHeap : public js::HeapBase<T>
{
  public:
    TenuredHeap() : bits(0) {
        static_assert(sizeof(T) == sizeof(TenuredHeap<T>),
                      "TenuredHeap<T> must be binary compatible with T.");
    }
    explicit TenuredHeap(T p) : bits(0) { setPtr(p); }
    explicit TenuredHeap(const TenuredHeap<T>& p) : bits(0) { setPtr(p.getPtr()); }

    bool operator==(const TenuredHeap<T>& other) { return bits == other.bits; }
    bool operator!=(const TenuredHeap<T>& other) { return bits != other.bits; }

    void setPtr(T newPtr) {
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(newPtr) & flagsMask) == 0);
        if (newPtr)
            AssertGCThingMustBeTenured(newPtr);
        bits = (bits & flagsMask) | reinterpret_cast<uintptr_t>(newPtr);
    }

    void setFlags(uintptr_t flagsToSet) {
        MOZ_ASSERT((flagsToSet & ~flagsMask) == 0);
        bits |= flagsToSet;
    }

    void unsetFlags(uintptr_t flagsToUnset) {
        MOZ_ASSERT((flagsToUnset & ~flagsMask) == 0);
        bits &= ~flagsToUnset;
    }

    bool hasFlag(uintptr_t flag) const {
        MOZ_ASSERT((flag & ~flagsMask) == 0);
        return (bits & flag) != 0;
    }

    T getPtr() const { return reinterpret_cast<T>(bits & ~flagsMask); }
    uintptr_t getFlags() const { return bits & flagsMask; }

    operator T() const { return getPtr(); }
    T operator->() const { return getPtr(); }

    TenuredHeap<T>& operator=(T p) {
        setPtr(p);
        return *this;
    }

    TenuredHeap<T>& operator=(const TenuredHeap<T>& other) {
        bits = other.bits;
        return *this;
    }

  private:
    enum {
        maskBits = 3,
        flagsMask = (1 << maskBits) - 1,
    };

    uintptr_t bits;
};

/**
 * Reference to a T that has been rooted elsewhere. This is most useful
 * as a parameter type, which guarantees that the T lvalue is properly
 * rooted. See "Move GC Stack Rooting" above.
 *
 * If you want to add additional methods to Handle for a specific
 * specialization, define a HandleBase<T> specialization containing them.
 */
template <typename T>
class MOZ_NONHEAP_CLASS Handle : public js::HandleBase<T>
{
    friend class JS::MutableHandle<T>;

  public:
    /* Creates a handle from a handle of a type convertible to T. */
    template <typename S>
    MOZ_IMPLICIT Handle(Handle<S> handle,
                        typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0)
    {
        static_assert(sizeof(Handle<T>) == sizeof(T*),
                      "Handle must be binary compatible with T*.");
        ptr = reinterpret_cast<const T*>(handle.address());
    }

    MOZ_IMPLICIT Handle(decltype(nullptr)) {
        static_assert(mozilla::IsPointer<T>::value,
                      "nullptr_t overload not valid for non-pointer types");
        ptr = reinterpret_cast<const T*>(&js::ConstNullValue);
    }

    MOZ_IMPLICIT Handle(MutableHandle<T> handle) {
        ptr = handle.address();
    }

    /*
     * Take care when calling this method!
     *
     * This creates a Handle from the raw location of a T.
     *
     * It should be called only if the following conditions hold:
     *
     *  1) the location of the T is guaranteed to be marked (for some reason
     *     other than being a Rooted), e.g., if it is guaranteed to be reachable
     *     from an implicit root.
     *
     *  2) the contents of the location are immutable, or at least cannot change
     *     for the lifetime of the handle, as its users may not expect its value
     *     to change underneath them.
     */
    static MOZ_CONSTEXPR Handle fromMarkedLocation(const T* p) {
        return Handle(p, DeliberatelyChoosingThisOverload,
                      ImUsingThisOnlyInFromFromMarkedLocation);
    }

    /*
     * Construct a handle from an explicitly rooted location. This is the
     * normal way to create a handle, and normally happens implicitly.
     */
    template <typename S>
    inline
    MOZ_IMPLICIT Handle(const Rooted<S>& root,
                        typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0);

    template <typename S>
    inline
    MOZ_IMPLICIT Handle(const PersistentRooted<S>& root,
                        typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0);

    /* Construct a read only handle from a mutable handle. */
    template <typename S>
    inline
    MOZ_IMPLICIT Handle(MutableHandle<S>& root,
                        typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy = 0);

    DECLARE_POINTER_COMPARISON_OPS(T);
    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(*ptr);

  private:
    Handle() {}
    DELETE_ASSIGNMENT_OPS(Handle, T);

    enum Disambiguator { DeliberatelyChoosingThisOverload = 42 };
    enum CallerIdentity { ImUsingThisOnlyInFromFromMarkedLocation = 17 };
    MOZ_CONSTEXPR Handle(const T* p, Disambiguator, CallerIdentity) : ptr(p) {}

    const T* ptr;
};

/**
 * Similar to a handle, but the underlying storage can be changed. This is
 * useful for outparams.
 *
 * If you want to add additional methods to MutableHandle for a specific
 * specialization, define a MutableHandleBase<T> specialization containing
 * them.
 */
template <typename T>
class MOZ_STACK_CLASS MutableHandle : public js::MutableHandleBase<T>
{
  public:
    inline MOZ_IMPLICIT MutableHandle(Rooted<T>* root);
    inline MOZ_IMPLICIT MutableHandle(PersistentRooted<T>* root);

  private:
    // Disallow nullptr for overloading purposes.
    MutableHandle(decltype(nullptr)) = delete;

  public:
    void set(T v) {
        *ptr = v;
    }

    /*
     * This may be called only if the location of the T is guaranteed
     * to be marked (for some reason other than being a Rooted),
     * e.g., if it is guaranteed to be reachable from an implicit root.
     *
     * Create a MutableHandle from a raw location of a T.
     */
    static MutableHandle fromMarkedLocation(T* p) {
        MutableHandle h;
        h.ptr = p;
        return h;
    }

    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(*ptr);
    DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(*ptr);

  private:
    MutableHandle() {}
    DELETE_ASSIGNMENT_OPS(MutableHandle, T);

    T* ptr;
};

} /* namespace JS */

namespace js {

/**
 * By default, things should use the inheritance hierarchy to find their
 * ThingRootKind. Some pointer types are explicitly set in jspubtd.h so that
 * Rooted<T> may be used without the class definition being available.
 */
template <typename T>
struct RootKind
{
    static ThingRootKind rootKind() { return T::rootKind(); }
};

template <typename T>
struct RootKind<T*>
{
    static ThingRootKind rootKind() { return T::rootKind(); }
};

template <typename T>
struct GCMethods<T*>
{
    static T* initial() { return nullptr; }
    static void postBarrier(T** vp, T* prev, T* next) {
        if (next)
            JS::AssertGCThingIsNotAnObjectSubclass(reinterpret_cast<js::gc::Cell*>(next));
    }
    static void relocate(T** vp) {}
};

template <>
struct GCMethods<JSObject*>
{
    static JSObject* initial() { return nullptr; }
    static gc::Cell* asGCThingOrNull(JSObject* v) {
        if (!v)
            return nullptr;
        MOZ_ASSERT(uintptr_t(v) > 32);
        return reinterpret_cast<gc::Cell*>(v);
    }
    static void postBarrier(JSObject** vp, JSObject* prev, JSObject* next) {
        JS::HeapObjectPostBarrier(vp, prev, next);
    }
};

template <>
struct GCMethods<JSFunction*>
{
    static JSFunction* initial() { return nullptr; }
    static void postBarrier(JSFunction** vp, JSFunction* prev, JSFunction* next) {
        JS::HeapObjectPostBarrier(reinterpret_cast<JSObject**>(vp),
                                  reinterpret_cast<JSObject*>(prev),
                                  reinterpret_cast<JSObject*>(next));
    }
};

// Provide hash codes for Cell kinds that may be relocated and, thus, not have
// a stable address to use as the base for a hash code. Instead of the address,
// this hasher uses Cell::getUniqueId to provide exact matches and as a base
// for generating hash codes.
//
// Note: this hasher, like PointerHasher can "hash" a nullptr. While a nullptr
// would not likely be a useful key, there are some cases where being able to
// hash a nullptr is useful, either on purpose or because of bugs:
// (1) existence checks where the key may happen to be null and (2) some
// aggregate Lookup kinds embed a JSObject* that is frequently null and do not
// null test before dispatching to the hasher.
template <typename T>
struct JS_PUBLIC_API(MovableCellHasher)
{
    using Key = T;
    using Lookup = T;

    static HashNumber hash(const Lookup& l);
    static bool match(const Key& k, const Lookup& l);
    static void rekey(Key& k, const Key& newKey) { k = newKey; }
};

template <typename T>
struct JS_PUBLIC_API(MovableCellHasher<JS::Heap<T>>)
{
    using Key = JS::Heap<T>;
    using Lookup = T;

    static HashNumber hash(const Lookup& l) { return MovableCellHasher<T>::hash(l); }
    static bool match(const Key& k, const Lookup& l) { return MovableCellHasher<T>::match(k, l); }
    static void rekey(Key& k, const Key& newKey) { k.unsafeSet(newKey); }
};

} /* namespace js */

namespace JS {

// Non pointer types -- structs or classes that contain GC pointers, either as
// a member or in a more complex container layout -- can also be stored in a
// [Persistent]Rooted if it derives from JS::Traceable. A JS::Traceable stored
// in a [Persistent]Rooted must implement the method:
//     |static void trace(T*, JSTracer*)|
class Traceable
{
  public:
    static js::ThingRootKind rootKind() { return js::THING_ROOT_TRACEABLE; }
};

} /* namespace JS */

namespace js {

template <typename T>
class DispatchWrapper
{
    static_assert(mozilla::IsBaseOf<JS::Traceable, T>::value,
                  "DispatchWrapper is intended only for usage with a Traceable");

    using TraceFn = void (*)(T*, JSTracer*);
    TraceFn tracer;
#if JS_BITS_PER_WORD == 32
    uint32_t padding; // Ensure the storage fields have CellSize alignment.
#endif
    T storage;

  public:
    template <typename U>
    MOZ_IMPLICIT DispatchWrapper(U&& initial)
      : tracer(&T::trace),
        storage(mozilla::Forward<U>(initial))
    { }

    // Mimic a pointer type, so that we can drop into Rooted.
    T* operator &() { return &storage; }
    const T* operator &() const { return &storage; }
    operator T&() { return storage; }
    operator const T&() const { return storage; }

    // Trace the contained storage (of unknown type) using the trace function
    // we set aside when we did know the type.
    static void TraceWrapped(JSTracer* trc, JS::Traceable* thingp, const char* name) {
        auto wrapper = reinterpret_cast<DispatchWrapper*>(
                           uintptr_t(thingp) - offsetof(DispatchWrapper, storage));
        wrapper->tracer(&wrapper->storage, trc);
    }
};

inline RootLists&
RootListsForRootingContext(JSContext* cx)
{
    return ContextFriendFields::get(cx)->roots;
}

inline RootLists&
RootListsForRootingContext(js::ContextFriendFields* cx)
{
    return cx->roots;
}

inline RootLists&
RootListsForRootingContext(JSRuntime* rt)
{
    return PerThreadDataFriendFields::getMainThread(rt)->roots;
}

inline RootLists&
RootListsForRootingContext(js::PerThreadDataFriendFields* pt)
{
    return pt->roots;
}

} /* namespace js */

namespace JS {

/**
 * Local variable of type T whose value is always rooted. This is typically
 * used for local variables, or for non-rooted values being passed to a
 * function that requires a handle, e.g. Foo(Root<T>(cx, x)).
 *
 * If you want to add additional methods to Rooted for a specific
 * specialization, define a RootedBase<T> specialization containing them.
 */
template <typename T>
class MOZ_RAII Rooted : public js::RootedBase<T>
{
    static_assert(!mozilla::IsConvertible<T, Traceable*>::value,
                  "Rooted takes pointer or Traceable types but not Traceable* type");

    /* Note: CX is a subclass of either ContextFriendFields or PerThreadDataFriendFields. */
    void registerWithRootLists(js::RootLists& roots) {
        js::ThingRootKind kind = js::RootKind<T>::rootKind();
        this->stack = &roots.stackRoots_[kind];
        this->prev = *stack;
        *stack = reinterpret_cast<Rooted<void*>*>(this);
    }

  public:
    template <typename RootingContext>
    explicit Rooted(const RootingContext& cx)
      : ptr(js::GCMethods<T>::initial())
    {
        registerWithRootLists(js::RootListsForRootingContext(cx));
    }

    template <typename RootingContext, typename S>
    Rooted(const RootingContext& cx, S&& initial)
      : ptr(mozilla::Forward<S>(initial))
    {
        registerWithRootLists(js::RootListsForRootingContext(cx));
    }

    ~Rooted() {
        MOZ_ASSERT(*stack == reinterpret_cast<Rooted<void*>*>(this));
        *stack = prev;
    }

    Rooted<T>* previous() { return reinterpret_cast<Rooted<T>*>(prev); }

    /*
     * This method is public for Rooted so that Codegen.py can use a Rooted
     * interchangeably with a MutableHandleValue.
     */
    void set(T value) {
        ptr = value;
    }

    DECLARE_POINTER_COMPARISON_OPS(T);
    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_POINTER_ASSIGN_OPS(Rooted, T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr);
    DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(ptr);

  private:
    /*
     * These need to be templated on void* to avoid aliasing issues between, for
     * example, Rooted<JSObject> and Rooted<JSFunction>, which use the same
     * stack head pointer for different classes.
     */
    Rooted<void*>** stack;
    Rooted<void*>* prev;

    /*
     * For pointer types, the TraceKind for tracing is based on the list it is
     * in (selected via rootKind), so no additional storage is required here.
     * All Traceable, however, share the same list, so the function to
     * call for tracing is stored adjacent to the struct. Since C++ cannot
     * templatize on storage class, this is implemented via the wrapper class
     * DispatchWrapper.
     */
    using MaybeWrapped = typename mozilla::Conditional<
        mozilla::IsBaseOf<Traceable, T>::value,
        js::DispatchWrapper<T>,
        T>::Type;
    MaybeWrapped ptr;

    Rooted(const Rooted&) = delete;
};

} /* namespace JS */

namespace js {

/**
 * Augment the generic Rooted<T> interface when T = JSObject* with
 * class-querying and downcasting operations.
 *
 * Given a Rooted<JSObject*> obj, one can view
 *   Handle<StringObject*> h = obj.as<StringObject*>();
 * as an optimization of
 *   Rooted<StringObject*> rooted(cx, &obj->as<StringObject*>());
 *   Handle<StringObject*> h = rooted;
 */
template <>
class RootedBase<JSObject*>
{
  public:
    template <class U>
    JS::Handle<U*> as() const;
};

/**
 * Augment the generic Handle<T> interface when T = JSObject* with
 * downcasting operations.
 *
 * Given a Handle<JSObject*> obj, one can view
 *   Handle<StringObject*> h = obj.as<StringObject*>();
 * as an optimization of
 *   Rooted<StringObject*> rooted(cx, &obj->as<StringObject*>());
 *   Handle<StringObject*> h = rooted;
 */
template <>
class HandleBase<JSObject*>
{
  public:
    template <class U>
    JS::Handle<U*> as() const;
};

/** Interface substitute for Rooted<T> which does not root the variable's memory. */
template <typename T>
class MOZ_RAII FakeRooted : public RootedBase<T>
{
  public:
    template <typename CX>
    explicit FakeRooted(CX* cx) : ptr(GCMethods<T>::initial()) {}

    template <typename CX>
    FakeRooted(CX* cx, T initial) : ptr(initial) {}

    DECLARE_POINTER_COMPARISON_OPS(T);
    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_POINTER_ASSIGN_OPS(FakeRooted, T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr);
    DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(ptr);

  private:
    T ptr;

    void set(const T& value) {
        ptr = value;
    }

    FakeRooted(const FakeRooted&) = delete;
};

/** Interface substitute for MutableHandle<T> which is not required to point to rooted memory. */
template <typename T>
class FakeMutableHandle : public js::MutableHandleBase<T>
{
  public:
    MOZ_IMPLICIT FakeMutableHandle(T* t) {
        ptr = t;
    }

    MOZ_IMPLICIT FakeMutableHandle(FakeRooted<T>* root) {
        ptr = root->address();
    }

    void set(T v) {
        *ptr = v;
    }

    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(*ptr);
    DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(*ptr);

  private:
    FakeMutableHandle() {}
    DELETE_ASSIGNMENT_OPS(FakeMutableHandle, T);

    T* ptr;
};

/**
 * Types for a variable that either should or shouldn't be rooted, depending on
 * the template parameter allowGC. Used for implementing functions that can
 * operate on either rooted or unrooted data.
 *
 * The toHandle() and toMutableHandle() functions are for calling functions
 * which require handle types and are only called in the CanGC case. These
 * allow the calling code to type check.
 */
enum AllowGC {
    NoGC = 0,
    CanGC = 1
};
template <typename T, AllowGC allowGC>
class MaybeRooted
{
};

template <typename T> class MaybeRooted<T, CanGC>
{
  public:
    typedef JS::Handle<T> HandleType;
    typedef JS::Rooted<T> RootType;
    typedef JS::MutableHandle<T> MutableHandleType;

    static inline JS::Handle<T> toHandle(HandleType v) {
        return v;
    }

    static inline JS::MutableHandle<T> toMutableHandle(MutableHandleType v) {
        return v;
    }

    template <typename T2>
    static inline JS::Handle<T2*> downcastHandle(HandleType v) {
        return v.template as<T2>();
    }
};

template <typename T> class MaybeRooted<T, NoGC>
{
  public:
    typedef T HandleType;
    typedef FakeRooted<T> RootType;
    typedef FakeMutableHandle<T> MutableHandleType;

    static JS::Handle<T> toHandle(HandleType v) {
        MOZ_CRASH("Bad conversion");
    }

    static JS::MutableHandle<T> toMutableHandle(MutableHandleType v) {
        MOZ_CRASH("Bad conversion");
    }

    template <typename T2>
    static inline T2* downcastHandle(HandleType v) {
        return &v->template as<T2>();
    }
};

} /* namespace js */

namespace JS {

template <typename T> template <typename S>
inline
Handle<T>::Handle(const Rooted<S>& root,
                  typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy)
{
    ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T> template <typename S>
inline
Handle<T>::Handle(const PersistentRooted<S>& root,
                  typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy)
{
    ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T> template <typename S>
inline
Handle<T>::Handle(MutableHandle<S>& root,
                  typename mozilla::EnableIf<mozilla::IsConvertible<S, T>::value, int>::Type dummy)
{
    ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T>
inline
MutableHandle<T>::MutableHandle(Rooted<T>* root)
{
    static_assert(sizeof(MutableHandle<T>) == sizeof(T*),
                  "MutableHandle must be binary compatible with T*.");
    ptr = root->address();
}

template <typename T>
inline
MutableHandle<T>::MutableHandle(PersistentRooted<T>* root)
{
    static_assert(sizeof(MutableHandle<T>) == sizeof(T*),
                  "MutableHandle must be binary compatible with T*.");
    ptr = root->address();
}

/**
 * A copyable, assignable global GC root type with arbitrary lifetime, an
 * infallible constructor, and automatic unrooting on destruction.
 *
 * These roots can be used in heap-allocated data structures, so they are not
 * associated with any particular JSContext or stack. They are registered with
 * the JSRuntime itself, without locking, so they require a full JSContext to be
 * initialized, not one of its more restricted superclasses.  Initialization may
 * take place on construction, or in two phases if the no-argument constructor
 * is called followed by init().
 *
 * Note that you must not use an PersistentRooted in an object owned by a JS
 * object:
 *
 * Whenever one object whose lifetime is decided by the GC refers to another
 * such object, that edge must be traced only if the owning JS object is traced.
 * This applies not only to JS objects (which obviously are managed by the GC)
 * but also to C++ objects owned by JS objects.
 *
 * If you put a PersistentRooted in such a C++ object, that is almost certainly
 * a leak. When a GC begins, the referent of the PersistentRooted is treated as
 * live, unconditionally (because a PersistentRooted is a *root*), even if the
 * JS object that owns it is unreachable. If there is any path from that
 * referent back to the JS object, then the C++ object containing the
 * PersistentRooted will not be destructed, and the whole blob of objects will
 * not be freed, even if there are no references to them from the outside.
 *
 * In the context of Firefox, this is a severe restriction: almost everything in
 * Firefox is owned by some JS object or another, so using PersistentRooted in
 * such objects would introduce leaks. For these kinds of edges, Heap<T> or
 * TenuredHeap<T> would be better types. It's up to the implementor of the type
 * containing Heap<T> or TenuredHeap<T> members to make sure their referents get
 * marked when the object itself is marked.
 */
template<typename T>
class PersistentRooted : public js::PersistentRootedBase<T>,
                         private mozilla::LinkedListElement<PersistentRooted<T>>
{
    typedef mozilla::LinkedListElement<PersistentRooted<T>> ListBase;

    friend class mozilla::LinkedList<PersistentRooted>;
    friend class mozilla::LinkedListElement<PersistentRooted>;

    friend struct js::gc::PersistentRootedMarker<T>;

    friend void js::gc::FinishPersistentRootedChains(js::RootLists&);

    void registerWithRootLists(js::RootLists& roots) {
        MOZ_ASSERT(!initialized());
        js::ThingRootKind kind = js::RootKind<T>::rootKind();
        roots.heapRoots_[kind].insertBack(reinterpret_cast<JS::PersistentRooted<void*>*>(this));
        // Until marking and destruction support the full set, we assert that
        // we don't try to add any unsupported types.
        MOZ_ASSERT(kind == js::THING_ROOT_OBJECT ||
                   kind == js::THING_ROOT_SCRIPT ||
                   kind == js::THING_ROOT_STRING ||
                   kind == js::THING_ROOT_ID ||
                   kind == js::THING_ROOT_VALUE ||
                   kind == js::THING_ROOT_TRACEABLE);
    }

  public:
    PersistentRooted() : ptr(js::GCMethods<T>::initial()) {}

    template <typename RootingContext>
    explicit PersistentRooted(const RootingContext& cx)
      : ptr(js::GCMethods<T>::initial())
    {
        registerWithRootLists(js::RootListsForRootingContext(cx));
    }

    template <typename RootingContext, typename U>
    PersistentRooted(const RootingContext& cx, U&& initial)
      : ptr(mozilla::Forward<U>(initial))
    {
        registerWithRootLists(js::RootListsForRootingContext(cx));
    }

    PersistentRooted(const PersistentRooted& rhs)
      : mozilla::LinkedListElement<PersistentRooted<T>>(),
        ptr(rhs.ptr)
    {
        /*
         * Copy construction takes advantage of the fact that the original
         * is already inserted, and simply adds itself to whatever list the
         * original was on - no JSRuntime pointer needed.
         *
         * This requires mutating rhs's links, but those should be 'mutable'
         * anyway. C++ doesn't let us declare mutable base classes.
         */
        const_cast<PersistentRooted&>(rhs).setNext(this);
    }

    bool initialized() {
        return ListBase::isInList();
    }

    template <typename RootingContext>
    void init(const RootingContext& cx) {
        init(cx, js::GCMethods<T>::initial());
    }

    template <typename RootingContext, typename U>
    void init(const RootingContext& cx, U&& initial) {
        ptr = mozilla::Forward<U>(initial);
        registerWithRootLists(js::RootListsForRootingContext(cx));
    }

    void reset() {
        if (initialized()) {
            set(js::GCMethods<T>::initial());
            ListBase::remove();
        }
    }

    DECLARE_POINTER_COMPARISON_OPS(T);
    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_POINTER_ASSIGN_OPS(PersistentRooted, T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr);

    // These are the same as DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS, except
    // they check that |this| is initialized in case the caller later stores
    // something in |ptr|.
    T* address() {
        MOZ_ASSERT(initialized());
        return &ptr;
    }
    T& get() {
        MOZ_ASSERT(initialized());
        return ptr;
    }

  private:
    void set(T value) {
        MOZ_ASSERT(initialized());
        ptr = value;
    }

    // See the comment above Rooted::ptr.
    using MaybeWrapped = typename mozilla::Conditional<
        mozilla::IsBaseOf<Traceable, T>::value,
        js::DispatchWrapper<T>,
        T>::Type;

    MaybeWrapped ptr;
};

class JS_PUBLIC_API(ObjectPtr)
{
    Heap<JSObject*> value;

  public:
    ObjectPtr() : value(nullptr) {}

    explicit ObjectPtr(JSObject* obj) : value(obj) {}

    /* Always call finalize before the destructor. */
    ~ObjectPtr() { MOZ_ASSERT(!value); }

    void finalize(JSRuntime* rt) {
        if (IsIncrementalBarrierNeeded(rt))
            IncrementalObjectBarrier(value);
        value = nullptr;
    }

    void init(JSObject* obj) { value = obj; }

    JSObject* get() const { return value; }

    void writeBarrierPre(JSRuntime* rt) {
        IncrementalObjectBarrier(value);
    }

    void updateWeakPointerAfterGC();

    ObjectPtr& operator=(JSObject* obj) {
        IncrementalObjectBarrier(value);
        value = obj;
        return *this;
    }

    void trace(JSTracer* trc, const char* name);

    JSObject& operator*() const { return *value; }
    JSObject* operator->() const { return value; }
    operator JSObject*() const { return value; }
};

} /* namespace JS */

namespace js {
namespace gc {

template <typename T, typename TraceCallbacks>
void
CallTraceCallbackOnNonHeap(T* v, const TraceCallbacks& aCallbacks, const char* aName, void* aClosure)
{
    static_assert(sizeof(T) == sizeof(JS::Heap<T>), "T and Heap<T> must be compatible.");
    MOZ_ASSERT(v);
    mozilla::DebugOnly<Cell*> cell = GCMethods<T>::asGCThingOrNull(*v);
    MOZ_ASSERT(cell);
    MOZ_ASSERT(!IsInsideNursery(cell));
    JS::Heap<T>* asHeapT = reinterpret_cast<JS::Heap<T>*>(v);
    aCallbacks.Trace(asHeapT, aName, aClosure);
}

} /* namespace gc */
} /* namespace js */

// mozilla::Swap uses a stack temporary, which prevents classes like Heap<T>
// from being declared MOZ_HEAP_CLASS.
namespace mozilla {

template <typename T>
inline void
Swap(JS::Heap<T>& aX, JS::Heap<T>& aY)
{
    T tmp = aX;
    aX = aY;
    aY = tmp;
}

template <typename T>
inline void
Swap(JS::TenuredHeap<T>& aX, JS::TenuredHeap<T>& aY)
{
    T tmp = aX;
    aX = aY;
    aY = tmp;
}

} /* namespace mozilla */

#undef DELETE_ASSIGNMENT_OPS

#endif  /* js_RootingAPI_h */

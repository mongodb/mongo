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

#include <type_traits>

#include "jspubtd.h"

#include "js/GCAnnotations.h"
#include "js/GCPolicyAPI.h"
#include "js/HeapAPI.h"
#include "js/ProfilingStack.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
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
struct BarrierMethods {
};

template <typename Element, typename Wrapper>
class WrappedPtrOperations {};

template <typename Element, typename Wrapper>
class MutableWrappedPtrOperations : public WrappedPtrOperations<Element, Wrapper> {};

template <typename T, typename Wrapper>
class RootedBase : public MutableWrappedPtrOperations<T, Wrapper> {};

template <typename T, typename Wrapper>
class HandleBase : public WrappedPtrOperations<T, Wrapper> {};

template <typename T, typename Wrapper>
class MutableHandleBase : public MutableWrappedPtrOperations<T, Wrapper> {};

template <typename T, typename Wrapper>
class HeapBase : public MutableWrappedPtrOperations<T, Wrapper> {};

// Cannot use FOR_EACH_HEAP_ABLE_GC_POINTER_TYPE, as this would import too many macros into scope
template <typename T> struct IsHeapConstructibleType    { static constexpr bool value = false; };
#define DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE(T) \
    template <> struct IsHeapConstructibleType<T> { static constexpr bool value = true; };
FOR_EACH_PUBLIC_GC_POINTER_TYPE(DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE)
FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE)
#undef DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE

template <typename T, typename Wrapper>
class PersistentRootedBase : public MutableWrappedPtrOperations<T, Wrapper> {};

template <typename T>
class FakeRooted;

template <typename T>
class FakeMutableHandle;

namespace gc {
struct Cell;
template<typename T>
struct PersistentRootedMarker;
} /* namespace gc */

// Important: Return a reference so passing a Rooted<T>, etc. to
// something that takes a |const T&| is not a GC hazard.
#define DECLARE_POINTER_CONSTREF_OPS(T)                                                           \
    operator const T&() const { return get(); }                                                   \
    const T& operator->() const { return get(); }

// Assignment operators on a base class are hidden by the implicitly defined
// operator= on the derived class. Thus, define the operator= directly on the
// class as we would need to manually pass it through anyway.
#define DECLARE_POINTER_ASSIGN_OPS(Wrapper, T)                                                    \
    Wrapper<T>& operator=(const T& p) {                                                           \
        set(p);                                                                                   \
        return *this;                                                                             \
    }                                                                                             \
    Wrapper<T>& operator=(T&& p) {                                                                \
        set(mozilla::Move(p));                                                                    \
        return *this;                                                                             \
    }                                                                                             \
    Wrapper<T>& operator=(const Wrapper<T>& other) {                                              \
        set(other.get());                                                                         \
        return *this;                                                                             \
    }                                                                                             \

#define DELETE_ASSIGNMENT_OPS(Wrapper, T)                                                         \
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
JS_FRIEND_API(void) HeapStringPostBarrier(JSString** objp, JSString* prev, JSString* next);

#ifdef JS_DEBUG
/**
 * For generational GC, assert that an object is in the tenured generation as
 * opposed to being in the nursery.
 */
extern JS_FRIEND_API(void)
AssertGCThingMustBeTenured(JSObject* obj);
extern JS_FRIEND_API(void)
AssertGCThingIsNotNurseryAllocable(js::gc::Cell* cell);
#else
inline void
AssertGCThingMustBeTenured(JSObject* obj) {}
inline void
AssertGCThingIsNotNurseryAllocable(js::gc::Cell* cell) {}
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
 * Type T must be a public GC pointer type.
 */
template <typename T>
class MOZ_NON_MEMMOVABLE Heap : public js::HeapBase<T, Heap<T>>
{
    // Please note: this can actually also be used by nsXBLMaybeCompiled<T>, for legacy reasons.
    static_assert(js::IsHeapConstructibleType<T>::value,
                  "Type T must be a public GC pointer type");
  public:
    using ElementType = T;

    Heap() {
        static_assert(sizeof(T) == sizeof(Heap<T>),
                      "Heap<T> must be binary compatible with T.");
        init(GCPolicy<T>::initial());
    }
    explicit Heap(const T& p) { init(p); }

    /*
     * For Heap, move semantics are equivalent to copy semantics. In C++, a
     * copy constructor taking const-ref is the way to get a single function
     * that will be used for both lvalue and rvalue copies, so we can simply
     * omit the rvalue variant.
     */
    explicit Heap(const Heap<T>& p) { init(p.ptr); }

    ~Heap() {
        post(ptr, GCPolicy<T>::initial());
    }

    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_POINTER_ASSIGN_OPS(Heap, T);

    const T* address() const { return &ptr; }

    void exposeToActiveJS() const {
        js::BarrierMethods<T>::exposeToJS(ptr);
    }
    const T& get() const {
        exposeToActiveJS();
        return ptr;
    }
    const T& unbarrieredGet() const {
        return ptr;
    }

    T* unsafeGet() { return &ptr; }

    explicit operator bool() const {
        return bool(js::BarrierMethods<T>::asGCThingOrNull(ptr));
    }
    explicit operator bool() {
        return bool(js::BarrierMethods<T>::asGCThingOrNull(ptr));
    }

  private:
    void init(const T& newPtr) {
        ptr = newPtr;
        post(GCPolicy<T>::initial(), ptr);
    }

    void set(const T& newPtr) {
        T tmp = ptr;
        ptr = newPtr;
        post(tmp, ptr);
    }

    void post(const T& prev, const T& next) {
        js::BarrierMethods<T>::postBarrier(&ptr, prev, next);
    }

    T ptr;
};

static MOZ_ALWAYS_INLINE bool
ObjectIsTenured(JSObject* obj)
{
    return !js::gc::IsInsideNursery(reinterpret_cast<js::gc::Cell*>(obj));
}

static MOZ_ALWAYS_INLINE bool
ObjectIsTenured(const Heap<JSObject*>& obj)
{
    return ObjectIsTenured(obj.unbarrieredGet());
}

static MOZ_ALWAYS_INLINE bool
ObjectIsMarkedGray(JSObject* obj)
{
    auto cell = reinterpret_cast<js::gc::Cell*>(obj);
    return js::gc::detail::CellIsMarkedGrayIfKnown(cell);
}

static MOZ_ALWAYS_INLINE bool
ObjectIsMarkedGray(const JS::Heap<JSObject*>& obj)
{
    return ObjectIsMarkedGray(obj.unbarrieredGet());
}

// The following *IsNotGray functions are for use in assertions and take account
// of the eventual gray marking state at the end of any ongoing incremental GC.
#ifdef DEBUG
inline bool
CellIsNotGray(js::gc::Cell* maybeCell)
{
    if (!maybeCell)
        return true;

    return js::gc::detail::CellIsNotGray(maybeCell);
}

inline bool
ObjectIsNotGray(JSObject* maybeObj)
{
    return CellIsNotGray(reinterpret_cast<js::gc::Cell*>(maybeObj));
}

inline bool
ObjectIsNotGray(const JS::Heap<JSObject*>& obj)
{
    return ObjectIsNotGray(obj.unbarrieredGet());
}
#endif

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
class TenuredHeap : public js::HeapBase<T, TenuredHeap<T>>
{
  public:
    using ElementType = T;

    TenuredHeap() : bits(0) {
        static_assert(sizeof(T) == sizeof(TenuredHeap<T>),
                      "TenuredHeap<T> must be binary compatible with T.");
    }
    explicit TenuredHeap(T p) : bits(0) { setPtr(p); }
    explicit TenuredHeap(const TenuredHeap<T>& p) : bits(0) { setPtr(p.getPtr()); }

    void setPtr(T newPtr) {
        MOZ_ASSERT((reinterpret_cast<uintptr_t>(newPtr) & flagsMask) == 0);
        MOZ_ASSERT(js::gc::IsCellPointerValidOrNull(newPtr));
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

    T unbarrieredGetPtr() const { return reinterpret_cast<T>(bits & ~flagsMask); }
    uintptr_t getFlags() const { return bits & flagsMask; }

    void exposeToActiveJS() const {
        js::BarrierMethods<T>::exposeToJS(unbarrieredGetPtr());
    }
    T getPtr() const {
        exposeToActiveJS();
        return unbarrieredGetPtr();
    }

    operator T() const { return getPtr(); }
    T operator->() const { return getPtr(); }

    explicit operator bool() const {
        return bool(js::BarrierMethods<T>::asGCThingOrNull(unbarrieredGetPtr()));
    }
    explicit operator bool() {
        return bool(js::BarrierMethods<T>::asGCThingOrNull(unbarrieredGetPtr()));
    }

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
class MOZ_NONHEAP_CLASS Handle : public js::HandleBase<T, Handle<T>>
{
    friend class JS::MutableHandle<T>;

  public:
    using ElementType = T;

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
        static void* const ConstNullValue = nullptr;
        ptr = reinterpret_cast<const T*>(&ConstNullValue);
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
    static constexpr Handle fromMarkedLocation(const T* p) {
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

    DECLARE_POINTER_CONSTREF_OPS(T);
    DECLARE_NONPOINTER_ACCESSOR_METHODS(*ptr);

  private:
    Handle() {}
    DELETE_ASSIGNMENT_OPS(Handle, T);

    enum Disambiguator { DeliberatelyChoosingThisOverload = 42 };
    enum CallerIdentity { ImUsingThisOnlyInFromFromMarkedLocation = 17 };
    constexpr Handle(const T* p, Disambiguator, CallerIdentity) : ptr(p) {}

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
class MOZ_STACK_CLASS MutableHandle : public js::MutableHandleBase<T, MutableHandle<T>>
{
  public:
    using ElementType = T;

    inline MOZ_IMPLICIT MutableHandle(Rooted<T>* root);
    inline MOZ_IMPLICIT MutableHandle(PersistentRooted<T>* root);

  private:
    // Disallow nullptr for overloading purposes.
    MutableHandle(decltype(nullptr)) = delete;

  public:
    void set(const T& v) {
        *ptr = v;
        MOZ_ASSERT(GCPolicy<T>::isValid(*ptr));
    }
    void set(T&& v) {
        *ptr = mozilla::Move(v);
        MOZ_ASSERT(GCPolicy<T>::isValid(*ptr));
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

template <typename T>
struct BarrierMethods<T*>
{
    static T* initial() { return nullptr; }
    static gc::Cell* asGCThingOrNull(T* v) {
        if (!v)
            return nullptr;
        MOZ_ASSERT(uintptr_t(v) > 32);
        return reinterpret_cast<gc::Cell*>(v);
    }
    static void postBarrier(T** vp, T* prev, T* next) {
        if (next)
            JS::AssertGCThingIsNotNurseryAllocable(reinterpret_cast<js::gc::Cell*>(next));
    }
    static void exposeToJS(T* t) {
        if (t)
            js::gc::ExposeGCThingToActiveJS(JS::GCCellPtr(t));
    }
};

template <>
struct BarrierMethods<JSObject*>
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
    static void exposeToJS(JSObject* obj) {
        if (obj)
            JS::ExposeObjectToActiveJS(obj);
    }
};

template <>
struct BarrierMethods<JSFunction*>
{
    static JSFunction* initial() { return nullptr; }
    static gc::Cell* asGCThingOrNull(JSFunction* v) {
        if (!v)
            return nullptr;
        MOZ_ASSERT(uintptr_t(v) > 32);
        return reinterpret_cast<gc::Cell*>(v);
    }
    static void postBarrier(JSFunction** vp, JSFunction* prev, JSFunction* next) {
        JS::HeapObjectPostBarrier(reinterpret_cast<JSObject**>(vp),
                                  reinterpret_cast<JSObject*>(prev),
                                  reinterpret_cast<JSObject*>(next));
    }
    static void exposeToJS(JSFunction* fun) {
        if (fun)
            JS::ExposeObjectToActiveJS(reinterpret_cast<JSObject*>(fun));
    }
};

template <>
struct BarrierMethods<JSString*>
{
    static JSString* initial() { return nullptr; }
    static gc::Cell* asGCThingOrNull(JSString* v) {
        if (!v)
            return nullptr;
        MOZ_ASSERT(uintptr_t(v) > 32);
        return reinterpret_cast<gc::Cell*>(v);
    }
    static void postBarrier(JSString** vp, JSString* prev, JSString* next) {
        JS::HeapStringPostBarrier(vp, prev, next);
    }
    static void exposeToJS(JSString* v) {
        if (v)
            js::gc::ExposeGCThingToActiveJS(JS::GCCellPtr(v));
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

    static bool hasHash(const Lookup& l);
    static bool ensureHash(const Lookup& l);
    static HashNumber hash(const Lookup& l);
    static bool match(const Key& k, const Lookup& l);
    static void rekey(Key& k, const Key& newKey) { k = newKey; }
};

template <typename T>
struct JS_PUBLIC_API(MovableCellHasher<JS::Heap<T>>)
{
    using Key = JS::Heap<T>;
    using Lookup = T;

    static bool hasHash(const Lookup& l) { return MovableCellHasher<T>::hasHash(l); }
    static bool ensureHash(const Lookup& l) { return MovableCellHasher<T>::ensureHash(l); }
    static HashNumber hash(const Lookup& l) { return MovableCellHasher<T>::hash(l); }
    static bool match(const Key& k, const Lookup& l) {
        return MovableCellHasher<T>::match(k.unbarrieredGet(), l);
    }
    static void rekey(Key& k, const Key& newKey) { k.unsafeSet(newKey); }
};

template <typename T>
struct FallibleHashMethods<MovableCellHasher<T>>
{
    template <typename Lookup> static bool hasHash(Lookup&& l) {
        return MovableCellHasher<T>::hasHash(mozilla::Forward<Lookup>(l));
    }
    template <typename Lookup> static bool ensureHash(Lookup&& l) {
        return MovableCellHasher<T>::ensureHash(mozilla::Forward<Lookup>(l));
    }
};

} /* namespace js */

namespace js {

// The alignment must be set because the Rooted and PersistentRooted ptr fields
// may be accessed through reinterpret_cast<Rooted<ConcreteTraceable>*>, and
// the compiler may choose a different alignment for the ptr field when it
// knows the actual type stored in DispatchWrapper<T>.
//
// It would make more sense to align only those specific fields of type
// DispatchWrapper, rather than DispatchWrapper itself, but that causes MSVC to
// fail when Rooted is used in an IsConvertible test.
template <typename T>
class alignas(8) DispatchWrapper
{
    static_assert(JS::MapTypeToRootKind<T>::kind == JS::RootKind::Traceable,
                  "DispatchWrapper is intended only for usage with a Traceable");

    using TraceFn = void (*)(JSTracer*, T*, const char*);
    TraceFn tracer;
    alignas(gc::CellAlignBytes) T storage;

  public:
    template <typename U>
    MOZ_IMPLICIT DispatchWrapper(U&& initial)
      : tracer(&JS::GCPolicy<T>::trace),
        storage(mozilla::Forward<U>(initial))
    { }

    // Mimic a pointer type, so that we can drop into Rooted.
    T* operator &() { return &storage; }
    const T* operator &() const { return &storage; }
    operator T&() { return storage; }
    operator const T&() const { return storage; }

    // Trace the contained storage (of unknown type) using the trace function
    // we set aside when we did know the type.
    static void TraceWrapped(JSTracer* trc, T* thingp, const char* name) {
        auto wrapper = reinterpret_cast<DispatchWrapper*>(
                           uintptr_t(thingp) - offsetof(DispatchWrapper, storage));
        wrapper->tracer(trc, &wrapper->storage, name);
    }
};

} /* namespace js */

namespace JS {

class JS_PUBLIC_API(AutoGCRooter);

// Our instantiations of Rooted<void*> and PersistentRooted<void*> require an
// instantiation of MapTypeToRootKind.
template <>
struct MapTypeToRootKind<void*> {
    static const RootKind kind = RootKind::Traceable;
};

using RootedListHeads = mozilla::EnumeratedArray<RootKind, RootKind::Limit,
                                                 Rooted<void*>*>;

// Superclass of JSContext which can be used for rooting data in use by the
// current thread but that does not provide all the functions of a JSContext.
class RootingContext
{
    // Stack GC roots for Rooted GC heap pointers.
    RootedListHeads stackRoots_;
    template <typename T> friend class JS::Rooted;

    // Stack GC roots for AutoFooRooter classes.
    JS::AutoGCRooter* autoGCRooters_;
    friend class JS::AutoGCRooter;

    // Gecko profiling metadata.
    // This isn't really rooting related. It's only here because we want
    // GetContextProfilingStack to be inlineable into non-JS code, and we
    // didn't want to add another superclass of JSContext just for this.
    js::GeckoProfilerThread geckoProfiler_;

  public:
    RootingContext();

    void traceStackRoots(JSTracer* trc);
    void checkNoGCRooters();

    js::GeckoProfilerThread& geckoProfiler() { return geckoProfiler_; }

  protected:
    // The remaining members in this class should only be accessed through
    // JSContext pointers. They are unrelated to rooting and are in place so
    // that inlined API functions can directly access the data.

    /* The current compartment. */
    JSCompartment*      compartment_;

    /* The current zone. */
    JS::Zone*           zone_;

  public:
    /* Limit pointer for checking native stack consumption. */
    uintptr_t nativeStackLimit[StackKindCount];

    static const RootingContext* get(const JSContext* cx) {
        return reinterpret_cast<const RootingContext*>(cx);
    }

    static RootingContext* get(JSContext* cx) {
        return reinterpret_cast<RootingContext*>(cx);
    }

    friend JSCompartment* js::GetContextCompartment(const JSContext* cx);
    friend JS::Zone* js::GetContextZone(const JSContext* cx);
};

class JS_PUBLIC_API(AutoGCRooter)
{
  public:
    AutoGCRooter(JSContext* cx, ptrdiff_t tag)
      : AutoGCRooter(JS::RootingContext::get(cx), tag)
    {}
    AutoGCRooter(JS::RootingContext* cx, ptrdiff_t tag)
      : down(cx->autoGCRooters_),
        tag_(tag),
        stackTop(&cx->autoGCRooters_)
    {
        MOZ_ASSERT(this != *stackTop);
        *stackTop = this;
    }

    ~AutoGCRooter() {
        MOZ_ASSERT(this == *stackTop);
        *stackTop = down;
    }

    /* Implemented in gc/RootMarking.cpp. */
    inline void trace(JSTracer* trc);
    static void traceAll(const js::CooperatingContext& target, JSTracer* trc);
    static void traceAllWrappers(const js::CooperatingContext& target, JSTracer* trc);

  protected:
    AutoGCRooter * const down;

    /*
     * Discriminates actual subclass of this being used.  If non-negative, the
     * subclass roots an array of values of the length stored in this field.
     * If negative, meaning is indicated by the corresponding value in the enum
     * below.  Any other negative value indicates some deeper problem such as
     * memory corruption.
     */
    ptrdiff_t tag_;

    enum {
        VALARRAY =     -2, /* js::AutoValueArray */
        PARSER =       -3, /* js::frontend::Parser */
#if defined(JS_BUILD_BINAST)
        BINPARSER =    -4, /* js::frontend::BinSource */
#endif // defined(JS_BUILD_BINAST)
        IONMASM =     -19, /* js::jit::MacroAssembler */
        WRAPVECTOR =  -20, /* js::AutoWrapperVector */
        WRAPPER =     -21, /* js::AutoWrapperRooter */
        CUSTOM =      -26  /* js::CustomAutoRooter */
    };

  private:
    AutoGCRooter ** const stackTop;

    /* No copy or assignment semantics. */
    AutoGCRooter(AutoGCRooter& ida) = delete;
    void operator=(AutoGCRooter& ida) = delete;
};

namespace detail {

/*
 * For pointer types, the TraceKind for tracing is based on the list it is
 * in (selected via MapTypeToRootKind), so no additional storage is
 * required here. Non-pointer types, however, share the same list, so the
 * function to call for tracing is stored adjacent to the struct. Since C++
 * cannot templatize on storage class, this is implemented via the wrapper
 * class DispatchWrapper.
 */
template <typename T>
using MaybeWrapped = typename mozilla::Conditional<
    MapTypeToRootKind<T>::kind == JS::RootKind::Traceable,
    js::DispatchWrapper<T>,
    T>::Type;

} /* namespace detail */

/**
 * Local variable of type T whose value is always rooted. This is typically
 * used for local variables, or for non-rooted values being passed to a
 * function that requires a handle, e.g. Foo(Root<T>(cx, x)).
 *
 * If you want to add additional methods to Rooted for a specific
 * specialization, define a RootedBase<T> specialization containing them.
 */
template <typename T>
class MOZ_RAII Rooted : public js::RootedBase<T, Rooted<T>>
{
    inline void registerWithRootLists(RootedListHeads& roots) {
        this->stack = &roots[JS::MapTypeToRootKind<T>::kind];
        this->prev = *stack;
        *stack = reinterpret_cast<Rooted<void*>*>(this);
    }

    inline RootedListHeads& rootLists(RootingContext* cx) {
        return cx->stackRoots_;
    }
    inline RootedListHeads& rootLists(JSContext* cx) {
        return rootLists(RootingContext::get(cx));
    }

  public:
    using ElementType = T;

    template <typename RootingContext>
    explicit Rooted(const RootingContext& cx)
      : ptr(GCPolicy<T>::initial())
    {
        registerWithRootLists(rootLists(cx));
    }

    template <typename RootingContext, typename S>
    Rooted(const RootingContext& cx, S&& initial)
      : ptr(mozilla::Forward<S>(initial))
    {
        MOZ_ASSERT(GCPolicy<T>::isValid(ptr));
        registerWithRootLists(rootLists(cx));
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
    void set(const T& value) {
        ptr = value;
        MOZ_ASSERT(GCPolicy<T>::isValid(ptr));
    }
    void set(T&& value) {
        ptr = mozilla::Move(value);
        MOZ_ASSERT(GCPolicy<T>::isValid(ptr));
    }

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

    detail::MaybeWrapped<T> ptr;

    Rooted(const Rooted&) = delete;
} JS_HAZ_ROOTED;

} /* namespace JS */

namespace js {

/*
 * Inlinable accessors for JSContext.
 *
 * - These must not be available on the more restricted superclasses of
 *   JSContext, so we can't simply define them on RootingContext.
 *
 * - They're perfectly ordinary JSContext functionality, so ought to be
 *   usable without resorting to jsfriendapi.h, and when JSContext is an
 *   incomplete type.
 */
inline JSCompartment*
GetContextCompartment(const JSContext* cx)
{
    return JS::RootingContext::get(cx)->compartment_;
}

inline JS::Zone*
GetContextZone(const JSContext* cx)
{
    return JS::RootingContext::get(cx)->zone_;
}

inline PseudoStack*
GetContextProfilingStack(JSContext* cx)
{
    return JS::RootingContext::get(cx)->geckoProfiler().getPseudoStack();
}

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
template <typename Container>
class RootedBase<JSObject*, Container> : public MutableWrappedPtrOperations<JSObject*, Container>
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
template <typename Container>
class HandleBase<JSObject*, Container> : public WrappedPtrOperations<JSObject*, Container>
{
  public:
    template <class U>
    JS::Handle<U*> as() const;
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

JS_PUBLIC_API(void)
AddPersistentRoot(RootingContext* cx, RootKind kind, PersistentRooted<void*>* root);

JS_PUBLIC_API(void)
AddPersistentRoot(JSRuntime* rt, RootKind kind, PersistentRooted<void*>* root);

/**
 * A copyable, assignable global GC root type with arbitrary lifetime, an
 * infallible constructor, and automatic unrooting on destruction.
 *
 * These roots can be used in heap-allocated data structures, so they are not
 * associated with any particular JSContext or stack. They are registered with
 * the JSRuntime itself, without locking, so they require a full JSContext to be
 * initialized, not one of its more restricted superclasses. Initialization may
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
class PersistentRooted : public js::RootedBase<T, PersistentRooted<T>>,
                         private mozilla::LinkedListElement<PersistentRooted<T>>
{
    using ListBase = mozilla::LinkedListElement<PersistentRooted<T>>;

    friend class mozilla::LinkedList<PersistentRooted>;
    friend class mozilla::LinkedListElement<PersistentRooted>;

    void registerWithRootLists(RootingContext* cx) {
        MOZ_ASSERT(!initialized());
        JS::RootKind kind = JS::MapTypeToRootKind<T>::kind;
        AddPersistentRoot(cx, kind, reinterpret_cast<JS::PersistentRooted<void*>*>(this));
    }

    void registerWithRootLists(JSRuntime* rt) {
        MOZ_ASSERT(!initialized());
        JS::RootKind kind = JS::MapTypeToRootKind<T>::kind;
        AddPersistentRoot(rt, kind, reinterpret_cast<JS::PersistentRooted<void*>*>(this));
    }

  public:
    using ElementType = T;

    PersistentRooted() : ptr(GCPolicy<T>::initial()) {}

    explicit PersistentRooted(RootingContext* cx)
      : ptr(GCPolicy<T>::initial())
    {
        registerWithRootLists(cx);
    }

    explicit PersistentRooted(JSContext* cx)
      : ptr(GCPolicy<T>::initial())
    {
        registerWithRootLists(RootingContext::get(cx));
    }

    template <typename U>
    PersistentRooted(RootingContext* cx, U&& initial)
      : ptr(mozilla::Forward<U>(initial))
    {
        registerWithRootLists(cx);
    }

    template <typename U>
    PersistentRooted(JSContext* cx, U&& initial)
      : ptr(mozilla::Forward<U>(initial))
    {
        registerWithRootLists(RootingContext::get(cx));
    }

    explicit PersistentRooted(JSRuntime* rt)
      : ptr(GCPolicy<T>::initial())
    {
        registerWithRootLists(rt);
    }

    template <typename U>
    PersistentRooted(JSRuntime* rt, U&& initial)
      : ptr(mozilla::Forward<U>(initial))
    {
        registerWithRootLists(rt);
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

    void init(JSContext* cx) {
        init(cx, GCPolicy<T>::initial());
    }

    template <typename U>
    void init(JSContext* cx, U&& initial) {
        ptr = mozilla::Forward<U>(initial);
        registerWithRootLists(RootingContext::get(cx));
    }

    void reset() {
        if (initialized()) {
            set(GCPolicy<T>::initial());
            ListBase::remove();
        }
    }

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
    template <typename U>
    void set(U&& value) {
        MOZ_ASSERT(initialized());
        ptr = mozilla::Forward<U>(value);
    }

    detail::MaybeWrapped<T> ptr;
} JS_HAZ_ROOTED;

class JS_PUBLIC_API(ObjectPtr)
{
    Heap<JSObject*> value;

  public:
    using ElementType = JSObject*;

    ObjectPtr() : value(nullptr) {}

    explicit ObjectPtr(JSObject* obj) : value(obj) {}

    ObjectPtr(const ObjectPtr& other) : value(other.value) {}

    ObjectPtr(ObjectPtr&& other)
      : value(other.value)
    {
        other.value = nullptr;
    }

    /* Always call finalize before the destructor. */
    ~ObjectPtr() { MOZ_ASSERT(!value); }

    void finalize(JSRuntime* rt);
    void finalize(JSContext* cx);

    void init(JSObject* obj) { value = obj; }

    JSObject* get() const { return value; }
    JSObject* unbarrieredGet() const { return value.unbarrieredGet(); }

    void writeBarrierPre(JSContext* cx) {
        IncrementalPreWriteBarrier(value);
    }

    void updateWeakPointerAfterGC();

    ObjectPtr& operator=(JSObject* obj) {
        IncrementalPreWriteBarrier(value);
        value = obj;
        return *this;
    }

    void trace(JSTracer* trc, const char* name);

    JSObject& operator*() const { return *value; }
    JSObject* operator->() const { return value; }
    operator JSObject*() const { return value; }

    explicit operator bool() const { return value.unbarrieredGet(); }
    explicit operator bool() { return value.unbarrieredGet(); }
};

} /* namespace JS */

namespace js {

template <typename T, typename D, typename Container>
class WrappedPtrOperations<UniquePtr<T, D>, Container>
{
    const UniquePtr<T, D>& uniquePtr() const { return static_cast<const Container*>(this)->get(); }

  public:
    explicit operator bool() const { return !!uniquePtr(); }
    T* get() const { return uniquePtr().get(); }
    T* operator->() const { return get(); }
    T& operator*() const { return *uniquePtr(); }
};

template <typename T, typename D, typename Container>
class MutableWrappedPtrOperations<UniquePtr<T, D>, Container>
  : public WrappedPtrOperations<UniquePtr<T, D>, Container>
{
    UniquePtr<T, D>& uniquePtr() { return static_cast<Container*>(this)->get(); }

  public:
    MOZ_MUST_USE typename UniquePtr<T, D>::Pointer release() { return uniquePtr().release(); }
    void reset(T* ptr = T()) { uniquePtr().reset(ptr); }
};

namespace gc {

template <typename T, typename TraceCallbacks>
void
CallTraceCallbackOnNonHeap(T* v, const TraceCallbacks& aCallbacks, const char* aName, void* aClosure)
{
    static_assert(sizeof(T) == sizeof(JS::Heap<T>), "T and Heap<T> must be compatible.");
    MOZ_ASSERT(v);
    mozilla::DebugOnly<Cell*> cell = BarrierMethods<T>::asGCThingOrNull(*v);
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

namespace js {
namespace detail {

// DefineComparisonOps is a trait which selects which wrapper classes to define
// operator== and operator!= for. It supplies a getter function to extract the
// value to compare. This is used to avoid triggering the automatic read
// barriers where appropriate.
//
// If DefineComparisonOps is not specialized for a particular wrapper you may
// get errors such as 'invalid operands to binary expression' or 'no match for
// operator==' when trying to compare against instances of the wrapper.

template <typename T>
struct DefineComparisonOps : mozilla::FalseType {};

template <typename T>
struct DefineComparisonOps<JS::Heap<T>> : mozilla::TrueType {
    static const T& get(const JS::Heap<T>& v) { return v.unbarrieredGet(); }
};

template <typename T>
struct DefineComparisonOps<JS::TenuredHeap<T>> : mozilla::TrueType {
    static const T get(const JS::TenuredHeap<T>& v) { return v.unbarrieredGetPtr(); }
};

template <>
struct DefineComparisonOps<JS::ObjectPtr> : mozilla::TrueType {
    static const JSObject* get(const JS::ObjectPtr& v) { return v.unbarrieredGet(); }
};

template <typename T>
struct DefineComparisonOps<JS::Rooted<T>> : mozilla::TrueType {
    static const T& get(const JS::Rooted<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<JS::Handle<T>> : mozilla::TrueType {
    static const T& get(const JS::Handle<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<JS::MutableHandle<T>> : mozilla::TrueType {
    static const T& get(const JS::MutableHandle<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<JS::PersistentRooted<T>> : mozilla::TrueType {
    static const T& get(const JS::PersistentRooted<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<js::FakeRooted<T>> : mozilla::TrueType {
    static const T& get(const js::FakeRooted<T>& v) { return v.get(); }
};

template <typename T>
struct DefineComparisonOps<js::FakeMutableHandle<T>> : mozilla::TrueType {
    static const T& get(const js::FakeMutableHandle<T>& v) { return v.get(); }
};

} /* namespace detail */
} /* namespace js */

// Overload operator== and operator!= for all types with the DefineComparisonOps
// trait using the supplied getter.
//
// There are four cases:

// Case 1: comparison between two wrapper objects.

template <typename T, typename U>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           js::detail::DefineComparisonOps<U>::value, bool>::Type
operator==(const T& a, const U& b) {
    return js::detail::DefineComparisonOps<T>::get(a) == js::detail::DefineComparisonOps<U>::get(b);
}

template <typename T, typename U>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           js::detail::DefineComparisonOps<U>::value, bool>::Type
operator!=(const T& a, const U& b) {
    return !(a == b);
}

// Case 2: comparison between a wrapper object and its unwrapped element type.

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value, bool>::Type
operator==(const T& a, const typename T::ElementType& b) {
    return js::detail::DefineComparisonOps<T>::get(a) == b;
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value, bool>::Type
operator!=(const T& a, const typename T::ElementType& b) {
    return !(a == b);
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value, bool>::Type
operator==(const typename T::ElementType& a, const T& b) {
    return a == js::detail::DefineComparisonOps<T>::get(b);
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value, bool>::Type
operator!=(const typename T::ElementType& a, const T& b) {
    return !(a == b);
}

// Case 3: For pointer wrappers, comparison between the wrapper and a const
// element pointer.

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator==(const typename mozilla::RemovePointer<typename T::ElementType>::Type* a, const T& b) {
    return a == js::detail::DefineComparisonOps<T>::get(b);
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator!=(const typename mozilla::RemovePointer<typename T::ElementType>::Type* a, const T& b) {
    return !(a == b);
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator==(const T& a, const typename mozilla::RemovePointer<typename T::ElementType>::Type* b) {
    return js::detail::DefineComparisonOps<T>::get(a) == b;
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator!=(const T& a, const typename mozilla::RemovePointer<typename T::ElementType>::Type* b) {
    return !(a == b);
}

// Case 4: For pointer wrappers, comparison between the wrapper and nullptr.

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator==(std::nullptr_t a, const T& b) {
    return a == js::detail::DefineComparisonOps<T>::get(b);
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator!=(std::nullptr_t a, const T& b) {
    return !(a == b);
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator==(const T& a, std::nullptr_t b) {
    return js::detail::DefineComparisonOps<T>::get(a) == b;
}

template <typename T>
typename mozilla::EnableIf<js::detail::DefineComparisonOps<T>::value &&
                           mozilla::IsPointer<typename T::ElementType>::value, bool>::Type
operator!=(const T& a, std::nullptr_t b) {
    return !(a == b);
}

#endif  /* js_RootingAPI_h */

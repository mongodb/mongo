/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_RootingAPI_h
#define js_RootingAPI_h

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/LinkedList.h"
#include "mozilla/Maybe.h"

#include <type_traits>
#include <utility>

#include "jspubtd.h"

#include "js/ComparisonOperators.h"  // JS::detail::DefineComparisonOps
#include "js/GCAnnotations.h"
#include "js/GCPolicyAPI.h"
#include "js/GCTypeMacros.h"  // JS_FOR_EACH_PUBLIC_{,TAGGED_}GC_POINTER_TYPE
#include "js/HashTable.h"
#include "js/HeapAPI.h"
#include "js/ProfilingStack.h"
#include "js/Realm.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"

/*
 * [SMDOC] Stack Rooting
 *
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
struct BarrierMethods {};

template <typename Element, typename Wrapper>
class WrappedPtrOperations {};

template <typename Element, typename Wrapper>
class MutableWrappedPtrOperations
    : public WrappedPtrOperations<Element, Wrapper> {};

template <typename T, typename Wrapper>
class RootedBase : public MutableWrappedPtrOperations<T, Wrapper> {};

template <typename T, typename Wrapper>
class HandleBase : public WrappedPtrOperations<T, Wrapper> {};

template <typename T, typename Wrapper>
class MutableHandleBase : public MutableWrappedPtrOperations<T, Wrapper> {};

template <typename T, typename Wrapper>
class HeapBase : public MutableWrappedPtrOperations<T, Wrapper> {};

// Cannot use FOR_EACH_HEAP_ABLE_GC_POINTER_TYPE, as this would import too many
// macros into scope
template <typename T>
struct IsHeapConstructibleType {
  static constexpr bool value = false;
};
#define DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE(T) \
  template <>                                 \
  struct IsHeapConstructibleType<T> {         \
    static constexpr bool value = true;       \
  };
JS_FOR_EACH_PUBLIC_GC_POINTER_TYPE(DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE)
JS_FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE)
#undef DECLARE_IS_HEAP_CONSTRUCTIBLE_TYPE

namespace gc {
struct Cell;
} /* namespace gc */

// Important: Return a reference so passing a Rooted<T>, etc. to
// something that takes a |const T&| is not a GC hazard.
#define DECLARE_POINTER_CONSTREF_OPS(T)       \
  operator const T&() const { return get(); } \
  const T& operator->() const { return get(); }

// Assignment operators on a base class are hidden by the implicitly defined
// operator= on the derived class. Thus, define the operator= directly on the
// class as we would need to manually pass it through anyway.
#define DECLARE_POINTER_ASSIGN_OPS(Wrapper, T)     \
  Wrapper<T>& operator=(const T& p) {              \
    set(p);                                        \
    return *this;                                  \
  }                                                \
  Wrapper<T>& operator=(T&& p) {                   \
    set(std::move(p));                             \
    return *this;                                  \
  }                                                \
  Wrapper<T>& operator=(const Wrapper<T>& other) { \
    set(other.get());                              \
    return *this;                                  \
  }

#define DELETE_ASSIGNMENT_OPS(Wrapper, T) \
  template <typename S>                   \
  Wrapper<T>& operator=(S) = delete;      \
  Wrapper<T>& operator=(const Wrapper<T>&) = delete;

#define DECLARE_NONPOINTER_ACCESSOR_METHODS(ptr) \
  const T* address() const { return &(ptr); }    \
  const T& get() const { return (ptr); }

#define DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(ptr) \
  T* address() { return &(ptr); }                        \
  T& get() { return (ptr); }

} /* namespace js */

namespace JS {

JS_PUBLIC_API void HeapObjectPostWriteBarrier(JSObject** objp, JSObject* prev,
                                              JSObject* next);
JS_PUBLIC_API void HeapStringPostWriteBarrier(JSString** objp, JSString* prev,
                                              JSString* next);
JS_PUBLIC_API void HeapBigIntPostWriteBarrier(JS::BigInt** bip,
                                              JS::BigInt* prev,
                                              JS::BigInt* next);
JS_PUBLIC_API void HeapObjectWriteBarriers(JSObject** objp, JSObject* prev,
                                           JSObject* next);
JS_PUBLIC_API void HeapStringWriteBarriers(JSString** objp, JSString* prev,
                                           JSString* next);
JS_PUBLIC_API void HeapBigIntWriteBarriers(JS::BigInt** bip, JS::BigInt* prev,
                                           JS::BigInt* next);
JS_PUBLIC_API void HeapScriptWriteBarriers(JSScript** objp, JSScript* prev,
                                           JSScript* next);

/**
 * Create a safely-initialized |T|, suitable for use as a default value in
 * situations requiring a safe but arbitrary |T| value.
 */
template <typename T>
inline T SafelyInitialized() {
  // This function wants to presume that |T()| -- which value-initializes a
  // |T| per C++11 [expr.type.conv]p2 -- will produce a safely-initialized,
  // safely-usable T that it can return.

#if defined(XP_WIN) || defined(XP_MACOSX) || \
    (defined(XP_UNIX) && !defined(__clang__))

  // That presumption holds for pointers, where value initialization produces
  // a null pointer.
  constexpr bool IsPointer = std::is_pointer_v<T>;

  // For classes and unions we *assume* that if |T|'s default constructor is
  // non-trivial it'll initialize correctly. (This is unideal, but C++
  // doesn't offer a type trait indicating whether a class's constructor is
  // user-defined, which better approximates our desired semantics.)
  constexpr bool IsNonTriviallyDefaultConstructibleClassOrUnion =
      (std::is_class_v<T> ||
       std::is_union_v<T>)&&!std::is_trivially_default_constructible_v<T>;

  static_assert(IsPointer || IsNonTriviallyDefaultConstructibleClassOrUnion,
                "T() must evaluate to a safely-initialized T");

#endif

  return T();
}

#ifdef JS_DEBUG
/**
 * For generational GC, assert that an object is in the tenured generation as
 * opposed to being in the nursery.
 */
extern JS_PUBLIC_API void AssertGCThingMustBeTenured(JSObject* obj);
extern JS_PUBLIC_API void AssertGCThingIsNotNurseryAllocable(
    js::gc::Cell* cell);
#else
inline void AssertGCThingMustBeTenured(JSObject* obj) {}
inline void AssertGCThingIsNotNurseryAllocable(js::gc::Cell* cell) {}
#endif

/**
 * The Heap<T> class is a heap-stored reference to a JS GC thing for use outside
 * the JS engine. All members of heap classes that refer to GC things should use
 * Heap<T> (or possibly TenuredHeap<T>, described below).
 *
 * Heap<T> is an abstraction that hides some of the complexity required to
 * maintain GC invariants for the contained reference. It uses operator
 * overloading to provide a normal pointer interface, but adds barriers to
 * notify the GC of changes.
 *
 * Heap<T> implements the following barriers:
 *
 *  - Post-write barrier (necessary for generational GC).
 *  - Read barrier (necessary for incremental GC and cycle collector
 *    integration).
 *
 * Note Heap<T> does not have a pre-write barrier as used internally in the
 * engine. The read barrier is used to mark anything read from a Heap<T> during
 * an incremental GC.
 *
 * Heap<T> may be moved or destroyed outside of GC finalization and hence may be
 * used in dynamic storage such as a Vector.
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
class MOZ_NON_MEMMOVABLE Heap : public js::HeapBase<T, Heap<T>> {
  // Please note: this can actually also be used by nsXBLMaybeCompiled<T>, for
  // legacy reasons.
  static_assert(js::IsHeapConstructibleType<T>::value,
                "Type T must be a public GC pointer type");

 public:
  using ElementType = T;

  Heap() : ptr(SafelyInitialized<T>()) {
    // No barriers are required for initialization to the default value.
    static_assert(sizeof(T) == sizeof(Heap<T>),
                  "Heap<T> must be binary compatible with T.");
  }
  explicit Heap(const T& p) { init(p); }

  /*
   * For Heap, move semantics are equivalent to copy semantics. However, we want
   * the copy constructor to be explicit, and an explicit move constructor
   * breaks common usage of move semantics, so we need to define both, even
   * though they are equivalent.
   */
  explicit Heap(const Heap<T>& other) { init(other.ptr); }
  Heap(Heap<T>&& other) { init(other.ptr); }

  Heap& operator=(Heap<T>&& other) {
    set(other.unbarrieredGet());
    other.set(SafelyInitialized<T>());
    return *this;
  }

  ~Heap() { postWriteBarrier(ptr, SafelyInitialized<T>()); }

  DECLARE_POINTER_CONSTREF_OPS(T);
  DECLARE_POINTER_ASSIGN_OPS(Heap, T);

  const T* address() const { return &ptr; }

  void exposeToActiveJS() const { js::BarrierMethods<T>::exposeToJS(ptr); }
  const T& get() const {
    exposeToActiveJS();
    return ptr;
  }
  const T& unbarrieredGet() const { return ptr; }

  void set(const T& newPtr) {
    T tmp = ptr;
    ptr = newPtr;
    postWriteBarrier(tmp, ptr);
  }

  T* unsafeGet() { return &ptr; }

  void unbarrieredSet(const T& newPtr) { ptr = newPtr; }

  explicit operator bool() const {
    return bool(js::BarrierMethods<T>::asGCThingOrNull(ptr));
  }
  explicit operator bool() {
    return bool(js::BarrierMethods<T>::asGCThingOrNull(ptr));
  }

 private:
  void init(const T& newPtr) {
    ptr = newPtr;
    postWriteBarrier(SafelyInitialized<T>(), ptr);
  }

  void postWriteBarrier(const T& prev, const T& next) {
    js::BarrierMethods<T>::postWriteBarrier(&ptr, prev, next);
  }

  T ptr;
};

namespace detail {

template <typename T>
struct DefineComparisonOps<Heap<T>> : std::true_type {
  static const T& get(const Heap<T>& v) { return v.unbarrieredGet(); }
};

}  // namespace detail

static MOZ_ALWAYS_INLINE bool ObjectIsTenured(JSObject* obj) {
  return !js::gc::IsInsideNursery(reinterpret_cast<js::gc::Cell*>(obj));
}

static MOZ_ALWAYS_INLINE bool ObjectIsTenured(const Heap<JSObject*>& obj) {
  return ObjectIsTenured(obj.unbarrieredGet());
}

static MOZ_ALWAYS_INLINE bool ObjectIsMarkedGray(JSObject* obj) {
  auto cell = reinterpret_cast<js::gc::Cell*>(obj);
  return js::gc::detail::CellIsMarkedGrayIfKnown(cell);
}

static MOZ_ALWAYS_INLINE bool ObjectIsMarkedGray(
    const JS::Heap<JSObject*>& obj) {
  return ObjectIsMarkedGray(obj.unbarrieredGet());
}

// The following *IsNotGray functions take account of the eventual
// gray marking state at the end of any ongoing incremental GC by
// delaying the checks if necessary.

#ifdef DEBUG

inline void AssertCellIsNotGray(const js::gc::Cell* maybeCell) {
  if (maybeCell) {
    js::gc::detail::AssertCellIsNotGray(maybeCell);
  }
}

inline void AssertObjectIsNotGray(JSObject* maybeObj) {
  AssertCellIsNotGray(reinterpret_cast<js::gc::Cell*>(maybeObj));
}

inline void AssertObjectIsNotGray(const JS::Heap<JSObject*>& obj) {
  AssertObjectIsNotGray(obj.unbarrieredGet());
}

#else

inline void AssertCellIsNotGray(js::gc::Cell* maybeCell) {}
inline void AssertObjectIsNotGray(JSObject* maybeObj) {}
inline void AssertObjectIsNotGray(const JS::Heap<JSObject*>& obj) {}

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
class TenuredHeap : public js::HeapBase<T, TenuredHeap<T>> {
 public:
  using ElementType = T;

  TenuredHeap() : bits(0) {
    static_assert(sizeof(T) == sizeof(TenuredHeap<T>),
                  "TenuredHeap<T> must be binary compatible with T.");
  }
  explicit TenuredHeap(T p) : bits(0) { setPtr(p); }
  explicit TenuredHeap(const TenuredHeap<T>& p) : bits(0) {
    setPtr(p.getPtr());
  }

  void setPtr(T newPtr) {
    MOZ_ASSERT((reinterpret_cast<uintptr_t>(newPtr) & flagsMask) == 0);
    MOZ_ASSERT(js::gc::IsCellPointerValidOrNull(newPtr));
    if (newPtr) {
      AssertGCThingMustBeTenured(newPtr);
    }
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

namespace detail {

template <typename T>
struct DefineComparisonOps<TenuredHeap<T>> : std::true_type {
  static const T get(const TenuredHeap<T>& v) { return v.unbarrieredGetPtr(); }
};

}  // namespace detail

// std::swap uses a stack temporary, which prevents classes like Heap<T>
// from being declared MOZ_HEAP_CLASS.
template <typename T>
void swap(TenuredHeap<T>& aX, TenuredHeap<T>& aY) {
  T tmp = aX;
  aX = aY;
  aY = tmp;
}

template <typename T>
void swap(Heap<T>& aX, Heap<T>& aY) {
  T tmp = aX;
  aX = aY;
  aY = tmp;
}

static MOZ_ALWAYS_INLINE bool ObjectIsMarkedGray(
    const JS::TenuredHeap<JSObject*>& obj) {
  return ObjectIsMarkedGray(obj.unbarrieredGetPtr());
}

template <typename T>
class MutableHandle;
template <typename T>
class Rooted;
template <typename T>
class PersistentRooted;

/**
 * Reference to a T that has been rooted elsewhere. This is most useful
 * as a parameter type, which guarantees that the T lvalue is properly
 * rooted. See "Move GC Stack Rooting" above.
 *
 * If you want to add additional methods to Handle for a specific
 * specialization, define a HandleBase<T> specialization containing them.
 */
template <typename T>
class MOZ_NONHEAP_CLASS Handle : public js::HandleBase<T, Handle<T>> {
  friend class MutableHandle<T>;

 public:
  using ElementType = T;

  Handle(const Handle<T>&) = default;

  /* Creates a handle from a handle of a type convertible to T. */
  template <typename S>
  MOZ_IMPLICIT Handle(
      Handle<S> handle,
      std::enable_if_t<std::is_convertible_v<S, T>, int> dummy = 0) {
    static_assert(sizeof(Handle<T>) == sizeof(T*),
                  "Handle must be binary compatible with T*.");
    ptr = reinterpret_cast<const T*>(handle.address());
  }

  MOZ_IMPLICIT Handle(decltype(nullptr)) {
    static_assert(std::is_pointer_v<T>,
                  "nullptr_t overload not valid for non-pointer types");
    static void* const ConstNullValue = nullptr;
    ptr = reinterpret_cast<const T*>(&ConstNullValue);
  }

  MOZ_IMPLICIT Handle(MutableHandle<T> handle) { ptr = handle.address(); }

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
  inline MOZ_IMPLICIT Handle(
      const Rooted<S>& root,
      std::enable_if_t<std::is_convertible_v<S, T>, int> dummy = 0);

  template <typename S>
  inline MOZ_IMPLICIT Handle(
      const PersistentRooted<S>& root,
      std::enable_if_t<std::is_convertible_v<S, T>, int> dummy = 0);

  /* Construct a read only handle from a mutable handle. */
  template <typename S>
  inline MOZ_IMPLICIT Handle(
      MutableHandle<S>& root,
      std::enable_if_t<std::is_convertible_v<S, T>, int> dummy = 0);

  DECLARE_POINTER_CONSTREF_OPS(T);
  DECLARE_NONPOINTER_ACCESSOR_METHODS(*ptr);

 private:
  Handle() = default;
  DELETE_ASSIGNMENT_OPS(Handle, T);

  enum Disambiguator { DeliberatelyChoosingThisOverload = 42 };
  enum CallerIdentity { ImUsingThisOnlyInFromFromMarkedLocation = 17 };
  constexpr Handle(const T* p, Disambiguator, CallerIdentity) : ptr(p) {}

  const T* ptr;
};

namespace detail {

template <typename T>
struct DefineComparisonOps<Handle<T>> : std::true_type {
  static const T& get(const Handle<T>& v) { return v.get(); }
};

}  // namespace detail

/**
 * Similar to a handle, but the underlying storage can be changed. This is
 * useful for outparams.
 *
 * If you want to add additional methods to MutableHandle for a specific
 * specialization, define a MutableHandleBase<T> specialization containing
 * them.
 */
template <typename T>
class MOZ_STACK_CLASS MutableHandle
    : public js::MutableHandleBase<T, MutableHandle<T>> {
 public:
  using ElementType = T;

  inline MOZ_IMPLICIT MutableHandle(Rooted<T>* root);
  inline MOZ_IMPLICIT MutableHandle(PersistentRooted<T>* root);

 private:
  // Disallow nullptr for overloading purposes.
  MutableHandle(decltype(nullptr)) = delete;

 public:
  MutableHandle(const MutableHandle<T>&) = default;
  void set(const T& v) {
    *ptr = v;
    MOZ_ASSERT(GCPolicy<T>::isValid(*ptr));
  }
  void set(T&& v) {
    *ptr = std::move(v);
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
  MutableHandle() = default;
  DELETE_ASSIGNMENT_OPS(MutableHandle, T);

  T* ptr;
};

namespace detail {

template <typename T>
struct DefineComparisonOps<MutableHandle<T>> : std::true_type {
  static const T& get(const MutableHandle<T>& v) { return v.get(); }
};

}  // namespace detail

} /* namespace JS */

namespace js {

namespace detail {

// Default implementations for barrier methods on GC thing pointers.
template <typename T>
struct PtrBarrierMethodsBase {
  static T* initial() { return nullptr; }
  static gc::Cell* asGCThingOrNull(T* v) {
    if (!v) {
      return nullptr;
    }
    MOZ_ASSERT(uintptr_t(v) > 32);
    return reinterpret_cast<gc::Cell*>(v);
  }
  static void exposeToJS(T* t) {
    if (t) {
      js::gc::ExposeGCThingToActiveJS(JS::GCCellPtr(t));
    }
  }
};

}  // namespace detail

template <typename T>
struct BarrierMethods<T*> : public detail::PtrBarrierMethodsBase<T> {
  static void postWriteBarrier(T** vp, T* prev, T* next) {
    if (next) {
      JS::AssertGCThingIsNotNurseryAllocable(
          reinterpret_cast<js::gc::Cell*>(next));
    }
  }
};

template <>
struct BarrierMethods<JSObject*>
    : public detail::PtrBarrierMethodsBase<JSObject> {
  static void postWriteBarrier(JSObject** vp, JSObject* prev, JSObject* next) {
    JS::HeapObjectPostWriteBarrier(vp, prev, next);
  }
  static void exposeToJS(JSObject* obj) {
    if (obj) {
      JS::ExposeObjectToActiveJS(obj);
    }
  }
};

template <>
struct BarrierMethods<JSFunction*>
    : public detail::PtrBarrierMethodsBase<JSFunction> {
  static void postWriteBarrier(JSFunction** vp, JSFunction* prev,
                               JSFunction* next) {
    JS::HeapObjectPostWriteBarrier(reinterpret_cast<JSObject**>(vp),
                                   reinterpret_cast<JSObject*>(prev),
                                   reinterpret_cast<JSObject*>(next));
  }
  static void exposeToJS(JSFunction* fun) {
    if (fun) {
      JS::ExposeObjectToActiveJS(reinterpret_cast<JSObject*>(fun));
    }
  }
};

template <>
struct BarrierMethods<JSString*>
    : public detail::PtrBarrierMethodsBase<JSString> {
  static void postWriteBarrier(JSString** vp, JSString* prev, JSString* next) {
    JS::HeapStringPostWriteBarrier(vp, prev, next);
  }
};

template <>
struct BarrierMethods<JS::BigInt*>
    : public detail::PtrBarrierMethodsBase<JS::BigInt> {
  static void postWriteBarrier(JS::BigInt** vp, JS::BigInt* prev,
                               JS::BigInt* next) {
    JS::HeapBigIntPostWriteBarrier(vp, prev, next);
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
struct JS_PUBLIC_API MovableCellHasher {
  using Key = T;
  using Lookup = T;

  static bool hasHash(const Lookup& l);
  static bool ensureHash(const Lookup& l);
  static HashNumber hash(const Lookup& l);
  static bool match(const Key& k, const Lookup& l);
  // The rekey hash policy method is not provided since you dont't need to
  // rekey any more when using this policy.
};

template <typename T>
struct JS_PUBLIC_API MovableCellHasher<JS::Heap<T>> {
  using Key = JS::Heap<T>;
  using Lookup = T;

  static bool hasHash(const Lookup& l) {
    return MovableCellHasher<T>::hasHash(l);
  }
  static bool ensureHash(const Lookup& l) {
    return MovableCellHasher<T>::ensureHash(l);
  }
  static HashNumber hash(const Lookup& l) {
    return MovableCellHasher<T>::hash(l);
  }
  static bool match(const Key& k, const Lookup& l) {
    return MovableCellHasher<T>::match(k.unbarrieredGet(), l);
  }
};

}  // namespace js

namespace mozilla {

template <typename T>
struct FallibleHashMethods<js::MovableCellHasher<T>> {
  template <typename Lookup>
  static bool hasHash(Lookup&& l) {
    return js::MovableCellHasher<T>::hasHash(std::forward<Lookup>(l));
  }
  template <typename Lookup>
  static bool ensureHash(Lookup&& l) {
    return js::MovableCellHasher<T>::ensureHash(std::forward<Lookup>(l));
  }
};

}  // namespace mozilla

namespace js {

struct VirtualTraceable {
  virtual ~VirtualTraceable() = default;
  virtual void trace(JSTracer* trc, const char* name) = 0;
};

template <typename T>
struct RootedTraceable final : public VirtualTraceable {
  static_assert(JS::MapTypeToRootKind<T>::kind == JS::RootKind::Traceable,
                "RootedTraceable is intended only for usage with a Traceable");

  T ptr;

  template <typename U>
  MOZ_IMPLICIT RootedTraceable(U&& initial) : ptr(std::forward<U>(initial)) {}

  operator T&() { return ptr; }
  operator const T&() const { return ptr; }

  void trace(JSTracer* trc, const char* name) override {
    JS::GCPolicy<T>::trace(trc, &ptr, name);
  }
};

template <typename T>
struct RootedTraceableTraits {
  static T* address(RootedTraceable<T>& self) { return &self.ptr; }
  static const T* address(const RootedTraceable<T>& self) { return &self.ptr; }
  static void trace(JSTracer* trc, VirtualTraceable* thingp, const char* name);
};

template <typename T>
struct RootedGCThingTraits {
  static T* address(T& self) { return &self; }
  static const T* address(const T& self) { return &self; }
  static void trace(JSTracer* trc, T* thingp, const char* name);
};

} /* namespace js */

namespace JS {

class JS_PUBLIC_API AutoGCRooter;

enum class AutoGCRooterKind : uint8_t {
  WrapperVector, /* js::AutoWrapperVector */
  Wrapper,       /* js::AutoWrapperRooter */
  Custom,        /* js::CustomAutoRooter */

  Limit
};

namespace detail {
// Dummy type to store root list entry pointers as. This code does not just use
// the actual type, because then eg JSObject* and JSFunction* would be assumed
// to never alias but they do (they are stored in the same list). Also, do not
// use `void*` so that `Rooted<void*>` is a compile error.
struct RootListEntry;
}  // namespace detail

template <>
struct MapTypeToRootKind<detail::RootListEntry*> {
  static const RootKind kind = RootKind::Traceable;
};

// Workaround MSVC issue where GCPolicy is needed even though this dummy type is
// never instantiated. Ideally, RootListEntry is removed in the future and an
// appropriate class hierarchy for the Rooted<T> types.
template <>
struct GCPolicy<detail::RootListEntry*>
    : public IgnoreGCPolicy<detail::RootListEntry*> {};

using RootedListHeads =
    mozilla::EnumeratedArray<RootKind, RootKind::Limit,
                             Rooted<detail::RootListEntry*>*>;

using AutoRooterListHeads =
    mozilla::EnumeratedArray<AutoGCRooterKind, AutoGCRooterKind::Limit,
                             AutoGCRooter*>;

// Superclass of JSContext which can be used for rooting data in use by the
// current thread but that does not provide all the functions of a JSContext.
class RootingContext {
  // Stack GC roots for Rooted GC heap pointers.
  RootedListHeads stackRoots_;
  template <typename T>
  friend class Rooted;

  // Stack GC roots for AutoFooRooter classes.
  AutoRooterListHeads autoGCRooters_;
  friend class AutoGCRooter;

  // Gecko profiling metadata.
  // This isn't really rooting related. It's only here because we want
  // GetContextProfilingStackIfEnabled to be inlineable into non-JS code, and
  // we didn't want to add another superclass of JSContext just for this.
  js::GeckoProfilerThread geckoProfiler_;

 public:
  RootingContext();

  void traceStackRoots(JSTracer* trc);

  /* Implemented in gc/RootMarking.cpp. */
  void traceAllGCRooters(JSTracer* trc);
  void traceWrapperGCRooters(JSTracer* trc);
  static void traceGCRooterList(JSTracer* trc, AutoGCRooter* head);

  void checkNoGCRooters();

  js::GeckoProfilerThread& geckoProfiler() { return geckoProfiler_; }

 protected:
  // The remaining members in this class should only be accessed through
  // JSContext pointers. They are unrelated to rooting and are in place so
  // that inlined API functions can directly access the data.

  /* The current realm. */
  Realm* realm_;

  /* The current zone. */
  Zone* zone_;

 public:
  /* Limit pointer for checking native stack consumption. */
  uintptr_t nativeStackLimit[StackKindCount];

#ifdef __wasi__
  // For WASI we can't catch call-stack overflows with stack-pointer checks, so
  // we count recursion depth with RAII based AutoCheckRecursionLimit.
  uint32_t wasiRecursionDepth = 0u;

  static constexpr uint32_t wasiRecursionDepthLimit = 100u;
#endif  // __wasi__

  static const RootingContext* get(const JSContext* cx) {
    return reinterpret_cast<const RootingContext*>(cx);
  }

  static RootingContext* get(JSContext* cx) {
    return reinterpret_cast<RootingContext*>(cx);
  }

  friend JS::Realm* js::GetContextRealm(const JSContext* cx);
  friend JS::Zone* js::GetContextZone(const JSContext* cx);
};

class JS_PUBLIC_API AutoGCRooter {
 public:
  using Kind = AutoGCRooterKind;

  AutoGCRooter(JSContext* cx, Kind kind)
      : AutoGCRooter(JS::RootingContext::get(cx), kind) {}
  AutoGCRooter(RootingContext* cx, Kind kind)
      : down(cx->autoGCRooters_[kind]),
        stackTop(&cx->autoGCRooters_[kind]),
        kind_(kind) {
    MOZ_ASSERT(this != *stackTop);
    *stackTop = this;
  }

  ~AutoGCRooter() {
    MOZ_ASSERT(this == *stackTop);
    *stackTop = down;
  }

  void trace(JSTracer* trc);

 private:
  friend class RootingContext;

  AutoGCRooter* const down;
  AutoGCRooter** const stackTop;

  /*
   * Discriminates actual subclass of this being used. The meaning is
   * indicated by the corresponding value in the Kind enum.
   */
  Kind kind_;

  /* No copy or assignment semantics. */
  AutoGCRooter(AutoGCRooter& ida) = delete;
  void operator=(AutoGCRooter& ida) = delete;
} JS_HAZ_ROOTED_BASE;

/**
 * Custom rooting behavior for internal and external clients.
 *
 * Deprecated. Where possible, use Rooted<> instead.
 */
class MOZ_RAII JS_PUBLIC_API CustomAutoRooter : private AutoGCRooter {
 public:
  template <typename CX>
  explicit CustomAutoRooter(const CX& cx)
      : AutoGCRooter(cx, AutoGCRooter::Kind::Custom) {}

  friend void AutoGCRooter::trace(JSTracer* trc);

 protected:
  virtual ~CustomAutoRooter() = default;

  /** Supplied by derived class to trace roots. */
  virtual void trace(JSTracer* trc) = 0;
};

namespace detail {

template <typename T>
using RootedPtr =
    std::conditional_t<MapTypeToRootKind<T>::kind == JS::RootKind::Traceable,
                       js::RootedTraceable<T>, T>;

template <typename T>
using RootedPtrTraits =
    std::conditional_t<MapTypeToRootKind<T>::kind == JS::RootKind::Traceable,
                       js::RootedTraceableTraits<T>,
                       js::RootedGCThingTraits<T>>;

// Dummy types to make it easier to understand template overload preference
// ordering.
struct FallbackOverload {};
struct PreferredOverload : FallbackOverload {};
using OverloadSelector = PreferredOverload;

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
class MOZ_RAII Rooted : public js::RootedBase<T, Rooted<T>> {
  using Ptr = detail::RootedPtr<T>;
  using PtrTraits = detail::RootedPtrTraits<T>;

  inline void registerWithRootLists(RootedListHeads& roots) {
    this->stack = &roots[JS::MapTypeToRootKind<T>::kind];
    this->prev = *stack;
    *stack = reinterpret_cast<Rooted<detail::RootListEntry*>*>(this);
  }

  inline RootedListHeads& rootLists(RootingContext* cx) {
    return cx->stackRoots_;
  }
  inline RootedListHeads& rootLists(JSContext* cx) {
    return rootLists(RootingContext::get(cx));
  }

  // Define either one or two Rooted(cx) constructors: the fallback one, which
  // constructs a Rooted holding a SafelyInitialized<T>, and a convenience one
  // for types that can be constructed with a cx, which will give a Rooted
  // holding a T(cx).

  // Dummy type to distinguish these constructors from Rooted(cx, initial)
  struct CtorDispatcher {};

  // Normal case: construct an empty Rooted holding a safely initialized but
  // empty T.
  template <typename RootingContext>
  Rooted(const RootingContext& cx, CtorDispatcher, detail::FallbackOverload)
      : Rooted(cx, SafelyInitialized<T>()) {}

  // If T can be constructed with a cx, then define another constructor for it
  // that will be preferred.
  template <
      typename RootingContext,
      typename = std::enable_if_t<std::is_constructible_v<T, RootingContext>>>
  Rooted(const RootingContext& cx, CtorDispatcher, detail::PreferredOverload)
      : Rooted(cx, T(cx)) {}

 public:
  using ElementType = T;

  // Construct an empty Rooted. Delegates to an internal constructor that
  // chooses a specific meaning of "empty" depending on whether T can be
  // constructed with a cx.
  template <typename RootingContext>
  explicit Rooted(const RootingContext& cx)
      : Rooted(cx, CtorDispatcher(), detail::OverloadSelector()) {}

  template <typename RootingContext, typename S>
  Rooted(const RootingContext& cx, S&& initial)
      : ptr(std::forward<S>(initial)) {
    MOZ_ASSERT(GCPolicy<T>::isValid(ptr));
    registerWithRootLists(rootLists(cx));
  }

  ~Rooted() {
    MOZ_ASSERT(*stack ==
               reinterpret_cast<Rooted<detail::RootListEntry*>*>(this));
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
    ptr = std::move(value);
    MOZ_ASSERT(GCPolicy<T>::isValid(ptr));
  }

  DECLARE_POINTER_CONSTREF_OPS(T);
  DECLARE_POINTER_ASSIGN_OPS(Rooted, T);

  T& get() { return ptr; }
  const T& get() const { return ptr; }

  T* address() { return PtrTraits::address(ptr); }
  const T* address() const { return PtrTraits::address(ptr); }

  void trace(JSTracer* trc, const char* name);

 private:
  /*
   * These need to be templated on RootListEntry* to avoid aliasing issues
   * between, for example, Rooted<JSObject*> and Rooted<JSFunction*>, which use
   * the same stack head pointer for different classes.
   */
  Rooted<detail::RootListEntry*>** stack;
  Rooted<detail::RootListEntry*>* prev;

  Ptr ptr;

  Rooted(const Rooted&) = delete;
} JS_HAZ_ROOTED;

namespace detail {

template <typename T>
struct DefineComparisonOps<Rooted<T>> : std::true_type {
  static const T& get(const Rooted<T>& v) { return v.get(); }
};

}  // namespace detail

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
inline JS::Realm* GetContextRealm(const JSContext* cx) {
  return JS::RootingContext::get(cx)->realm_;
}

inline JS::Compartment* GetContextCompartment(const JSContext* cx) {
  if (JS::Realm* realm = GetContextRealm(cx)) {
    return GetCompartmentForRealm(realm);
  }
  return nullptr;
}

inline JS::Zone* GetContextZone(const JSContext* cx) {
  return JS::RootingContext::get(cx)->zone_;
}

inline ProfilingStack* GetContextProfilingStackIfEnabled(JSContext* cx) {
  return JS::RootingContext::get(cx)
      ->geckoProfiler()
      .getProfilingStackIfEnabled();
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
class RootedBase<JSObject*, Container>
    : public MutableWrappedPtrOperations<JSObject*, Container> {
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
class HandleBase<JSObject*, Container>
    : public WrappedPtrOperations<JSObject*, Container> {
 public:
  template <class U>
  JS::Handle<U*> as() const;
};

} /* namespace js */

namespace JS {

template <typename T>
template <typename S>
inline Handle<T>::Handle(
    const Rooted<S>& root,
    std::enable_if_t<std::is_convertible_v<S, T>, int> dummy) {
  ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T>
template <typename S>
inline Handle<T>::Handle(
    const PersistentRooted<S>& root,
    std::enable_if_t<std::is_convertible_v<S, T>, int> dummy) {
  ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T>
template <typename S>
inline Handle<T>::Handle(
    MutableHandle<S>& root,
    std::enable_if_t<std::is_convertible_v<S, T>, int> dummy) {
  ptr = reinterpret_cast<const T*>(root.address());
}

template <typename T>
inline MutableHandle<T>::MutableHandle(Rooted<T>* root) {
  static_assert(sizeof(MutableHandle<T>) == sizeof(T*),
                "MutableHandle must be binary compatible with T*.");
  ptr = root->address();
}

template <typename T>
inline MutableHandle<T>::MutableHandle(PersistentRooted<T>* root) {
  static_assert(sizeof(MutableHandle<T>) == sizeof(T*),
                "MutableHandle must be binary compatible with T*.");
  ptr = root->address();
}

JS_PUBLIC_API void AddPersistentRoot(
    RootingContext* cx, RootKind kind,
    PersistentRooted<detail::RootListEntry*>* root);

JS_PUBLIC_API void AddPersistentRoot(
    JSRuntime* rt, RootKind kind,
    PersistentRooted<detail::RootListEntry*>* root);

/**
 * A copyable, assignable global GC root type with arbitrary lifetime, an
 * infallible constructor, and automatic unrooting on destruction.
 *
 * These roots can be used in heap-allocated data structures, so they are not
 * associated with any particular JSContext or stack. They are registered with
 * the JSRuntime itself, without locking. Initialization may take place on
 * construction, or in two phases if the no-argument constructor is called
 * followed by init().
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
template <typename T>
class PersistentRooted
    : public js::RootedBase<T, PersistentRooted<T>>,
      private mozilla::LinkedListElement<PersistentRooted<T>> {
  using ListBase = mozilla::LinkedListElement<PersistentRooted<T>>;
  using Ptr = detail::RootedPtr<T>;
  using PtrTraits = detail::RootedPtrTraits<T>;

  friend class mozilla::LinkedList<PersistentRooted>;
  friend class mozilla::LinkedListElement<PersistentRooted>;

  void registerWithRootLists(RootingContext* cx) {
    MOZ_ASSERT(!initialized());
    JS::RootKind kind = JS::MapTypeToRootKind<T>::kind;
    AddPersistentRoot(
        cx, kind,
        reinterpret_cast<JS::PersistentRooted<detail::RootListEntry*>*>(this));
  }

  void registerWithRootLists(JSRuntime* rt) {
    MOZ_ASSERT(!initialized());
    JS::RootKind kind = JS::MapTypeToRootKind<T>::kind;
    AddPersistentRoot(
        rt, kind,
        reinterpret_cast<JS::PersistentRooted<detail::RootListEntry*>*>(this));
  }

 public:
  using ElementType = T;

  PersistentRooted() : ptr(SafelyInitialized<T>()) {}

  explicit PersistentRooted(RootingContext* cx) : ptr(SafelyInitialized<T>()) {
    registerWithRootLists(cx);
  }

  explicit PersistentRooted(JSContext* cx) : ptr(SafelyInitialized<T>()) {
    registerWithRootLists(RootingContext::get(cx));
  }

  template <typename U>
  PersistentRooted(RootingContext* cx, U&& initial)
      : ptr(std::forward<U>(initial)) {
    registerWithRootLists(cx);
  }

  template <typename U>
  PersistentRooted(JSContext* cx, U&& initial) : ptr(std::forward<U>(initial)) {
    registerWithRootLists(RootingContext::get(cx));
  }

  explicit PersistentRooted(JSRuntime* rt) : ptr(SafelyInitialized<T>()) {
    registerWithRootLists(rt);
  }

  template <typename U>
  PersistentRooted(JSRuntime* rt, U&& initial) : ptr(std::forward<U>(initial)) {
    registerWithRootLists(rt);
  }

  PersistentRooted(const PersistentRooted& rhs)
      : mozilla::LinkedListElement<PersistentRooted<T>>(), ptr(rhs.ptr) {
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

  bool initialized() const { return ListBase::isInList(); }

  void init(RootingContext* cx) { init(cx, SafelyInitialized<T>()); }
  void init(JSContext* cx) { init(RootingContext::get(cx)); }

  template <typename U>
  void init(RootingContext* cx, U&& initial) {
    ptr = std::forward<U>(initial);
    registerWithRootLists(cx);
  }
  template <typename U>
  void init(JSContext* cx, U&& initial) {
    ptr = std::forward<U>(initial);
    registerWithRootLists(RootingContext::get(cx));
  }

  void reset() {
    if (initialized()) {
      set(SafelyInitialized<T>());
      ListBase::remove();
    }
  }

  DECLARE_POINTER_CONSTREF_OPS(T);
  DECLARE_POINTER_ASSIGN_OPS(PersistentRooted, T);

  T& get() { return ptr; }
  const T& get() const { return ptr; }

  T* address() {
    MOZ_ASSERT(initialized());
    return PtrTraits::address(ptr);
  }
  const T* address() const { return PtrTraits::address(ptr); }

  template <typename U>
  void set(U&& value) {
    MOZ_ASSERT(initialized());
    ptr = std::forward<U>(value);
  }

  void trace(JSTracer* trc, const char* name);

 private:
  Ptr ptr;
} JS_HAZ_ROOTED;

namespace detail {

template <typename T>
struct DefineComparisonOps<PersistentRooted<T>> : std::true_type {
  static const T& get(const PersistentRooted<T>& v) { return v.get(); }
};

}  // namespace detail

} /* namespace JS */

namespace js {

template <typename T, typename D, typename Container>
class WrappedPtrOperations<UniquePtr<T, D>, Container> {
  const UniquePtr<T, D>& uniquePtr() const {
    return static_cast<const Container*>(this)->get();
  }

 public:
  explicit operator bool() const { return !!uniquePtr(); }
  T* get() const { return uniquePtr().get(); }
  T* operator->() const { return get(); }
  T& operator*() const { return *uniquePtr(); }
};

template <typename T, typename D, typename Container>
class MutableWrappedPtrOperations<UniquePtr<T, D>, Container>
    : public WrappedPtrOperations<UniquePtr<T, D>, Container> {
  UniquePtr<T, D>& uniquePtr() { return static_cast<Container*>(this)->get(); }

 public:
  [[nodiscard]] typename UniquePtr<T, D>::Pointer release() {
    return uniquePtr().release();
  }
  void reset(T* ptr = T()) { uniquePtr().reset(ptr); }
};

template <typename T, typename Container>
class WrappedPtrOperations<mozilla::Maybe<T>, Container> {
  const mozilla::Maybe<T>& maybe() const {
    return static_cast<const Container*>(this)->get();
  }

 public:
  // This only supports a subset of Maybe's interface.
  bool isSome() const { return maybe().isSome(); }
  bool isNothing() const { return maybe().isNothing(); }
  const T value() const { return maybe().value(); }
  const T* operator->() const { return maybe().ptr(); }
  const T& operator*() const { return maybe().ref(); }
};

template <typename T, typename Container>
class MutableWrappedPtrOperations<mozilla::Maybe<T>, Container>
    : public WrappedPtrOperations<mozilla::Maybe<T>, Container> {
  mozilla::Maybe<T>& maybe() { return static_cast<Container*>(this)->get(); }

 public:
  // This only supports a subset of Maybe's interface.
  T* operator->() { return maybe().ptr(); }
  T& operator*() { return maybe().ref(); }
  void reset() { return maybe().reset(); }
};

namespace gc {

template <typename T, typename TraceCallbacks>
void CallTraceCallbackOnNonHeap(T* v, const TraceCallbacks& aCallbacks,
                                const char* aName, void* aClosure) {
  static_assert(sizeof(T) == sizeof(JS::Heap<T>),
                "T and Heap<T> must be compatible.");
  MOZ_ASSERT(v);
  mozilla::DebugOnly<Cell*> cell = BarrierMethods<T>::asGCThingOrNull(*v);
  MOZ_ASSERT(cell);
  MOZ_ASSERT(!IsInsideNursery(cell));
  JS::Heap<T>* asHeapT = reinterpret_cast<JS::Heap<T>*>(v);
  aCallbacks.Trace(asHeapT, aName, aClosure);
}

} /* namespace gc */

} /* namespace js */

#endif /* js_RootingAPI_h */

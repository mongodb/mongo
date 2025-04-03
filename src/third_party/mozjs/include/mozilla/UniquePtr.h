/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Smart pointer managing sole ownership of a resource. */

#ifndef mozilla_UniquePtr_h
#define mozilla_UniquePtr_h

#include <type_traits>
#include <utility>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/CompactPair.h"
#include "mozilla/Compiler.h"

namespace mozilla {

template <typename T>
class DefaultDelete;
template <typename T, class D = DefaultDelete<T>>
class UniquePtr;

}  // namespace mozilla

namespace mozilla {

namespace detail {

struct HasPointerTypeHelper {
  template <class U>
  static double Test(...);
  template <class U>
  static char Test(typename U::pointer* = 0);
};

template <class T>
class HasPointerType
    : public std::integral_constant<bool, sizeof(HasPointerTypeHelper::Test<T>(
                                              0)) == 1> {};

template <class T, class D, bool = HasPointerType<D>::value>
struct PointerTypeImpl {
  typedef typename D::pointer Type;
};

template <class T, class D>
struct PointerTypeImpl<T, D, false> {
  typedef T* Type;
};

template <class T, class D>
struct PointerType {
  typedef typename PointerTypeImpl<T, std::remove_reference_t<D>>::Type Type;
};

}  // namespace detail

/**
 * UniquePtr is a smart pointer that wholly owns a resource.  Ownership may be
 * transferred out of a UniquePtr through explicit action, but otherwise the
 * resource is destroyed when the UniquePtr is destroyed.
 *
 * UniquePtr is similar to C++98's std::auto_ptr, but it improves upon auto_ptr
 * in one crucial way: it's impossible to copy a UniquePtr.  Copying an auto_ptr
 * obviously *can't* copy ownership of its singly-owned resource.  So what
 * happens if you try to copy one?  Bizarrely, ownership is implicitly
 * *transferred*, preserving single ownership but breaking code that assumes a
 * copy of an object is identical to the original.  (This is why auto_ptr is
 * prohibited in STL containers.)
 *
 * UniquePtr solves this problem by being *movable* rather than copyable.
 * Instead of passing a |UniquePtr u| directly to the constructor or assignment
 * operator, you pass |Move(u)|.  In doing so you indicate that you're *moving*
 * ownership out of |u|, into the target of the construction/assignment.  After
 * the transfer completes, |u| contains |nullptr| and may be safely destroyed.
 * This preserves single ownership but also allows UniquePtr to be moved by
 * algorithms that have been made move-safe.  (Note: if |u| is instead a
 * temporary expression, don't use |Move()|: just pass the expression, because
 * it's already move-ready.  For more information see Move.h.)
 *
 * UniquePtr is also better than std::auto_ptr in that the deletion operation is
 * customizable.  An optional second template parameter specifies a class that
 * (through its operator()(T*)) implements the desired deletion policy.  If no
 * policy is specified, mozilla::DefaultDelete<T> is used -- which will either
 * |delete| or |delete[]| the resource, depending whether the resource is an
 * array.  Custom deletion policies ideally should be empty classes (no member
 * fields, no member fields in base classes, no virtual methods/inheritance),
 * because then UniquePtr can be just as efficient as a raw pointer.
 *
 * Use of UniquePtr proceeds like so:
 *
 *   UniquePtr<int> g1; // initializes to nullptr
 *   g1.reset(new int); // switch resources using reset()
 *   g1 = nullptr; // clears g1, deletes the int
 *
 *   UniquePtr<int> g2(new int); // owns that int
 *   int* p = g2.release(); // g2 leaks its int -- still requires deletion
 *   delete p; // now freed
 *
 *   struct S { int x; S(int x) : x(x) {} };
 *   UniquePtr<S> g3, g4(new S(5));
 *   g3 = std::move(g4); // g3 owns the S, g4 cleared
 *   S* p = g3.get(); // g3 still owns |p|
 *   assert(g3->x == 5); // operator-> works (if .get() != nullptr)
 *   assert((*g3).x == 5); // also operator* (again, if not cleared)
 *   std::swap(g3, g4); // g4 now owns the S, g3 cleared
 *   g3.swap(g4);  // g3 now owns the S, g4 cleared
 *   UniquePtr<S> g5(std::move(g3)); // g5 owns the S, g3 cleared
 *   g5.reset(); // deletes the S, g5 cleared
 *
 *   struct FreePolicy { void operator()(void* p) { free(p); } };
 *   UniquePtr<int, FreePolicy> g6(static_cast<int*>(malloc(sizeof(int))));
 *   int* ptr = g6.get();
 *   g6 = nullptr; // calls free(ptr)
 *
 * Now, carefully note a few things you *can't* do:
 *
 *   UniquePtr<int> b1;
 *   b1 = new int; // BAD: can only assign another UniquePtr
 *   int* ptr = b1; // BAD: no auto-conversion to pointer, use get()
 *
 *   UniquePtr<int> b2(b1); // BAD: can't copy a UniquePtr
 *   UniquePtr<int> b3 = b1; // BAD: can't copy-assign a UniquePtr
 *
 * (Note that changing a UniquePtr to store a direct |new| expression is
 * permitted, but usually you should use MakeUnique, defined at the end of this
 * header.)
 *
 * A few miscellaneous notes:
 *
 * UniquePtr, when not instantiated for an array type, can be move-constructed
 * and move-assigned, not only from itself but from "derived" UniquePtr<U, E>
 * instantiations where U converts to T and E converts to D.  If you want to use
 * this, you're going to have to specify a deletion policy for both UniquePtr
 * instantations, and T pretty much has to have a virtual destructor.  In other
 * words, this doesn't work:
 *
 *   struct Base { virtual ~Base() {} };
 *   struct Derived : Base {};
 *
 *   UniquePtr<Base> b1;
 *   // BAD: DefaultDelete<Base> and DefaultDelete<Derived> don't interconvert
 *   UniquePtr<Derived> d1(std::move(b));
 *
 *   UniquePtr<Base> b2;
 *   UniquePtr<Derived, DefaultDelete<Base>> d2(std::move(b2)); // okay
 *
 * UniquePtr is specialized for array types.  Specializing with an array type
 * creates a smart-pointer version of that array -- not a pointer to such an
 * array.
 *
 *   UniquePtr<int[]> arr(new int[5]);
 *   arr[0] = 4;
 *
 * What else is different?  Deletion of course uses |delete[]|.  An operator[]
 * is provided.  Functionality that doesn't make sense for arrays is removed.
 * The constructors and mutating methods only accept array pointers (not T*, U*
 * that converts to T*, or UniquePtr<U[]> or UniquePtr<U>) or |nullptr|.
 *
 * It's perfectly okay for a function to return a UniquePtr. This transfers
 * the UniquePtr's sole ownership of the data, to the fresh UniquePtr created
 * in the calling function, that will then solely own that data. Such functions
 * can return a local variable UniquePtr, |nullptr|, |UniquePtr(ptr)| where
 * |ptr| is a |T*|, or a UniquePtr |Move()|'d from elsewhere.
 *
 * UniquePtr will commonly be a member of a class, with lifetime equivalent to
 * that of that class.  If you want to expose the related resource, you could
 * expose a raw pointer via |get()|, but ownership of a raw pointer is
 * inherently unclear.  So it's better to expose a |const UniquePtr&| instead.
 * This prohibits mutation but still allows use of |get()| when needed (but
 * operator-> is preferred).  Of course, you can only use this smart pointer as
 * long as the enclosing class instance remains live -- no different than if you
 * exposed the |get()| raw pointer.
 *
 * To pass a UniquePtr-managed resource as a pointer, use a |const UniquePtr&|
 * argument.  To specify an inout parameter (where the method may or may not
 * take ownership of the resource, or reset it), or to specify an out parameter
 * (where simply returning a |UniquePtr| isn't possible), use a |UniquePtr&|
 * argument.  To unconditionally transfer ownership of a UniquePtr
 * into a method, use a |UniquePtr| argument.  To conditionally transfer
 * ownership of a resource into a method, should the method want it, use a
 * |UniquePtr&&| argument.
 */
template <typename T, class D>
class UniquePtr {
 public:
  typedef T ElementType;
  typedef D DeleterType;
  typedef typename detail::PointerType<T, DeleterType>::Type Pointer;

 private:
  mozilla::CompactPair<Pointer, DeleterType> mTuple;

  Pointer& ptr() { return mTuple.first(); }
  const Pointer& ptr() const { return mTuple.first(); }

  DeleterType& del() { return mTuple.second(); }
  const DeleterType& del() const { return mTuple.second(); }

 public:
  /**
   * Construct a UniquePtr containing |nullptr|.
   */
  constexpr UniquePtr() : mTuple(static_cast<Pointer>(nullptr), DeleterType()) {
    static_assert(!std::is_pointer_v<D>, "must provide a deleter instance");
    static_assert(!std::is_reference_v<D>, "must provide a deleter instance");
  }

  /**
   * Construct a UniquePtr containing |aPtr|.
   */
  explicit UniquePtr(Pointer aPtr) : mTuple(aPtr, DeleterType()) {
    static_assert(!std::is_pointer_v<D>, "must provide a deleter instance");
    static_assert(!std::is_reference_v<D>, "must provide a deleter instance");
  }

  UniquePtr(Pointer aPtr,
            std::conditional_t<std::is_reference_v<D>, D, const D&> aD1)
      : mTuple(aPtr, aD1) {}

  UniquePtr(Pointer aPtr, std::remove_reference_t<D>&& aD2)
      : mTuple(aPtr, std::move(aD2)) {
    static_assert(!std::is_reference_v<D>,
                  "rvalue deleter can't be stored by reference");
  }

  UniquePtr(UniquePtr&& aOther)
      : mTuple(aOther.release(),
               std::forward<DeleterType>(aOther.get_deleter())) {}

  MOZ_IMPLICIT constexpr UniquePtr(decltype(nullptr)) : UniquePtr() {}

  template <typename U, class E>
  MOZ_IMPLICIT UniquePtr(
      UniquePtr<U, E>&& aOther,
      std::enable_if_t<
          std::is_convertible_v<typename UniquePtr<U, E>::Pointer, Pointer> &&
              !std::is_array_v<U> &&
              (std::is_reference_v<D> ? std::is_same_v<D, E>
                                      : std::is_convertible_v<E, D>),
          int>
          aDummy = 0)
      : mTuple(aOther.release(), std::forward<E>(aOther.get_deleter())) {}

  ~UniquePtr() { reset(nullptr); }

  UniquePtr& operator=(UniquePtr&& aOther) {
    reset(aOther.release());
    get_deleter() = std::forward<DeleterType>(aOther.get_deleter());
    return *this;
  }

  template <typename U, typename E>
  UniquePtr& operator=(UniquePtr<U, E>&& aOther) {
    static_assert(
        std::is_convertible_v<typename UniquePtr<U, E>::Pointer, Pointer>,
        "incompatible UniquePtr pointees");
    static_assert(!std::is_array_v<U>,
                  "can't assign from UniquePtr holding an array");

    reset(aOther.release());
    get_deleter() = std::forward<E>(aOther.get_deleter());
    return *this;
  }

  UniquePtr& operator=(decltype(nullptr)) {
    reset(nullptr);
    return *this;
  }

  std::add_lvalue_reference_t<T> operator*() const {
    MOZ_ASSERT(get(), "dereferencing a UniquePtr containing nullptr with *");
    return *get();
  }
  Pointer operator->() const {
    MOZ_ASSERT(get(), "dereferencing a UniquePtr containing nullptr with ->");
    return get();
  }

  explicit operator bool() const { return get() != nullptr; }

  Pointer get() const { return ptr(); }

  DeleterType& get_deleter() { return del(); }
  const DeleterType& get_deleter() const { return del(); }

  [[nodiscard]] Pointer release() {
    Pointer p = ptr();
    ptr() = nullptr;
    return p;
  }

  void reset(Pointer aPtr = Pointer()) {
    Pointer old = ptr();
    ptr() = aPtr;
    if (old != nullptr) {
      get_deleter()(old);
    }
  }

  void swap(UniquePtr& aOther) { mTuple.swap(aOther.mTuple); }

  UniquePtr(const UniquePtr& aOther) = delete;  // construct using std::move()!
  void operator=(const UniquePtr& aOther) =
      delete;  // assign using std::move()!
};

// In case you didn't read the comment by the main definition (you should!): the
// UniquePtr<T[]> specialization exists to manage array pointers.  It deletes
// such pointers using delete[], it will reject construction and modification
// attempts using U* or U[].  Otherwise it works like the normal UniquePtr.
template <typename T, class D>
class UniquePtr<T[], D> {
 public:
  typedef T* Pointer;
  typedef T ElementType;
  typedef D DeleterType;

 private:
  mozilla::CompactPair<Pointer, DeleterType> mTuple;

 public:
  /**
   * Construct a UniquePtr containing nullptr.
   */
  constexpr UniquePtr() : mTuple(static_cast<Pointer>(nullptr), DeleterType()) {
    static_assert(!std::is_pointer_v<D>, "must provide a deleter instance");
    static_assert(!std::is_reference_v<D>, "must provide a deleter instance");
  }

  /**
   * Construct a UniquePtr containing |aPtr|.
   */
  explicit UniquePtr(Pointer aPtr) : mTuple(aPtr, DeleterType()) {
    static_assert(!std::is_pointer_v<D>, "must provide a deleter instance");
    static_assert(!std::is_reference_v<D>, "must provide a deleter instance");
  }

  // delete[] knows how to handle *only* an array of a single class type.  For
  // delete[] to work correctly, it must know the size of each element, the
  // fields and base classes of each element requiring destruction, and so on.
  // So forbid all overloads which would end up invoking delete[] on a pointer
  // of the wrong type.
  template <typename U>
  UniquePtr(U&& aU,
            std::enable_if_t<
                std::is_pointer_v<U> && std::is_convertible_v<U, Pointer>, int>
                aDummy = 0) = delete;

  UniquePtr(Pointer aPtr,
            std::conditional_t<std::is_reference_v<D>, D, const D&> aD1)
      : mTuple(aPtr, aD1) {}

  UniquePtr(Pointer aPtr, std::remove_reference_t<D>&& aD2)
      : mTuple(aPtr, std::move(aD2)) {
    static_assert(!std::is_reference_v<D>,
                  "rvalue deleter can't be stored by reference");
  }

  // Forbidden for the same reasons as stated above.
  template <typename U, typename V>
  UniquePtr(U&& aU, V&& aV,
            std::enable_if_t<
                std::is_pointer_v<U> && std::is_convertible_v<U, Pointer>, int>
                aDummy = 0) = delete;

  UniquePtr(UniquePtr&& aOther)
      : mTuple(aOther.release(),
               std::forward<DeleterType>(aOther.get_deleter())) {}

  MOZ_IMPLICIT
  UniquePtr(decltype(nullptr)) : mTuple(nullptr, DeleterType()) {
    static_assert(!std::is_pointer_v<D>, "must provide a deleter instance");
    static_assert(!std::is_reference_v<D>, "must provide a deleter instance");
  }

  ~UniquePtr() { reset(nullptr); }

  UniquePtr& operator=(UniquePtr&& aOther) {
    reset(aOther.release());
    get_deleter() = std::forward<DeleterType>(aOther.get_deleter());
    return *this;
  }

  UniquePtr& operator=(decltype(nullptr)) {
    reset();
    return *this;
  }

  explicit operator bool() const { return get() != nullptr; }

  T& operator[](decltype(sizeof(int)) aIndex) const { return get()[aIndex]; }
  Pointer get() const { return mTuple.first(); }

  DeleterType& get_deleter() { return mTuple.second(); }
  const DeleterType& get_deleter() const { return mTuple.second(); }

  [[nodiscard]] Pointer release() {
    Pointer p = mTuple.first();
    mTuple.first() = nullptr;
    return p;
  }

  void reset(Pointer aPtr = Pointer()) {
    Pointer old = mTuple.first();
    mTuple.first() = aPtr;
    if (old != nullptr) {
      mTuple.second()(old);
    }
  }

  void reset(decltype(nullptr)) {
    Pointer old = mTuple.first();
    mTuple.first() = nullptr;
    if (old != nullptr) {
      mTuple.second()(old);
    }
  }

  template <typename U>
  void reset(U) = delete;

  void swap(UniquePtr& aOther) { mTuple.swap(aOther.mTuple); }

  UniquePtr(const UniquePtr& aOther) = delete;  // construct using std::move()!
  void operator=(const UniquePtr& aOther) =
      delete;  // assign using std::move()!
};

/**
 * A default deletion policy using plain old operator delete.
 *
 * Note that this type can be specialized, but authors should beware of the risk
 * that the specialization may at some point cease to match (either because it
 * gets moved to a different compilation unit or the signature changes). If the
 * non-specialized (|delete|-based) version compiles for that type but does the
 * wrong thing, bad things could happen.
 *
 * This is a non-issue for types which are always incomplete (i.e. opaque handle
 * types), since |delete|-ing such a type will always trigger a compilation
 * error.
 */
template <typename T>
class DefaultDelete {
 public:
  constexpr DefaultDelete() = default;

  template <typename U>
  MOZ_IMPLICIT DefaultDelete(
      const DefaultDelete<U>& aOther,
      std::enable_if_t<std::is_convertible_v<U*, T*>, int> aDummy = 0) {}

  void operator()(T* aPtr) const {
    static_assert(sizeof(T) > 0, "T must be complete");
    delete aPtr;
  }
};

/** A default deletion policy using operator delete[]. */
template <typename T>
class DefaultDelete<T[]> {
 public:
  constexpr DefaultDelete() = default;

  void operator()(T* aPtr) const {
    static_assert(sizeof(T) > 0, "T must be complete");
    delete[] aPtr;
  }

  template <typename U>
  void operator()(U* aPtr) const = delete;
};

template <typename T, class D, typename U, class E>
bool operator==(const UniquePtr<T, D>& aX, const UniquePtr<U, E>& aY) {
  return aX.get() == aY.get();
}

template <typename T, class D, typename U, class E>
bool operator!=(const UniquePtr<T, D>& aX, const UniquePtr<U, E>& aY) {
  return aX.get() != aY.get();
}

template <typename T, class D>
bool operator==(const UniquePtr<T, D>& aX, const T* aY) {
  return aX.get() == aY;
}

template <typename T, class D>
bool operator==(const T* aY, const UniquePtr<T, D>& aX) {
  return aY == aX.get();
}

template <typename T, class D>
bool operator!=(const UniquePtr<T, D>& aX, const T* aY) {
  return aX.get() != aY;
}

template <typename T, class D>
bool operator!=(const T* aY, const UniquePtr<T, D>& aX) {
  return aY != aX.get();
}

template <typename T, class D>
bool operator==(const UniquePtr<T, D>& aX, decltype(nullptr)) {
  return !aX;
}

template <typename T, class D>
bool operator==(decltype(nullptr), const UniquePtr<T, D>& aX) {
  return !aX;
}

template <typename T, class D>
bool operator!=(const UniquePtr<T, D>& aX, decltype(nullptr)) {
  return bool(aX);
}

template <typename T, class D>
bool operator!=(decltype(nullptr), const UniquePtr<T, D>& aX) {
  return bool(aX);
}

// No operator<, operator>, operator<=, operator>= for now because simplicity.

namespace detail {

template <typename T>
struct UniqueSelector {
  typedef UniquePtr<T> SingleObject;
};

template <typename T>
struct UniqueSelector<T[]> {
  typedef UniquePtr<T[]> UnknownBound;
};

template <typename T, decltype(sizeof(int)) N>
struct UniqueSelector<T[N]> {
  typedef UniquePtr<T[N]> KnownBound;
};

}  // namespace detail

/**
 * MakeUnique is a helper function for allocating new'd objects and arrays,
 * returning a UniquePtr containing the resulting pointer.  The semantics of
 * MakeUnique<Type>(...) are as follows.
 *
 *   If Type is an array T[n]:
 *     Disallowed, deleted, no overload for you!
 *   If Type is an array T[]:
 *     MakeUnique<T[]>(size_t) is the only valid overload.  The pointer returned
 *     is as if by |new T[n]()|, which value-initializes each element.  (If T
 *     isn't a class type, this will zero each element.  If T is a class type,
 *     then roughly speaking, each element will be constructed using its default
 *     constructor.  See C++11 [dcl.init]p7 for the full gory details.)
 *   If Type is non-array T:
 *     The arguments passed to MakeUnique<T>(...) are forwarded into a
 *     |new T(...)| call, initializing the T as would happen if executing
 *     |T(...)|.
 *
 * There are various benefits to using MakeUnique instead of |new| expressions.
 *
 * First, MakeUnique eliminates use of |new| from code entirely.  If objects are
 * only created through UniquePtr, then (assuming all explicit release() calls
 * are safe, including transitively, and no type-safety casting funniness)
 * correctly maintained ownership of the UniquePtr guarantees no leaks are
 * possible.  (This pays off best if a class is only ever created through a
 * factory method on the class, using a private constructor.)
 *
 * Second, initializing a UniquePtr using a |new| expression requires repeating
 * the name of the new'd type, whereas MakeUnique in concert with the |auto|
 * keyword names it only once:
 *
 *   UniquePtr<char> ptr1(new char()); // repetitive
 *   auto ptr2 = MakeUnique<char>();   // shorter
 *
 * Of course this assumes the reader understands the operation MakeUnique
 * performs.  In the long run this is probably a reasonable assumption.  In the
 * short run you'll have to use your judgment about what readers can be expected
 * to know, or to quickly look up.
 *
 * Third, a call to MakeUnique can be assigned directly to a UniquePtr.  In
 * contrast you can't assign a pointer into a UniquePtr without using the
 * cumbersome reset().
 *
 *   UniquePtr<char> p;
 *   p = new char;           // ERROR
 *   p.reset(new char);      // works, but fugly
 *   p = MakeUnique<char>(); // preferred
 *
 * (And third, although not relevant to Mozilla: MakeUnique is exception-safe.
 * An exception thrown after |new T| succeeds will leak that memory, unless the
 * pointer is assigned to an object that will manage its ownership.  UniquePtr
 * ably serves this function.)
 */

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject MakeUnique(Args&&... aArgs) {
  return UniquePtr<T>(new T(std::forward<Args>(aArgs)...));
}

template <typename T>
typename detail::UniqueSelector<T>::UnknownBound MakeUnique(
    decltype(sizeof(int)) aN) {
  using ArrayType = std::remove_extent_t<T>;
  return UniquePtr<T>(new ArrayType[aN]());
}

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound MakeUnique(Args&&... aArgs) =
    delete;

/**
 * WrapUnique is a helper function to transfer ownership from a raw pointer
 * into a UniquePtr<T>. It can only be used with a single non-array type.
 *
 * It is generally used this way:
 *
 *   auto p = WrapUnique(new char);
 *
 * It can be used when MakeUnique is not usable, for example, when the
 * constructor you are using is private, or you want to use aggregate
 * initialization.
 */

template <typename T>
typename detail::UniqueSelector<T>::SingleObject WrapUnique(T* aPtr) {
  return UniquePtr<T>(aPtr);
}

}  // namespace mozilla

namespace std {

template <typename T, class D>
void swap(mozilla::UniquePtr<T, D>& aX, mozilla::UniquePtr<T, D>& aY) {
  aX.swap(aY);
}

}  // namespace std

#endif /* mozilla_UniquePtr_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Smart pointer managing sole ownership of a resource. */

#ifndef mozilla_UniquePtr_h
#define mozilla_UniquePtr_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Compiler.h"
#include "mozilla/Move.h"
#include "mozilla/Pair.h"
#include "mozilla/TypeTraits.h"

namespace mozilla {

template<typename T> class DefaultDelete;
template<typename T, class D = DefaultDelete<T>> class UniquePtr;

} // namespace mozilla

namespace mozilla {

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
 *   g3 = Move(g4); // g3 owns the S, g4 cleared
 *   S* p = g3.get(); // g3 still owns |p|
 *   assert(g3->x == 5); // operator-> works (if .get() != nullptr)
 *   assert((*g3).x == 5); // also operator* (again, if not cleared)
 *   Swap(g3, g4); // g4 now owns the S, g3 cleared
 *   g3.swap(g4);  // g3 now owns the S, g4 cleared
 *   UniquePtr<S> g5(Move(g3)); // g5 owns the S, g3 cleared
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
 *   UniquePtr<Derived> d1(Move(b));
 *
 *   UniquePtr<Base> b2;
 *   UniquePtr<Derived, DefaultDelete<Base>> d2(Move(b2)); // okay
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
 * It's perfectly okay to return a UniquePtr from a method to assure the related
 * resource is properly deleted.  You'll need to use |Move()| when returning a
 * local UniquePtr.  Otherwise you can return |nullptr|, or you can return
 * |UniquePtr(ptr)|.
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
template<typename T, class D>
class UniquePtr
{
public:
  typedef T* Pointer;
  typedef T ElementType;
  typedef D DeleterType;

private:
  Pair<Pointer, DeleterType> mTuple;

  Pointer& ptr() { return mTuple.first(); }
  const Pointer& ptr() const { return mTuple.first(); }

  DeleterType& del() { return mTuple.second(); }
  const DeleterType& del() const { return mTuple.second(); }

public:
  /**
   * Construct a UniquePtr containing |nullptr|.
   */
  MOZ_CONSTEXPR UniquePtr()
    : mTuple(static_cast<Pointer>(nullptr), DeleterType())
  {
    static_assert(!IsPointer<D>::value, "must provide a deleter instance");
    static_assert(!IsReference<D>::value, "must provide a deleter instance");
  }

  /**
   * Construct a UniquePtr containing |aPtr|.
   */
  explicit UniquePtr(Pointer aPtr)
    : mTuple(aPtr, DeleterType())
  {
    static_assert(!IsPointer<D>::value, "must provide a deleter instance");
    static_assert(!IsReference<D>::value, "must provide a deleter instance");
  }

  UniquePtr(Pointer aPtr,
            typename Conditional<IsReference<D>::value,
                                 D,
                                 const D&>::Type aD1)
    : mTuple(aPtr, aD1)
  {}

  // If you encounter an error with MSVC10 about RemoveReference below, along
  // the lines that "more than one partial specialization matches the template
  // argument list": don't use UniquePtr<T, reference to function>!  Ideally
  // you should make deletion use the same function every time, using a
  // deleter policy:
  //
  //   // BAD, won't compile with MSVC10, deleter doesn't need to be a
  //   // variable at all
  //   typedef void (&FreeSignature)(void*);
  //   UniquePtr<int, FreeSignature> ptr((int*) malloc(sizeof(int)), free);
  //
  //   // GOOD, compiles with MSVC10, deletion behavior statically known and
  //   // optimizable
  //   struct DeleteByFreeing
  //   {
  //     void operator()(void* aPtr) { free(aPtr); }
  //   };
  //
  // If deletion really, truly, must be a variable: you might be able to work
  // around this with a deleter class that contains the function reference.
  // But this workaround is untried and untested, because variable deletion
  // behavior really isn't something you should use.
  UniquePtr(Pointer aPtr,
            typename RemoveReference<D>::Type&& aD2)
    : mTuple(aPtr, Move(aD2))
  {
    static_assert(!IsReference<D>::value,
                  "rvalue deleter can't be stored by reference");
  }

  UniquePtr(UniquePtr&& aOther)
    : mTuple(aOther.release(), Forward<DeleterType>(aOther.getDeleter()))
  {}

  MOZ_IMPLICIT
  UniquePtr(decltype(nullptr))
    : mTuple(nullptr, DeleterType())
  {
    static_assert(!IsPointer<D>::value, "must provide a deleter instance");
    static_assert(!IsReference<D>::value, "must provide a deleter instance");
  }

  template<typename U, class E>
  MOZ_IMPLICIT
  UniquePtr(UniquePtr<U, E>&& aOther,
            typename EnableIf<IsConvertible<typename UniquePtr<U, E>::Pointer,
                                            Pointer>::value &&
                              !IsArray<U>::value &&
                              (IsReference<D>::value
                               ? IsSame<D, E>::value
                               : IsConvertible<E, D>::value),
                              int>::Type aDummy = 0)
    : mTuple(aOther.release(), Forward<E>(aOther.getDeleter()))
  {
  }

  ~UniquePtr() { reset(nullptr); }

  UniquePtr& operator=(UniquePtr&& aOther)
  {
    reset(aOther.release());
    getDeleter() = Forward<DeleterType>(aOther.getDeleter());
    return *this;
  }

  template<typename U, typename E>
  UniquePtr& operator=(UniquePtr<U, E>&& aOther)
  {
    static_assert(IsConvertible<typename UniquePtr<U, E>::Pointer,
                                Pointer>::value,
                  "incompatible UniquePtr pointees");
    static_assert(!IsArray<U>::value,
                  "can't assign from UniquePtr holding an array");

    reset(aOther.release());
    getDeleter() = Forward<E>(aOther.getDeleter());
    return *this;
  }

  UniquePtr& operator=(decltype(nullptr))
  {
    reset(nullptr);
    return *this;
  }

  T& operator*() const { return *get(); }
  Pointer operator->() const
  {
    MOZ_ASSERT(get(), "dereferencing a UniquePtr containing nullptr");
    return get();
  }

  explicit operator bool() const { return get() != nullptr; }

  Pointer get() const { return ptr(); }

  DeleterType& getDeleter() { return del(); }
  const DeleterType& getDeleter() const { return del(); }

  Pointer release()
  {
    Pointer p = ptr();
    ptr() = nullptr;
    return p;
  }

  void reset(Pointer aPtr = Pointer())
  {
    Pointer old = ptr();
    ptr() = aPtr;
    if (old != nullptr) {
      getDeleter()(old);
    }
  }

  void swap(UniquePtr& aOther)
  {
    mTuple.swap(aOther.mTuple);
  }

  UniquePtr(const UniquePtr& aOther) = delete; // construct using Move()!
  void operator=(const UniquePtr& aOther) = delete; // assign using Move()!
};

// In case you didn't read the comment by the main definition (you should!): the
// UniquePtr<T[]> specialization exists to manage array pointers.  It deletes
// such pointers using delete[], it will reject construction and modification
// attempts using U* or U[].  Otherwise it works like the normal UniquePtr.
template<typename T, class D>
class UniquePtr<T[], D>
{
public:
  typedef T* Pointer;
  typedef T ElementType;
  typedef D DeleterType;

private:
  Pair<Pointer, DeleterType> mTuple;

public:
  /**
   * Construct a UniquePtr containing nullptr.
   */
  MOZ_CONSTEXPR UniquePtr()
    : mTuple(static_cast<Pointer>(nullptr), DeleterType())
  {
    static_assert(!IsPointer<D>::value, "must provide a deleter instance");
    static_assert(!IsReference<D>::value, "must provide a deleter instance");
  }

  /**
   * Construct a UniquePtr containing |aPtr|.
   */
  explicit UniquePtr(Pointer aPtr)
    : mTuple(aPtr, DeleterType())
  {
    static_assert(!IsPointer<D>::value, "must provide a deleter instance");
    static_assert(!IsReference<D>::value, "must provide a deleter instance");
  }

  // delete[] knows how to handle *only* an array of a single class type.  For
  // delete[] to work correctly, it must know the size of each element, the
  // fields and base classes of each element requiring destruction, and so on.
  // So forbid all overloads which would end up invoking delete[] on a pointer
  // of the wrong type.
  template<typename U>
  UniquePtr(U&& aU,
            typename EnableIf<IsPointer<U>::value &&
                              IsConvertible<U, Pointer>::value,
                              int>::Type aDummy = 0)
  = delete;

  UniquePtr(Pointer aPtr,
            typename Conditional<IsReference<D>::value,
                                 D,
                                 const D&>::Type aD1)
    : mTuple(aPtr, aD1)
  {}

  // If you encounter an error with MSVC10 about RemoveReference below, along
  // the lines that "more than one partial specialization matches the template
  // argument list": don't use UniquePtr<T[], reference to function>!  See the
  // comment by this constructor in the non-T[] specialization above.
  UniquePtr(Pointer aPtr,
            typename RemoveReference<D>::Type&& aD2)
    : mTuple(aPtr, Move(aD2))
  {
    static_assert(!IsReference<D>::value,
                  "rvalue deleter can't be stored by reference");
  }

  // Forbidden for the same reasons as stated above.
  template<typename U, typename V>
  UniquePtr(U&& aU, V&& aV,
            typename EnableIf<IsPointer<U>::value &&
                              IsConvertible<U, Pointer>::value,
                              int>::Type aDummy = 0)
  = delete;

  UniquePtr(UniquePtr&& aOther)
    : mTuple(aOther.release(), Forward<DeleterType>(aOther.getDeleter()))
  {}

  MOZ_IMPLICIT
  UniquePtr(decltype(nullptr))
    : mTuple(nullptr, DeleterType())
  {
    static_assert(!IsPointer<D>::value, "must provide a deleter instance");
    static_assert(!IsReference<D>::value, "must provide a deleter instance");
  }

  ~UniquePtr() { reset(nullptr); }

  UniquePtr& operator=(UniquePtr&& aOther)
  {
    reset(aOther.release());
    getDeleter() = Forward<DeleterType>(aOther.getDeleter());
    return *this;
  }

  UniquePtr& operator=(decltype(nullptr))
  {
    reset();
    return *this;
  }

  explicit operator bool() const { return get() != nullptr; }

  T& operator[](decltype(sizeof(int)) aIndex) const { return get()[aIndex]; }
  Pointer get() const { return mTuple.first(); }

  DeleterType& getDeleter() { return mTuple.second(); }
  const DeleterType& getDeleter() const { return mTuple.second(); }

  Pointer release()
  {
    Pointer p = mTuple.first();
    mTuple.first() = nullptr;
    return p;
  }

  void reset(Pointer aPtr = Pointer())
  {
    Pointer old = mTuple.first();
    mTuple.first() = aPtr;
    if (old != nullptr) {
      mTuple.second()(old);
    }
  }

  void reset(decltype(nullptr))
  {
    Pointer old = mTuple.first();
    mTuple.first() = nullptr;
    if (old != nullptr) {
      mTuple.second()(old);
    }
  }

  template<typename U>
  void reset(U) = delete;

  void swap(UniquePtr& aOther) { mTuple.swap(aOther.mTuple); }

  UniquePtr(const UniquePtr& aOther) = delete; // construct using Move()!
  void operator=(const UniquePtr& aOther) = delete; // assign using Move()!
};

/** A default deletion policy using plain old operator delete. */
template<typename T>
class DefaultDelete
{
public:
  MOZ_CONSTEXPR DefaultDelete() {}

  template<typename U>
  MOZ_IMPLICIT DefaultDelete(const DefaultDelete<U>& aOther,
                             typename EnableIf<mozilla::IsConvertible<U*, T*>::value,
                                               int>::Type aDummy = 0)
  {}

  void operator()(T* aPtr) const
  {
    static_assert(sizeof(T) > 0, "T must be complete");
    delete aPtr;
  }
};

/** A default deletion policy using operator delete[]. */
template<typename T>
class DefaultDelete<T[]>
{
public:
  MOZ_CONSTEXPR DefaultDelete() {}

  void operator()(T* aPtr) const
  {
    static_assert(sizeof(T) > 0, "T must be complete");
    delete[] aPtr;
  }

  template<typename U>
  void operator()(U* aPtr) const = delete;
};

template<typename T, class D>
void
Swap(UniquePtr<T, D>& aX, UniquePtr<T, D>& aY)
{
  aX.swap(aY);
}

template<typename T, class D, typename U, class E>
bool
operator==(const UniquePtr<T, D>& aX, const UniquePtr<U, E>& aY)
{
  return aX.get() == aY.get();
}

template<typename T, class D, typename U, class E>
bool
operator!=(const UniquePtr<T, D>& aX, const UniquePtr<U, E>& aY)
{
  return aX.get() != aY.get();
}

template<typename T, class D>
bool
operator==(const UniquePtr<T, D>& aX, decltype(nullptr))
{
  return !aX;
}

template<typename T, class D>
bool
operator==(decltype(nullptr), const UniquePtr<T, D>& aX)
{
  return !aX;
}

template<typename T, class D>
bool
operator!=(const UniquePtr<T, D>& aX, decltype(nullptr))
{
  return bool(aX);
}

template<typename T, class D>
bool
operator!=(decltype(nullptr), const UniquePtr<T, D>& aX)
{
  return bool(aX);
}

// No operator<, operator>, operator<=, operator>= for now because simplicity.

namespace detail {

template<typename T>
struct UniqueSelector
{
  typedef UniquePtr<T> SingleObject;
};

template<typename T>
struct UniqueSelector<T[]>
{
  typedef UniquePtr<T[]> UnknownBound;
};

template<typename T, decltype(sizeof(int)) N>
struct UniqueSelector<T[N]>
{
  typedef UniquePtr<T[N]> KnownBound;
};

} // namespace detail

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

template<typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject
MakeUnique(Args&&... aArgs)
{
  return UniquePtr<T>(new T(Forward<Args>(aArgs)...));
}

template<typename T>
typename detail::UniqueSelector<T>::UnknownBound
MakeUnique(decltype(sizeof(int)) aN)
{
  typedef typename RemoveExtent<T>::Type ArrayType;
  return UniquePtr<T>(new ArrayType[aN]());
}

template<typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound
MakeUnique(Args&&... aArgs) = delete;

} // namespace mozilla

#endif /* mozilla_UniquePtr_h */

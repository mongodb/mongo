/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A template class for tagged unions. */

#include <new>

#include "mozilla/Alignment.h"
#include "mozilla/Assertions.h"
#include "mozilla/Move.h"

#ifndef mozilla_Variant_h
#define mozilla_Variant_h

namespace mozilla {

template<typename... Ts>
class Variant;

namespace detail {

// MaxSizeOf computes the maximum sizeof(T) for each T in Ts.

template<typename T, typename... Ts>
struct MaxSizeOf
{
  static const size_t size = sizeof(T) > MaxSizeOf<Ts...>::size
    ? sizeof(T)
    : MaxSizeOf<Ts...>::size;
};

template<typename T>
struct MaxSizeOf<T>
{
  static const size_t size = sizeof(T);
};

// The `IsVariant` helper is used in conjunction with static_assert and
// `mozilla::EnableIf` to catch passing non-variant types to `Variant::is<T>()`
// and friends at compile time, rather than at runtime. It ensures that the
// given type `Needle` is one of the types in the set of types `Haystack`.

template<typename Needle, typename... Haystack>
struct IsVariant;

template<typename Needle>
struct IsVariant<Needle>
{
  static const bool value = false;
};

template<typename Needle, typename... Haystack>
struct IsVariant<Needle, Needle, Haystack...>
{
  static const bool value = true;
};

template<typename Needle, typename T, typename... Haystack>
struct IsVariant<Needle, T, Haystack...> : public IsVariant<Needle, Haystack...> { };

// TagHelper gets the given sentinel tag value for the given type T. This has to
// be split out from VariantImplementation because you can't nest a partial template
// specialization within a template class.

template<size_t N, typename T, typename U, typename Next, bool isMatch>
struct TagHelper;

// In the case where T != U, we continue recursion.
template<size_t N, typename T, typename U, typename Next>
struct TagHelper<N, T, U, Next, false>
{
  static size_t tag() { return Next::template tag<U>(); }
};

// In the case where T == U, return the tag number.
template<size_t N, typename T, typename U, typename Next>
struct TagHelper<N, T, U, Next, true>
{
  static size_t tag() { return N; }
};

// The VariantImplementation template provides the guts of mozilla::Variant. We create
// an VariantImplementation for each T in Ts... which handles construction,
// destruction, etc for when the Variant's type is T. If the Variant's type is
// not T, it punts the request on to the next VariantImplementation.

template<size_t N, typename... Ts>
struct VariantImplementation;

// The singly typed Variant / recursion base case.
template<size_t N, typename T>
struct VariantImplementation<N, T> {
  template<typename U>
  static size_t tag() {
    static_assert(mozilla::IsSame<T, U>::value,
                  "mozilla::Variant: tag: bad type!");
    return N;
  }

  template<typename Variant>
  static void copyConstruct(void* aLhs, const Variant& aRhs) {
    new (aLhs) T(aRhs.template as<T>());
  }

  template<typename Variant>
  static void moveConstruct(void* aLhs, Variant&& aRhs) {
    new (aLhs) T(aRhs.template extract<T>());
  }

  template<typename Variant>
  static void destroy(Variant& aV) {
    aV.template as<T>().~T();
  }

  template<typename Variant>
  static bool
  equal(const Variant& aLhs, const Variant& aRhs) {
      return aLhs.template as<T>() == aRhs.template as<T>();
  }

  template<typename Matcher, typename ConcreteVariant>
  static typename Matcher::ReturnType
  match(Matcher& aMatcher, ConcreteVariant& aV) {
    return aMatcher.match(aV.template as<T>());
  }
};

// VariantImplementation for some variant type T.
template<size_t N, typename T, typename... Ts>
struct VariantImplementation<N, T, Ts...>
{
  // The next recursive VariantImplementation.
  using Next = VariantImplementation<N + 1, Ts...>;

  template<typename U>
  static size_t tag() {
    return TagHelper<N, T, U, Next, IsSame<T, U>::value>::tag();
  }

  template<typename Variant>
  static void copyConstruct(void* aLhs, const Variant& aRhs) {
    if (aRhs.template is<T>()) {
      new (aLhs) T(aRhs.template as<T>());
    } else {
      Next::copyConstruct(aLhs, aRhs);
    }
  }

  template<typename Variant>
  static void moveConstruct(void* aLhs, Variant&& aRhs) {
    if (aRhs.template is<T>()) {
      new (aLhs) T(aRhs.template extract<T>());
    } else {
      Next::moveConstruct(aLhs, aRhs);
    }
  }

  template<typename Variant>
  static void destroy(Variant& aV) {
    if (aV.template is<T>()) {
      aV.template as<T>().~T();
    } else {
      Next::destroy(aV);
    }
  }

  template<typename Variant>
  static bool equal(const Variant& aLhs, const Variant& aRhs) {
    if (aLhs.template is<T>()) {
      MOZ_ASSERT(aRhs.template is<T>());
      return aLhs.template as<T>() == aRhs.template as<T>();
    } else {
      return Next::equal(aLhs, aRhs);
    }
  }

  template<typename Matcher, typename ConcreteVariant>
  static typename Matcher::ReturnType
  match(Matcher& aMatcher, ConcreteVariant& aV)
  {
    if (aV.template is<T>()) {
      return aMatcher.match(aV.template as<T>());
    } else {
      // If you're seeing compilation errors here like "no matching
      // function for call to 'match'" then that means that the
      // Matcher doesn't exhaust all variant types. There must exist a
      // Matcher::match(T&) for every variant type T.
      //
      // If you're seeing compilation errors here like "cannot
      // initialize return object of type <...> with an rvalue of type
      // <...>" then that means that the Matcher::match(T&) overloads
      // are returning different types. They must all return the same
      // Matcher::ReturnType type.
      return Next::match(aMatcher, aV);
    }
  }
};

} // namespace detail

/**
 * # mozilla::Variant
 *
 * A variant / tagged union / heterogenous disjoint union / sum-type template
 * class. Similar in concept to (but not derived from) `boost::variant`.
 *
 * Sometimes, you may wish to use a C union with non-POD types. However, this is
 * forbidden in C++ because it is not clear which type in the union should have
 * its constructor and destructor run on creation and deletion
 * respectively. This is the problem that `mozilla::Variant` solves.
 *
 * ## Usage
 *
 * A `mozilla::Variant` instance is constructed (via move or copy) from one of
 * its variant types (ignoring const and references). It does *not* support
 * construction from subclasses of variant types or types that coerce to one of
 * the variant types.
 *
 *     Variant<char, uint32_t> v1('a');
 *     Variant<UniquePtr<A>, B, C> v2(MakeUnique<A>());
 *
 * All access to the contained value goes through type-safe accessors.
 *
 *     void
 *     Foo(Variant<A, B, C> v)
 *     {
 *       if (v.is<A>()) {
 *         A& ref = v.as<A>();
 *         ...
 *       } else {
 *         ...
 *       }
 *     }
 *
 * Attempting to use the contained value as type `T1` when the `Variant`
 * instance contains a value of type `T2` causes an assertion failure.
 *
 *     A a;
 *     Variant<A, B, C> v(a);
 *     v.as<B>(); // <--- Assertion failure!
 *
 * Trying to use a `Variant<Ts...>` instance as some type `U` that is not a
 * member of the set of `Ts...` is a compiler error.
 *
 *     A a;
 *     Variant<A, B, C> v(a);
 *     v.as<SomeRandomType>(); // <--- Compiler error!
 *
 * Additionally, you can turn a `Variant` that `is<T>` into a `T` by moving it
 * out of the containing `Variant` instance with the `extract<T>` method:
 *
 *     Variant<UniquePtr<A>, B, C> v(MakeUnique<A>());
 *     auto ptr = v.extract<UniquePtr<A>>();
 *
 * Finally, you can exhaustively match on the contained variant and branch into
 * different code paths depending which type is contained. This is preferred to
 * manually checking every variant type T with is<T>() because it provides
 * compile-time checking that you handled every type, rather than runtime
 * assertion failures.
 *
 *     // Bad!
 *     char* foo(Variant<A, B, C, D>& v) {
 *       if (v.is<A>()) {
 *         return ...;
 *       } else if (v.is<B>()) {
 *         return ...;
 *       } else {
 *         return doSomething(v.as<C>()); // Forgot about case D!
 *       }
 *     }
 *
 *     // Good!
 *     struct FooMatcher
 *     {
 *       using ReturnType = char*;
 *       ReturnType match(A& a) { ... }
 *       ReturnType match(B& b) { ... }
 *       ReturnType match(C& c) { ... }
 *       ReturnType match(D& d) { ... } // Compile-time error to forget D!
 *     }
 *     char* foo(Variant<A, B, C, D>& v) {
 *       return v.match(FooMatcher());
 *     }
 *
 * ## Examples
 *
 * A tree is either an empty leaf, or a node with a value and two children:
 *
 *     struct Leaf { };
 *
 *     template<typename T>
 *     struct Node
 *     {
 *       T value;
 *       Tree<T>* left;
 *       Tree<T>* right;
 *     };
 *
 *     template<typename T>
 *     using Tree = Variant<Leaf, Node<T>>;
 *
 * A copy-on-write string is either a non-owning reference to some existing
 * string, or an owning reference to our copy:
 *
 *     class CopyOnWriteString
 *     {
 *       Variant<const char*, UniquePtr<char[]>> string;
 *
 *       ...
 *     };
 */
template<typename... Ts>
class Variant
{
  using Impl = detail::VariantImplementation<0, Ts...>;
  using RawData = AlignedStorage<detail::MaxSizeOf<Ts...>::size>;

  // Each type is given a unique size_t sentinel. This tag lets us keep track of
  // the contained variant value's type.
  size_t tag;

  // Raw storage for the contained variant value.
  RawData raw;

  void* ptr() {
    return reinterpret_cast<void*>(&raw);
  }

public:
  /** Perfect forwarding construction for some variant type T. */
  template<typename RefT,
           // RefT captures both const& as well as && (as intended, to support
           // perfect forwarding), so we have to remove those qualifiers here
           // when ensuring that T is a variant of this type, and getting T's
           // tag, etc.
           typename T = typename RemoveReference<typename RemoveConst<RefT>::Type>::Type,
           typename = typename EnableIf<detail::IsVariant<T, Ts...>::value, void>::Type>
  explicit Variant(RefT&& aT)
    : tag(Impl::template tag<T>())
  {
    new (ptr()) T(Forward<T>(aT));
  }

  /** Copy construction. */
  Variant(const Variant& aRhs)
    : tag(aRhs.tag)
  {
    Impl::copyConstruct(ptr(), aRhs);
  }

  /** Move construction. */
  Variant(Variant&& aRhs)
    : tag(aRhs.tag)
  {
    Impl::moveConstruct(ptr(), Move(aRhs));
  }

  /** Copy assignment. */
  Variant& operator=(const Variant& aRhs) {
    MOZ_ASSERT(&aRhs != this, "self-assign disallowed");
    this->~Variant();
    new (this) Variant(aRhs);
    return *this;
  }

  /** Move assignment. */
  Variant& operator=(Variant&& aRhs) {
    MOZ_ASSERT(&aRhs != this, "self-assign disallowed");
    this->~Variant();
    new (this) Variant(Move(aRhs));
    return *this;
  }

  ~Variant()
  {
    Impl::destroy(*this);
  }

  /** Check which variant type is currently contained. */
  template<typename T>
  bool is() const {
    static_assert(detail::IsVariant<T, Ts...>::value,
                  "provided a type not found in this Variant's type list");
    return Impl::template tag<T>() == tag;
  }

  /**
   * Operator == overload that defers to the variant type's operator==
   * implementation if the rhs is tagged as the same type as this one.
   */
  bool operator==(const Variant& aRhs) const {
    return tag == aRhs.tag && Impl::equal(*this, aRhs);
  }

  /**
   * Operator != overload that defers to the negation of the variant type's
   * operator== implementation if the rhs is tagged as the same type as this
   * one.
   */
  bool operator!=(const Variant& aRhs) const {
    return !(*this == aRhs);
  }

  // Accessors for working with the contained variant value.

  /** Mutable reference. */
  template<typename T>
  T& as() {
    static_assert(detail::IsVariant<T, Ts...>::value,
                  "provided a type not found in this Variant's type list");
    MOZ_ASSERT(is<T>());
    return *reinterpret_cast<T*>(&raw);
  }

  /** Immutable const reference. */
  template<typename T>
  const T& as() const {
    static_assert(detail::IsVariant<T, Ts...>::value,
                  "provided a type not found in this Variant's type list");
    MOZ_ASSERT(is<T>());
    return *reinterpret_cast<const T*>(&raw);
  }

  /**
   * Extract the contained variant value from this container into a temporary
   * value.  On completion, the value in the variant will be in a
   * safely-destructible state, as determined by the behavior of T's move
   * constructor when provided the variant's internal value.
   */
  template<typename T>
  T extract() {
    static_assert(detail::IsVariant<T, Ts...>::value,
                  "provided a type not found in this Variant's type list");
    MOZ_ASSERT(is<T>());
    return T(Move(as<T>()));
  }

  // Exhaustive matching of all variant types no the contained value.

  /** Match on an immutable const reference. */
  template<typename Matcher>
  typename Matcher::ReturnType
  match(Matcher& aMatcher) const {
    return Impl::match(aMatcher, *this);
  }

  /**  Match on a mutable non-const reference. */
  template<typename Matcher>
  typename Matcher::ReturnType
  match(Matcher& aMatcher) {
    return Impl::match(aMatcher, *this);
  }
};

} // namespace mozilla

#endif /* mozilla_Variant_h */

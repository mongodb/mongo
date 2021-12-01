/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A template class for tagged unions. */

#include <new>
#include <stdint.h>

#include "mozilla/Assertions.h"
#include "mozilla/Move.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/TypeTraits.h"

#ifndef mozilla_Variant_h
#define mozilla_Variant_h

namespace IPC {
template <typename T> struct ParamTraits;
} // namespace IPC

namespace mozilla {

template<typename... Ts>
class Variant;

namespace detail {

// Nth<N, types...>::Type is the Nth type (0-based) in the list of types Ts.
template<size_t N, typename... Ts>
struct Nth;

template<typename T, typename... Ts>
struct Nth<0, T, Ts...>
{
  using Type = T;
};

template<size_t N, typename T, typename... Ts>
struct Nth<N, T, Ts...>
{
  using Type = typename Nth<N - 1, Ts...>::Type;
};

/// SelectVariantTypeHelper is used in the implementation of SelectVariantType.
template<typename T, typename... Variants>
struct SelectVariantTypeHelper;

template<typename T>
struct SelectVariantTypeHelper<T>
{
  static constexpr size_t count = 0;
};

template<typename T, typename... Variants>
struct SelectVariantTypeHelper<T, T, Variants...>
{
  typedef T Type;
  static constexpr size_t count = 1 + SelectVariantTypeHelper<T, Variants...>::count;
};

template<typename T, typename... Variants>
struct SelectVariantTypeHelper<T, const T, Variants...>
{
  typedef const T Type;
  static constexpr size_t count = 1 + SelectVariantTypeHelper<T, Variants...>::count;
};

template<typename T, typename... Variants>
struct SelectVariantTypeHelper<T, const T&, Variants...>
{
  typedef const T& Type;
  static constexpr size_t count = 1 + SelectVariantTypeHelper<T, Variants...>::count;
};

template<typename T, typename... Variants>
struct SelectVariantTypeHelper<T, T&&, Variants...>
{
  typedef T&& Type;
  static constexpr size_t count = 1 + SelectVariantTypeHelper<T, Variants...>::count;
};

template<typename T, typename Head, typename... Variants>
struct SelectVariantTypeHelper<T, Head, Variants...>
  : public SelectVariantTypeHelper<T, Variants...>
{ };

/**
 * SelectVariantType takes a type T and a list of variant types Variants and
 * yields a type Type, selected from Variants, that can store a value of type T
 * or a reference to type T. If no such type was found, Type is not defined.
 * SelectVariantType also has a `count` member that contains the total number of
 * selectable types (which will be used to check that a requested type is not
 * ambiguously present twice.)
 */
template <typename T, typename... Variants>
struct SelectVariantType
  : public SelectVariantTypeHelper<typename RemoveConst<typename RemoveReference<T>::Type>::Type,
                                   Variants...>
{ };

// Compute a fast, compact type that can be used to hold integral values that
// distinctly map to every type in Ts.
template<typename... Ts>
struct VariantTag
{
private:
  static const size_t TypeCount = sizeof...(Ts);

public:
  using Type =
    typename Conditional<TypeCount < 3,
                         bool,
                         typename Conditional<TypeCount < (1 << 8),
                                              uint_fast8_t,
                                              size_t // stop caring past a certain point :-)
                                              >::Type
                         >::Type;
};

// TagHelper gets the given sentinel tag value for the given type T. This has to
// be split out from VariantImplementation because you can't nest a partial
// template specialization within a template class.

template<typename Tag, size_t N, typename T, typename U, typename Next, bool isMatch>
struct TagHelper;

// In the case where T != U, we continue recursion.
template<typename Tag, size_t N, typename T, typename U, typename Next>
struct TagHelper<Tag, N, T, U, Next, false>
{
  static Tag tag() { return Next::template tag<U>(); }
};

// In the case where T == U, return the tag number.
template<typename Tag, size_t N, typename T, typename U, typename Next>
struct TagHelper<Tag, N, T, U, Next, true>
{
  static Tag tag() { return Tag(N); }
};

// The VariantImplementation template provides the guts of mozilla::Variant.  We
// create a VariantImplementation for each T in Ts... which handles
// construction, destruction, etc for when the Variant's type is T.  If the
// Variant's type isn't T, it punts the request on to the next
// VariantImplementation.

template<typename Tag, size_t N, typename... Ts>
struct VariantImplementation;

// The singly typed Variant / recursion base case.
template<typename Tag, size_t N, typename T>
struct VariantImplementation<Tag, N, T>
{
  template<typename U>
  static Tag tag() {
    static_assert(mozilla::IsSame<T, U>::value,
                  "mozilla::Variant: tag: bad type!");
    return Tag(N);
  }

  template<typename Variant>
  static void copyConstruct(void* aLhs, const Variant& aRhs) {
    ::new (KnownNotNull, aLhs) T(aRhs.template as<N>());
  }

  template<typename Variant>
  static void moveConstruct(void* aLhs, Variant&& aRhs) {
    ::new (KnownNotNull, aLhs) T(aRhs.template extract<N>());
  }

  template<typename Variant>
  static void destroy(Variant& aV) {
    aV.template as<N>().~T();
  }

  template<typename Variant>
  static bool
  equal(const Variant& aLhs, const Variant& aRhs) {
      return aLhs.template as<N>() == aRhs.template as<N>();
  }

  template<typename Matcher, typename ConcreteVariant>
  static auto
  match(Matcher&& aMatcher, ConcreteVariant& aV)
    -> decltype(aMatcher.match(aV.template as<N>()))
  {
    return aMatcher.match(aV.template as<N>());
  }
};

// VariantImplementation for some variant type T.
template<typename Tag, size_t N, typename T, typename... Ts>
struct VariantImplementation<Tag, N, T, Ts...>
{
  // The next recursive VariantImplementation.
  using Next = VariantImplementation<Tag, N + 1, Ts...>;

  template<typename U>
  static Tag tag() {
    return TagHelper<Tag, N, T, U, Next, IsSame<T, U>::value>::tag();
  }

  template<typename Variant>
  static void copyConstruct(void* aLhs, const Variant& aRhs) {
    if (aRhs.template is<N>()) {
      ::new (KnownNotNull, aLhs) T(aRhs.template as<N>());
    } else {
      Next::copyConstruct(aLhs, aRhs);
    }
  }

  template<typename Variant>
  static void moveConstruct(void* aLhs, Variant&& aRhs) {
    if (aRhs.template is<N>()) {
      ::new (KnownNotNull, aLhs) T(aRhs.template extract<N>());
    } else {
      Next::moveConstruct(aLhs, Move(aRhs));
    }
  }

  template<typename Variant>
  static void destroy(Variant& aV) {
    if (aV.template is<N>()) {
      aV.template as<N>().~T();
    } else {
      Next::destroy(aV);
    }
  }

  template<typename Variant>
  static bool equal(const Variant& aLhs, const Variant& aRhs) {
    if (aLhs.template is<N>()) {
      MOZ_ASSERT(aRhs.template is<N>());
      return aLhs.template as<N>() == aRhs.template as<N>();
    } else {
      return Next::equal(aLhs, aRhs);
    }
  }

  template<typename Matcher, typename ConcreteVariant>
  static auto
  match(Matcher&& aMatcher, ConcreteVariant& aV)
    -> decltype(aMatcher.match(aV.template as<N>()))
  {
    if (aV.template is<N>()) {
      return aMatcher.match(aV.template as<N>());
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

/**
 * AsVariantTemporary stores a value of type T to allow construction of a
 * Variant value via type inference. Because T is copied and there's no
 * guarantee that the copy can be elided, AsVariantTemporary is best used with
 * primitive or very small types.
 */
template <typename T>
struct AsVariantTemporary
{
  explicit AsVariantTemporary(const T& aValue)
    : mValue(aValue)
  {}

  template<typename U>
  explicit AsVariantTemporary(U&& aValue)
    : mValue(Forward<U>(aValue))
  {}

  AsVariantTemporary(const AsVariantTemporary& aOther)
    : mValue(aOther.mValue)
  {}

  AsVariantTemporary(AsVariantTemporary&& aOther)
    : mValue(Move(aOther.mValue))
  {}

  AsVariantTemporary() = delete;
  void operator=(const AsVariantTemporary&) = delete;
  void operator=(AsVariantTemporary&&) = delete;

  typename RemoveConst<typename RemoveReference<T>::Type>::Type mValue;
};

} // namespace detail

// Used to unambiguously specify one of the Variant's type.
template<typename T> struct VariantType { using Type = T; };

// Used to specify one of the Variant's type by index.
template<size_t N> struct VariantIndex { static constexpr size_t index = N; };

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
 *     Variant<bool, char> v3(VariantType<char>, 0); // disambiguation needed
 *     Variant<int, int> v4(VariantIndex<1>, 0); // 2nd int
 *
 * Because specifying the full type of a Variant value is often verbose,
 * there are two easier ways to construct values:
 *
 * A. AsVariant() can be used to construct a Variant value using type inference
 * in contexts such as expressions or when returning values from functions.
 * Because AsVariant() must copy or move the value into a temporary and this
 * cannot necessarily be elided by the compiler, it's mostly appropriate only
 * for use with primitive or very small types.
 *
 *     Variant<char, uint32_t> Foo() { return AsVariant('x'); }
 *     // ...
 *     Variant<char, uint32_t> v1 = Foo();  // v1 holds char('x').
 *
 * B. Brace-construction with VariantType or VariantIndex; this also allows
 * in-place construction with any number of arguments.
 *
 *     struct AB { AB(int, int){...} };
 *     static Variant<AB, bool> foo()
 *     {
 *       return {VariantIndex<0>{}, 1, 2};
 *     }
 *     // ...
 *     Variant<AB, bool> v0 = Foo();  // v0 holds AB(1,2).
 *
 * All access to the contained value goes through type-safe accessors.
 * Either the stored type, or the type index may be provided.
 *
 *     void
 *     Foo(Variant<A, B, C> v)
 *     {
 *       if (v.is<A>()) {
 *         A& ref = v.as<A>();
 *         ...
 *       } else (v.is<1>()) { // Instead of v.is<B>.
 *         ...
 *       } else {
 *         ...
 *       }
 *     }
 *
 * In some situation, a Variant may be constructed from templated types, in
 * which case it is possible that the same type could be given multiple times by
 * an external developer. Or seemingly-different types could be aliases.
 * In this case, repeated types can only be accessed through their index, to
 * prevent ambiguous access by type.
 *
 *    // Bad!
 *    template <typename T>
 *    struct ResultOrError
 *    {
 *      Variant<T, int> m;
 *      ResultOrError() : m(int(0)) {} // Error '0' by default
 *      ResultOrError(const T& r) : m(r) {}
 *      bool IsResult() const { return m.is<T>(); }
 *      bool IsError() const { return m.is<int>(); }
 *    };
 *    // Now instantiante with the result being an int too:
 *    ResultOrError<int> myResult(123); // Fail!
 *    // In Variant<int, int>, which 'int' are we refering to, from inside
 *    // ResultOrError functions?
 *
 *    // Good!
 *    template <typename T>
 *    struct ResultOrError
 *    {
 *      Variant<T, int> m;
 *      ResultOrError() : m(VariantIndex<1>{}, 0) {} // Error '0' by default
 *      ResultOrError(const T& r) : m(VariantIndex<0>{}, r) {}
 *      bool IsResult() const { return m.is<0>(); } // 0 -> T
 *      bool IsError() const { return m.is<1>(); } // 1 -> int
 *    };
 *    // Now instantiante with the result being an int too:
 *    ResultOrError<int> myResult(123); // It now works!
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
 * different code paths depending on which type is contained. This is preferred
 * to manually checking every variant type T with is<T>() because it provides
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
 *       // The return type of all matchers must be identical.
 *       char* match(A& a) { ... }
 *       char* match(B& b) { ... }
 *       char* match(C& c) { ... }
 *       char* match(D& d) { ... } // Compile-time error to forget D!
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
 *
 * Because Variant must be aligned suitable to hold any value stored within it,
 * and because |alignas| requirements don't affect platform ABI with respect to
 * how parameters are laid out in memory, Variant can't be used as the type of a
 * function parameter.  Pass Variant to functions by pointer or reference
 * instead.
 */
template<typename... Ts>
class MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS MOZ_NON_PARAM Variant
{
  friend struct IPC::ParamTraits<mozilla::Variant<Ts...>>;

  using Tag = typename detail::VariantTag<Ts...>::Type;
  using Impl = detail::VariantImplementation<Tag, 0, Ts...>;

  static constexpr size_t RawDataAlignment = tl::Max<alignof(Ts)...>::value;
  static constexpr size_t RawDataSize = tl::Max<sizeof(Ts)...>::value;

  // Raw storage for the contained variant value.
  alignas(RawDataAlignment) unsigned char rawData[RawDataSize];

  // Each type is given a unique tag value that lets us keep track of the
  // contained variant value's type.
  Tag tag;

  // Some versions of GCC treat it as a -Wstrict-aliasing violation (ergo a
  // -Werror compile error) to reinterpret_cast<> |rawData| to |T*|, even
  // through |void*|.  Placing the latter cast in these separate functions
  // breaks the chain such that affected GCC versions no longer warn/error.
  void* ptr() {
    return rawData;
  }

  const void* ptr() const {
    return rawData;
  }

public:
  /** Perfect forwarding construction for some variant type T. */
  template<typename RefT,
           // RefT captures both const& as well as && (as intended, to support
           // perfect forwarding), so we have to remove those qualifiers here
           // when ensuring that T is a variant of this type, and getting T's
           // tag, etc.
           typename T = typename detail::SelectVariantType<RefT, Ts...>::Type>
  explicit Variant(RefT&& aT)
    : tag(Impl::template tag<T>())
  {
    static_assert(detail::SelectVariantType<RefT, Ts...>::count == 1,
                  "Variant can only be selected by type if that type is unique");
    ::new (KnownNotNull, ptr()) T(Forward<RefT>(aT));
  }

  /**
   * Perfect forwarding construction for some variant type T, by
   * explicitly giving the type.
   * This is necessary to construct from any number of arguments,
   * or to convert from a type that is not in the Variant's type list.
   */
  template<typename T, typename... Args>
  MOZ_IMPLICIT Variant(const VariantType<T>&, Args&&... aTs)
    : tag(Impl::template tag<T>())
  {
    ::new (KnownNotNull, ptr()) T(Forward<Args>(aTs)...);
  }

  /**
   * Perfect forwarding construction for some variant type T, by
   * explicitly giving the type index.
   * This is necessary to construct from any number of arguments,
   * or to convert from a type that is not in the Variant's type list,
   * or to construct a type that is present more than once in the Variant.
   */
  template<size_t N, typename... Args>
  MOZ_IMPLICIT Variant(const VariantIndex<N>&, Args&&... aTs)
    : tag(N)
  {
    using T = typename detail::Nth<N, Ts...>::Type;
    ::new (KnownNotNull, ptr()) T(Forward<Args>(aTs)...);
  }

  /**
   * Constructs this Variant from an AsVariantTemporary<T> such that T can be
   * stored in one of the types allowable in this Variant. This is used in the
   * implementation of AsVariant().
   */
  template<typename RefT>
  MOZ_IMPLICIT Variant(detail::AsVariantTemporary<RefT>&& aValue)
    : tag(Impl::template tag<typename detail::SelectVariantType<RefT, Ts...>::Type>())
  {
    using T = typename detail::SelectVariantType<RefT, Ts...>::Type;
    static_assert(detail::SelectVariantType<RefT, Ts...>::count == 1,
                  "Variant can only be selected by type if that type is unique");
    ::new (KnownNotNull, ptr()) T(Move(aValue.mValue));
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
    ::new (KnownNotNull, this) Variant(aRhs);
    return *this;
  }

  /** Move assignment. */
  Variant& operator=(Variant&& aRhs) {
    MOZ_ASSERT(&aRhs != this, "self-assign disallowed");
    this->~Variant();
    ::new (KnownNotNull, this) Variant(Move(aRhs));
    return *this;
  }

  /** Move assignment from AsVariant(). */
  template<typename T>
  Variant& operator=(detail::AsVariantTemporary<T>&& aValue)
  {
    static_assert(detail::SelectVariantType<T, Ts...>::count == 1,
                  "Variant can only be selected by type if that type is unique");
    this->~Variant();
    ::new (KnownNotNull, this) Variant(Move(aValue));
    return *this;
  }

  ~Variant()
  {
    Impl::destroy(*this);
  }

  /** Check which variant type is currently contained. */
  template<typename T>
  bool is() const {
    static_assert(detail::SelectVariantType<T, Ts...>::count == 1,
                  "provided a type not uniquely found in this Variant's type list");
    return Impl::template tag<T>() == tag;
  }

  template<size_t N>
  bool is() const
  {
    static_assert(N < sizeof...(Ts),
                  "provided an index outside of this Variant's type list");
    return N == size_t(tag);
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
    static_assert(detail::SelectVariantType<T, Ts...>::count == 1,
                  "provided a type not uniquely found in this Variant's type list");
    MOZ_RELEASE_ASSERT(is<T>());
    return *static_cast<T*>(ptr());
  }

  template<size_t N>
  typename detail::Nth<N, Ts...>::Type& as()
  {
    static_assert(N < sizeof...(Ts),
                  "provided an index outside of this Variant's type list");
    MOZ_RELEASE_ASSERT(is<N>());
    return *static_cast<typename detail::Nth<N, Ts...>::Type*>(ptr());
  }

  /** Immutable const reference. */
  template<typename T>
  const T& as() const {
    static_assert(detail::SelectVariantType<T, Ts...>::count == 1,
                  "provided a type not found in this Variant's type list");
    MOZ_RELEASE_ASSERT(is<T>());
    return *static_cast<const T*>(ptr());
  }

  template<size_t N>
  const typename detail::Nth<N, Ts...>::Type& as() const
  {
    static_assert(N < sizeof...(Ts),
                  "provided an index outside of this Variant's type list");
    MOZ_RELEASE_ASSERT(is<N>());
    return *static_cast<const typename detail::Nth<N, Ts...>::Type*>(ptr());
  }

  /**
   * Extract the contained variant value from this container into a temporary
   * value.  On completion, the value in the variant will be in a
   * safely-destructible state, as determined by the behavior of T's move
   * constructor when provided the variant's internal value.
   */
  template<typename T>
  T extract() {
    static_assert(detail::SelectVariantType<T, Ts...>::count == 1,
                  "provided a type not uniquely found in this Variant's type list");
    MOZ_ASSERT(is<T>());
    return T(Move(as<T>()));
  }

  template<size_t N>
  typename detail::Nth<N, Ts...>::Type extract()
  {
    static_assert(N < sizeof...(Ts),
                  "provided an index outside of this Variant's type list");
    MOZ_RELEASE_ASSERT(is<N>());
    return typename detail::Nth<N, Ts...>::Type(Move(as<N>()));
  }

  // Exhaustive matching of all variant types on the contained value.

  /** Match on an immutable const reference. */
  template<typename Matcher>
  auto
  match(Matcher&& aMatcher) const
    -> decltype(Impl::match(aMatcher, *this))
  {
    return Impl::match(aMatcher, *this);
  }

  /** Match on a mutable non-const reference. */
  template<typename Matcher>
  auto
  match(Matcher&& aMatcher)
    -> decltype(Impl::match(aMatcher, *this))
  {
    return Impl::match(aMatcher, *this);
  }
};

/*
 * AsVariant() is used to construct a Variant<T,...> value containing the
 * provided T value using type inference. It can be used to construct Variant
 * values in expressions or return them from functions without specifying the
 * entire Variant type.
 *
 * Because AsVariant() must copy or move the value into a temporary and this
 * cannot necessarily be elided by the compiler, it's mostly appropriate only
 * for use with primitive or very small types.
 *
 * AsVariant() returns a AsVariantTemporary value which is implicitly
 * convertible to any Variant that can hold a value of type T.
 */
template<typename T>
detail::AsVariantTemporary<T>
AsVariant(T&& aValue)
{
  return detail::AsVariantTemporary<T>(Forward<T>(aValue));
}

} // namespace mozilla

#endif /* mozilla_Variant_h */

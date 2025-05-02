// (C) Copyright Jeremy Siek 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ITERATOR_ARCHETYPES_HPP
#define BOOST_ITERATOR_ARCHETYPES_HPP

#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/detail/facade_iterator_category.hpp>

#include <boost/operators.hpp>
#include <boost/concept_archetype.hpp>
#include <boost/mp11/utility.hpp>

#include <cstddef>
#include <type_traits>

namespace boost {
namespace iterators {

template <class Value, class AccessCategory>
struct access_archetype;

template <class Derived, class Value, class AccessCategory, class TraversalCategory>
struct traversal_archetype;

namespace archetypes {

enum
{
    readable_iterator_bit = 1,
    writable_iterator_bit = 2,
    swappable_iterator_bit = 4,
    lvalue_iterator_bit = 8
};

// Not quite tags, since dispatching wouldn't work.
using readable_iterator_t = std::integral_constant<unsigned int, readable_iterator_bit>;
using writable_iterator_t = std::integral_constant<unsigned int, writable_iterator_bit>;

using readable_writable_iterator_t = std::integral_constant<
    unsigned int,
    (readable_iterator_bit | writable_iterator_bit)
>;

using readable_lvalue_iterator_t = std::integral_constant<
    unsigned int,
    (readable_iterator_bit | lvalue_iterator_bit)
>;

using writable_lvalue_iterator_t = std::integral_constant<
    unsigned int,
    (lvalue_iterator_bit | writable_iterator_bit)
>;

using swappable_iterator_t = std::integral_constant<unsigned int, swappable_iterator_bit>;
using lvalue_iterator_t = std::integral_constant<unsigned int, lvalue_iterator_bit>;

template <class Derived, class Base>
struct has_access :
    public std::integral_constant<bool, (Derived::value & Base::value) == Base::value>
{};

} // namespace archetypes

namespace detail {

template <class T>
struct assign_proxy
{
    assign_proxy& operator=(T) { return *this; }
};

template <class T>
struct read_proxy
{
    operator T() { return static_object<T>::get(); }
};

template <class T>
struct read_write_proxy :
    public read_proxy<T> // Used to inherit from assign_proxy, but that doesn't work. -JGS
{
    read_write_proxy& operator=(T) { return *this; }
};

template <class T>
struct arrow_proxy
{
    T const* operator->() const { return 0; }
};

struct no_operator_brackets {};

template <class ValueType>
struct readable_operator_brackets
{
    read_proxy<ValueType> operator[](std::ptrdiff_t n) const { return read_proxy<ValueType>(); }
};

template <class ValueType>
struct writable_operator_brackets
{
    read_write_proxy<ValueType> operator[](std::ptrdiff_t n) const { return read_write_proxy<ValueType>(); }
};

template <class Value, class AccessCategory, class TraversalCategory>
struct operator_brackets :
    public mp11::mp_eval_if_c<
        !std::is_convertible<TraversalCategory, random_access_traversal_tag>::value,
        no_operator_brackets,
        mp11::mp_cond,
            archetypes::has_access<AccessCategory, archetypes::writable_iterator_t>, writable_operator_brackets<Value>,
            archetypes::has_access<AccessCategory, archetypes::readable_iterator_t>, readable_operator_brackets<Value>,
            std::true_type, no_operator_brackets
    >
{};

template <class TraversalCategory>
struct traversal_archetype_impl
{
    template <class Derived,class Value> struct archetype;
};

// Constructor argument for those iterators that
// are not default constructible
struct ctor_arg {};

template <class Derived, class Value, class TraversalCategory>
struct traversal_archetype_ :
    public traversal_archetype_impl<TraversalCategory>::template archetype<Derived,Value>
{
    using base = typename traversal_archetype_impl<TraversalCategory>::template archetype<Derived,Value>;

    traversal_archetype_() {}

    traversal_archetype_(ctor_arg arg) : base(arg)
    {}
};

template <>
struct traversal_archetype_impl<incrementable_traversal_tag>
{
    template<class Derived, class Value>
    struct archetype
    {
        explicit archetype(ctor_arg) {}

        struct bogus { }; // This used to be void, but that causes trouble for iterator_facade. Need more research. -JGS
        using difference_type = bogus;

        Derived& operator++() { return (Derived&)static_object<Derived>::get(); }
        Derived  operator++(int) const { return (Derived&)static_object<Derived>::get(); }
    };
};

template <>
struct traversal_archetype_impl<single_pass_traversal_tag>
{
    template<class Derived, class Value>
    struct archetype :
        public equality_comparable< traversal_archetype_<Derived, Value, single_pass_traversal_tag> >,
        public traversal_archetype_<Derived, Value, incrementable_traversal_tag>
    {
        explicit archetype(ctor_arg arg) :
            traversal_archetype_<Derived, Value, incrementable_traversal_tag>(arg)
        {}

        using difference_type = std::ptrdiff_t;
    };
};

template <class Derived, class Value>
bool operator==(
    traversal_archetype_<Derived, Value, single_pass_traversal_tag> const&,
    traversal_archetype_<Derived, Value, single_pass_traversal_tag> const&) { return true; }

template <>
struct traversal_archetype_impl<forward_traversal_tag>
{
    template<class Derived, class Value>
    struct archetype :
        public traversal_archetype_<Derived, Value, single_pass_traversal_tag>
    {
        archetype() :
            traversal_archetype_<Derived, Value, single_pass_traversal_tag>(ctor_arg())
        {}
    };
};

template <>
struct traversal_archetype_impl<bidirectional_traversal_tag>
{
    template<class Derived, class Value>
    struct archetype :
        public traversal_archetype_<Derived, Value, forward_traversal_tag>
    {
        Derived& operator--() { return static_object<Derived>::get(); }
        Derived  operator--(int) const { return static_object<Derived>::get(); }
    };
};

template <>
struct traversal_archetype_impl<random_access_traversal_tag>
{
    template<class Derived, class Value>
    struct archetype :
        public traversal_archetype_<Derived, Value, bidirectional_traversal_tag>
    {
        Derived& operator+=(std::ptrdiff_t) { return static_object<Derived>::get(); }
        Derived& operator-=(std::ptrdiff_t) { return static_object<Derived>::get(); }
    };
};

template <class Derived, class Value>
Derived& operator+(
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&,
    std::ptrdiff_t) { return static_object<Derived>::get(); }

template <class Derived, class Value>
Derived& operator+(
    std::ptrdiff_t,
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&)
    { return static_object<Derived>::get(); }

template <class Derived, class Value>
Derived& operator-(
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&,
    std::ptrdiff_t) { return static_object<Derived>::get(); }

template <class Derived, class Value>
std::ptrdiff_t operator-(
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&,
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&)
    { return 0; }

template <class Derived, class Value>
bool operator<(
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&,
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&)
    { return true; }

template <class Derived, class Value>
bool operator>(
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&,
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&)
    { return true; }

template <class Derived, class Value>
bool operator<=(
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&,
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&)
    { return true; }

template <class Derived, class Value>
bool operator>=(
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&,
    traversal_archetype_<Derived, Value, random_access_traversal_tag> const&)
    { return true; }

struct bogus_type;

template <class Value>
struct convertible_type
{
    using type = bogus_type;
};

template <class Value>
struct convertible_type<const Value>
{
    using type = Value;
};

} // namespace detail


template <class> struct undefined;

template <class AccessCategory>
struct iterator_access_archetype_impl
{
    template <class Value> struct archetype;
};

template <class Value, class AccessCategory>
struct iterator_access_archetype :
    public iterator_access_archetype_impl<AccessCategory>::template archetype<Value>
{
};

template <>
struct iterator_access_archetype_impl<archetypes::readable_iterator_t>
{
    template <class Value>
    struct archetype
    {
        using value_type = typename std::remove_cv<Value>::type;
        using reference = Value;
        using pointer = Value*;

        value_type operator*() const { return static_object<value_type>::get(); }

        detail::arrow_proxy<Value> operator->() const { return detail::arrow_proxy<Value>(); }
    };
};

template <>
struct iterator_access_archetype_impl<archetypes::writable_iterator_t>
{
    template <class Value>
    struct archetype
    {
        static_assert(!std::is_const<Value>::value, "Value type must not be const.");
        using value_type = void;
        using reference = void;
        using pointer = void;

        detail::assign_proxy<Value> operator*() const { return detail::assign_proxy<Value>(); }
    };
};

template <>
struct iterator_access_archetype_impl<archetypes::readable_writable_iterator_t>
{
    template <class Value>
    struct archetype :
        public virtual iterator_access_archetype<Value, archetypes::readable_iterator_t>
    {
        using reference = detail::read_write_proxy<Value>;

        detail::read_write_proxy<Value> operator*() const { return detail::read_write_proxy<Value>(); }
    };
};

template <>
struct iterator_access_archetype_impl<archetypes::readable_lvalue_iterator_t>
{
    template <class Value>
    struct archetype :
        public virtual iterator_access_archetype<Value, archetypes::readable_iterator_t>
    {
        using reference = Value&;

        Value& operator*() const { return static_object<Value>::get(); }
        Value* operator->() const { return 0; }
    };
};

template <>
struct iterator_access_archetype_impl<archetypes::writable_lvalue_iterator_t>
{
    template <class Value>
    struct archetype :
        public virtual iterator_access_archetype<Value, archetypes::readable_lvalue_iterator_t>
    {
        static_assert(!std::is_const<Value>::value, "Value type must not be const.");
    };
};


template <class Value, class AccessCategory, class TraversalCategory>
struct iterator_archetype;

template <class Value, class AccessCategory, class TraversalCategory>
struct traversal_archetype_base :
    public detail::operator_brackets<
        typename std::remove_cv<Value>::type,
        AccessCategory,
        TraversalCategory
    >,
    public detail::traversal_archetype_<
        iterator_archetype<Value, AccessCategory, TraversalCategory>,
        Value,
        TraversalCategory
    >
{
};

namespace detail {

template <class Value, class AccessCategory, class TraversalCategory>
struct iterator_archetype_base :
    public iterator_access_archetype<Value, AccessCategory>,
    public traversal_archetype_base<Value, AccessCategory, TraversalCategory>
{
    using access = iterator_access_archetype<Value, AccessCategory>;

    using iterator_category = typename detail::facade_iterator_category<
        TraversalCategory,
        typename std::conditional<
            archetypes::has_access<
                AccessCategory, archetypes::writable_iterator_t
            >::value,
            std::remove_const<Value>,
            std::add_const<Value>
        >::type::type,
        typename access::reference
    >::type;

    // Needed for some broken libraries (see below)
    struct workaround_iterator_base
    {
        using iterator_category = typename iterator_archetype_base::iterator_category;
        using value_type = Value;
        using difference_type = typename traversal_archetype_base<
            Value, AccessCategory, TraversalCategory
        >::difference_type;
        using pointer = typename access::pointer;
        using reference = typename access::reference;
    };
};

} // namespace detail

template <class Value, class AccessCategory, class TraversalCategory>
struct iterator_archetype :
    public detail::iterator_archetype_base<Value, AccessCategory, TraversalCategory>

    // These broken libraries require derivation from std::iterator
    // (or related magic) in order to handle iter_swap and other
    // iterator operations
# if BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, < 310)           \
    || BOOST_WORKAROUND(_RWSTD_VER, BOOST_TESTED_AT(0x20101))
  , public detail::iterator_archetype_base<
        Value, AccessCategory, TraversalCategory
    >::workaround_iterator_base
# endif
{
    // Derivation from std::iterator above caused references to nested
    // types to be ambiguous, so now we have to redeclare them all
    // here.
# if BOOST_WORKAROUND(BOOST_DINKUMWARE_STDLIB, < 310)           \
    || BOOST_WORKAROUND(_RWSTD_VER, BOOST_TESTED_AT(0x20101))

    using base = detail::iterator_archetype_base<
        Value, AccessCategory, TraversalCategory
    >;

    using value_type = typename base::value_type;
    using reference = typename base::reference;
    using pointer = typename base::pointer;
    using difference_type = typename base::difference_type;
    using iterator_category = typename base::iterator_category;
# endif

    iterator_archetype() { }
    iterator_archetype(iterator_archetype const& x) :
        detail::iterator_archetype_base<Value, AccessCategory, TraversalCategory>(x)
    {}

    iterator_archetype& operator=(iterator_archetype const&) { return *this; }

# if 0
    // Optional conversion from mutable
    iterator_archetype(
        iterator_archetype<
        typename detail::convertible_type<Value>::type
      , AccessCategory
      , TraversalCategory> const&
    );
# endif
};

} // namespace iterators

// Backward compatibility names
namespace iterator_archetypes = iterators::archetypes;
using iterators::access_archetype;
using iterators::traversal_archetype;
using iterators::iterator_archetype;
using iterators::undefined;
using iterators::iterator_access_archetype_impl;
using iterators::traversal_archetype_base;

} // namespace boost

#endif // BOOST_ITERATOR_ARCHETYPES_HPP

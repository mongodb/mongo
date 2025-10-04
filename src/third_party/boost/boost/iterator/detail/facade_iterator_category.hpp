// Copyright David Abrahams 2003. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef FACADE_ITERATOR_CATEGORY_DWA20031118_HPP
#define FACADE_ITERATOR_CATEGORY_DWA20031118_HPP

#include <iterator>
#include <type_traits>

#include <boost/mp11/utility.hpp>

#include <boost/iterator/iterator_categories.hpp>
#include <boost/iterator/detail/type_traits/conjunction.hpp>
#include <boost/iterator/detail/type_traits/disjunction.hpp>
#include <boost/iterator/detail/config_def.hpp> // try to keep this last

//
// iterator_category deduction for iterator_facade
//

namespace boost {
namespace iterators {
namespace detail {

#ifdef BOOST_ITERATOR_REF_CONSTNESS_KILLS_WRITABILITY

template< typename T >
struct is_const_lvalue_reference :
    public std::false_type
{};

template< typename T >
struct is_const_lvalue_reference< T const& > :
    public std::true_type
{};

#endif // BOOST_ITERATOR_REF_CONSTNESS_KILLS_WRITABILITY

//
// True iff the user has explicitly disabled writability of this
// iterator.  Pass the iterator_facade's Value parameter and its
// nested ::reference type.
//
template< typename ValueParam, typename Reference >
struct iterator_writability_disabled :
# ifdef BOOST_ITERATOR_REF_CONSTNESS_KILLS_WRITABILITY // Adding Thomas' logic?
    public detail::disjunction<
        detail::is_const_lvalue_reference< Reference >,
        std::is_const< Reference >,
        std::is_const< ValueParam >
    >
# else
    public std::is_const< ValueParam >
# endif
{};


template< typename Traversal, typename ValueParam, typename Reference >
using is_traversal_of_input_iterator = detail::conjunction<
    std::is_convertible< Traversal, single_pass_traversal_tag >,

    // check for readability
    std::is_convertible< Reference, ValueParam >
>;

//
// Convert an iterator_facade's traversal category, Value parameter,
// and ::reference type to an appropriate old-style category.
//
// Due to changeset 21683, this now never results in a category convertible
// to output_iterator_tag.
//
// Change at: https://svn.boost.org/trac/boost/changeset/21683
template< typename Traversal, typename ValueParam, typename Reference >
struct iterator_facade_default_category
{
    using type = typename std::conditional<
        detail::is_traversal_of_input_iterator< Traversal, ValueParam, Reference >::value,
        std::input_iterator_tag,
        Traversal
    >::type;
};

// Specialization for the (typical) case when the reference type is an actual reference
template< typename Traversal, typename ValueParam, typename Referenced >
struct iterator_facade_default_category< Traversal, ValueParam, Referenced& >
{
    using type = mp11::mp_cond<
        std::is_convertible< Traversal, random_access_traversal_tag >, std::random_access_iterator_tag,
        std::is_convertible< Traversal, bidirectional_traversal_tag >, std::bidirectional_iterator_tag,
        std::is_convertible< Traversal, forward_traversal_tag >, std::forward_iterator_tag,
        detail::is_traversal_of_input_iterator< Traversal, ValueParam, Referenced& >, std::input_iterator_tag,
        std::true_type, Traversal
    >;
};

template< typename Traversal, typename ValueParam, typename Reference >
using iterator_facade_default_category_t = typename iterator_facade_default_category< Traversal, ValueParam, Reference >::type;

// True iff T is convertible to an old-style iterator category.
template< typename T >
struct is_iterator_category :
    public detail::disjunction<
        std::is_convertible< T, std::input_iterator_tag >,
        std::is_convertible< T, std::output_iterator_tag >
    >
{};

template< typename T >
struct is_iterator_traversal :
    public std::is_convertible< T, incrementable_traversal_tag >
{};


//
// A composite iterator_category tag convertible to Category (a pure
// old-style category) and Traversal (a pure traversal tag).
// Traversal must be a strict increase of the traversal power given by
// Category.
//
template< typename Category, typename Traversal >
struct iterator_category_with_traversal :
    public Category,
    public Traversal
{
    // Make sure this isn't used to build any categories where
    // convertibility to Traversal is redundant.  Should just use the
    // Category element in that case.
    static_assert(
        !std::is_convertible< iterator_category_to_traversal_t< Category >, Traversal >::value,
        "Category transformed to corresponding traversal must be convertible to Traversal."
    );

    static_assert(is_iterator_category< Category >::value, "Category must be an STL iterator category.");
    static_assert(!is_iterator_category< Traversal >::value, "Traversal must not be an STL iterator category.");
    static_assert(!is_iterator_traversal< Category >::value, "Category must not be a traversal tag.");
    static_assert(is_iterator_traversal< Traversal >::value, "Traversal must be a traversal tag.");
};

// Computes an iterator_category tag whose traversal is Traversal and
// which is appropriate for an iterator
template< typename Traversal, typename ValueParam, typename Reference >
struct facade_iterator_category_impl
{
    static_assert(!is_iterator_category< Traversal >::value, "Traversal must not be an STL iterator category.");

    using category = iterator_facade_default_category_t< Traversal, ValueParam, Reference >;

    using type = typename std::conditional<
        std::is_same<
            Traversal,
            typename iterator_category_to_traversal< category >::type
        >::value,
        category,
        iterator_category_with_traversal< category, Traversal >
    >::type;
};

template< typename Traversal, typename ValueParam, typename Reference >
using facade_iterator_category_impl_t = typename facade_iterator_category_impl< Traversal, ValueParam, Reference >::type;

//
// Compute an iterator_category for iterator_facade
//
template< typename CategoryOrTraversal, typename ValueParam, typename Reference >
struct facade_iterator_category
{
    using type = mp11::mp_eval_if<
        is_iterator_category< CategoryOrTraversal >,
        CategoryOrTraversal, // old-style categories are fine as-is
        facade_iterator_category_impl_t, CategoryOrTraversal, ValueParam, Reference
    >;
};

}}} // namespace boost::iterators::detail

#include <boost/iterator/detail/config_undef.hpp>

#endif // FACADE_ITERATOR_CATEGORY_DWA20031118_HPP

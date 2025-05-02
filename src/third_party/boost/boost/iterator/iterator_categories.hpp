// (C) Copyright Jeremy Siek 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_ITERATOR_CATEGORIES_HPP
#define BOOST_ITERATOR_CATEGORIES_HPP

#include <iterator>
#include <type_traits>

#include <boost/mpl/arg_fwd.hpp>
#include <boost/mp11/utility.hpp>

namespace boost {
namespace iterators {

//
// Traversal Categories
//
struct no_traversal_tag {};
struct incrementable_traversal_tag : public no_traversal_tag {};
struct single_pass_traversal_tag : public incrementable_traversal_tag {};
struct forward_traversal_tag : public single_pass_traversal_tag {};
struct bidirectional_traversal_tag : public forward_traversal_tag {};
struct random_access_traversal_tag : public bidirectional_traversal_tag {};

//
// Convert an iterator category into a traversal tag
//
template< typename Cat >
using iterator_category_to_traversal_t = mp11::mp_cond<
    // if already convertible to a traversal tag, we're done.
    std::is_convertible< Cat, incrementable_traversal_tag >, Cat,
    std::is_convertible< Cat, std::random_access_iterator_tag >, random_access_traversal_tag,
    std::is_convertible< Cat, std::bidirectional_iterator_tag >, bidirectional_traversal_tag,
    std::is_convertible< Cat, std::forward_iterator_tag >, forward_traversal_tag,
    std::is_convertible< Cat, std::input_iterator_tag >, single_pass_traversal_tag,
    std::is_convertible< Cat, std::output_iterator_tag >, incrementable_traversal_tag,
    std::true_type, void
>;

template< typename Cat >
struct iterator_category_to_traversal
{
    using type = iterator_category_to_traversal_t< Cat >;
};

// Trait to get an iterator's traversal category
template< typename Iterator >
using iterator_traversal_t = iterator_category_to_traversal_t<
    typename std::iterator_traits< Iterator >::iterator_category
>;

template< typename Iterator = mpl::arg< 1 > >
struct iterator_traversal
{
    using type = iterator_traversal_t< Iterator >;
};

//
// Convert an iterator traversal to one of the traversal tags.
//
template< typename Traversal >
using pure_traversal_tag_t = mp11::mp_cond<
    std::is_convertible< Traversal, random_access_traversal_tag >, random_access_traversal_tag,
    std::is_convertible< Traversal, bidirectional_traversal_tag >, bidirectional_traversal_tag,
    std::is_convertible< Traversal, forward_traversal_tag >, forward_traversal_tag,
    std::is_convertible< Traversal, single_pass_traversal_tag >, single_pass_traversal_tag,
    std::is_convertible< Traversal, incrementable_traversal_tag >, incrementable_traversal_tag,
    std::true_type, void
>;

template< typename Traversal >
struct pure_traversal_tag
{
    using type = pure_traversal_tag_t< Traversal >;
};

//
// Trait to retrieve one of the iterator traversal tags from the iterator category or traversal.
//
template< typename Iterator >
using pure_iterator_traversal_t = pure_traversal_tag_t<
    iterator_traversal_t< Iterator >
>;

template< typename Iterator = mpl::arg< 1 > >
struct pure_iterator_traversal
{
    using type = pure_iterator_traversal_t< Iterator >;
};

} // namespace iterators

using iterators::no_traversal_tag;
using iterators::incrementable_traversal_tag;
using iterators::single_pass_traversal_tag;
using iterators::forward_traversal_tag;
using iterators::bidirectional_traversal_tag;
using iterators::random_access_traversal_tag;
using iterators::iterator_category_to_traversal;
using iterators::iterator_traversal;

} // namespace boost

#endif // BOOST_ITERATOR_CATEGORIES_HPP

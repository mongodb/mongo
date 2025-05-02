#ifndef BOOST_POINTEE_DWA200415_HPP
#define BOOST_POINTEE_DWA200415_HPP

//
// Copyright David Abrahams 2004. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// typename pointee<P>::type provides the pointee type of P.
//
// For example, it is T for T* and X for shared_ptr<X>.
//
// http://www.boost.org/libs/iterator/doc/pointee.html
//

#include <iterator>
#include <type_traits>

#include <boost/detail/is_incrementable.hpp>

namespace boost {
namespace detail {

template< typename P >
struct smart_ptr_pointee
{
    using type = typename P::element_type;
};

template<
    typename Iterator,
    typename = typename std::remove_reference< decltype(*std::declval< Iterator& >()) >::type
>
struct iterator_pointee
{
    using type = typename std::iterator_traits< Iterator >::value_type;
};

template< typename Iterator, typename Reference >
struct iterator_pointee< Iterator, const Reference >
{
    using type = typename std::add_const< typename std::iterator_traits< Iterator >::value_type >::type;
};

} // namespace detail

template< typename P >
struct pointee :
    public std::conditional<
        detail::is_incrementable< P >::value,
        detail::iterator_pointee< P >,
        detail::smart_ptr_pointee< P >
    >::type
{
};

template< typename P >
using pointee_t = typename pointee< P >::type;

} // namespace boost

#endif // BOOST_POINTEE_DWA200415_HPP

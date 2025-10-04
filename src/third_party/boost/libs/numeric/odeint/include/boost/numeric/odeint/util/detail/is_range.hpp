/*
 [auto_generated]
 boost/numeric/odeint/util/detail/is_range.hpp

 [begin_description]
 is_range implementation. Taken from the boost::range library.
 [end_description]

 Copyright 2011-2013 Karsten Ahnert
 Copyright 2011-2013 Thorsten Ottosen



 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
 */


#ifndef BOOST_NUMERIC_ODEINT_UTIL_DETAIL_IS_RANGE_HPP_INCLUDED
#define BOOST_NUMERIC_ODEINT_UTIL_DETAIL_IS_RANGE_HPP_INCLUDED


#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <cstddef>
#include <type_traits>
#include <boost/range/config.hpp>
#include <boost/numeric/odeint/tools/traits.hpp>

namespace boost {
namespace numeric {
namespace odeint {

namespace detail {

BOOST_NUMERIC_ODEINT_HAS_NAMED_TRAIT(has_iterator, iterator);
BOOST_NUMERIC_ODEINT_HAS_NAMED_TRAIT(has_const_iterator, const_iterator);

template< typename Range >
struct is_range : std::integral_constant<bool, (has_iterator<Range>::value && has_const_iterator<Range>::value)>
{
};

//////////////////////////////////////////////////////////////////////////
// pair
//////////////////////////////////////////////////////////////////////////

template< typename iteratorT >
struct is_range< std::pair<iteratorT,iteratorT> > : std::integral_constant<bool, true>
{
};

template< typename iteratorT >
struct is_range< const std::pair<iteratorT,iteratorT> > : std::integral_constant<bool, true>
{
};

//////////////////////////////////////////////////////////////////////////
// array
//////////////////////////////////////////////////////////////////////////

template< typename elementT, std::size_t sz >
struct is_range< elementT[sz] > : std::integral_constant<bool, true>
{
};

template< typename elementT, std::size_t sz >
struct is_range< const elementT[sz] > : std::integral_constant<bool, true>
{
};

//////////////////////////////////////////////////////////////////////////
// string
//////////////////////////////////////////////////////////////////////////

template<>
struct is_range< char* > : std::integral_constant<bool, true>
{
};

template<>
struct is_range< wchar_t* > : std::integral_constant<bool, true>
{
};

template<>
struct is_range< const char* > : std::integral_constant<bool, true>
{
};

template<>
struct is_range< const wchar_t* > : std::integral_constant<bool, true>
{
};

template<>
struct is_range< char* const > : std::integral_constant<bool, true>
{
};

template<>
struct is_range< wchar_t* const > : std::integral_constant<bool, true>
{
};

template<>
struct is_range< const char* const > : std::integral_constant<bool, true>
{
};

template<>
struct is_range< const wchar_t* const > : std::integral_constant<bool, true>
{
};

} // namespace detail

} // namespace odeint
} // namespace numeric
} // namespace boost



#endif // BOOST_NUMERIC_ODEINT_UTIL_DETAIL_IS_RANGE_HPP_INCLUDED

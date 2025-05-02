// Copyright (C) 2017 Andrzej Krzemienski.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/optional for documentation.
//
// You are welcome to contact the author at:
//  akrzemi1@gmail.com

#ifndef BOOST_OPTIONAL_DETAIL_EXPERIMENTAL_TRAITS_04NOV2017_HPP
#define BOOST_OPTIONAL_DETAIL_EXPERIMENTAL_TRAITS_04NOV2017_HPP

#include <boost/type_traits.hpp>

// The condition to use POD implementation

#ifdef BOOST_OPTIONAL_CONFIG_NO_POD_SPEC
# define BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
#elif defined BOOST_OPTIONAL_CONFIG_NO_SPEC_FOR_TRIVIAL_TYPES
# define BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
#elif !defined BOOST_HAS_TRIVIAL_MOVE_ASSIGN
# define BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
#elif !defined BOOST_HAS_TRIVIAL_MOVE_CONSTRUCTOR
# define BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
#elif !defined BOOST_HAS_TRIVIAL_COPY
# define BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
#elif !defined BOOST_HAS_TRIVIAL_ASSIGN
# define BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
#elif !defined BOOST_HAS_TRIVIAL_DESTRUCTOR
# define BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
#endif


namespace boost { namespace optional_detail {

#ifndef BOOST_OPTIONAL_DETAIL_NO_SPEC_FOR_TRIVIAL_TYPES
template <typename T>
struct is_trivially_semiregular
  : boost::conditional<(boost::has_trivial_copy_constructor<T>::value &&
                        boost::has_trivial_move_constructor<T>::value &&
                        boost::has_trivial_destructor<T>::value &&
                        boost::has_trivial_move_assign<T>::value &&
                        boost::has_trivial_assign<T>::value),
                        boost::true_type, boost::false_type>::type
{};
#else
template <typename T>
struct is_trivially_semiregular
: boost::conditional<(boost::is_scalar<T>::value && !boost::is_const<T>::value && !boost::is_volatile<T>::value),
                     boost::true_type, boost::false_type>::type
{};
#endif


}} // boost::optional_detail

#endif

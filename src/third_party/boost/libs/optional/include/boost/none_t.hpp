// Copyright (C) 2003, Fernando Luis Cacciola Carballal.
// Copyright (C) 2014, 2024 Andrzej Krzemienski.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/optional for documentation.
//
// You are welcome to contact the author at:
//  fernando_cacciola@hotmail.com
//
#ifndef BOOST_NONE_T_17SEP2003_HPP
#define BOOST_NONE_T_17SEP2003_HPP

#include <boost/config.hpp>
#include <boost/config/pragma_message.hpp>

#if defined (BOOST_NO_CXX11_RVALUE_REFERENCES) || defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) \
|| defined(BOOST_NO_CXX11_LAMBDAS) || defined(BOOST_NO_CXX11_DECLTYPE_N3276)  \
|| defined(BOOST_NO_CXX11_DELETED_FUNCTIONS) || defined(BOOST_NO_CXX11_DEFAULTED_FUNCTIONS) \
|| defined(BOOST_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS) || defined(BOOST_NO_CXX11_STATIC_ASSERT)

#error "Boost.Optional requires some C++11 features since version 1.87. If you have an older C++ version use Boost.Optional version 1.86 or earlier."

#elif defined(BOOST_NO_CXX11_REF_QUALIFIERS) || defined(BOOST_NO_CXX11_NOEXCEPT) || defined(BOOST_NO_CXX11_DEFAULTED_MOVES)

BOOST_PRAGMA_MESSAGE("C++03 support is deprecated in Boost.Optional 1.83 and will be removed in Boost.Optional 1.88.")

#endif

namespace boost {

#ifdef BOOST_OPTIONAL_USE_OLD_DEFINITION_OF_NONE

namespace detail { struct none_helper{}; }
typedef int detail::none_helper::*none_t ;

#elif defined BOOST_OPTIONAL_USE_SINGLETON_DEFINITION_OF_NONE

class none_t {};

#else

struct none_t
{
  struct init_tag{};
  explicit BOOST_CONSTEXPR none_t(init_tag){} // to disable default constructor
};

#endif // old implementation workarounds

} // namespace boost

#endif // header guard

#ifndef BOOST_CORE_LAUNDER_HPP_INCLUDED
#define BOOST_CORE_LAUNDER_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  Copyright 2023 Peter Dimov
//  Distributed under the Boost Software License, Version 1.0.
//  https://www.boost.org/LICENSE_1_0.txt

#include <boost/config.hpp>

#if defined(__has_builtin)
# if __has_builtin(__builtin_launder)
#  define BOOST_CORE_HAS_BUILTIN_LAUNDER
# endif
#endif

#if defined(BOOST_MSVC) && BOOST_MSVC < 1920

// msvc-14.1 suffers from internal compiler errors when using std::launder
// https://github.com/boostorg/core/issues/160
// https://github.com/boostorg/optional/issues/122

#elif (BOOST_CXX_VERSION >= 201703L) && !defined(BOOST_CORE_HAS_BUILTIN_LAUNDER)

#include <new>

#if defined(__cpp_lib_launder)
# define BOOST_CORE_HAS_STD_LAUNDER
#endif

#endif

namespace boost
{
namespace core
{

#if defined(BOOST_CORE_HAS_BUILTIN_LAUNDER)

template<class T> T* launder( T* p )
{
    return __builtin_launder( p );
}

#elif defined(BOOST_CORE_HAS_STD_LAUNDER)

template<class T> T* launder( T* p )
{
    return std::launder( p );
}

#else

template<class T> T* launder( T* p )
{
    return p;
}

#endif

} // namespace core
} // namespace boost

#endif  // #ifndef BOOST_CORE_LAUNDER_HPP_INCLUDED

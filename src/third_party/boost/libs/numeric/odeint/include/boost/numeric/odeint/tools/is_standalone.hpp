//  Copyright Matt Borland 2021.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_NUMERIC_ODEINT_TOOLS_IS_STANDALONE_HPP
#define BOOST_NUMERIC_ODEINT_TOOLS_IS_STANDALONE_HPP

// If one or more of our required dependencies are missing assume we are
// in standalone mode

#ifdef __has_include
#if !__has_include(<boost/config.hpp>) || !__has_include(<boost/assert.hpp>) || !__has_include(<boost/lexical_cast.hpp>) || \
    !__has_include(<boost/throw_exception.hpp>) || !__has_include(<boost/predef/other/endian.h>)
#   ifndef BOOST_NUMERIC_ODEINT_STANDALONE
#       define BOOST_NUMERIC_ODEINT_STANDALONE
#   endif
#endif
#endif

#endif //BOOST_NUMERIC_ODEINT_TOOLS_IS_STANDALONE_HPP

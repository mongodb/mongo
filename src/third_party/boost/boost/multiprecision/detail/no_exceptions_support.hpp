///////////////////////////////////////////////////////////////////////////////
//  Copyright 2004 - 2021 Pavel Vozenilek.
//  Copyright 2021 Matt Borland. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_DETAIL_NO_EXCEPTIONS_SUPPORT_HPP
#define BOOST_MP_DETAIL_NO_EXCEPTIONS_SUPPORT_HPP

#include <boost/multiprecision/detail/standalone_config.hpp>

#ifdef BOOST_MP_STANDALONE

#ifndef BOOST_NO_EXCEPTIONS
#   define BOOST_MP_TRY { try
#   define BOOST_MP_CATCH(x) catch(x)
#   define BOOST_MP_RETHROW throw;
#   define BOOST_MP_CATCH_END }
#   define BOOST_MP_THROW_EXCEPTION(x) throw (x);
#else
#   if !defined(BOOST_MSVC) || BOOST_MSVC >= 1900
#       define BOOST_MP_TRY { if (true)
#       define BOOST_MP_CATCH(x) else if (false)
#   else
        // warning C4127: conditional expression is constant
#       define BOOST_MP_TRY { \
            __pragma(warning(push)) \
            __pragma(warning(disable: 4127)) \
            if (true) \
            __pragma(warning(pop))
#       define BOOST_MP_CATCH(x) else \
            __pragma(warning(push)) \
            __pragma(warning(disable: 4127)) \
            if (false) \
            __pragma(warning(pop))
#   endif
#   define BOOST_MP_RETHROW
#   define BOOST_MP_CATCH_END }
#   define BOOST_MP_THROW_EXCEPTION(x) {(void)(x);}
#endif

#else // Not standalone mode

#   include <boost/core/no_exceptions_support.hpp>
#   include <boost/throw_exception.hpp>

#   define BOOST_MP_TRY BOOST_TRY
#   define BOOST_MP_CATCH(x) BOOST_CATCH(x)
#   define BOOST_MP_RETHROW BOOST_RETHROW
#   define BOOST_MP_CATCH_END BOOST_CATCH_END
#   define BOOST_MP_THROW_EXCEPTION(x) BOOST_THROW_EXCEPTION(x)

#endif // BOOST_MP_STANDALONE

#endif // BOOST_MP_DETAIL_NO_EXCEPTIONS_SUPPORT_HPP


//  Copyright Daniel James 2005-2006. Use, modification, and distribution are
//  subject to the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(BOOST_FUNCTIONAL_DETAIL_FLOAT_FUNCTIONS_HPP)
#define BOOST_FUNCTIONAL_DETAIL_FLOAT_FUNCTIONS_HPP

#include <cmath>

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

// The C++ standard requires that the C float functions are overloarded
// for float, double and long double in the std namespace, but some of the older
// library implementations don't support this. On some that don't, the C99
// float functions (frexpf, frexpl, etc.) are available.
//
// Some of this is based on guess work. If I don't know any better I assume that
// the standard C++ overloaded functions are available. If they're not then this
// means that the argument is cast to a double and back, which is inefficient
// and will give pretty bad results for long doubles - so if you know better
// let me know.

// STLport:
#if defined(__SGI_STL_PORT) || defined(_STLPORT_VERSION)
#  if (defined(__GNUC__) && __GNUC__ < 3 && (defined(linux) || defined(__linux) || defined(__linux__))) || defined(__DMC__)
#    define BOOST_HASH_USE_C99_FLOAT_FUNCS
#  elif defined(BOOST_MSVC) && BOOST_MSVC < 1300
#    define BOOST_HASH_USE_C99_FLOAT_FUNCS
#  else
#    define BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS
#  endif

// Roguewave:
//
// On borland 5.51, with roguewave 2.1.1 the standard C++ overloads aren't
// defined, but for the same version of roguewave on sunpro they are.
#elif defined(_RWSTD_VER)
#  if defined(__BORLANDC__)
#    define BOOST_HASH_USE_C99_FLOAT_FUNCS
#    define BOOST_HASH_C99_NO_FLOAT_FUNCS
#  elif defined(__DECCXX)
#    define BOOST_HASH_USE_C99_FLOAT_FUNCS
#  else
#    define BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS
#  endif

// libstdc++ (gcc 3.0 onwards, I think)
#elif defined(__GLIBCPP__) || defined(__GLIBCXX__)
#  define BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS

// SGI:
#elif defined(__STL_CONFIG_H)
#  if defined(linux) || defined(__linux) || defined(__linux__)
#    define BOOST_HASH_USE_C99_FLOAT_FUNCS
#  else
#    define BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS
#  endif

// Dinkumware.
#elif (defined(_YVALS) && !defined(__IBMCPP__)) || defined(_CPPLIB_VER)
// Overloaded float functions were probably introduced in an earlier version
// than this.
#  if defined(_CPPLIB_VER) && (_CPPLIB_VER >= 402)
#    define BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS
#  else
#    define BOOST_HASH_USE_C99_FLOAT_FUNCS
#  endif

// Digital Mars
#elif defined(__DMC__)
#  define BOOST_HASH_USE_C99_FLOAT_FUNCS

// Use overloaded float functions by default.
#else
#  define BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS
#endif

namespace boost
{
    namespace hash_detail
    {

        inline float call_ldexp(float v, int exp)
        {
            using namespace std;
#if defined(BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS) || \
    defined(BOOST_HASH_C99_NO_FLOAT_FUNCS)
            return ldexp(v, exp);
#else
            return ldexpf(v, exp);
#endif
        }

        inline double call_ldexp(double v, int exp)
        {
            using namespace std;
            return ldexp(v, exp);
        }

        inline long double call_ldexp(long double v, int exp)
        {
            using namespace std;
#if defined(BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS)
            return ldexp(v, exp);
#else
            return ldexpl(v, exp);
#endif
        }

        inline float call_frexp(float v, int* exp)
        {
            using namespace std;
#if defined(BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS) || \
    defined(BOOST_HASH_C99_NO_FLOAT_FUNCS)
            return frexp(v, exp);
#else
            return frexpf(v, exp);
#endif
        }

        inline double call_frexp(double v, int* exp)
        {
            using namespace std;
            return frexp(v, exp);
        }

        inline long double call_frexp(long double v, int* exp)
        {
            using namespace std;
#if defined(BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS)
            return frexp(v, exp);
#else
            return frexpl(v, exp);
#endif
        }
    }
}

#if defined(BOOST_HASH_USE_C99_FLOAT_FUNCS)
#undef BOOST_HASH_USE_C99_FLOAT_FUNCS
#endif

#if defined(BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS)
#undef BOOST_HASH_USE_OVERLOAD_FLOAT_FUNCS
#endif

#if defined(BOOST_HASH_C99_NO_FLOAT_FUNCS)
#undef BOOST_HASH_C99_NO_FLOAT_FUNCS
#endif

#endif

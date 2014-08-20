
//  Copyright (c) 2011 John Maddock
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MATH_TOOLS_BIG_CONSTANT_HPP
#define BOOST_MATH_TOOLS_BIG_CONSTANT_HPP

#include <boost/math/tools/config.hpp>
#ifndef BOOST_MATH_NO_LEXICAL_CAST
#include <boost/lexical_cast.hpp>
#endif
#include <boost/type_traits/is_convertible.hpp>
#include <boost/math/cstdfloat/cstdfloat_types.hpp>

namespace boost{ namespace math{ 

namespace tools{

template <class T>
inline BOOST_CONSTEXPR_OR_CONST T make_big_value(boost::floatmax_t v, const char*, mpl::true_ const&, mpl::false_ const&)
{
   return static_cast<T>(v);
}
template <class T>
inline BOOST_CONSTEXPR_OR_CONST T make_big_value(boost::floatmax_t v, const char*, mpl::true_ const&, mpl::true_ const&)
{
   return static_cast<T>(v);
}
#ifndef BOOST_MATH_NO_LEXICAL_CAST
template <class T>
inline T make_big_value(boost::floatmax_t, const char* s, mpl::false_ const&, mpl::false_ const&)
{
   return boost::lexical_cast<T>(s);
}
#endif
template <class T>
inline BOOST_CONSTEXPR const char* make_big_value(boost::floatmax_t, const char* s, mpl::false_ const&, mpl::true_ const&)
{
   return s;
}

//
// For constants which might fit in a long double (if it's big enough):
//
#define BOOST_MATH_BIG_CONSTANT(T, D, x)\
   boost::math::tools::make_big_value<T>(\
      BOOST_FLOATMAX_C(x), \
      BOOST_STRINGIZE(x), \
      mpl::bool_< (is_convertible<boost::floatmax_t, T>::value) && \
      ((D <= std::numeric_limits<boost::floatmax_t>::digits) \
          || is_floating_point<T>::value \
          || (std::numeric_limits<T>::is_specialized && \
          (std::numeric_limits<T>::digits10 <= std::numeric_limits<boost::floatmax_t>::digits10))) >(), \
      boost::is_convertible<const char*, T>())
//
// For constants too huge for any conceivable long double (and which generate compiler errors if we try and declare them as such):
//
#define BOOST_MATH_HUGE_CONSTANT(T, D, x)\
   boost::math::tools::make_big_value<T>(0.0L, BOOST_STRINGIZE(x), \
   mpl::bool_<is_floating_point<T>::value || (std::numeric_limits<T>::is_specialized && std::numeric_limits<T>::max_exponent <= std::numeric_limits<long double>::max_exponent && std::numeric_limits<T>::digits <= std::numeric_limits<long double>::digits)>(), \
   boost::is_convertible<const char*, T>())

}}} // namespaces

#endif


//  (C) Copyright John Maddock 2021.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// We deliberately use assert in here:
//

#ifndef BOOST_MP_DETAIL_FLOAT128_FUNCTIONS_HPP
#define BOOST_MP_DETAIL_FLOAT128_FUNCTIONS_HPP

#include <boost/multiprecision/detail/standalone_config.hpp>

#ifndef BOOST_MP_STANDALONE
#include <boost/cstdfloat.hpp>
#if defined(BOOST_MATH_USE_FLOAT128) && !defined(BOOST_CSTDFLOAT_NO_LIBQUADMATH_SUPPORT)
#  define BOOST_MP_HAVE_CSTDFLOAT
#endif
#endif

#if defined(BOOST_HAS_FLOAT128)

namespace boost 
{
namespace multiprecision 
{
namespace float128_procs 
{
extern "C" __float128 ldexpq(__float128, int) throw();
extern "C" __float128 frexpq(__float128, int*) throw();
extern "C" __float128 floorq(__float128) throw();
extern "C" __float128 nextafterq(__float128, __float128) throw();
extern "C" int        isinfq(__float128) throw();
extern "C" int        isnanq(__float128) throw();
extern "C" __float128 strtoflt128(const char*, char**) throw();

#ifdef BOOST_MP_HAVE_CSTDFLOAT
using std::ldexp;
using std::frexp;
using std::floor;
using std::nextafter;
#else
inline __float128 ldexp(__float128 f, int i) throw() { return ldexpq(f, i); }
inline __float128 frexp(__float128 f, int* p) throw() { return frexpq(f, p); }
inline __float128 floor(__float128 f) throw() { return floorq(f); }
inline __float128 nextafter(__float128 a, __float128 b) throw() { return nextafterq(a, b); }
#endif
}

namespace detail {

template <class T>
struct is_float128 : public std::is_same<__float128, T>
{};

}
}
}

namespace boost {
namespace math {

   inline __float128 float_next(const __float128& f)
   {
      return boost::multiprecision::float128_procs::nextafterq(f, 2 * f);
   }
   inline int (isinf)(const __float128& f)
   {
      return boost::multiprecision::float128_procs::isinfq(f);
   }
   inline int (isnan)(const __float128& f)
   {
      return boost::multiprecision::float128_procs::isnanq(f);
   }

}}

#define BOOST_MP_FLOAT128_USING using boost::multiprecision::float128_procs::ldexp; using boost::multiprecision::float128_procs::frexp; using boost::multiprecision::float128_procs::floor; using boost::multiprecision::float128_procs::nextafter; using boost::math::isinf; using boost::math::isnan;

#else
#define BOOST_MP_FLOAT128_USING

namespace boost {
namespace multiprecision {
namespace detail {

template <class T>
struct is_float128 : public std::false_type
{};

}}} // namespace boost::multiprecision::detail

#endif

#endif // BOOST_MP_DETAIL_FLOAT128_FUNCTIONS_HPP

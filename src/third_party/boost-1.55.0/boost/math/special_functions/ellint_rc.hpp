//  Copyright (c) 2006 Xiaogang Zhang
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  History:
//  XZ wrote the original of this file as part of the Google
//  Summer of Code 2006.  JM modified it to fit into the
//  Boost.Math conceptual framework better, and to correctly
//  handle the y < 0 case.
//

#ifndef BOOST_MATH_ELLINT_RC_HPP
#define BOOST_MATH_ELLINT_RC_HPP

#ifdef _MSC_VER
#pragma once
#endif

#include <boost/math/policies/error_handling.hpp>
#include <boost/math/tools/config.hpp>
#include <boost/math/special_functions/math_fwd.hpp>

// Carlson's degenerate elliptic integral
// R_C(x, y) = R_F(x, y, y) = 0.5 * \int_{0}^{\infty} (t+x)^{-1/2} (t+y)^{-1} dt
// Carlson, Numerische Mathematik, vol 33, 1 (1979)

namespace boost { namespace math { namespace detail{

template <typename T, typename Policy>
T ellint_rc_imp(T x, T y, const Policy& pol)
{
    T value, S, u, lambda, tolerance, prefix;
    unsigned long k;

    BOOST_MATH_STD_USING
    using namespace boost::math::tools;

    static const char* function = "boost::math::ellint_rc<%1%>(%1%,%1%)";

    if(x < 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument x must be non-negative but got %1%", x, pol);
    }
    if(y == 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument y must not be zero but got %1%", y, pol);
    }

    // error scales as the 6th power of tolerance
    tolerance = pow(4 * tools::epsilon<T>(), T(1) / 6);

    // for y < 0, the integral is singular, return Cauchy principal value
    if (y < 0)
    {
        prefix = sqrt(x / (x - y));
        x = x - y;
        y = -y;
    }
    else
       prefix = 1;

    // duplication:
    k = 1;
    do
    {
        u = (x + y + y) / 3;
        S = y / u - 1;               // 1 - x / u = 2 * S

        if (2 * abs(S) < tolerance) 
           break;

        T sx = sqrt(x);
        T sy = sqrt(y);
        lambda = 2 * sx * sy + y;
        x = (x + lambda) / 4;
        y = (y + lambda) / 4;
        ++k;
    }while(k < policies::get_max_series_iterations<Policy>());
    // Check to see if we gave up too soon:
    policies::check_series_iterations<T>(function, k, pol);

    // Taylor series expansion to the 5th order
    value = (1 + S * S * (T(3) / 10 + S * (T(1) / 7 + S * (T(3) / 8 + S * T(9) / 22)))) / sqrt(u);

    return value * prefix;
}

} // namespace detail

template <class T1, class T2, class Policy>
inline typename tools::promote_args<T1, T2>::type 
   ellint_rc(T1 x, T2 y, const Policy& pol)
{
   typedef typename tools::promote_args<T1, T2>::type result_type;
   typedef typename policies::evaluation<result_type, Policy>::type value_type;
   return policies::checked_narrowing_cast<result_type, Policy>(
      detail::ellint_rc_imp(
         static_cast<value_type>(x),
         static_cast<value_type>(y), pol), "boost::math::ellint_rc<%1%>(%1%,%1%)");
}

template <class T1, class T2>
inline typename tools::promote_args<T1, T2>::type 
   ellint_rc(T1 x, T2 y)
{
   return ellint_rc(x, y, policies::policy<>());
}

}} // namespaces

#endif // BOOST_MATH_ELLINT_RC_HPP


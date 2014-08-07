//  Copyright (c) 2006 Xiaogang Zhang
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  History:
//  XZ wrote the original of this file as part of the Google
//  Summer of Code 2006.  JM modified it to fit into the
//  Boost.Math conceptual framework better, and to handle
//  types longer than 80-bit reals.
//
#ifndef BOOST_MATH_ELLINT_RF_HPP
#define BOOST_MATH_ELLINT_RF_HPP

#ifdef _MSC_VER
#pragma once
#endif

#include <boost/math/special_functions/math_fwd.hpp>
#include <boost/math/tools/config.hpp>

#include <boost/math/policies/error_handling.hpp>

// Carlson's elliptic integral of the first kind
// R_F(x, y, z) = 0.5 * \int_{0}^{\infty} [(t+x)(t+y)(t+z)]^{-1/2} dt
// Carlson, Numerische Mathematik, vol 33, 1 (1979)

namespace boost { namespace math { namespace detail{

template <typename T, typename Policy>
T ellint_rf_imp(T x, T y, T z, const Policy& pol)
{
    T value, X, Y, Z, E2, E3, u, lambda, tolerance;
    unsigned long k;

    BOOST_MATH_STD_USING
    using namespace boost::math::tools;

    static const char* function = "boost::math::ellint_rf<%1%>(%1%,%1%,%1%)";

    if (x < 0 || y < 0 || z < 0)
    {
       return policies::raise_domain_error<T>(function,
            "domain error, all arguments must be non-negative, "
            "only sensible result is %1%.",
            std::numeric_limits<T>::quiet_NaN(), pol);
    }
    if (x + y == 0 || y + z == 0 || z + x == 0)
    {
       return policies::raise_domain_error<T>(function,
            "domain error, at most one argument can be zero, "
            "only sensible result is %1%.",
            std::numeric_limits<T>::quiet_NaN(), pol);
    }

    // Carlson scales error as the 6th power of tolerance,
    // but this seems not to work for types larger than
    // 80-bit reals, this heuristic seems to work OK:
    if(policies::digits<T, Policy>() > 64)
    {
      tolerance = pow(tools::epsilon<T>(), T(1)/4.25f);
      BOOST_MATH_INSTRUMENT_VARIABLE(tolerance);
    }
    else
    {
      tolerance = pow(4*tools::epsilon<T>(), T(1)/6);
      BOOST_MATH_INSTRUMENT_VARIABLE(tolerance);
    }

    // duplication
    k = 1;
    do
    {
        u = (x + y + z) / 3;
        X = (u - x) / u;
        Y = (u - y) / u;
        Z = (u - z) / u;

        // Termination condition: 
        if ((tools::max)(abs(X), abs(Y), abs(Z)) < tolerance) 
           break; 

        T sx = sqrt(x);
        T sy = sqrt(y);
        T sz = sqrt(z);
        lambda = sy * (sx + sz) + sz * sx;
        x = (x + lambda) / 4;
        y = (y + lambda) / 4;
        z = (z + lambda) / 4;
        ++k;
    }
    while(k < policies::get_max_series_iterations<Policy>());

    // Check to see if we gave up too soon:
    policies::check_series_iterations<T>(function, k, pol);
    BOOST_MATH_INSTRUMENT_VARIABLE(k);

    // Taylor series expansion to the 5th order
    E2 = X * Y - Z * Z;
    E3 = X * Y * Z;
    value = (1 + E2*(E2/24 - E3*T(3)/44 - T(0.1)) + E3/14) / sqrt(u);
    BOOST_MATH_INSTRUMENT_VARIABLE(value);

    return value;
}

} // namespace detail

template <class T1, class T2, class T3, class Policy>
inline typename tools::promote_args<T1, T2, T3>::type 
   ellint_rf(T1 x, T2 y, T3 z, const Policy& pol)
{
   typedef typename tools::promote_args<T1, T2, T3>::type result_type;
   typedef typename policies::evaluation<result_type, Policy>::type value_type;
   return policies::checked_narrowing_cast<result_type, Policy>(
      detail::ellint_rf_imp(
         static_cast<value_type>(x),
         static_cast<value_type>(y),
         static_cast<value_type>(z), pol), "boost::math::ellint_rf<%1%>(%1%,%1%,%1%)");
}

template <class T1, class T2, class T3>
inline typename tools::promote_args<T1, T2, T3>::type 
   ellint_rf(T1 x, T2 y, T3 z)
{
   return ellint_rf(x, y, z, policies::policy<>());
}

}} // namespaces

#endif // BOOST_MATH_ELLINT_RF_HPP


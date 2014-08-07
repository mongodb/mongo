//  Copyright (c) 2006 Xiaogang Zhang
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  History:
//  XZ wrote the original of this file as part of the Google
//  Summer of Code 2006.  JM modified it slightly to fit into the
//  Boost.Math conceptual framework better.

#ifndef BOOST_MATH_ELLINT_RD_HPP
#define BOOST_MATH_ELLINT_RD_HPP

#ifdef _MSC_VER
#pragma once
#endif

#include <boost/math/special_functions/math_fwd.hpp>
#include <boost/math/tools/config.hpp>
#include <boost/math/policies/error_handling.hpp>

// Carlson's elliptic integral of the second kind
// R_D(x, y, z) = R_J(x, y, z, z) = 1.5 * \int_{0}^{\infty} [(t+x)(t+y)]^{-1/2} (t+z)^{-3/2} dt
// Carlson, Numerische Mathematik, vol 33, 1 (1979)

namespace boost { namespace math { namespace detail{

template <typename T, typename Policy>
T ellint_rd_imp(T x, T y, T z, const Policy& pol)
{
    T value, u, lambda, sigma, factor, tolerance;
    T X, Y, Z, EA, EB, EC, ED, EE, S1, S2;
    unsigned long k;

    BOOST_MATH_STD_USING
    using namespace boost::math::tools;

    static const char* function = "boost::math::ellint_rd<%1%>(%1%,%1%,%1%)";

    if (x < 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument x must be >= 0, but got %1%", x, pol);
    }
    if (y < 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument y must be >= 0, but got %1%", y, pol);
    }
    if (z <= 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument z must be > 0, but got %1%", z, pol);
    }
    if (x + y == 0)
    {
       return policies::raise_domain_error<T>(function,
            "At most one argument can be zero, but got, x + y = %1%", x+y, pol);
    }

    // error scales as the 6th power of tolerance
    tolerance = pow(tools::epsilon<T>() / 3, T(1)/6);

    // duplication
    sigma = 0;
    factor = 1;
    k = 1;
    do
    {
        u = (x + y + z + z + z) / 5;
        X = (u - x) / u;
        Y = (u - y) / u;
        Z = (u - z) / u;
        if ((tools::max)(abs(X), abs(Y), abs(Z)) < tolerance) 
           break;
        T sx = sqrt(x);
        T sy = sqrt(y);
        T sz = sqrt(z);
        lambda = sy * (sx + sz) + sz * sx; //sqrt(x * y) + sqrt(y * z) + sqrt(z * x);
        sigma += factor / (sz * (z + lambda));
        factor /= 4;
        x = (x + lambda) / 4;
        y = (y + lambda) / 4;
        z = (z + lambda) / 4;
        ++k;
    }
    while(k < policies::get_max_series_iterations<Policy>());

    // Check to see if we gave up too soon:
    policies::check_series_iterations<T>(function, k, pol);

    // Taylor series expansion to the 5th order
    EA = X * Y;
    EB = Z * Z;
    EC = EA - EB;
    ED = EA - 6 * EB;
    EE = ED + EC + EC;
    S1 = ED * (ED * T(9) / 88 - Z * EE * T(9) / 52 - T(3) / 14);
    S2 = Z * (EE / 6 + Z * (-EC * T(9) / 22 + Z * EA * T(3) / 26));
    value = 3 * sigma + factor * (1 + S1 + S2) / (u * sqrt(u));

    return value;
}

} // namespace detail

template <class T1, class T2, class T3, class Policy>
inline typename tools::promote_args<T1, T2, T3>::type 
   ellint_rd(T1 x, T2 y, T3 z, const Policy& pol)
{
   typedef typename tools::promote_args<T1, T2, T3>::type result_type;
   typedef typename policies::evaluation<result_type, Policy>::type value_type;
   return policies::checked_narrowing_cast<result_type, Policy>(
      detail::ellint_rd_imp(
         static_cast<value_type>(x),
         static_cast<value_type>(y),
         static_cast<value_type>(z), pol), "boost::math::ellint_rd<%1%>(%1%,%1%,%1%)");
}

template <class T1, class T2, class T3>
inline typename tools::promote_args<T1, T2, T3>::type 
   ellint_rd(T1 x, T2 y, T3 z)
{
   return ellint_rd(x, y, z, policies::policy<>());
}

}} // namespaces

#endif // BOOST_MATH_ELLINT_RD_HPP


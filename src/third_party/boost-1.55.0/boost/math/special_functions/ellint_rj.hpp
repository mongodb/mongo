//  Copyright (c) 2006 Xiaogang Zhang
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//  History:
//  XZ wrote the original of this file as part of the Google
//  Summer of Code 2006.  JM modified it to fit into the
//  Boost.Math conceptual framework better, and to correctly
//  handle the p < 0 case.
//

#ifndef BOOST_MATH_ELLINT_RJ_HPP
#define BOOST_MATH_ELLINT_RJ_HPP

#ifdef _MSC_VER
#pragma once
#endif

#include <boost/math/special_functions/math_fwd.hpp>
#include <boost/math/tools/config.hpp>
#include <boost/math/policies/error_handling.hpp>
#include <boost/math/special_functions/ellint_rc.hpp>
#include <boost/math/special_functions/ellint_rf.hpp>

// Carlson's elliptic integral of the third kind
// R_J(x, y, z, p) = 1.5 * \int_{0}^{\infty} (t+p)^{-1} [(t+x)(t+y)(t+z)]^{-1/2} dt
// Carlson, Numerische Mathematik, vol 33, 1 (1979)

namespace boost { namespace math { namespace detail{

template <typename T, typename Policy>
T ellint_rj_imp(T x, T y, T z, T p, const Policy& pol)
{
    T value, u, lambda, alpha, beta, sigma, factor, tolerance;
    T X, Y, Z, P, EA, EB, EC, E2, E3, S1, S2, S3;
    unsigned long k;

    BOOST_MATH_STD_USING
    using namespace boost::math::tools;

    static const char* function = "boost::math::ellint_rj<%1%>(%1%,%1%,%1%)";

    if (x < 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument x must be non-negative, but got x = %1%", x, pol);
    }
    if(y < 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument y must be non-negative, but got y = %1%", y, pol);
    }
    if(z < 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument z must be non-negative, but got z = %1%", z, pol);
    }
    if(p == 0)
    {
       return policies::raise_domain_error<T>(function,
            "Argument p must not be zero, but got p = %1%", p, pol);
    }
    if (x + y == 0 || y + z == 0 || z + x == 0)
    {
       return policies::raise_domain_error<T>(function,
            "At most one argument can be zero, "
            "only possible result is %1%.", std::numeric_limits<T>::quiet_NaN(), pol);
    }

    // error scales as the 6th power of tolerance
    tolerance = pow(T(1) * tools::epsilon<T>() / 3, T(1) / 6);

    // for p < 0, the integral is singular, return Cauchy principal value
    if (p < 0)
    {
       //
       // We must ensure that (z - y) * (y - x) is positive.
       // Since the integral is symmetrical in x, y and z
       // we can just permute the values:
       //
       if(x > y)
          std::swap(x, y);
       if(y > z)
          std::swap(y, z);
       if(x > y)
          std::swap(x, y);

       T q = -p;
       T pmy = (z - y) * (y - x) / (y + q);  // p - y

       BOOST_ASSERT(pmy >= 0);

       p = pmy + y;
       value = boost::math::ellint_rj(x, y, z, p, pol);
       value *= pmy;
       value -= 3 * boost::math::ellint_rf(x, y, z, pol);
       value += 3 * sqrt((x * y * z) / (x * z + p * q)) * boost::math::ellint_rc(x * z + p * q, p * q, pol);
       value /= (y + q);
       return value;
    }

    // duplication
    sigma = 0;
    factor = 1;
    k = 1;
    do
    {
        u = (x + y + z + p + p) / 5;
        X = (u - x) / u;
        Y = (u - y) / u;
        Z = (u - z) / u;
        P = (u - p) / u;
        
        if ((tools::max)(abs(X), abs(Y), abs(Z), abs(P)) < tolerance) 
           break;

        T sx = sqrt(x);
        T sy = sqrt(y);
        T sz = sqrt(z);
        
        lambda = sy * (sx + sz) + sz * sx;
        alpha = p * (sx + sy + sz) + sx * sy * sz;
        alpha *= alpha;
        beta = p * (p + lambda) * (p + lambda);
        sigma += factor * boost::math::ellint_rc(alpha, beta, pol);
        factor /= 4;
        x = (x + lambda) / 4;
        y = (y + lambda) / 4;
        z = (z + lambda) / 4;
        p = (p + lambda) / 4;
        ++k;
    }
    while(k < policies::get_max_series_iterations<Policy>());

    // Check to see if we gave up too soon:
    policies::check_series_iterations<T>(function, k, pol);

    // Taylor series expansion to the 5th order
    EA = X * Y + Y * Z + Z * X;
    EB = X * Y * Z;
    EC = P * P;
    E2 = EA - 3 * EC;
    E3 = EB + 2 * P * (EA - EC);
    S1 = 1 + E2 * (E2 * T(9) / 88 - E3 * T(9) / 52 - T(3) / 14);
    S2 = EB * (T(1) / 6 + P * (T(-6) / 22 + P * T(3) / 26));
    S3 = P * ((EA - EC) / 3 - P * EA * T(3) / 22);
    value = 3 * sigma + factor * (S1 + S2 + S3) / (u * sqrt(u));

    return value;
}

} // namespace detail

template <class T1, class T2, class T3, class T4, class Policy>
inline typename tools::promote_args<T1, T2, T3, T4>::type 
   ellint_rj(T1 x, T2 y, T3 z, T4 p, const Policy& pol)
{
   typedef typename tools::promote_args<T1, T2, T3, T4>::type result_type;
   typedef typename policies::evaluation<result_type, Policy>::type value_type;
   return policies::checked_narrowing_cast<result_type, Policy>(
      detail::ellint_rj_imp(
         static_cast<value_type>(x),
         static_cast<value_type>(y),
         static_cast<value_type>(z),
         static_cast<value_type>(p),
         pol), "boost::math::ellint_rj<%1%>(%1%,%1%,%1%,%1%)");
}

template <class T1, class T2, class T3, class T4>
inline typename tools::promote_args<T1, T2, T3, T4>::type 
   ellint_rj(T1 x, T2 y, T3 z, T4 p)
{
   return ellint_rj(x, y, z, p, policies::policy<>());
}

}} // namespaces

#endif // BOOST_MATH_ELLINT_RJ_HPP


//==============================================================================
//         Copyright 2014          LRI    UMR 8623 CNRS/Univ Paris Sud XI
//         Copyright 2014          NumScale SAS
//
//          Distributed under the Boost Software License, Version 1.0.
//                 See accompanying file LICENSE.txt or copy at
//                     http://www.boost.org/LICENSE_1_0.txt
//==============================================================================
#include <boost/numeric/odeint.hpp>
#include <nt2/table.hpp>
#include <nt2/include/functions/zeros.hpp>
#include <nt2/include/functions/ones.hpp>

#include <boost/config.hpp>
#ifdef BOOST_MSVC
    #pragma warning(disable:4996)
#endif

#define BOOST_TEST_MODULE odeint_nt2_copy

#include <boost/test/included/unit_test.hpp>
#include <boost/test/floating_point_comparison.hpp>
#include <boost/numeric/odeint/external/nt2/nt2_norm_inf.hpp>

#include <boost/mpl/list.hpp>

using namespace boost::unit_test;
using namespace boost::numeric::odeint;

typedef boost::mpl::list< float , double > fp_types;

BOOST_AUTO_TEST_SUITE( nt2_norm_inf )

BOOST_AUTO_TEST_CASE_TEMPLATE( test_norm_inf, T, fp_types )
{
  nt2::table<T> x = nt2::ones(10,1, nt2::meta::as_<T>() );
  x(4) = 55;

  nt2::table<T> y = nt2::zeros(8,8, nt2::meta::as_<T>() );
  y(6,4) = -42;

  BOOST_CHECK_SMALL(vector_space_norm_inf<nt2::table<T> >()(x) - T(55), T(1e-10));
  BOOST_CHECK_SMALL(vector_space_norm_inf<nt2::table<T> >()(y) - T(42), T(1e-10));
}

BOOST_AUTO_TEST_SUITE_END()

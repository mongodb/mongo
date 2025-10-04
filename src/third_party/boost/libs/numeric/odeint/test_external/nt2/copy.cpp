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
#include <nt2/include/functions/linspace.hpp>

#include <boost/config.hpp>
#ifdef BOOST_MSVC
    #pragma warning(disable:4996)
#endif

#define BOOST_TEST_MODULE odeint_nt2_copy

#include <boost/test/included/unit_test.hpp>
#include <boost/numeric/odeint/external/nt2/nt2_copy.hpp>

#include <boost/mpl/list.hpp>

using namespace boost::unit_test;
using namespace boost::numeric::odeint;

typedef boost::mpl::list< float , double > fp_types;

BOOST_AUTO_TEST_SUITE( nt2_copy )

BOOST_AUTO_TEST_CASE_TEMPLATE( test_copy, T, fp_types )
{
  nt2::table<T> x = nt2::linspace(T(1),T(0),7);

  nt2::table<T> y;

  copy(y,x);

  for (std::size_t ii=1; ii<=x.size();ii++)
    BOOST_CHECK_EQUAL(x(ii),y(ii));
}

BOOST_AUTO_TEST_SUITE_END()

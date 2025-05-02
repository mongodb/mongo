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

#include <boost/config.hpp>
#ifdef BOOST_MSVC
    #pragma warning(disable:4996)
#endif

#define BOOST_TEST_MODULE odeint_nt2_resize

#include <boost/test/included/unit_test.hpp>
#include <boost/numeric/odeint/external/nt2/nt2_resize.hpp>

#include <boost/mpl/list.hpp>

using namespace boost::unit_test;
using namespace boost::numeric::odeint;

typedef boost::mpl::list< float , double > fp_types;

BOOST_AUTO_TEST_SUITE( nt2_resize )

BOOST_AUTO_TEST_CASE_TEMPLATE( test_resize, T, fp_types )
{
  nt2::table<T> x;
  x.resize(nt2::of_size(10,10));

  nt2::table<T> y;

  BOOST_CHECK_EQUAL(same_size(x,y),false);

  resize(y,x);

  BOOST_CHECK_EQUAL(same_size(x,y),true);
}

BOOST_AUTO_TEST_SUITE_END()

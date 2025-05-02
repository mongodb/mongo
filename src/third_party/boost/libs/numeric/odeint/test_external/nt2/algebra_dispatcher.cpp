//==============================================================================
//         Copyright 2014          LRI    UMR 8623 CNRS/Univ Paris Sud XI
//         Copyright 2014          NumScale SAS
//
//          Distributed under the Boost Software License, Version 1.0.
//                 See accompanying file LICENSE.txt or copy at
//                     http://www.boost.org/LICENSE_1_0.txt
//==============================================================================
#define BOOST_TEST_MODULE odeint_nt2_algebra_dispatcher

#include <boost/test/included/unit_test.hpp>
#include <boost/test/floating_point_comparison.hpp>
#include <boost/numeric/odeint/external/nt2/nt2_algebra_dispatcher.hpp>
#include <boost/numeric/odeint/algebra/default_operations.hpp>
#include <boost/mpl/list.hpp>

#include <boost/preprocessor/repetition.hpp>
#include <boost/preprocessor/arithmetic/mul.hpp>

#include <nt2/table.hpp>
#include <nt2/sdk/meta/as.hpp>
#include <nt2/include/functions/ones.hpp>

using namespace boost::unit_test;
using namespace boost::numeric::odeint;

typedef boost::mpl::list< float , double > fp_types;

#define TABLE(z,n,text) nt2::table<T> y ## n =                                   \
                        nt2::ones(1,2,nt2::meta::as_<T>() )*T(BOOST_PP_ADD(n,1));

#define PARAMS(z,n,text) T(BOOST_PP_ADD(n,1)),

#define SUM(z,n,text) +BOOST_PP_MUL(BOOST_PP_ADD(n,3),BOOST_PP_ADD(n,2))

#define TEST(z,n,text) BOOST_CHECK_SMALL( y0(BOOST_PP_ADD(n,1))                  \
                       -T( 2 BOOST_PP_REPEAT(text, SUM, text) ), T(1e-10) );

#define TEST_CASE(z,n,text) BOOST_AUTO_TEST_CASE_TEMPLATE (                      \
                            BOOST_PP_CAT(odeint_foreach, n), T, fp_types )       \
{                                                                                \
  vector_space_algebra algebra;                                                  \
  BOOST_PP_REPEAT(BOOST_PP_ADD(n,2),TABLE,tt)                                    \
  BOOST_PP_CAT(algebra.for_each,BOOST_PP_ADD(n,2))(                              \
               BOOST_PP_ENUM_PARAMS(BOOST_PP_ADD(n,2),y), default_operations::   \
  BOOST_PP_CAT(scale_sum,BOOST_PP_ADD(n,1)) <T>(                                 \
               BOOST_PP_REPEAT(n, PARAMS, text ) T(BOOST_PP_ADD(n,1))));         \
  BOOST_PP_REPEAT(2,TEST,n)                                                      \
}

BOOST_AUTO_TEST_SUITE( nt2_algebra )

BOOST_PP_REPEAT(7,TEST_CASE,dummy)

BOOST_AUTO_TEST_SUITE_END()

/*
 [auto_generated]
 integrate.cpp

 [begin_description]
 tba.
 [end_description]

 Copyright 2009-2012 Karsten Ahnert
 Copyright 2009-2012 Mario Mulansky

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#include <boost/config.hpp>
#ifdef BOOST_MSVC
    #pragma warning(disable:4996)
#endif

#define BOOST_TEST_MODULE odeint_eigen_integrate

#include <boost/numeric/odeint/integrate/integrate.hpp>
#include <boost/numeric/odeint/external/eigen/eigen.hpp>

#include <boost/test/unit_test.hpp>

#include "dummy_odes.hpp"


using namespace boost::unit_test;
using namespace boost::numeric::odeint;


BOOST_AUTO_TEST_SUITE( eigen_integrate )

BOOST_AUTO_TEST_CASE( test_const_sys )
{
    typedef Eigen::Matrix< double , 1 , 1 > state_type;
    state_type x;
    x[0] = 10.0;
    double t_start = 0.0 , t_end = 1.0 , dt = 0.1;
    integrate< double >( constant_system_functor_standard() , x , t_start , t_end , dt );
    BOOST_CHECK_CLOSE( x[0] , 11.0 , 1.0e-13 );
}

BOOST_AUTO_TEST_CASE( test_lorenz )
{
    typedef Eigen::Matrix< double , 3 , 1 > state_type;
    state_type x;
    x[0] = 10.0;
    x[1] = 10.0;
    x[2] = 10.0;
    double t_start = 0.0 , t_end = 1000.0 , dt = 0.1;
    integrate< double >( lorenz() , x , t_start , t_end , dt );
    
    std::vector< double > x2( 3 );
    x2[0] = 10.0;
    x2[1] = 10.0;
    x2[2] = 10.0;
    integrate( lorenz() , x2 , t_start , t_end , dt );
    
    BOOST_CHECK_CLOSE( x[0] , x2[0] , 1.0e-13 );
    BOOST_CHECK_CLOSE( x[1] , x2[1] , 1.0e-13 );
    BOOST_CHECK_CLOSE( x[2] , x2[2] , 1.0e-13 );
}

BOOST_AUTO_TEST_SUITE_END()

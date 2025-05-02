/* Boost check_mkl.cpp test file

 Copyright 2010-2011 Mario Mulansky
 Copyright 2011 Karsten Ahnert

 This file tests the odeint library with the intel mkl blas1 routines

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
*/

#define BOOST_TEST_MODULE test_mkl

#include <boost/test/unit_test.hpp>

#include <boost/numeric/odeint/stepper/euler.hpp>
#include <boost/numeric/odeint/stepper/runge_kutta4.hpp>
#include <boost/numeric/odeint/algebra/vector_space_algebra.hpp>
#include <boost/numeric/odeint/external/mkl/mkl_operations.hpp>

using namespace boost::numeric::odeint;

typedef double value_type;
typedef std::array< value_type , 1 > state_type;


void constant_system( state_type &x , state_type &dxdt , value_type t )
{
    dxdt[0] = 1.0;
}

const double eps = 1E-14;


BOOST_AUTO_TEST_CASE( test_mkl )
{

    //to use mkl routines we have to use the vector_space_algebra and the mkl_operations
    runge_kutta4< state_type , value_type , state_type , value_type , vector_space_algebra , mkl_operations > stepper;
    state_type x;
    x[0] = 0.0;

    stepper.do_step( constant_system , x , 0.0 , 0.1 );

    using std::abs;

    std::cout << x[0] << " ?= " << 0.1 << std::endl;
    BOOST_CHECK_SMALL( abs( x[0] - 0.1 ) , eps );

}

/* Boost check_thrust.cu test file
 
 Copyright 2010-2013 Mario Mulansky
 Copyright 2010-2011 Karsten Ahnert
 
 This file tests the use of the euler stepper
  
 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
*/

//#include <boost/test/unit_test.hpp>

#include <boost/numeric/odeint/stepper/euler.hpp>
#include <boost/numeric/odeint/external/thrust/thrust.hpp>

#include <thrust/device_vector.h>
#include <thrust/fill.h>

using namespace boost::numeric::odeint;

typedef float base_type;
// typedef thrust::device_vector< base_type > state_type;
typedef thrust::host_vector< base_type > state_type;

void constant_system( const state_type &x , state_type &dxdt , base_type t )
{
	thrust::fill( dxdt.begin() , dxdt.end() , static_cast<base_type>(1.0) );
}

const base_type eps = 1.0e-7;


template< class Stepper , class System >
void check_stepper_concept( Stepper &stepper , System system , typename Stepper::state_type &x )
{
    typedef Stepper stepper_type;
    typedef typename stepper_type::state_type container_type;
    typedef typename stepper_type::order_type order_type;
    typedef typename stepper_type::time_type time_type;

    stepper.do_step( system , x , 0.0 , 0.1 );
    base_type xval = *boost::begin( x );
    if( fabs( xval - 0.1 ) < eps )
    	std::clog << "TEST PASSED" << std::endl;
    else
    	std::clog << "TEST FAILED" << std::endl;
}

void test_euler_with_thrust( void )
{
	state_type x(1);
	thrust::fill( x.begin() , x.end() , static_cast<base_type>(0.0) );
	euler< state_type , base_type , state_type , base_type > euler;
	check_stepper_concept( euler , constant_system , x );


}

/*test_suite* init_unit_test_suite( int argc, char* argv[] )
{
    test_suite *test = BOOST_TEST_SUITE("check stepper with thrust");

    test->add( BOOST_TEST_CASE( &test_euler_with_thrust ) );

    return test;
}*/

int main() {
	test_euler_with_thrust();
}

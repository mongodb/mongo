/* Boost check_gmp.cpp test file

 Copyright 2010-2011 Karsten Ahnert
 Copyright 2011 Mario Mulansky

 This file tests the odeint library with the gmp arbitrary precision types

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
*/

#define BOOST_TEST_MODULE odeint_gsl

#include <gsl/gsl_vector.h>
#include <boost/numeric/odeint/stepper/euler.hpp>
#include <boost/numeric/odeint/external/gsl/gsl_wrapper.hpp>

#include <boost/test/unit_test.hpp>

using namespace boost::unit_test;
using namespace boost::numeric::odeint;

typedef gsl_vector *state_type;

const double sigma = 10.0;
const double R = 28.0;
const double b = 8.0 / 3.0;

void lorenz( const state_type x , state_type dxdt , double t )
{
    gsl_vector_set( dxdt , 0 , sigma * ( gsl_vector_get(x , 1 ) - gsl_vector_get( x , 0 ) ) );
    gsl_vector_set( dxdt , 1 , R * gsl_vector_get( x , 0 ) - gsl_vector_get( x , 1 ) - gsl_vector_get( x , 0 ) * gsl_vector_get( x , 2) );
    gsl_vector_set( dxdt , 2 , gsl_vector_get( x , 0 ) * gsl_vector_get( x , 1 ) - b * gsl_vector_get( x , 2) );
}

BOOST_AUTO_TEST_CASE( gsl )
{
    euler< state_type > euler;

    state_type x = gsl_vector_alloc( 3 );

    // check resizing
    state_type y = 0;
    boost::numeric::odeint::resize( y , x );
    BOOST_CHECK( 0 != y );

    gsl_vector_set( x , 0 , 1.0);
    gsl_vector_set( x , 1 , 1.0);
    gsl_vector_set( x , 2 , 2.0);

    euler.do_step( lorenz , x , 0.0 , 0.1 );

    //cout << gsl_vector_get( x , 0 ) << "  " << gsl_vector_get( x , 1 ) << "  " << gsl_vector_get( x , 2 ) << endl;

    gsl_vector_free( x );

}

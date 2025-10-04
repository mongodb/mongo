/* Boost check_gmp.cpp test file

 Copyright 2010-2012 Mario Mulansky
 Copyright 2011-2012 Karsten Ahnert

 This file tests the odeint library with the gmp arbitrary precision types

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
*/

#define BOOST_TEST_MODULE odeint_gmp

#include <iostream>

#include <gmpxx.h>

#include <boost/test/unit_test.hpp>
#include <array>

#include <boost/mpl/vector.hpp>

#include <boost/numeric/odeint.hpp>
//#include <boost/numeric/odeint/algebra/vector_space_algebra.hpp>

using namespace boost::unit_test;
using namespace boost::numeric::odeint;

namespace mpl = boost::mpl;

const int precision = 1024;

typedef mpf_class value_type;
typedef mpf_class state_type;

//provide min, max and pow functions for mpf types - required for controlled steppers
value_type min( const value_type a , const value_type b )
{
    if( a<b ) return a;
    else return b;
}
value_type max( const value_type a , const value_type b )
{
    if( a>b ) return a;
    else return b;
}
value_type pow( const value_type a , const value_type b )
{
    // do calculation in double precision
    return value_type( std::pow( a.get_d() , b.get_d() ) );
}


//provide vector_space reduce:

namespace boost { namespace numeric { namespace odeint {

template<>
struct vector_space_reduce< state_type >
{
  template< class Op >
  state_type operator()( state_type x , Op op , state_type init ) const
  {
      init = op( init , x );
      return init;
  }
};

} } }


void constant_system( const state_type &x , state_type &dxdt , value_type t )
{
    dxdt = value_type( 1.0 , precision );
}


/* check runge kutta stepers */
typedef mpl::vector<
    euler< state_type , value_type , state_type , value_type , vector_space_algebra > ,
    modified_midpoint< state_type , value_type , state_type , value_type , vector_space_algebra > ,
    runge_kutta4< state_type , value_type , state_type , value_type , vector_space_algebra > ,
    runge_kutta4_classic< state_type , value_type , state_type , value_type , vector_space_algebra > ,
    runge_kutta_cash_karp54_classic< state_type , value_type , state_type , value_type , vector_space_algebra > ,
    runge_kutta_cash_karp54< state_type , value_type , state_type , value_type , vector_space_algebra > ,
    runge_kutta_dopri5< state_type , value_type , state_type , value_type , vector_space_algebra > ,
    runge_kutta_fehlberg78< state_type , value_type , state_type , value_type , vector_space_algebra >
    > stepper_types;


template< class Stepper >
struct perform_runge_kutta_test {

    void operator()( void )
    {
        /* We have to specify the desired precision in advance! */
        mpf_set_default_prec( precision );

        mpf_t eps_ , unity;
        mpf_init( eps_ ); mpf_init( unity );
        mpf_set_d( unity , 1.0 );
        mpf_div_2exp( eps_ , unity , precision-1 ); // 2^(-precision+1) : smallest number that can be represented with used precision
        value_type eps( eps_ );

        Stepper stepper;
        state_type x;
        x = 0.0;

        stepper.do_step( constant_system , x , 0.0 , 0.1 );

        BOOST_MESSAGE( eps );
        BOOST_CHECK_MESSAGE( abs( x - value_type( 0.1 , precision ) ) < eps , x - 0.1 );
    }
};


BOOST_AUTO_TEST_CASE_TEMPLATE( runge_kutta_stepper_test , Stepper , stepper_types )
{
    perform_runge_kutta_test< Stepper > tester;
    tester();
}


/* check controlled steppers */
typedef mpl::vector<
    controlled_runge_kutta< runge_kutta_cash_karp54_classic< state_type , value_type , state_type , value_type , vector_space_algebra > > ,
    controlled_runge_kutta< runge_kutta_dopri5< state_type , value_type , state_type , value_type , vector_space_algebra > > , 
    controlled_runge_kutta< runge_kutta_fehlberg78< state_type , value_type , state_type , value_type , vector_space_algebra > > ,
    bulirsch_stoer< state_type , value_type , state_type , value_type , vector_space_algebra >
    > controlled_stepper_types;


template< class Stepper >
struct perform_controlled_test {

    void operator()( void )
    {
        mpf_set_default_prec( precision );

        mpf_t eps_ , unity;
        mpf_init( eps_ ); mpf_init( unity );
        mpf_set_d( unity , 1.0 );
        mpf_div_2exp( eps_ , unity , precision-1 ); // 2^(-precision+1) : smallest number that can be represented with used precision
        value_type eps( eps_ );

        Stepper stepper;
        state_type x;
        x = 0.0;

        value_type t(0.0);
        value_type dt(0.1);

        stepper.try_step( constant_system , x , t , dt );

        BOOST_MESSAGE( eps );
        BOOST_CHECK_MESSAGE( abs( x - value_type( 0.1 , precision ) ) < eps , x - 0.1 );
    }
};

BOOST_AUTO_TEST_CASE_TEMPLATE( controlled_stepper_test , Stepper , controlled_stepper_types )
{
    perform_controlled_test< Stepper > tester;
    tester();
}

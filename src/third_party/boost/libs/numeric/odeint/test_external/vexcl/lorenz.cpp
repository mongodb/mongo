/* Boost lorenz.cpp test file

 Copyright 2012 Karsten Ahnert
 Copyright 2012 Mario Mulansky

 This file tests the odeint library with the vexcl types

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
*/

#define BOOST_TEST_MODULE odeint_vexcl

#include <vector>
#include <iostream>

#include <vexcl/vexcl.hpp>

#include <boost/numeric/odeint/stepper/runge_kutta4.hpp>
#include <boost/numeric/odeint/stepper/runge_kutta_cash_karp54.hpp>
#include <boost/numeric/odeint/stepper/runge_kutta_dopri5.hpp>
#include <boost/numeric/odeint/stepper/controlled_runge_kutta.hpp>
#include <boost/numeric/odeint/stepper/dense_output_runge_kutta.hpp>
#include <boost/numeric/odeint/algebra/vector_space_algebra.hpp>
#include <boost/numeric/odeint/integrate/integrate_const.hpp>
#include <boost/numeric/odeint/external/vexcl/vexcl.hpp>

#include <boost/test/unit_test.hpp>

using namespace boost::unit_test;
namespace odeint = boost::numeric::odeint;


typedef vex::vector< double >    vector_type;
typedef vex::multivector< double, 3 > state_type;



const double sigma = 10.0;
const double b = 8.0 / 3.0;

struct sys_func
{
    const vector_type &R;

    sys_func( const vector_type &_R ) : R( _R ) { }

    void operator()( const state_type &x , state_type &dxdt , double t ) const
    {
        dxdt(0) = -sigma * ( x(0) - x(1) );
        dxdt(1) = R * x(0) - x(1) - x(0) * x(2);
        dxdt(2) = - b * x(2) + x(0) * x(1);
    }
};

const size_t n = 1024 ;
const double dt = 0.01;
const double t_max = 1.0;


BOOST_AUTO_TEST_CASE( integrate_const_rk4 )
{
    vex::Context ctx( vex::Filter::Type(CL_DEVICE_TYPE_GPU) );

    double Rmin = 0.1 , Rmax = 50.0 , dR = ( Rmax - Rmin ) / double( n - 1 );
    std::vector<double> x( n * 3 ) , r( n );
    for( size_t i=0 ; i<n ; ++i ) r[i] = Rmin + dR * double( i );

    state_type X( ctx.queue() , n );
    X(0) = 10.0;
    X(1) = 10.0;
    X(2) = 10.0;

    vector_type R( ctx.queue() , r );

    odeint::runge_kutta4<
        state_type , double , state_type , double ,
        odeint::vector_space_algebra , odeint::default_operations
        > stepper;

    odeint::integrate_const( stepper , sys_func( R ) , X , 0.0 , t_max , dt );

    std::vector< double > res( 3 * n );
    vex::copy( X(0) , res );
    std::cout << res[0] << std::endl;
}

BOOST_AUTO_TEST_CASE( integrate_const_controlled_rk54 )
{
    vex::Context ctx( vex::Filter::Type(CL_DEVICE_TYPE_GPU) );

    double Rmin = 0.1 , Rmax = 50.0 , dR = ( Rmax - Rmin ) / double( n - 1 );
    std::vector<double> x( n * 3 ) , r( n );
    for( size_t i=0 ; i<n ; ++i ) r[i] = Rmin + dR * double( i );

    state_type X( ctx.queue() , n );
    X(0) = 10.0;
    X(1) = 10.0;
    X(2) = 10.0;

    vector_type R( ctx.queue() , r );

    typedef odeint::runge_kutta_cash_karp54<
        state_type , double , state_type , double ,
        odeint::vector_space_algebra , odeint::default_operations
        > stepper_type;
    typedef odeint::controlled_runge_kutta< stepper_type > controlled_stepper_type;

    odeint::integrate_const( controlled_stepper_type() , sys_func( R ) , X , 0.0 , t_max , dt );

    std::vector< double > res( 3 * n );
    vex::copy( X(0) , res );
    std::cout << res[0] << std::endl;
}

BOOST_AUTO_TEST_CASE( integrate_const_dense_output_dopri5 )
{
    vex::Context ctx( vex::Filter::Type(CL_DEVICE_TYPE_GPU) );

    double Rmin = 0.1 , Rmax = 50.0 , dR = ( Rmax - Rmin ) / double( n - 1 );
    std::vector<double> x( n * 3 ) , r( n );
    for( size_t i=0 ; i<n ; ++i ) r[i] = Rmin + dR * double( i );

    state_type X( ctx.queue() , n );
    X(0) = 10.0;
    X(1) = 10.0;
    X(2) = 10.0;

    vector_type R( ctx.queue() , r );

    typedef odeint::runge_kutta_dopri5<
        state_type , double , state_type , double ,
        odeint::vector_space_algebra , odeint::default_operations
        > stepper_type;
    typedef odeint::controlled_runge_kutta< stepper_type > controlled_stepper_type;
    typedef odeint::dense_output_runge_kutta< controlled_stepper_type > dense_output_stepper_type;

    odeint::integrate_const( dense_output_stepper_type() , sys_func( R ) , X , 0.0 , t_max , dt );

    std::vector< double > res( 3 * n );
    vex::copy( X(0) , res );
    std::cout << res[0] << std::endl;
}




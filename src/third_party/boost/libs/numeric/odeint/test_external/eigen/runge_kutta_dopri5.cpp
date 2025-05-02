/*
 [auto_generated]
 libs/numeric/odeint/test_external/eigen/runge_kutta_dopri5.cpp

 [begin_description]
 tba.
 [end_description]

 Copyright 2013 Karsten Ahnert
 Copyright 2013 Mario Mulansky

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#include <boost/config.hpp>
#ifdef BOOST_MSVC
    #pragma warning(disable:4996)
#endif

#define BOOST_TEST_MODULE odeint_eigen_runge_kutta4

#include <boost/test/unit_test.hpp>

#include <boost/numeric/odeint/algebra/vector_space_algebra.hpp>
#include <boost/numeric/odeint/stepper/runge_kutta_dopri5.hpp>
#include <boost/numeric/odeint/stepper/runge_kutta4.hpp>
#include <boost/numeric/odeint/stepper/controlled_runge_kutta.hpp>
#include <boost/numeric/odeint/stepper/dense_output_runge_kutta.hpp>

// #include <boost/numeric/odeint/external/eigen/eigen_resize.hpp>
#include <boost/numeric/odeint/external/eigen/eigen_algebra.hpp>


using namespace boost::unit_test;
using namespace boost::numeric::odeint;

struct sys
{
    template< class State , class Deriv >
    void operator()( const State &x , Deriv &dxdt , double t ) const
    {
        dxdt[0] = 1.0;
    }
};

template< class State >
struct stepper
{
    typedef runge_kutta_dopri5< State , double , State , double , vector_space_algebra > type;
};

template< class State >
struct controlled_stepper
{
    typedef controlled_runge_kutta< typename stepper< State >::type > type;
};

template< class State >
struct dense_output_stepper
{
    typedef dense_output_runge_kutta< typename controlled_stepper< State >::type > type;
};




BOOST_AUTO_TEST_SUITE( eigen_runge_kutta_dopri5 )

BOOST_AUTO_TEST_CASE( compile_time_matrix )
{
    typedef Eigen::Matrix< double , 1 , 1 > state_type;
    state_type x;
    x[0] = 10.0;
    double t = 0.0 , dt = 0.1 ;
    
    // dense_output_stepper< state_type >::type s;
    // s.initialize( x , t , dt );

    // controlled_stepper< state_type >::type s;
    // s.try_step( sys() , x , t , dt );

    stepper< state_type >::type s;
    s.do_step( sys() , x , t , dt );

    // runge_kutta4< state_type , double , state_type , double , vector_space_algebra > rk4;
    // rk4.do_step( sys() , x , 0.0 , 0.1 );
    // BOOST_CHECK_CLOSE( x[0] , 10.1 , 1.0e-13 );
    
}

BOOST_AUTO_TEST_CASE( runtime_matrix )
{
    typedef Eigen::Matrix< double , Eigen::Dynamic , 1 > state_type;
    state_type x( 1 );
    x[0] = 10.0;
    
    // runge_kutta4< state_type , double , state_type , double , vector_space_algebra > rk4;
    // rk4.do_step( sys() , x , 0.0 , 0.1 );
    // BOOST_CHECK_CLOSE( x[0] , 10.1 , 1.0e-13 );
}






BOOST_AUTO_TEST_CASE( compile_time_array )
{
    typedef Eigen::Array< double , 1 , 1 > state_type;
    state_type x;
    x[0] = 10.0;
    // runge_kutta4< state_type , double , state_type , double , vector_space_algebra > rk4;
    // rk4.do_step( sys() , x , 0.0 , 0.1 );
    // BOOST_CHECK_CLOSE( x[0] , 10.1 , 1.0e-13 );
    
}

BOOST_AUTO_TEST_CASE( runtime_array )
{
    typedef Eigen::Array< double , Eigen::Dynamic , 1 > state_type;
    state_type x( 1 );
    x[0] = 10.0;
    // runge_kutta4< state_type , double , state_type , double , vector_space_algebra > rk4;
    // rk4.do_step( sys() , x , 0.0 , 0.1 );
    // BOOST_CHECK_CLOSE( x[0] , 10.1 , 1.0e-13 );
}



BOOST_AUTO_TEST_SUITE_END()

/*
 [auto_generated]
 libs/numeric/odeint/test_external/eigen/resize.cpp

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

#define BOOST_TEST_MODULE odeint_eigen_resize

#include <boost/test/unit_test.hpp>
#include <boost/numeric/odeint/external/eigen/eigen_resize.hpp>


using namespace boost::unit_test;
using namespace boost::numeric::odeint;


BOOST_AUTO_TEST_SUITE( eigen_resize )

BOOST_AUTO_TEST_CASE( test_compile_time_matrix )
{
    typedef Eigen::Matrix< double , 1 , 1 > matrix_type;
    matrix_type a , b;
    boost::numeric::odeint::resize( a , b );
    BOOST_CHECK( boost::numeric::odeint::same_size( a , b ) );
    BOOST_CHECK_EQUAL( a.rows() , 1 );
    BOOST_CHECK_EQUAL( a.cols() , 1 );
}

BOOST_AUTO_TEST_CASE( test_rumtime_matrix )
{
    typedef Eigen::Matrix< double , Eigen::Dynamic , Eigen::Dynamic > matrix_type;
    matrix_type a( 5 , 2 ) , b;

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 0 );
    BOOST_CHECK_EQUAL( b.cols() , 0 );
    BOOST_CHECK( !boost::numeric::odeint::same_size( a , b ) );

    boost::numeric::odeint::resize( b , a );

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 5 );
    BOOST_CHECK_EQUAL( b.cols() , 2 );

    BOOST_CHECK( boost::numeric::odeint::same_size( a , b ) );
}

BOOST_AUTO_TEST_CASE( test_rumtime_matrix2 )
{
    typedef Eigen::Matrix< double , Eigen::Dynamic , Eigen::Dynamic > matrix_type;
    matrix_type a( 5 , 2 ) , b( 2 , 3 );

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 2 );
    BOOST_CHECK_EQUAL( b.cols() , 3 );
    BOOST_CHECK( !boost::numeric::odeint::same_size( a , b ) );

    boost::numeric::odeint::resize( b , a );

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 5 );
    BOOST_CHECK_EQUAL( b.cols() , 2 );

    BOOST_CHECK( boost::numeric::odeint::same_size( a , b ) );
}






BOOST_AUTO_TEST_CASE( test_compile_time_array )
{
    typedef Eigen::Array< double , 1 , 1 > array_type;
    array_type a , b;
    boost::numeric::odeint::resize( a , b );
    BOOST_CHECK( boost::numeric::odeint::same_size( a , b ) );
    BOOST_CHECK_EQUAL( a.rows() , 1 );
    BOOST_CHECK_EQUAL( a.cols() , 1 );
}

BOOST_AUTO_TEST_CASE( test_rumtime_array )
{
    typedef Eigen::Array< double , Eigen::Dynamic , Eigen::Dynamic > array_type;
    array_type a( 5 , 2 ) , b;

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 0 );
    BOOST_CHECK_EQUAL( b.cols() , 0 );
    BOOST_CHECK( !boost::numeric::odeint::same_size( a , b ) );

    boost::numeric::odeint::resize( b , a );

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 5 );
    BOOST_CHECK_EQUAL( b.cols() , 2 );

    BOOST_CHECK( boost::numeric::odeint::same_size( a , b ) );
}

BOOST_AUTO_TEST_CASE( test_rumtime_array2 )
{
    typedef Eigen::Array< double , Eigen::Dynamic , Eigen::Dynamic > array_type;
    array_type a( 5 , 2 ) , b( 2 , 3 );

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 2 );
    BOOST_CHECK_EQUAL( b.cols() , 3 );
    BOOST_CHECK( !boost::numeric::odeint::same_size( a , b ) );

    boost::numeric::odeint::resize( b , a );

    BOOST_CHECK_EQUAL( a.rows() , 5 );
    BOOST_CHECK_EQUAL( a.cols() , 2 );
    BOOST_CHECK_EQUAL( b.rows() , 5 );
    BOOST_CHECK_EQUAL( b.cols() , 2 );

    BOOST_CHECK( boost::numeric::odeint::same_size( a , b ) );
}



BOOST_AUTO_TEST_SUITE_END()

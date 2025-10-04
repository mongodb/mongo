/* Boost mtl4_resize.cpp test file

 Copyright 2012 Karsten Ahnert
 Copyright 2012 Mario Mulansky

 This file tests the odeint library with the mtl4 routines.

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
*/

#define BOOST_TEST_MODULE test_mtl4_resize

#include <boost/test/unit_test.hpp>

#include <boost/numeric/odeint/external/mtl4/mtl4_resize.hpp>
#include <boost/numeric/mtl/vector/dense_vector.hpp>

namespace odeint = boost::numeric::odeint;


BOOST_AUTO_TEST_CASE( test_dense_vector_resizeability )
{
    BOOST_CHECK( odeint::is_resizeable< mtl::dense_vector< double > >::value );
}

BOOST_AUTO_TEST_CASE( test_dense2D_resizeability )
{
    BOOST_CHECK( odeint::is_resizeable< mtl::dense2D< double > >::value );
}

BOOST_AUTO_TEST_CASE( test_compressed2D_resizeability )
{
    BOOST_CHECK( odeint::is_resizeable< mtl::compressed2D< double > >::value );
}



BOOST_AUTO_TEST_CASE( test_dense_vector_vector_same_size )
{
    mtl::dense_vector< double > v1( 10 ) , v2( 10 );
    BOOST_CHECK( odeint::same_size( v2 , v1 ) );
}

BOOST_AUTO_TEST_CASE( test_dense_vector_dense2D_same_size )
{
    mtl::dense_vector< double > v( 10 );
    mtl::dense2D< double > m( 10 , 10 );
    BOOST_CHECK( odeint::same_size( m , v ) );
}

BOOST_AUTO_TEST_CASE( test_dense_vector_compressed2D_same_size )
{
    mtl::dense_vector< double > v( 10 );
    mtl::compressed2D< double > m( 10 , 10 );
    BOOST_CHECK( odeint::same_size( m , v ) );
}




BOOST_AUTO_TEST_CASE( test_dense_vector_vector_resize )
{
    mtl::dense_vector< double > v1( 10 );
    mtl::dense_vector< double > v2;
    odeint::resize( v2 , v1 );
    BOOST_CHECK( mtl::size( v2 ) == mtl::size( v1 ) );
}

BOOST_AUTO_TEST_CASE( test_dense_vector_dense2D_resize )
{
    mtl::dense_vector< double > v( 10 );
    mtl::dense2D< double > m;

    odeint::resize( m , v );
    BOOST_CHECK( m.num_cols() == mtl::size( v ) );
    BOOST_CHECK( m.num_rows() == mtl::size( v ) );
}

BOOST_AUTO_TEST_CASE( test_dense_vector_compressed2D_resize )
{
    mtl::dense_vector< double > v( 10 );
    mtl::compressed2D< double > m;

    odeint::resize( m , v );
    BOOST_CHECK( m.num_cols() == mtl::size( v ) );
    BOOST_CHECK( m.num_rows() == mtl::size( v ) );
}

/*
 [auto_generated]
 libs/numeric/odeint/test_external/eigen/is_resizeable.cpp

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

#define BOOST_TEST_MODULE odeint_eigen_is_resizeable

#include <boost/test/unit_test.hpp>

#include <boost/numeric/odeint/external/eigen/eigen_resize.hpp>

using namespace boost::unit_test;
using namespace boost::numeric::odeint;


BOOST_AUTO_TEST_SUITE( is_resizeable )

BOOST_AUTO_TEST_CASE( test_compile_time_matrix )
{
    typedef Eigen::Matrix< double , 1 , 1 > matrix_type;
    static_assert(( boost::numeric::odeint::is_resizeable< matrix_type >::value ), "Matrix is not resizeable");
}

BOOST_AUTO_TEST_CASE( test_compile_time_array )
{
    typedef Eigen::Array< double , 1 , 1 > array_type;
    static_assert(( boost::numeric::odeint::is_resizeable< array_type >::value ), "Array is not resizeable");
}


BOOST_AUTO_TEST_SUITE_END()

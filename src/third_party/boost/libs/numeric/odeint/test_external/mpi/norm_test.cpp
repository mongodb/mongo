/*
 Copyright 2013 Karsten Ahnert
 Copyright 2013 Mario Mulansky
 Copyright 2013 Pascal Germroth

 Distributed under the Boost Software License, Version 1.0.
 (See accompanying file LICENSE_1_0.txt or
 copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#include <iostream>
#include <sstream>
#include <cstdlib>

#define BOOST_TEST_MODULE odeint_mpi
#include <boost/test/unit_test.hpp>

#include <boost/numeric/odeint/external/mpi/mpi.hpp>

using namespace boost::numeric::odeint;

boost::mpi::environment env;

BOOST_AUTO_TEST_SUITE( norm_test_suite )

BOOST_AUTO_TEST_CASE( norm_test )
{
    boost::mpi::communicator world;

    int ref_value = 0;
    std::vector<int> in_data;
    mpi_state< std::vector<int> > state(world);

    // generate data and reference value on master
    if(world.rank() == 0) {
        for(size_t i = 0 ; i < 400 ; i++)
            in_data.push_back( rand() % 10000 );
        ref_value = *std::max_element(in_data.begin(), in_data.end());
    }
    boost::mpi::broadcast(world, ref_value, 0);

    // copy to nodes
    split( in_data, state );

    int value = mpi_nested_algebra< range_algebra >::norm_inf( state );

    {
        std::ostringstream ss;
        ss << "state[" << world.rank() << "]"
           << " local:" << range_algebra::norm_inf( state() )
           << " global:" << value
           << " ref:" << ref_value << "\n";
        std::clog << ss.str() << std::flush;
    }

    BOOST_REQUIRE_EQUAL( value, ref_value );
}


BOOST_AUTO_TEST_SUITE_END()



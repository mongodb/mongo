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

#define BOOST_TEST_MODULE odeint_mpi
#include <boost/test/unit_test.hpp>

#include <boost/numeric/odeint/external/mpi/mpi.hpp>

using namespace boost::numeric::odeint;

boost::mpi::environment env;

BOOST_AUTO_TEST_SUITE( split_test_suite )

BOOST_AUTO_TEST_CASE( split_test )
{
    boost::mpi::communicator world;

    const size_t total_size = 31;

    std::vector<size_t> in_data, out_data;
    mpi_state< std::vector<size_t> > state(world);

    // generate data on master
    if(world.rank() == 0)
        for(size_t i = 0 ; i < total_size ; i++) in_data.push_back(i);

    // copy to nodes
    split( in_data, state );

    BOOST_REQUIRE((state().size() == total_size / world.size())
               || (state().size() == total_size / world.size() + 1));

    {
        std::ostringstream ss;
        ss << "state[" << world.rank() << "].data = {";
        std::copy(state().begin(), state().end(), std::ostream_iterator<size_t>(ss, ", "));
        ss << "}\n";
        std::clog << ss.str() << std::flush;
    }

    // copy back to master
    if(world.rank() == 0) out_data.resize(in_data.size());
    unsplit( state, out_data );

    if(world.rank() == 0)
        BOOST_REQUIRE_EQUAL_COLLECTIONS(in_data.begin(), in_data.end(), out_data.begin(), out_data.end());
}


BOOST_AUTO_TEST_SUITE_END()

//  Copyright (c) 2001-2010 Hartmut Kaiser
// 
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <climits>
#include <cstdlib>

#include <iostream> 
#include <sstream>
#include <boost/format.hpp>

#include "../high_resolution_timer.hpp"

//  This value specifies, how to unroll the integer string generation loop in 
//  Karma.
//      Set this to some integer in between 0 (no unrolling) and max expected 
//      integer string len (complete unrolling). If not specified, this value
//      defaults to 6.
#define BOOST_KARMA_NUMERICS_LOOP_UNROLL 6

#include <boost/spirit/include/karma.hpp>

using namespace std;
using namespace boost::spirit;

#define MAX_ITERATION 10000000

///////////////////////////////////////////////////////////////////////////////
struct random_fill
{
    int operator()() const
    {
        int scale = std::rand() / 100 + 1;
        return (std::rand() * std::rand()) / scale;
    }
};

///////////////////////////////////////////////////////////////////////////////
int main()
{
    namespace karma = boost::spirit::karma;

    cout << "Converting " << MAX_ITERATION 
         << " randomly generated int values to strings." << flush << endl;

    std::srand(0);
    std::vector<int> v (MAX_ITERATION);
    std::generate(v.begin(), v.end(), random_fill()); // randomly fill the vector

    // test the C libraries ltoa function (the most low level function for
    // string conversion available)
    {
        //[karma_int_performance_ltoa
        char buffer[65]; // we don't expect more than 64 bytes to be generated here
        //<-
        std::string str;
        util::high_resolution_timer t;
        //->
        for (int i = 0; i < MAX_ITERATION; ++i)
        {
            ltoa(v[i], buffer, 10);
            //<-
            str = buffer;      // compensate for string ops in other benchmarks
            //->
        }
        //]

        cout << "ltoa:\t\t" << t.elapsed() << " [s]" << flush << endl;
    }

    // test the iostreams library
    {
        //[karma_int_performance_iostreams
        std::stringstream str;
        //<-
        util::high_resolution_timer t;
        //->
        for (int i = 0; i < MAX_ITERATION; ++i)
        {
            str.str("");
            str << v[i];
        }
        //]

        cout << "iostreams:\t" << t.elapsed() << " [s]" << flush << endl;
    }

    // test the Boost.Format library
    {
        //[karma_int_performance_format
        std::string str;
        boost::format int_format("%d");
        //<-
        util::high_resolution_timer t;
        //->
        for (int i = 0; i < MAX_ITERATION; ++i)
        {
            str = boost::str(int_format % v[i]);
        }
        //]

        cout << "Boost.Format:\t" << t.elapsed() << " [s]" << flush << endl;
    }

    // test the Karma int_ generation routines 
    {
        std::string str;
        util::high_resolution_timer t;

        //[karma_int_performance_plain
        char buffer[65]; // we don't expect more than 64 bytes to be generated here
        for (int i = 0; i < MAX_ITERATION; ++i)
        {
            char *ptr = buffer;
            karma::generate(ptr, int_, v[i]);
            *ptr = '\0';
            //<-
            str = buffer;      // compensate for string ops in other benchmarks
            //->
        }
        //]

        cout << "int_:\t\t" << t.elapsed() << " [s]" << flush << endl;
    }

    return 0;
}


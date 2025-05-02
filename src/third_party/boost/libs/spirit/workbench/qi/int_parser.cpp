/*=============================================================================
    Copyright (c) 2001-2010 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#include "../measure.hpp"
#include <string>
#include <vector>
#include <cstdlib>
#include <boost/spirit/include/qi.hpp>

namespace
{
    ///////////////////////////////////////////////////////////////////////////
    // Generate a random number string with N digits
    std::string
    gen_int(int digits)
    {
        std::string result;
        if (rand()%2)                       // Prepend a '-'
            result += '-';
        result += '1' + (rand()%9);         // The first digit cannot be '0'
        
        for (int i = 1; i < digits; ++i)    // Generate the remaining digits
            result += '0' + (rand()%10);
        return result;
    }
    
    std::string numbers[9];
    char const* first[9];
    char const* last[9];

    ///////////////////////////////////////////////////////////////////////////
    struct atoi_test : test::base
    {
        void benchmark()
        {
            for (int i = 0; i < 9; ++i) 
                this->val += atoi(first[i]);
        }
    };
    
    ///////////////////////////////////////////////////////////////////////////
    struct strtol_test : test::base
    {        
        void benchmark()
        {
            for (int i = 0; i < 9; ++i) 
                this->val += strtol(first[i], const_cast<char**>(&last[i]), 10);
        }
    };
    
    ///////////////////////////////////////////////////////////////////////////
    struct spirit_int_test : test::base
    {
        static int parse(char const* first, char const* last)
        {
            int n;
            namespace qi = boost::spirit::qi;
            using qi::int_;
            qi::parse(first, last, int_, n);
            return n;
        }

        void benchmark()
        {           
            for (int i = 0; i < 9; ++i) 
                this->val += parse(first[i], last[i]);
        }
    };
}

int main()
{
    // Seed the random generator
    srand(time(0));
    
    // Generate random integers with 1 .. 9 digits
    // We test only 9 digits to avoid overflow
    std::cout << "///////////////////////////////////////////////////////////////////////////" << std::endl;
    std::cout << "Numbers to test:" << std::endl;
    for (int i = 0; i < 9; ++i)
    {
        numbers[i] = gen_int(i+1);
        first[i] = numbers[i].c_str();
        last[i] = first[i];
        while (*last[i])
            last[i]++;
        std::cout << i+1 << " digit number:" << numbers[i] << std::endl;
    }
    std::cout << "///////////////////////////////////////////////////////////////////////////" << std::endl;

    BOOST_SPIRIT_TEST_BENCHMARK(
        10000000,     // This is the maximum repetitions to execute
        (atoi_test)
        (strtol_test)
        (spirit_int_test)
    )
    
    // This is ultimately responsible for preventing all the test code
    // from being optimized away.  Change this to return 0 and you
    // unplug the whole test's life support system.
    return test::live_code != 0;
}


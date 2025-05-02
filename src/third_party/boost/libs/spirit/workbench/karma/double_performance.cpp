//   Copyright (c) 2002-2010 Hartmut Kaiser
//   Copyright (c) 2002-2010 Joel de Guzman
// 
//   Distributed under the Boost Software License, Version 1.0. (See accompanying
//   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/spirit/include/karma.hpp>
#include <boost/format.hpp>

#include <iostream>

#include "../high_resolution_timer.hpp"

#define NUMITERATIONS 1000000

///////////////////////////////////////////////////////////////////////////////
// We generate plain floating point numbers in this test
//[karma_double_performance_definitions
using boost::spirit::karma::double_;
//]

void format_performance_karma()
{
    using boost::spirit::karma::generate;

    //[karma_double_performance_plain
    char buffer[256];
    //<-
    util::high_resolution_timer t;
    //->
    for (int i = 0; i < NUMITERATIONS; ++i) {
        char *p = buffer;
        generate(p, double_, 12345.12345);
        *p = '\0';
    }
    //]

    std::cout << "karma:\t\t" << t.elapsed() << std::endl;
//     std::cout << buffer << std::endl;
}

void format_performance_rule()
{
    using boost::spirit::karma::generate;

    boost::spirit::karma::rule<char*, double()> r;

    //[karma_double_performance_rule
    char buffer[256];
    r %= double_;
    //<-
    util::high_resolution_timer t;
    //->
    for (int i = 0; i < NUMITERATIONS; ++i) {
        char *p = buffer;
        generate(p, r, 12345.12345);
        *p = '\0';
    }
    //]

    std::cout << "karma (rule):\t" << t.elapsed() << std::endl;
//     std::cout << buffer << std::endl;
}

void format_performance_direct()
{
    using boost::spirit::karma::generate;
    using boost::spirit::karma::real_inserter;

    //[karma_double_performance_direct
    typedef real_inserter<double> inserter;
    char buffer[256];
    //<-
    util::high_resolution_timer t;
    //->
    for (int i = 0; i < NUMITERATIONS; ++i) {
        char *p = buffer;
        inserter::call(p, 12345.12345);
        *p = '\0';
    }
    //]

    std::cout << "karma (direct):\t" << t.elapsed() << std::endl;
//     std::cout << buffer << std::endl;
}

void format_performance_string()
{
    using boost::spirit::karma::generate;

    //[karma_double_performance_string
    std::string generated;
    std::back_insert_iterator<std::string> sink(generated);
    //<-
    util::high_resolution_timer t;
    //->
    for (int i = 0; i < NUMITERATIONS; ++i) {
        generated.clear();
        generate(sink, double_, 12345.12345);
    }
    //]

    std::cout << "karma (string):\t" << t.elapsed() << std::endl;
//     std::cout << generated << std::endl;
}

// Boost.Format  
void format_performance_boost_format()
{
    //[karma_double_performance_format
    std::string generated;
    boost::format double_format("%f");
    //<-
    util::high_resolution_timer t;
    //->
    for (int i = 0; i < NUMITERATIONS; ++i) 
        generated = boost::str(double_format % 12345.12345);
    //]

    std::cout << "format:\t\t" << t.elapsed() << std::endl;
//     std::cout << strm.str() << std::endl;
}

void format_performance_sprintf()
{
    util::high_resolution_timer t;

    //[karma_double_performance_printf
    char buffer[256];
    for (int i = 0; i < NUMITERATIONS; ++i) {
        sprintf(buffer, "%f", 12345.12345);
    }
    //]

    std::cout << "sprintf:\t" << t.elapsed() << std::endl;
//     std::cout << buffer << std::endl;
}

void format_performance_iostreams()
{
    //[karma_double_performance_iostreams
    std::stringstream strm;
    //<-
    util::high_resolution_timer t;
    //->
    for (int i = 0; i < NUMITERATIONS; ++i) {
        strm.str("");
        strm << 12345.12345;
    }
    //]

    std::cout << "iostreams:\t" << t.elapsed() << std::endl;
//     std::cout << strm.str() << std::endl;
}

///////////////////////////////////////////////////////////////////////////////
int main()
{
    format_performance_sprintf();
    format_performance_iostreams();
    format_performance_boost_format();
    format_performance_karma();
    format_performance_string();
    format_performance_rule();
    format_performance_direct();
    return 0;
}


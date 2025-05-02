//   Copyright (c) 2002-2010 Joel de Guzman
// 
//   Distributed under the Boost Software License, Version 1.0. (See accompanying
//   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <boost/spirit/include/qi.hpp>
#include <boost/phoenix/core.hpp>
#include <boost/phoenix/operator.hpp>
#include <boost/phoenix/object.hpp>
#include <boost/phoenix/stl.hpp>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/fusion/include/io.hpp>

#include <iostream>
#include <string>
#include <complex>

using boost::spirit::qi::grammar;
using boost::spirit::qi::rule;
using boost::spirit::qi::char_;
using boost::spirit::qi::parse;
using boost::spirit::qi::_val;
using boost::spirit::qi::_1;
using boost::phoenix::push_back;

#define ATTR_PROPAGATE

struct test_attr
{
    test_attr()
    {
        std::cout << "default construct" << std::endl;
    }

    test_attr(char)
    {
        std::cout << "construct from char" << std::endl;
    }

    test_attr(test_attr const&)
    {
        std::cout << "copy construct" << std::endl;
    }

    test_attr& operator=(test_attr const&)
    {
        std::cout << "assign" << std::endl;
        return *this;
    }
};

template <typename Iterator>
struct test_parser : grammar<Iterator, std::vector<test_attr>() >
{
    test_parser() : test_parser::base_type(start)
    {
#ifdef ATTR_PROPAGATE
        start = char_ >> *(',' >> char_);
#else
        start = char_[push_back(_val, _1)] >> *(',' >> char_[push_back(_val, _1)]);
#endif
    }

    rule<Iterator, std::vector<test_attr>()> start;
};

int main()
{
    typedef std::string::const_iterator iterator_type;
    typedef test_parser<iterator_type> test_parser;

    test_parser g;
    std::string str = "a,b,c,d,e";

    std::vector<test_attr> result;
    result.reserve(20);
    std::string::const_iterator iter = str.begin();
    std::string::const_iterator end = str.end();
    bool r = parse(iter, end, g, result);

    if (r && iter == end)
    {
        std::cout << "-------------------------\n";
        std::cout << "Parsing succeeded\n";
        std::cout << "\n-------------------------\n";
    }
    else
    {
        std::cout << "-------------------------\n";
        std::cout << "Parsing failed\n";
        std::cout << "-------------------------\n";
    }

    std::cout << "Bye... :-) \n\n";
    return 0;
}


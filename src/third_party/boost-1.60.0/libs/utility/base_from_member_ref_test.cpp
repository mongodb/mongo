//
// Test that a base_from_member<T&> can be properly constructed
//
// Copyright 2014 Agustin Berge
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt
//

#include <boost/utility/base_from_member.hpp>

#include <boost/detail/lightweight_test.hpp>

struct foo : boost::base_from_member<int&>
{
    explicit foo(int& ref) : boost::base_from_member<int&>(ref)
    {
        BOOST_TEST(&member == &ref);
    }
};

int main()
{
    int i = 0;
    foo f(i);

    return boost::report_errors();
}

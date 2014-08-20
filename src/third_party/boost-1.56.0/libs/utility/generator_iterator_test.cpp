//
// Copyright 2014 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt
//

#include <boost/generator_iterator.hpp>
#include <boost/detail/lightweight_test.hpp>
#include <algorithm>

class X
{
private:

    int v;

public:

    typedef int result_type;

    X(): v( 0 )
    {
    }

    int operator()()
    {
        return ++v;
    }
};

template<class InputIterator, class Size, class OutputIterator> OutputIterator copy_n( InputIterator first, Size n, OutputIterator result )
{
    while( n-- > 0 )
    {
        *result++ = *first++;
    }

    return result;
}

void copy_test()
{
    X x;
    boost::generator_iterator<X> in( &x );

    int const N = 4;
    int v[ N ] = { 0 };

    ::copy_n( in, 4, v );

    BOOST_TEST_EQ( v[0], 1 );
    BOOST_TEST_EQ( v[1], 2 );
    BOOST_TEST_EQ( v[2], 3 );
    BOOST_TEST_EQ( v[3], 4 );
}

int main()
{
    copy_test();
    return boost::report_errors();
}

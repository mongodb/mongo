//  Boost next_prior.hpp header file  ---------------------------------------//

//  (C) Copyright Dave Abrahams and Daniel Walker 1999-2003.
//  Copyright (c) Andrey Semashev 2017
//
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/utility for documentation.

//  Revision History
//  13 Dec 2003  Added next(x, n) and prior(x, n) (Daniel Walker)

#ifndef BOOST_NEXT_PRIOR_HPP_INCLUDED
#define BOOST_NEXT_PRIOR_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/type_traits/has_plus.hpp>
#include <boost/type_traits/has_plus_assign.hpp>
#include <boost/type_traits/has_minus.hpp>
#include <boost/type_traits/has_minus_assign.hpp>
#include <boost/iterator/is_iterator.hpp>
#include <boost/iterator/advance.hpp>
#include <boost/iterator/reverse_iterator.hpp>

namespace boost {

//  Helper functions for classes like bidirectional iterators not supporting
//  operator+ and operator-
//
//  Usage:
//    const std::list<T>::iterator p = get_some_iterator();
//    const std::list<T>::iterator prev = boost::prior(p);
//    const std::list<T>::iterator next = boost::next(prev, 2);

//  Contributed by Dave Abrahams

namespace next_prior_detail {

template< typename T, typename Distance, bool HasPlus = has_plus< T, Distance >::value >
struct next_plus_impl;

template< typename T, typename Distance >
struct next_plus_impl< T, Distance, true >
{
    static T call(T x, Distance n)
    {
        return x + n;
    }
};

template< typename T, typename Distance, bool HasPlusAssign = has_plus_assign< T, Distance >::value >
struct next_plus_assign_impl :
    public next_plus_impl< T, Distance >
{
};

template< typename T, typename Distance >
struct next_plus_assign_impl< T, Distance, true >
{
    static T call(T x, Distance n)
    {
        x += n;
        return x;
    }
};

template< typename T, typename Distance, bool IsIterator = boost::iterators::is_iterator< T >::value >
struct next_advance_impl :
    public next_plus_assign_impl< T, Distance >
{
};

template< typename T, typename Distance >
struct next_advance_impl< T, Distance, true >
{
    static T call(T x, Distance n)
    {
        boost::iterators::advance(x, n);
        return x;
    }
};


template< typename T, typename Distance, bool HasMinus = has_minus< T, Distance >::value >
struct prior_minus_impl;

template< typename T, typename Distance >
struct prior_minus_impl< T, Distance, true >
{
    static T call(T x, Distance n)
    {
        return x - n;
    }
};

template< typename T, typename Distance, bool HasMinusAssign = has_minus_assign< T, Distance >::value >
struct prior_minus_assign_impl :
    public prior_minus_impl< T, Distance >
{
};

template< typename T, typename Distance >
struct prior_minus_assign_impl< T, Distance, true >
{
    static T call(T x, Distance n)
    {
        x -= n;
        return x;
    }
};

template< typename T, typename Distance, bool IsIterator = boost::iterators::is_iterator< T >::value >
struct prior_advance_impl :
    public prior_minus_assign_impl< T, Distance >
{
};

template< typename T, typename Distance >
struct prior_advance_impl< T, Distance, true >
{
    static T call(T x, Distance n)
    {
        // Avoid negating n to sidestep possible integer overflow
        boost::iterators::reverse_iterator< T > rx(x);
        boost::iterators::advance(rx, n);
        return rx.base();
    }
};

} // namespace next_prior_detail

template <class T>
inline T next(T x) { return ++x; }

template <class T, class Distance>
inline T next(T x, Distance n)
{
    return next_prior_detail::next_advance_impl< T, Distance >::call(x, n);
}

template <class T>
inline T prior(T x) { return --x; }

template <class T, class Distance>
inline T prior(T x, Distance n)
{
    return next_prior_detail::prior_advance_impl< T, Distance >::call(x, n);
}

} // namespace boost

#endif  // BOOST_NEXT_PRIOR_HPP_INCLUDED

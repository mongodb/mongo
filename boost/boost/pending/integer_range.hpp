// (C) Copyright David Abrahams and Jeremy Siek 2000-2001.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Revision History:
// 04 Jan 2001  Factored counting_iterator stuff into
//              boost/counting_iterator.hpp (David Abrahams)

#ifndef BOOST_INTEGER_RANGE_HPP_
#define BOOST_INTEGER_RANGE_HPP_

#include <boost/config.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <algorithm>

namespace boost {

//=============================================================================
// Counting Iterator and Integer Range Class

template <class IntegerType>
struct integer_range {
    typedef counting_iterator<IntegerType> iterator;

    typedef iterator const_iterator;
    typedef IntegerType value_type;
    typedef std::ptrdiff_t difference_type;
    typedef IntegerType reference;
    typedef IntegerType const_reference;
    typedef const IntegerType* pointer;
    typedef const IntegerType* const_pointer;
    typedef IntegerType size_type;

    integer_range(IntegerType start, IntegerType finish)
        : m_start(start), m_finish(finish) { }

    iterator begin() const { return iterator(m_start); }
    iterator end() const { return iterator(m_finish); }
    size_type size() const { return m_finish - m_start; }
    bool empty() const { return m_finish == m_start; }
    void swap(integer_range& x) {
        std::swap(m_start, x.m_start);
        std::swap(m_finish, x.m_finish);
    }
protected:
    IntegerType m_start, m_finish;
};

template <class IntegerType>
inline integer_range<IntegerType>
make_integer_range(IntegerType first, IntegerType last)
{
  return integer_range<IntegerType>(first, last);
}

} // namespace boost

#endif // BOOST_INTEGER_RANGE_HPP_

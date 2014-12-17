/* 
   Copyright (c) Marshall Clow 2008-2012.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*/

/// \file  copy_if.hpp
/// \brief Copy a subset of a sequence to a new sequence
/// \author Marshall Clow

#ifndef BOOST_ALGORITHM_COPY_IF_HPP
#define BOOST_ALGORITHM_COPY_IF_HPP

#include <algorithm>    // for std::copy_if, if available
#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>

namespace boost { namespace algorithm {

#if __cplusplus >= 201103L
//  Use the C++11 versions of copy_if if it is available
using std::copy_if;         // Section 25.3.1
#else
/// \fn copy_if ( InputIterator first, InputIterator last, OutputIterator result, Predicate p )
/// \brief Copies all the elements from the input range that satisfy the
/// predicate to the output range.
/// \return The updated output iterator
/// 
/// \param first    The start of the input sequence
/// \param last     One past the end of the input sequence
/// \param result   An output iterator to write the results into
/// \param p        A predicate for testing the elements of the range
/// \note           This function is part of the C++2011 standard library.
///  We will use the standard one if it is available, 
///  otherwise we have our own implementation.
template<typename InputIterator, typename OutputIterator, typename Predicate> 
OutputIterator copy_if ( InputIterator first, InputIterator last, OutputIterator result, Predicate p )
{
    for ( ; first != last; ++first )
        if (p(*first))
            *result++ = *first;
    return result;
}
#endif

/// \fn copy_if ( const Range &r, OutputIterator result, Predicate p )
/// \brief Copies all the elements from the input range that satisfy the
/// predicate to the output range.
/// \return The updated output iterator
/// 
/// \param r        The input range
/// \param result   An output iterator to write the results into
/// \param p        A predicate for testing the elements of the range
///
template<typename Range, typename OutputIterator, typename Predicate>
OutputIterator copy_if ( const Range &r, OutputIterator result, Predicate p )
{
    return boost::algorithm::copy_if (boost::begin (r), boost::end(r), result, p);
}


/// \fn copy_while ( InputIterator first, InputIterator last, OutputIterator result, Predicate p )
/// \brief Copies all the elements at the start of the input range that
///     satisfy the predicate to the output range.
/// \return The updated input and output iterators
/// 
/// \param first    The start of the input sequence
/// \param last     One past the end of the input sequence
/// \param result   An output iterator to write the results into
/// \param p        A predicate for testing the elements of the range
///
template<typename InputIterator, typename OutputIterator, typename Predicate> 
std::pair<InputIterator, OutputIterator>
copy_while ( InputIterator first, InputIterator last, OutputIterator result, Predicate p )
{
    for ( ; first != last && p(*first); ++first )
        *result++ = *first;
    return std::make_pair(first, result);
}

/// \fn copy_while ( const Range &r, OutputIterator result, Predicate p )
/// \brief Copies all the elements at the start of the input range that
///     satisfy the predicate to the output range.
/// \return The updated input and output iterators
/// 
/// \param r        The input range
/// \param result   An output iterator to write the results into
/// \param p        A predicate for testing the elements of the range
///
template<typename Range, typename OutputIterator, typename Predicate>
std::pair<typename boost::range_iterator<const Range>::type, OutputIterator> 
copy_while ( const Range &r, OutputIterator result, Predicate p )
{
    return boost::algorithm::copy_while (boost::begin (r), boost::end(r), result, p);
}


/// \fn copy_until ( InputIterator first, InputIterator last, OutputIterator result, Predicate p )
/// \brief Copies all the elements at the start of the input range that do not
///     satisfy the predicate to the output range.
/// \return The updated output iterator
/// 
/// \param first    The start of the input sequence
/// \param last     One past the end of the input sequence
/// \param result   An output iterator to write the results into
/// \param p        A predicate for testing the elements of the range
///
template<typename InputIterator, typename OutputIterator, typename Predicate> 
std::pair<InputIterator, OutputIterator>
copy_until ( InputIterator first, InputIterator last, OutputIterator result, Predicate p )
{
    for ( ; first != last && !p(*first); ++first )
        *result++ = *first;
    return std::make_pair(first, result);
}

/// \fn copy_until ( const Range &r, OutputIterator result, Predicate p )
/// \brief Copies all the elements at the start of the input range that do not
///     satisfy the predicate to the output range.
/// \return The updated output iterator
/// 
/// \param r        The input range
/// \param result   An output iterator to write the results into
/// \param p        A predicate for testing the elements of the range
///
template<typename Range, typename OutputIterator, typename Predicate>
std::pair<typename boost::range_iterator<const Range>::type, OutputIterator> 
copy_until ( const Range &r, OutputIterator result, Predicate p )
{
    return boost::algorithm::copy_until (boost::begin (r), boost::end(r), result, p);
}

}} // namespace boost and algorithm

#endif  // BOOST_ALGORITHM_COPY_IF_HPP

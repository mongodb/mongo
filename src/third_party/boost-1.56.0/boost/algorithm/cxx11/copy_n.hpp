/* 
   Copyright (c) Marshall Clow 2011-2012.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*/

/// \file  copy_n.hpp
/// \brief Copy n items from one sequence to another
/// \author Marshall Clow

#ifndef BOOST_ALGORITHM_COPY_N_HPP
#define BOOST_ALGORITHM_COPY_N_HPP

#include <algorithm>    // for std::copy_n, if available

namespace boost { namespace algorithm {

#if __cplusplus >= 201103L
//  Use the C++11 versions of copy_n if it is available
using std::copy_n;          // Section 25.3.1
#else
/// \fn copy_n ( InputIterator first, Size n, OutputIterator result )
/// \brief Copies exactly n (n > 0) elements from the range starting at first to
///     the range starting at result.
/// \return         The updated output iterator
/// 
/// \param first    The start of the input sequence
/// \param n        The number of elements to copy
/// \param result   An output iterator to write the results into
/// \note           This function is part of the C++2011 standard library.
///  We will use the standard one if it is available, 
///     otherwise we have our own implementation.
template <typename InputIterator, typename Size, typename OutputIterator>
OutputIterator copy_n ( InputIterator first, Size n, OutputIterator result )
{
    for ( ; n > 0; --n, ++first, ++result )
        *result = *first;
    return result;
}
#endif
}} // namespace boost and algorithm

#endif  // BOOST_ALGORITHM_COPY_IF_HPP

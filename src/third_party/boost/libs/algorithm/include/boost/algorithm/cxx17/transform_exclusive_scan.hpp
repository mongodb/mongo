/*
   Copyright (c) Marshall Clow 2017.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
*/

/// \file  transform_exclusive_scan.hpp
/// \brief ????
/// \author Marshall Clow

#ifndef BOOST_ALGORITHM_TRANSFORM_EXCLUSIVE_SCAN_HPP
#define BOOST_ALGORITHM_TRANSFORM_EXCLUSIVE_SCAN_HPP

#include <functional>     // for std::plus
#include <iterator>       // for std::iterator_traits

#include <boost/config.hpp>
#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>
#include <boost/range/value_type.hpp>

namespace boost { namespace algorithm {

/// \fn transform_exclusive_scan ( InputIterator first, InputIterator last, OutputIterator result, BinaryOperation bOp, UnaryOperation uOp, T init )
/// \brief Transforms elements from the input range with uOp and then combines
/// those transformed elements with bOp such that the n-1th element and the nth
/// element are combined. Exclusivity means that the nth element is not
/// included in the nth combination.
/// \return The updated output iterator
///
/// \param first  The start of the input sequence
/// \param last   The end of the input sequence
/// \param result The output iterator to write the results into
/// \param bOp    The operation for combining transformed input elements
/// \param uOp    The operation for transforming input elements
/// \param init   The initial value
///
/// \note This function is part of the C++17 standard library
template<class InputIterator, class OutputIterator, class T,
         class BinaryOperation, class UnaryOperation>
OutputIterator transform_exclusive_scan(InputIterator first, InputIterator last,
                                        OutputIterator result, T init,
                                        BinaryOperation bOp, UnaryOperation uOp)
{
    if (first != last)
    {
        T saved = init;
        do
        {
            init = bOp(init, uOp(*first));
            *result = saved;
            saved = init;
            ++result;
        } while (++first != last);
    }
    return result;
}

}} // namespace boost and algorithm

#endif // BOOST_ALGORITHM_TRANSFORM_EXCLUSIVE_SCAN_HPP

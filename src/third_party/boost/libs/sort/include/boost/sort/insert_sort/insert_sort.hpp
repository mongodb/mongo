//----------------------------------------------------------------------------
/// @file insert_sort.hpp
/// @brief Insertion Sort algorithm
///
/// @author Copyright (c) 2016 Francisco Jose Tapia (fjtapia@gmail.com )\n
///         Distributed under the Boost Software License, Version 1.0.\n
///         ( See accompanying file LICENSE_1_0.txt or copy at
///           http://www.boost.org/LICENSE_1_0.txt  )
/// @version 0.1
///
/// @remarks
//-----------------------------------------------------------------------------
#ifndef __BOOST_SORT_INTROSORT_DETAIL_INSERT_SORT_HPP
#define __BOOST_SORT_INTROSORT_DETAIL_INSERT_SORT_HPP

#include <functional>
#include <iterator>
#include <algorithm>
#include <utility> // std::swap
#include <boost/sort/common/util/traits.hpp>

namespace boost
{
namespace sort
{
using common::util::compare_iter;
using common::util::value_iter;
//
//-----------------------------------------------------------------------------
//  function : insert_sort
/// @brief : Insertion sort algorithm
/// @param first: iterator to the first element of the range
/// @param last : iterator to the next element of the last in the range
/// @param comp : object for to do the comparison between the elements
/// @remarks This algorithm is O(N^2)
//-----------------------------------------------------------------------------
template < class Iter_t, typename Compare = compare_iter < Iter_t > >
static void insert_sort (Iter_t first, Iter_t last,
                         Compare comp = Compare())
{
    //--------------------------------------------------------------------
    //                   DEFINITIONS
    //--------------------------------------------------------------------
    typedef value_iter< Iter_t > value_t;

    if ((last - first) < 2) return;

    for (Iter_t it_examine = first + 1; it_examine != last; ++it_examine)
    {
        value_t Aux = std::move (*it_examine);
        Iter_t it_insertion = it_examine;

        while (it_insertion != first && comp (Aux, *(it_insertion - 1)))
        {
            *it_insertion = std::move (*(it_insertion - 1));
            --it_insertion;
        };
        *it_insertion = std::move (Aux);
    };
}

//
//****************************************************************************
} //    End namespace sort
} //    End namespace boost
//****************************************************************************
//
#endif

//  (C) Copyright Gennadiy Rozental 2004-2008.
//  Distributed under the Boost Software License, Version 1.0.
//  (See accompanying file LICENSE_1_0.txt or copy at 
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/test for the library home page.
//
//  File        : $RCSfile$
//
//  Version     : $Revision: 49312 $
//
//  Description : addition to STL algorithms
// ***************************************************************************

#ifndef BOOST_ALGORITHM_HPP_062304GER
#define BOOST_ALGORITHM_HPP_062304GER

#include <utility>
#include <algorithm> // std::find
#include <functional> // std::bind1st

#include <boost/test/detail/suppress_warnings.hpp>

//____________________________________________________________________________//

namespace boost {

namespace unit_test {

/// @brief this algorithm search through two collections for first mismatch position that get returned as a pair
/// of iterators, first pointing to the mismatch position in first collection, second iterator in second one

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
template <class InputIter1, class InputIter2>
inline std::pair<InputIter1, InputIter2>
mismatch( InputIter1 first1, InputIter1 last1,
          InputIter2 first2, InputIter2 last2 )
{
    while( first1 != last1 && first2 != last2 && *first1 == *first2 ) {
        ++first1;
        ++first2;
    }

    return std::pair<InputIter1, InputIter2>(first1, first2);
}

//____________________________________________________________________________//

/// @brief this algorithm search through two collections for first mismatch position that get returned as a pair
/// of iterators, first pointing to the mismatch position in first collection, second iterator in second one. This algorithms
/// uses supplied predicate for collection elements comparison

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
/// @param pred - predicate to be used for search
template <class InputIter1, class InputIter2, class Predicate>
inline std::pair<InputIter1, InputIter2>
mismatch( InputIter1 first1, InputIter1 last1,
          InputIter2 first2, InputIter2 last2,
          Predicate pred )
{
    while( first1 != last1 && first2 != last2 && pred( *first1, *first2 ) ) {
        ++first1;
        ++first2;
    }

    return std::pair<InputIter1, InputIter2>(first1, first2);
}

//____________________________________________________________________________//

/// @brief this algorithm search through first collection for first element that does not belong a second one

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
template<class ForwardIterator1, class ForwardIterator2>
inline ForwardIterator1
find_first_not_of( ForwardIterator1 first1, ForwardIterator1 last1, 
                   ForwardIterator2 first2, ForwardIterator2 last2 )
{
    while( first1 != last1 ) {
        if( std::find( first2, last2, *first1 ) == last2 )
            break;
        ++first1;
    }

    return first1;
}

//____________________________________________________________________________//

/// @brief this algorithm search through first collection for first element that does not satisfy binary 
/// predicate in conjunction will any element in second collection

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
/// @param pred - predicate to be used for search
template<class ForwardIterator1, class ForwardIterator2, class Predicate>
inline ForwardIterator1
find_first_not_of( ForwardIterator1 first1, ForwardIterator1 last1, 
                   ForwardIterator2 first2, ForwardIterator2 last2, 
                   Predicate pred )
{
    while( first1 != last1 ) {
        if( std::find_if( first2, last2, std::bind1st( pred, *first1 ) ) == last2 )
            break;
        ++first1;
    }

    return first1;
}

//____________________________________________________________________________//

/// @brief this algorithm search through first collection for last element that belongs to a second one

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
template<class BidirectionalIterator1, class ForwardIterator2>
inline BidirectionalIterator1
find_last_of( BidirectionalIterator1 first1, BidirectionalIterator1 last1, 
              ForwardIterator2 first2, ForwardIterator2 last2 )
{
    if( first1 == last1 || first2 == last2 )
        return last1;

    BidirectionalIterator1 it1 = last1;
    while( --it1 != first1 && std::find( first2, last2, *it1 ) == last2 ) {}

    return it1 == first1 && std::find( first2, last2, *it1 ) == last2 ? last1 : it1;
}

//____________________________________________________________________________//

/// @brief this algorithm search through first collection for last element that satisfy binary 
/// predicate in conjunction will at least one element in second collection

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
/// @param pred - predicate to be used for search
template<class BidirectionalIterator1, class ForwardIterator2, class Predicate>
inline BidirectionalIterator1
find_last_of( BidirectionalIterator1 first1, BidirectionalIterator1 last1, 
              ForwardIterator2 first2, ForwardIterator2 last2, 
              Predicate pred )
{
    if( first1 == last1 || first2 == last2 )
        return last1;

    BidirectionalIterator1 it1 = last1;
    while( --it1 != first1 && std::find_if( first2, last2, std::bind1st( pred, *it1 ) ) == last2 ) {}

    return it1 == first1 && std::find_if( first2, last2, std::bind1st( pred, *it1 ) ) == last2 ? last1 : it1;
}

//____________________________________________________________________________//

/// @brief this algorithm search through first collection for last element that does not belong to a second one

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
template<class BidirectionalIterator1, class ForwardIterator2>
inline BidirectionalIterator1
find_last_not_of( BidirectionalIterator1 first1, BidirectionalIterator1 last1, 
                  ForwardIterator2 first2, ForwardIterator2 last2 )
{
    if( first1 == last1 || first2 == last2 )
        return last1;

    BidirectionalIterator1 it1 = last1;
    while( --it1 != first1 && std::find( first2, last2, *it1 ) != last2 ) {}

    return it1 == first1 && std::find( first2, last2, *it1 ) != last2 ? last1 : it1;
}

//____________________________________________________________________________//

/// @brief this algorithm search through first collection for last element that does not satisfy binary 
/// predicate in conjunction will any element in second collection

/// @param first1 - first collection begin iterator
/// @param last1 - first collection end iterator
/// @param first2 - second collection begin iterator
/// @param last2 - second collection end iterator
/// @param pred - predicate to be used for search
template<class BidirectionalIterator1, class ForwardIterator2, class Predicate>
inline BidirectionalIterator1
find_last_not_of( BidirectionalIterator1 first1, BidirectionalIterator1 last1, 
                  ForwardIterator2 first2, ForwardIterator2 last2, 
                  Predicate pred )
{
    if( first1 == last1 || first2 == last2 )
        return last1;

    BidirectionalIterator1 it1 = last1;
    while( --it1 != first1 && std::find_if( first2, last2, std::bind1st( pred, *it1 ) ) != last2 ) {}

    return it1 == first1 && std::find_if( first2, last2, std::bind1st( pred, *it1 ) ) == last2 ? last1 : it1;
}

//____________________________________________________________________________//

} // namespace unit_test

} // namespace boost

//____________________________________________________________________________//

#include <boost/test/detail/enable_warnings.hpp>

#endif // BOOST_ALGORITHM_HPP_062304GER



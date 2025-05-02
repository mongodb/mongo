/* 
   Copyright (c) Ivan Matek, Marshall Clow 2021.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
  
*/

/// \file is_clamped.hpp
/// \brief IsClamped algorithm
/// \authors Ivan Matek, Marshall Clow
///

#ifndef BOOST_ALGORITHM_IS_CLAMPED_HPP
#define BOOST_ALGORITHM_IS_CLAMPED_HPP

#include <functional>       //  for std::less
#include <cassert>

#include <boost/type_traits/type_identity.hpp> // for boost::type_identity

namespace boost { namespace algorithm {

/// \fn is_clamped ( T const& val,
///               typename boost::type_identity<T>::type const & lo,
///               typename boost::type_identity<T>::type const & hi, Pred p )
/// \returns true if value "val" is in the range [ lo, hi ]
///     using the comparison predicate p.
///     If p ( val, lo ) return false.
///     If p ( hi, val ) return false.
///     Otherwise, returns true.
///
/// \param val   The value to be checked
/// \param lo    The lower bound of the range
/// \param hi    The upper bound of the range
/// \param p     A predicate to use to compare the values.
///                 p ( a, b ) returns a boolean.
///
  template <typename T, typename Pred>
  BOOST_CXX14_CONSTEXPR bool is_clamped(
      T const& val, typename boost::type_identity<T>::type const& lo,
      typename boost::type_identity<T>::type const& hi, Pred p) {
    //    assert ( !p ( hi, lo ));    // Can't assert p ( lo, hi ) b/c they
    //    might be equal
    return p(val, lo) ? false : p(hi, val) ? false : true;
  }
  
/// \fn is_clamped ( T const& val,
///               typename boost::type_identity<T>::type const & lo,
///               typename boost::type_identity<T>::type const & hi)
/// \returns true if value "val" is in the range [ lo, hi ]
///     using operator < for comparison.
///     If the value is less than lo, return false.
///     If the value is greater than hi, return false.
///     Otherwise, returns true.
///
/// \param val   The value to be checked
/// \param lo    The lower bound of the range
/// \param hi    The upper bound of the range
///
  
  template<typename T> 
  BOOST_CXX14_CONSTEXPR bool is_clamped ( const T& val,
    typename boost::type_identity<T>::type const & lo,
    typename boost::type_identity<T>::type const & hi )
  {
    return boost::algorithm::is_clamped ( val, lo, hi, std::less<T>());
  } 

}}

#endif // BOOST_ALGORITHM_CLAMP_HPP

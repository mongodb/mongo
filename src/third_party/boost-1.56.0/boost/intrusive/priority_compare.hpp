/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_PRIORITY_COMPARE_HPP
#define BOOST_INTRUSIVE_PRIORITY_COMPARE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <functional>

namespace boost {
namespace intrusive {

template <class T>
struct priority_compare
{
   //Compatibility with std::binary_function
	typedef T      first_argument_type;
	typedef T      second_argument_type;
	typedef bool   result_type;

   bool operator()(const T &val, const T &val2) const
   {
      return priority_order(val, val2);
   }
};

/// @cond

template<class Less, class T>
struct get_prio
{
   typedef Less type;
};


template<class T>
struct get_prio<void, T>
{
   typedef ::boost::intrusive::priority_compare<T> type;
};

/// @endcond

} //namespace intrusive 
} //namespace boost 

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_PRIORITY_COMPARE_HPP

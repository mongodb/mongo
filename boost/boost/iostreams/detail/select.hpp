// (C) Copyright Jonathan Turkanis 2003.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt.)

// See http://www.boost.org/libs/iostreams for documentation.

// Contains the metafunction select, which mimics the effect of a chain of
// nested mpl if_'s.
//
// -----------------------------------------------------------------------------
//
// Usage:
//      
// typedef typename select<
//                      case1,  type1,
//                      case2,  type2,
//                      ...
//                      true_,  typen
//                  >::type selection;
//
// Here case1, case2, ... are models of MPL::IntegralConstant with value type
// bool, and n <= 10.

#ifndef BOOST_IOSTREAMS_SELECT_HPP_INCLUDED
#define BOOST_IOSTREAMS_SELECT_HPP_INCLUDED   

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif                  
 
#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/void.hpp>

namespace boost { namespace iostreams { 

typedef mpl::true_ else_;

template< typename Case1 = mpl::true_,
          typename Type1 = mpl::void_,
          typename Case2 = mpl::true_,
          typename Type2 = mpl::void_,
          typename Case3 = mpl::true_,
          typename Type3 = mpl::void_,
          typename Case4 = mpl::true_,
          typename Type4 = mpl::void_,
          typename Case5 = mpl::true_,
          typename Type5 = mpl::void_,
          typename Case6 = mpl::true_,
          typename Type6 = mpl::void_,
          typename Case7 = mpl::true_,
          typename Type7 = mpl::void_,
          typename Case8 = mpl::true_,
          typename Type8 = mpl::void_,
          typename Case9 = mpl::true_,
          typename Type9 = mpl::void_,
          typename Case10 = mpl::true_,
          typename Type10 = mpl::void_ >
struct select {
    typedef typename
            mpl::eval_if<
                Case1, mpl::identity<Type1>, mpl::eval_if<
                Case2, mpl::identity<Type2>, mpl::eval_if<
                Case3, mpl::identity<Type3>, mpl::eval_if<
                Case4, mpl::identity<Type4>, mpl::eval_if<
                Case5, mpl::identity<Type5>, mpl::eval_if<
                Case6, mpl::identity<Type6>, mpl::eval_if<
                Case7, mpl::identity<Type7>, mpl::eval_if<
                Case8, mpl::identity<Type8>, mpl::eval_if<
                Case9, mpl::identity<Type9>, mpl::if_<
                Case10, Type10, mpl::void_ > > > > > > > > >
            >::type type;
};

} } // End namespaces iostreams, boost.

#endif // #ifndef BOOST_IOSTREAMS_SELECT_HPP_INCLUDED

/* Copyright 2003-2008 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_MSVC_INDEX_SPECIFIER_HPP
#define BOOST_MULTI_INDEX_DETAIL_MSVC_INDEX_SPECIFIER_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <boost/detail/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC,<1310)
/* Workaround for a problem in MSVC with dependent template typedefs
 * when accesing index specifiers.
 * Modeled after <boost/mpl/aux_/msvc_dtw.hpp> (thanks, Aleksey!)
 */

#include <boost/mpl/aux_/msvc_never_true.hpp>

namespace boost{

namespace multi_index{

namespace detail{

template<typename IndexSpecifier>
struct msvc_index_specifier
{
  template<bool> struct fake_index_type:IndexSpecifier{};
  template<> struct fake_index_type<true>
  {
    template<typename Super>
    struct node_class{};

    template<typename Super>
    struct index_class{};
  };

  template<typename Super>
  struct result_node_class:
    fake_index_type<mpl::aux::msvc_never_true<IndexSpecifier>::value>::
      template node_class<Super>
  {
  };

  template<typename Super>
  struct result_index_class:
    fake_index_type<mpl::aux::msvc_never_true<IndexSpecifier>::value>::
      template index_class<Super>
  {
  };
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif /* workaround */

#endif

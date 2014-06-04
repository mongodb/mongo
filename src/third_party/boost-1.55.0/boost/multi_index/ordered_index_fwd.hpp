/* Copyright 2003-2008 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_ORDERED_INDEX_FWD_HPP
#define BOOST_MULTI_INDEX_ORDERED_INDEX_FWD_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/multi_index/detail/ord_index_args.hpp>

namespace boost{

namespace multi_index{

namespace detail{

template<
  typename KeyFromValue,typename Compare,
  typename SuperMeta,typename TagList,typename Category
>
class ordered_index;

template<
  typename KeyFromValue1,typename Compare1,
  typename SuperMeta1,typename TagList1,typename Category1,
  typename KeyFromValue2,typename Compare2,
  typename SuperMeta2,typename TagList2,typename Category2
>
bool operator==(
  const ordered_index<
    KeyFromValue1,Compare1,SuperMeta1,TagList1,Category1>& x,
  const ordered_index<
    KeyFromValue2,Compare2,SuperMeta2,TagList2,Category2>& y);

template<
  typename KeyFromValue1,typename Compare1,
  typename SuperMeta1,typename TagList1,typename Category1,
  typename KeyFromValue2,typename Compare2,
  typename SuperMeta2,typename TagList2,typename Category2
>
bool operator<(
  const ordered_index<
    KeyFromValue1,Compare1,SuperMeta1,TagList1,Category1>& x,
  const ordered_index<
    KeyFromValue2,Compare2,SuperMeta2,TagList2,Category2>& y);

template<
  typename KeyFromValue1,typename Compare1,
  typename SuperMeta1,typename TagList1,typename Category1,
  typename KeyFromValue2,typename Compare2,
  typename SuperMeta2,typename TagList2,typename Category2
>
bool operator!=(
  const ordered_index<
    KeyFromValue1,Compare1,SuperMeta1,TagList1,Category1>& x,
  const ordered_index<
    KeyFromValue2,Compare2,SuperMeta2,TagList2,Category2>& y);

template<
  typename KeyFromValue1,typename Compare1,
  typename SuperMeta1,typename TagList1,typename Category1,
  typename KeyFromValue2,typename Compare2,
  typename SuperMeta2,typename TagList2,typename Category2
>
bool operator>(
  const ordered_index<
    KeyFromValue1,Compare1,SuperMeta1,TagList1,Category1>& x,
  const ordered_index<
    KeyFromValue2,Compare2,SuperMeta2,TagList2,Category2>& y);

template<
  typename KeyFromValue1,typename Compare1,
  typename SuperMeta1,typename TagList1,typename Category1,
  typename KeyFromValue2,typename Compare2,
  typename SuperMeta2,typename TagList2,typename Category2
>
bool operator>=(
  const ordered_index<
    KeyFromValue1,Compare1,SuperMeta1,TagList1,Category1>& x,
  const ordered_index<
    KeyFromValue2,Compare2,SuperMeta2,TagList2,Category2>& y);

template<
  typename KeyFromValue1,typename Compare1,
  typename SuperMeta1,typename TagList1,typename Category1,
  typename KeyFromValue2,typename Compare2,
  typename SuperMeta2,typename TagList2,typename Category2
>
bool operator<=(
  const ordered_index<
    KeyFromValue1,Compare1,SuperMeta1,TagList1,Category1>& x,
  const ordered_index<
    KeyFromValue2,Compare2,SuperMeta2,TagList2,Category2>& y);

template<
  typename KeyFromValue,typename Compare,
  typename SuperMeta,typename TagList,typename Category
>
void swap(
  ordered_index<KeyFromValue,Compare,SuperMeta,TagList,Category>& x,
  ordered_index<KeyFromValue,Compare,SuperMeta,TagList,Category>& y);

} /* namespace multi_index::detail */

/* ordered_index specifiers */

template<typename Arg1,typename Arg2=mpl::na,typename Arg3=mpl::na>
struct ordered_unique;

template<typename Arg1,typename Arg2=mpl::na,typename Arg3=mpl::na>
struct ordered_non_unique;

} /* namespace multi_index */

} /* namespace boost */

#endif

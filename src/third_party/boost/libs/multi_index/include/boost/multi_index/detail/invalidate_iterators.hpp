/* Copyright 2003-2021 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_INVALIDATE_ITERATORS_HPP
#define BOOST_MULTI_INDEX_DETAIL_INVALIDATE_ITERATORS_HPP

#if defined(_MSC_VER)
#pragma once
#endif

namespace boost{

namespace multi_index{

namespace detail{

/* invalidate_iterators mimics the interface of index_access_sequence (see
 * index_access_sequence.hpp) but returns dummy indices whose iterator type
 * won't ever match those of the source: the net effect is that safe iterator
 * transfer resolves to iterator invalidation, so backbone function invocation
 * extract_(x,invalidate_iterators()) is used in extraction scenarios other
 * than merging.
 */

struct invalidate_iterators
{
  typedef void iterator;

  invalidate_iterators& get(){return *this;}
  invalidate_iterators& next(){return *this;}
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

/* Copyright 2003-2005 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_DEF_CTOR_TUPLE_CONS_HPP
#define BOOST_MULTI_INDEX_DETAIL_DEF_CTOR_TUPLE_CONS_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp>

#if defined(BOOST_MSVC)
/* In MSVC, tuples::cons is not default constructible. We provide a
 * tiny wrapper around tuple::cons filling that hole.
 */

#include <boost/tuple/tuple.hpp>

namespace boost{

namespace multi_index{

namespace detail{

template<typename Cons>
struct default_constructible_tuple_cons:Cons
{
  default_constructible_tuple_cons():
    Cons(
      Cons::head_type(),
      static_cast<const Cons::tail_type&>(
        default_constructible_tuple_cons<Cons::tail_type>()))
  {}

  default_constructible_tuple_cons(const Cons& cons):Cons(cons){}
};

template<>
struct default_constructible_tuple_cons<tuples::null_type>:tuples::null_type
{
  default_constructible_tuple_cons(){}
  default_constructible_tuple_cons(const tuples::null_type&){}
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif /* BOOST_MSVC */

#endif

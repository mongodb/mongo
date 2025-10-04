/* Copyright 2003-2021 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_ANY_CONTAINER_VIEW_HPP
#define BOOST_MULTI_INDEX_DETAIL_ANY_CONTAINER_VIEW_HPP

#if defined(_MSC_VER)
#pragma once
#endif

namespace boost{

namespace multi_index{

namespace detail{

/* type-erased, non-owning view over a ConstIterator-container's range */

template<typename ConstIterator>
class any_container_view
{
public:
  template<typename Container>
  any_container_view(const Container& x):px(&x),pt(vtable_for<Container>()){}

  const void*   container()const{return px;}
  ConstIterator begin()const{return pt->begin(px);}
  ConstIterator end()const{return pt->end(px);}

private:
  struct vtable
  {
    ConstIterator (*begin)(const void*);
    ConstIterator (*end)(const void*);
  };

  template<typename Container>
  static ConstIterator begin_for(const void* px)
  {
    return static_cast<const Container*>(px)->begin();
  }

  template<typename Container>
  static ConstIterator end_for(const void* px)
  {
    return static_cast<const Container*>(px)->end();
  }

  template<typename Container>
  vtable* vtable_for()
  {
    static vtable v=
    {
      &begin_for<Container>,
      &end_for<Container>
    };

    return &v;
  }

  const void* px;
  vtable*     pt;
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

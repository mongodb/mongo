/* Copyright 2023 Christian Mazakas.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_DETAIL_FOA_ELEMENT_TYPE_HPP
#define BOOST_UNORDERED_DETAIL_FOA_ELEMENT_TYPE_HPP

#include <boost/core/pointer_traits.hpp>

namespace boost{
namespace unordered{
namespace detail{
namespace foa{

template<class T,class VoidPtr>
struct element_type
{
  using value_type=T;
  using pointer=typename boost::pointer_traits<VoidPtr>::template rebind<T>;

  pointer p;

  /*
   * we use a deleted copy constructor here so the type is no longer
   * trivially copy-constructible which inhibits our memcpy
   * optimizations when copying the tables
   */
  element_type()=default;
  element_type(pointer p_):p(p_){}
  element_type(element_type const&)=delete;
  element_type(element_type&& rhs)noexcept
  {
    p = rhs.p;
    rhs.p = nullptr;
  }

  element_type& operator=(element_type const&)=delete;
  element_type& operator=(element_type&& rhs)noexcept
  {
    if (this!=&rhs){
      p=rhs.p;
      rhs.p=nullptr;
    }
    return *this;
  }

  void swap(element_type& rhs)noexcept
  {
    auto tmp=p;
    p=rhs.p;
    rhs.p=tmp;
  }
};

}
}
}
}

#endif // BOOST_UNORDERED_DETAIL_FOA_ELEMENT_TYPE_HPP

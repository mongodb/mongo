/* Copyright 2003-2005 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_RND_INDEX_PTR_ARRAY_HPP
#define BOOST_MULTI_INDEX_DETAIL_RND_INDEX_PTR_ARRAY_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <algorithm>
#include <boost/multi_index/detail/auto_space.hpp>
#include <boost/multi_index/detail/rnd_index_node.hpp>
#include <boost/noncopyable.hpp>
#include <cstddef>

namespace boost{

namespace multi_index{

namespace detail{

/* pointer structure for use by random access indices */

template<typename Allocator>
class random_access_index_ptr_array:private noncopyable
{
public:
  typedef random_access_index_node_impl* value_type;

  random_access_index_ptr_array(
    const Allocator& al,value_type end_,std::size_t size):
    size_(size),
    capacity_(size),
    spc(al,capacity_+1)
  {
    *end()=end_;
    end_->up()=end();
  }

  std::size_t size()const{return size_;}
  std::size_t capacity()const{return capacity_;}

  void room_for_one()
  {
    if(size_==capacity_){
      reserve(capacity_<=10?15:capacity_+capacity_/2);
    }
  }

  void reserve(std::size_t c)
  {
    if(c>capacity_){
      auto_space<value_type,Allocator> spc1(spc.get_allocator(),c+1);
      random_access_index_node_impl::transfer(begin(),end()+1,spc1.data());
      spc.swap(spc1);
      capacity_=c;
    }
  }

  value_type* begin()const{return &ptrs()[0];}
  value_type* end()const{return &ptrs()[size_];}
  value_type* at(std::size_t n)const{return &ptrs()[n];}

  void push_back(value_type x)
  {
    *(end()+1)=*end();
    (*(end()+1))->up()=end()+1;
    *end()=x;
    (*end())->up()=end();
    ++size_;
  }

  void erase(value_type x)
  {
    random_access_index_node_impl::extract(x->up(),end()+1);
    --size_;
  }

  void clear()
  {
    *begin()=*end();
    (*begin())->up()=begin();
    size_=0;
  }

  void swap(random_access_index_ptr_array& x)
  {
    std::swap(size_,x.size_);
    std::swap(capacity_,x.capacity_);
    spc.swap(x.spc);
  }

private:
  std::size_t                      size_;
  std::size_t                      capacity_;
  auto_space<value_type,Allocator> spc;

  value_type* ptrs()const
  {
    return spc.data();
  }
};

template<typename Allocator>
void swap(
  random_access_index_ptr_array<Allocator>& x,
  random_access_index_ptr_array<Allocator>& y)
{
  x.swap(y);
}

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

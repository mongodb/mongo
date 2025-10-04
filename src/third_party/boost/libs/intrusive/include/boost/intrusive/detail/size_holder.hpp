/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2014-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_SIZE_HOLDER_HPP
#define BOOST_INTRUSIVE_DETAIL_SIZE_HOLDER_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template<bool ConstantSize, class SizeType, class Tag = void>
struct size_holder
{
   static const bool constant_time_size = ConstantSize;
   typedef SizeType  size_type;

   inline SizeType get_size() const
   {  return size_;  }

   inline void set_size(SizeType size)
   {  size_ = size; }

   inline void decrement()
   {  --size_; }

   inline void increment()
   {  ++size_; }

   inline void increase(SizeType n)
   {  size_ += n; }

   inline void decrease(SizeType n)
   {  size_ -= n; }

   inline void swap(size_holder &other)
   {  SizeType tmp(size_); size_ = other.size_; other.size_ = tmp; }

   SizeType size_;
};

template<class SizeType, class Tag>
struct size_holder<false, SizeType, Tag>
{
   static const bool constant_time_size = false;
   typedef SizeType  size_type;

   inline size_type get_size() const
   {  return 0;  }

   inline void set_size(size_type)
   {}

   inline void decrement()
   {}

   inline void increment()
   {}

   inline void increase(SizeType)
   {}

   inline void decrease(SizeType)
   {}

   inline void swap(size_holder){}
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_SIZE_HOLDER_HPP

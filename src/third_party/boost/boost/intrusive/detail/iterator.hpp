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

#ifndef BOOST_INTRUSIVE_DETAIL_ITERATOR_HPP
#define BOOST_INTRUSIVE_DETAIL_ITERATOR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <cstddef>
#include <boost/intrusive/detail/std_fwd.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/move/detail/iterator_traits.hpp>
#include <boost/move/detail/meta_utils_core.hpp>

namespace boost{
namespace iterators{

struct incrementable_traversal_tag;
struct single_pass_traversal_tag;
struct forward_traversal_tag;
struct bidirectional_traversal_tag;
struct random_access_traversal_tag;

namespace detail{

template <class Category, class Traversal>
struct iterator_category_with_traversal;

} //namespace boost{
} //namespace iterators{
} //namespace detail{

namespace boost {
namespace intrusive {

using boost::movelib::iterator_traits;
using boost::movelib::iter_difference;
using boost::movelib::iter_value;
using boost::movelib::iter_category;
using boost::movelib::iter_size;


////////////////////
//    iterator
////////////////////
template<class Category, class T, class Difference, class Pointer, class Reference>
struct iterator
{
   typedef Category     iterator_category;
   typedef T            value_type;
   typedef Difference   difference_type;
   typedef Pointer      pointer;
   typedef Reference    reference;
};

////////////////////////////////////////////////////////////////////////////////
//    Conversion from boost::iterator traversals to std tags
////////////////////////////////////////////////////////////////////////////////

template<class Tag>
struct get_std_category_from_tag
{
   typedef Tag type;
};

template <class Category>
struct get_std_category_from_tag
   <boost::iterators::detail::iterator_category_with_traversal
      <Category, boost::iterators::incrementable_traversal_tag> >
{
   typedef std::input_iterator_tag type;
};

template <class Category>
struct get_std_category_from_tag
   <boost::iterators::detail::iterator_category_with_traversal
      <Category, boost::iterators::single_pass_traversal_tag> >
{
   typedef std::input_iterator_tag type;
};

template <class Category>
struct get_std_category_from_tag
   <boost::iterators::detail::iterator_category_with_traversal
      <Category, boost::iterators::forward_traversal_tag> >
{
   typedef std::input_iterator_tag type;
};

template <class Category>
struct get_std_category_from_tag
   <boost::iterators::detail::iterator_category_with_traversal
      <Category, boost::iterators::bidirectional_traversal_tag> >
{
   typedef std::bidirectional_iterator_tag type;
};

template <class Category>
struct get_std_category_from_tag
   <boost::iterators::detail::iterator_category_with_traversal
      <Category, boost::iterators::random_access_traversal_tag> >
{
   typedef std::random_access_iterator_tag type;
};

template<class It>
struct get_std_category_from_it
   : get_std_category_from_tag< typename boost::intrusive::iter_category<It>::type >
{};

////////////////////////////////////////
//    iterator_[dis|en]able_if_tag
////////////////////////////////////////
template<class I, class Tag, class R = void>
struct iterator_enable_if_tag
   : ::boost::move_detail::enable_if_c
      < ::boost::move_detail::is_same
         < typename get_std_category_from_it<I>::type
         , Tag
         >::value
         , R>
{};

template<class I, class Tag, class R = void>
struct iterator_disable_if_tag
   : ::boost::move_detail::enable_if_c
      < !::boost::move_detail::is_same
         < typename get_std_category_from_it<I>::type
         , Tag
         >::value
         , R>
{};

////////////////////////////////////////
//    iterator_[dis|en]able_if_tag
////////////////////////////////////////
template<class I, class Tag, class Tag2, class R = void>
struct iterator_enable_if_convertible_tag
   : ::boost::move_detail::enable_if_c
      < ::boost::move_detail::is_same_or_convertible
         < typename get_std_category_from_it<I>::type
         , Tag
         >::value &&
        !::boost::move_detail::is_same_or_convertible
         < typename get_std_category_from_it<I>::type
         , Tag2
         >::value
         , R>
{};

////////////////////////////////////////
//    iterator_[dis|en]able_if_tag_difference_type
////////////////////////////////////////
template<class I, class Tag>
struct iterator_enable_if_tag_difference_type
   : iterator_enable_if_tag<I, Tag, typename boost::intrusive::iter_difference<I>::type>
{};

template<class I, class Tag>
struct iterator_disable_if_tag_difference_type
   : iterator_disable_if_tag<I, Tag, typename boost::intrusive::iter_difference<I>::type>
{};

////////////////////
//    advance
////////////////////

template<class InputIt>
BOOST_INTRUSIVE_FORCEINLINE typename iterator_enable_if_tag<InputIt, std::input_iterator_tag>::type
   iterator_advance(InputIt& it, typename iter_difference<InputIt>::type n)
{
   while(n--)
      ++it;
}

template<class InputIt>
typename iterator_enable_if_tag<InputIt, std::forward_iterator_tag>::type
   iterator_advance(InputIt& it, typename iter_difference<InputIt>::type n)
{
   while(n--)
      ++it;
}

template<class InputIt>
BOOST_INTRUSIVE_FORCEINLINE typename iterator_enable_if_tag<InputIt, std::bidirectional_iterator_tag>::type
   iterator_advance(InputIt& it, typename iter_difference<InputIt>::type n)
{
   for (; 0 < n; --n)
      ++it;
   for (; n < 0; ++n)
      --it;
}

template<class InputIt, class Distance>
BOOST_INTRUSIVE_FORCEINLINE typename iterator_enable_if_tag<InputIt, std::random_access_iterator_tag>::type
   iterator_advance(InputIt& it, Distance n)
{
   it += n;
}

template<class It>
BOOST_INTRUSIVE_FORCEINLINE 
   void iterator_uadvance(It& it, typename iter_size<It>::type n)
{
   (iterator_advance)(it, (typename iterator_traits<It>::difference_type)n);
}

////////////////////////////////////////
//    iterator_distance
////////////////////////////////////////
template<class InputIt> inline
typename iterator_disable_if_tag_difference_type
   <InputIt, std::random_access_iterator_tag>::type
      iterator_distance(InputIt first, InputIt last)
{
   typename iter_difference<InputIt>::type off = 0;
   while(first != last){
      ++off;
      ++first;
   }
   return off;
}

template<class InputIt>
BOOST_INTRUSIVE_FORCEINLINE typename iterator_enable_if_tag_difference_type
   <InputIt, std::random_access_iterator_tag>::type
      iterator_distance(InputIt first, InputIt last)
{
   typename iter_difference<InputIt>::type off = last - first;
   return off;
}

////////////////////////////////////////
//    iterator_udistance
////////////////////////////////////////

template<class It>
BOOST_INTRUSIVE_FORCEINLINE typename iter_size<It>::type
   iterator_udistance(It first, It last)
{
   return (typename iter_size<It>::type)(iterator_distance)(first, last);
}

////////////////////////////////////////
//    iterator_next
////////////////////////////////////////

template<class InputIt>
BOOST_INTRUSIVE_FORCEINLINE InputIt iterator_next(InputIt it, typename iter_difference<InputIt>::type n)
{
   (iterator_advance)(it, n);
   return it;
}

template<class InputIt>
BOOST_INTRUSIVE_FORCEINLINE InputIt iterator_unext(InputIt it, typename iterator_traits<InputIt>::size_type n)
{
   (iterator_uadvance)(it, n);
   return it;
}

////////////////////////////////////////
// iterator_arrow_result
////////////////////////////////////////

template<class I>
BOOST_INTRUSIVE_FORCEINLINE typename iterator_traits<I>::pointer iterator_arrow_result(const I &i)
{  return i.operator->();  }

template<class T>
BOOST_INTRUSIVE_FORCEINLINE T * iterator_arrow_result(T *p)
{  return p;   }

} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_DETAIL_ITERATOR_HPP

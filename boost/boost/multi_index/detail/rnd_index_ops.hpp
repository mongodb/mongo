/* Copyright 2003-2005 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_RND_INDEX_OPS_HPP
#define BOOST_MULTI_INDEX_DETAIL_RND_INDEX_OPS_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <algorithm>
#include <boost/multi_index/detail/rnd_index_ptr_array.hpp>
#include <functional>

namespace boost{

namespace multi_index{

namespace detail{

/* Common code for random_access_index memfuns having templatized and
 * non-templatized versions.
 */

template<typename Node,typename Allocator,typename Predicate>
Node* random_access_index_remove(
  random_access_index_ptr_array<Allocator>& ptrs,Predicate pred
  BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(Node))
{
  typedef typename Node::value_type value_type;

  random_access_index_node_impl** first=ptrs.begin(),
                               ** res=first,
                               ** last=ptrs.end();
  for(;first!=last;++first){
    if(!pred(
        const_cast<const value_type&>(Node::from_impl(*first)->value()))){
      if(first!=res){
        std::swap(*first,*res);
        (*first)->up()=first;
        (*res)->up()=res;
      }
      ++res;
    }
  }
  return Node::from_impl(*res);
}

template<typename Node,typename Allocator,class BinaryPredicate>
Node* random_access_index_unique(
  random_access_index_ptr_array<Allocator>& ptrs,BinaryPredicate binary_pred
  BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(Node))
{
  typedef typename Node::value_type value_type;

  random_access_index_node_impl** first=ptrs.begin(),
                               ** res=first,
                               ** last=ptrs.end();
  if(first!=last){
    for(;++first!=last;){
      if(!binary_pred(
           const_cast<const value_type&>(Node::from_impl(*res)->value()),
           const_cast<const value_type&>(Node::from_impl(*first)->value()))){
        ++res;
        if(first!=res){
          std::swap(*first,*res);
          (*first)->up()=first;
          (*res)->up()=res;
        }
      }
    }
    ++res;
  }
  return Node::from_impl(*res);
}

template<typename Node,typename Allocator,typename Compare>
void random_access_index_inplace_merge(
  const Allocator& al,
  random_access_index_ptr_array<Allocator>& ptrs,
  random_access_index_node_impl** first1,Compare comp
  BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(Node))
{
  typedef typename Node::value_type value_type;

  auto_space<random_access_index_node_impl*,Allocator> spc(al,ptrs.size());

  random_access_index_node_impl** first0=ptrs.begin(),
                               ** last0=first1,
                               ** last1=ptrs.end(),
                               ** out=spc.data();
  while(first0!=last0&&first1!=last1){
    if(comp(
        const_cast<const value_type&>(Node::from_impl(*first1)->value()),
        const_cast<const value_type&>(Node::from_impl(*first0)->value()))){
      *out++=*first1++;
    }
    else{
      *out++=*first0++;
    }
  }
  std::copy(first0,last0,out);
  std::copy(first1,last1,out);

  first1=ptrs.begin();
  out=spc.data();
  while(first1!=last1){
    *first1=*out++;
    (*first1)->up()=first1;
    ++first1;
  }
}

/* sorting */

/* auxiliary stuff */

template<typename Node,typename Compare>
struct random_access_index_sort_compare:
  std::binary_function<
    const typename Node::value_type*,const typename Node::value_type*,bool>
{
  random_access_index_sort_compare(Compare comp_=Compare()):comp(comp_){}

  bool operator()(
    random_access_index_node_impl* x,
    random_access_index_node_impl* y)const
  {
    typedef typename Node::value_type value_type;

    return comp(
      const_cast<const value_type&>(Node::from_impl(x)->value()),
      const_cast<const value_type&>(Node::from_impl(y)->value()));
  }

private:
  Compare comp;
};

template<typename Node,typename Allocator,class Compare>
void random_access_index_sort(
  const Allocator& al,
  random_access_index_ptr_array<Allocator>& ptrs,
  Compare comp
  BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(Node))
{
  /* The implementation is extremely simple: an auxiliary
   * array of pointers is sorted using stdlib facilities and
   * then used to rearrange the index. This is suboptimal
   * in space and time, but has some advantages over other
   * possible approaches:
   *   - Use std::stable_sort() directly on ptrs using some
   *     special iterator in charge of maintaining pointers
   *     and up() pointers in sync: we cannot guarantee
   *     preservation of the container invariants in the face of
   *     exceptions, if, for instance, std::stable_sort throws
   *     when ptrs transitorily contains duplicate elements.
   *   - Rewrite the internal algorithms of std::stable_sort
   *     adapted for this case: besides being a fair amount of
   *     work, making a stable sort compatible with Boost.MultiIndex
   *     invariants (basically, no duplicates or missing elements
   *     even if an exception is thrown) is complicated, error-prone
   *     and possibly won't perform much better than the
   *     solution adopted.
   */

  typedef typename Node::value_type         value_type;
  typedef random_access_index_sort_compare<
    Node,Compare>                           ptr_compare;
  
  random_access_index_node_impl**   first=ptrs.begin();
  random_access_index_node_impl**   last=ptrs.end();
  auto_space<
    random_access_index_node_impl*,
    Allocator>                      spc(al,ptrs.size());
  random_access_index_node_impl**   buf=spc.data();

  std::copy(first,last,buf);
  std::stable_sort(buf,buf+ptrs.size(),ptr_compare(comp));

  while(first!=last){
    *first=*buf++;
    (*first)->up()=first;
    ++first;
  }
}

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

/* Copyright 2003-2008 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_HASH_INDEX_NODE_HPP
#define BOOST_MULTI_INDEX_DETAIL_HASH_INDEX_NODE_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <boost/detail/allocator_utilities.hpp>
#include <boost/multi_index/detail/prevent_eti.hpp>
#include <functional>

namespace boost{

namespace multi_index{

namespace detail{

/* singly-linked node for use by hashed_index */

template<typename Allocator>
struct hashed_index_node_impl
{
  typedef typename prevent_eti<
    Allocator,
    typename boost::detail::allocator::rebind_to<
      Allocator,hashed_index_node_impl
    >::type
  >::type::pointer                                pointer;
  typedef typename prevent_eti<
    Allocator,
    typename boost::detail::allocator::rebind_to<
      Allocator,hashed_index_node_impl
    >::type
  >::type::const_pointer                          const_pointer;

  pointer& next(){return next_;}
  pointer  next()const{return next_;}

  /* algorithmic stuff */

  static void increment(pointer& x,pointer bbegin,pointer bend)
  {
    std::less_equal<pointer> leq;

    x=x->next();
    if(leq(bbegin,x)&&leq(x,bend)){ /* bucket node */
      do{
        ++x;
      }while(x->next()==x);
      x=x->next();
    }
  }

  static void link(pointer x,pointer pos)
  {
    x->next()=pos->next();
    pos->next()=x;
  };

  static void unlink(pointer x)
  {
    pointer y=x->next();
    while(y->next()!=x){y=y->next();}
    y->next()=x->next();
  }

  static pointer prev(pointer x)
  {
    pointer y=x->next();
    while(y->next()!=x){y=y->next();}
    return y;
  }

  static void unlink_next(pointer x)
  {
    x->next()=x->next()->next();
  }

private:
  pointer next_;
};

template<typename Super>
struct hashed_index_node_trampoline:
  prevent_eti<
    Super,
    hashed_index_node_impl<
      typename boost::detail::allocator::rebind_to<
        typename Super::allocator_type,
        char
      >::type
    >
  >::type
{
  typedef typename prevent_eti<
    Super,
    hashed_index_node_impl<
      typename boost::detail::allocator::rebind_to<
        typename Super::allocator_type,
        char
      >::type
    >
  >::type impl_type;
};

template<typename Super>
struct hashed_index_node:Super,hashed_index_node_trampoline<Super>
{
private:
  typedef hashed_index_node_trampoline<Super> trampoline;

public:
  typedef typename trampoline::impl_type     impl_type;
  typedef typename trampoline::pointer       impl_pointer;
  typedef typename trampoline::const_pointer const_impl_pointer;

  impl_pointer impl()
  {
    return static_cast<impl_pointer>(
      static_cast<impl_type*>(static_cast<trampoline*>(this)));
  }

  const_impl_pointer impl()const
  {
    return static_cast<const_impl_pointer>(
      static_cast<const impl_type*>(static_cast<const trampoline*>(this)));
  }

  static hashed_index_node* from_impl(impl_pointer x)
  {
    return static_cast<hashed_index_node*>(
      static_cast<trampoline*>(&*x));
  }

  static const hashed_index_node* from_impl(const_impl_pointer x)
  {
    return static_cast<const hashed_index_node*>(
      static_cast<const trampoline*>(&*x));
  }

  static void increment(
    hashed_index_node*& x,impl_pointer bbegin,impl_pointer bend)
  {
    impl_pointer xi=x->impl();
    trampoline::increment(xi,bbegin,bend);
    x=from_impl(xi);
  }
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

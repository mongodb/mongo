/* Copyright 2003-2006 Joaquín M López Muñoz.
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
#include <functional>

namespace boost{

namespace multi_index{

namespace detail{

/* singly-linked node for use by hashed_index */

struct hashed_index_node_impl
{
  hashed_index_node_impl*& next(){return next_;}
  hashed_index_node_impl*  next()const{return next_;}

  /* algorithmic stuff */

  static void increment(
    hashed_index_node_impl*& x,
    hashed_index_node_impl* bbegin,hashed_index_node_impl* bbend)
  {
    std::less_equal<hashed_index_node_impl*> leq;

    x=x->next();
    if(leq(bbegin,x)&&leq(x,bbend)){ /* bucket node */
      do{
        ++x;
      }while(x->next()==x);
      x=x->next();
    }
  }

  static void link(
    hashed_index_node_impl* x,hashed_index_node_impl* pos)
  {
    x->next()=pos->next();
    pos->next()=x;
  };

  static void unlink(hashed_index_node_impl* x)
  {
    hashed_index_node_impl* y=x->next();
    while(y->next()!=x){y=y->next();}
    y->next()=x->next();
  }

  static hashed_index_node_impl* prev(hashed_index_node_impl* x)
  {
    hashed_index_node_impl* y=x->next();
    while(y->next()!=x){y=y->next();}
    return y;
  }

  static void unlink_next(hashed_index_node_impl* x)
  {
    x->next()=x->next()->next();
  }

private:
  hashed_index_node_impl* next_;
};

template<typename Super>
struct hashed_index_node_trampoline:hashed_index_node_impl{};

template<typename Super>
struct hashed_index_node:Super,hashed_index_node_trampoline<Super>
{
  hashed_index_node_impl*       impl()
    {return static_cast<impl_type*>(this);}
  const hashed_index_node_impl* impl()const
    {return static_cast<const impl_type*>(this);}

  static hashed_index_node* from_impl(hashed_index_node_impl *x)
    {return static_cast<hashed_index_node*>(static_cast<impl_type*>(x));}
  static const hashed_index_node* from_impl(const hashed_index_node_impl* x)
  {
    return static_cast<const hashed_index_node*>(
      static_cast<const impl_type*>(x));
  }

  static void increment(
    hashed_index_node*& x,
    hashed_index_node_impl* bbegin,hashed_index_node_impl* bend)
  {
    hashed_index_node_impl* xi=x->impl();
    impl_type::increment(xi,bbegin,bend);
    x=from_impl(xi);
  }

private:
  typedef hashed_index_node_trampoline<Super> impl_type;
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

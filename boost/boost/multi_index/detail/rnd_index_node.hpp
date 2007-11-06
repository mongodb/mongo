/* Copyright 2003-2006 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_RND_INDEX_NODE_HPP
#define BOOST_MULTI_INDEX_DETAIL_RND_INDEX_NODE_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <algorithm>
#include <boost/math/common_factor_rt.hpp>
#include <cstddef>
#include <functional>

namespace boost{

namespace multi_index{

namespace detail{

struct random_access_index_node_impl
{
  random_access_index_node_impl**& up(){return up_;}
  random_access_index_node_impl**  up()const{return up_;}

  /* interoperability with rnd_node_iterator */

  static void increment(random_access_index_node_impl*& x)
  {
    x=*(x->up()+1);
  }

  static void decrement(random_access_index_node_impl*& x)
  {
    x=*(x->up()-1);
  }

  static void advance(
    random_access_index_node_impl*& x,std::ptrdiff_t n)
  {
    x=*(x->up()+n);
  }

  static std::ptrdiff_t distance(
    random_access_index_node_impl* x,random_access_index_node_impl* y)
  {
    return y->up()-x->up();
  }

  /* algorithmic stuff */

  static void relocate(
    random_access_index_node_impl** pos,
    random_access_index_node_impl** x)
  {
    random_access_index_node_impl* n=*x;
    if(x<pos){
      extract(x,pos);
      *(pos-1)=n;
      n->up()=pos-1;
    }
    else{
      while(x!=pos){
        *x=*(x-1);
        (*x)->up()=x;
        --x;
      }
      *pos=n;
      n->up()=pos;
    }
  };

  static void relocate(
    random_access_index_node_impl** pos,
    random_access_index_node_impl** first,
    random_access_index_node_impl** last)
  {
    random_access_index_node_impl** begin,**middle,**end;
    if(pos<first){
      begin=pos;
      middle=first;
      end=last;
    }
    else{
      begin=first;
      middle=last;
      end=pos;
    }

    std::ptrdiff_t n=end-begin;
    std::ptrdiff_t m=middle-begin;
    std::ptrdiff_t n_m=n-m;
    std::ptrdiff_t p=math::gcd(n,m);

    for(std::ptrdiff_t i=0;i<p;++i){
      random_access_index_node_impl* tmp=begin[i];
      for(std::ptrdiff_t j=i,k;;){
        if(j<n_m)k=j+m;
        else     k=j-n_m;
        if(k==i){
          begin[j]=tmp;
          begin[j]->up()=&begin[j];
          break;
        }
        else{
          begin[j]=begin[k];
          begin[j]->up()=&begin[j];
        }

        if(k<n_m)j=k+m;
        else     j=k-n_m;
        if(j==i){
          begin[k]=tmp;
          begin[k]->up()=&begin[k];
          break;
        }
        else{
          begin[k]=begin[j];
          begin[k]->up()=&begin[k];
        }
      }
    }
  };

  static void extract(
    random_access_index_node_impl** x,
    random_access_index_node_impl** pend)
  {
    --pend;
    while(x!=pend){
      *x=*(x+1);
      (*x)->up()=x;
      ++x;
    }
  }

  static void transfer(
    random_access_index_node_impl** pbegin0,
    random_access_index_node_impl** pend0,
    random_access_index_node_impl** pbegin1)
  {
    while(pbegin0!=pend0){
      *pbegin1=*pbegin0++;
      (*pbegin1)->up()=pbegin1;
      ++pbegin1;
    }
  }

  static void reverse(
    random_access_index_node_impl** pbegin,
    random_access_index_node_impl** pend)
  {
    std::ptrdiff_t d=(pend-pbegin)/2;
    for(std::ptrdiff_t i=0;i<d;++i){
      std::swap(*pbegin,*--pend);
      (*pbegin)->up()=pbegin;
      (*pend)->up()=pend;
      ++pbegin;
    }
  }

private:
  random_access_index_node_impl** up_;
};

template<typename Super>
struct random_access_index_node_trampoline:random_access_index_node_impl{};

template<typename Super>
struct random_access_index_node:
  Super,random_access_index_node_trampoline<Super>
{
  random_access_index_node_impl**& up(){return impl_type::up();}
  random_access_index_node_impl**  up()const{return impl_type::up();}

  random_access_index_node_impl*       impl()
    {return static_cast<impl_type*>(this);}
  const random_access_index_node_impl* impl()const
    {return static_cast<const impl_type*>(this);}

  static random_access_index_node* from_impl(random_access_index_node_impl *x)
  {
    return static_cast<random_access_index_node*>(
      static_cast<impl_type*>(x));
  }

  static const random_access_index_node* from_impl(
    const random_access_index_node_impl* x)
  {
    return static_cast<const random_access_index_node*>(
      static_cast<const impl_type*>(x));
  }

  /* interoperability with rnd_node_iterator */

  static void increment(random_access_index_node*& x)
  {
    random_access_index_node_impl* xi=x->impl();
    impl_type::increment(xi);
    x=from_impl(xi);
  }

  static void decrement(random_access_index_node*& x)
  {
    random_access_index_node_impl* xi=x->impl();
    impl_type::decrement(xi);
    x=from_impl(xi);
  }

  static void advance(random_access_index_node*& x,std::ptrdiff_t n)
  {
    random_access_index_node_impl* xi=x->impl();
    impl_type::advance(xi,n);
    x=from_impl(xi);
  }

  static std::ptrdiff_t distance(
    random_access_index_node* x,random_access_index_node* y)
  {
    return impl_type::distance(x->impl(),y->impl());
  }

private:
  typedef random_access_index_node_trampoline<Super> impl_type;
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

/* Copyright 2003-2005 Joaquín M López Muñoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_ITER_ADAPTOR_HPP
#define BOOST_MULTI_INDEX_DETAIL_ITER_ADAPTOR_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <boost/mpl/apply.hpp>
#include <boost/multi_index/detail/prevent_eti.hpp>
#include <boost/operators.hpp>

namespace boost{

namespace multi_index{

namespace detail{

/* Poor man's version of boost::iterator_adaptor. Used instead of the
 * original as compile times for the latter are significantly higher.
 * The interface is not replicated exactly, only to the extent necessary
 * for internal consumption.
 */

class iter_adaptor_access
{
public:
  template<class Class>
    static typename Class::reference dereference(const Class& x)
  {
    return x.dereference();
  }

  template<class Class>
  static bool equal(const Class& x,const Class& y)
  {
    return x.equal(y);
  }

  template<class Class>
  static void increment(Class& x)
  {
    x.increment();
  }

  template<class Class>
  static void decrement(Class& x)
  {
    x.decrement();
  }

  template<class Class>
  static void advance(Class& x,typename Class::difference_type n)
  {
    x.advance(n);
  }

  template<class Class>
  static typename Class::difference_type distance_to(
    const Class& x,const Class& y)
  {
    return x.distance_to(y);
  }
};

template<typename Category>
struct iter_adaptor_selector;

template<class Derived,class Base>
class forward_iter_adaptor_base:
  public forward_iterator_helper<
    Derived,
    typename Base::value_type,
    typename Base::difference_type,
    typename Base::pointer,
    typename Base::reference>
{
public:
  typedef typename Base::reference reference;

  reference operator*()const
  {
    return iter_adaptor_access::dereference(final());
  }

  friend bool operator==(const Derived& x,const Derived& y)
  {
    return iter_adaptor_access::equal(x,y);
  }

  Derived& operator++()
  {
    iter_adaptor_access::increment(final());
    return final();
  }

private:
  Derived& final(){return *static_cast<Derived*>(this);}
  const Derived& final()const{return *static_cast<const Derived*>(this);}
};

template<>
struct iter_adaptor_selector<std::forward_iterator_tag>
{
  template<class Derived,class Base>
  struct apply
  {
    typedef forward_iter_adaptor_base<Derived,Base> type;
  };
};

template<class Derived,class Base>
class bidirectional_iter_adaptor_base:
  public bidirectional_iterator_helper<
    Derived,
    typename Base::value_type,
    typename Base::difference_type,
    typename Base::pointer,
    typename Base::reference>
{
public:
  typedef typename Base::reference reference;

  reference operator*()const
  {
    return iter_adaptor_access::dereference(final());
  }

  friend bool operator==(const Derived& x,const Derived& y)
  {
    return iter_adaptor_access::equal(x,y);
  }

  Derived& operator++()
  {
    iter_adaptor_access::increment(final());
    return final();
  }

  Derived& operator--()
  {
    iter_adaptor_access::decrement(final());
    return final();
  }

private:
  Derived& final(){return *static_cast<Derived*>(this);}
  const Derived& final()const{return *static_cast<const Derived*>(this);}
};

template<>
struct iter_adaptor_selector<std::bidirectional_iterator_tag>
{
  template<class Derived,class Base>
  struct apply
  {
    typedef bidirectional_iter_adaptor_base<Derived,Base> type;
  };
};

template<class Derived,class Base>
class random_access_iter_adaptor_base:
  public random_access_iterator_helper<
    Derived,
    typename Base::value_type,
    typename Base::difference_type,
    typename Base::pointer,
    typename Base::reference>
{
public:
  typedef typename Base::reference       reference;
  typedef typename Base::difference_type difference_type;

  reference operator*()const
  {
    return iter_adaptor_access::dereference(final());
  }

  friend bool operator==(const Derived& x,const Derived& y)
  {
    return iter_adaptor_access::equal(x,y);
  }

  friend bool operator<(const Derived& x,const Derived& y)
  {
    return iter_adaptor_access::distance_to(x,y)>0;
  }

  Derived& operator++()
  {
    iter_adaptor_access::increment(final());
    return final();
  }

  Derived& operator--()
  {
    iter_adaptor_access::decrement(final());
    return final();
  }

  Derived& operator+=(difference_type n)
  {
    iter_adaptor_access::advance(final(),n);
    return final();
  }

  Derived& operator-=(difference_type n)
  {
    iter_adaptor_access::advance(final(),-n);
    return final();
  }

  friend difference_type operator-(const Derived& x,const Derived& y)
  {
    return iter_adaptor_access::distance_to(y,x);
  }

private:
  Derived& final(){return *static_cast<Derived*>(this);}
  const Derived& final()const{return *static_cast<const Derived*>(this);}
};

template<>
struct iter_adaptor_selector<std::random_access_iterator_tag>
{
  template<class Derived,class Base>
  struct apply
  {
    typedef random_access_iter_adaptor_base<Derived,Base> type;
  };
};

template<class Derived,class Base>
struct iter_adaptor_base
{
  typedef iter_adaptor_selector<
    typename Base::iterator_category>        selector;
  typedef typename prevent_eti<
    selector,
    typename mpl::apply2<
      selector,Derived,Base>::type
  >::type                                    type;
};

template<class Derived,class Base>
class iter_adaptor:public iter_adaptor_base<Derived,Base>::type
{
protected:
  iter_adaptor(){}
  explicit iter_adaptor(const Base& b_):b(b_){}

  const Base& base_reference()const{return b;}
  Base&       base_reference(){return b;}

private:
  Base b;
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif

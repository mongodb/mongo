/* Copyright 2003-2008 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org/libs/multi_index for library home page.
 */

#ifndef BOOST_MULTI_INDEX_DETAIL_SAFE_CTR_PROXY_HPP
#define BOOST_MULTI_INDEX_DETAIL_SAFE_CTR_PROXY_HPP

#if defined(_MSC_VER)&&(_MSC_VER>=1200)
#pragma once
#endif

#if defined(BOOST_MULTI_INDEX_ENABLE_SAFE_MODE)
#include <boost/config.hpp> /* keep it first to prevent nasty warns in MSVC */
#include <boost/detail/workaround.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC,<1300)
#include <boost/multi_index/detail/safe_mode.hpp>

namespace boost{

namespace multi_index{

namespace detail{

/* A safe iterator is instantiated in the form
 * safe_iterator<Iterator,Container>: MSVC++ 6.0 has serious troubles with
 * the resulting symbols names, given that index names (which stand for
 * Container) are fairly long themselves. safe_ctr_proxy does not statically
 * depend on Container, and provides the necessary methods (begin and end) to
 * the safe mode framework via an abstract interface. With safe_ctr_proxy,
 * instead of deriving from safe_container<Container> the following base class
 * must be used:
 *
 *   safe_ctr_proxy_impl<Iterator,Container>
 *
 * where Iterator is the type of the *unsafe* iterator being wrapped.
 * The corresponding safe iterator instantiation is then
 * 
 *   safe_iterator<Iterator,safe_ctr_proxy<Iterator> >,
 *
 * which does not include the name of Container.
 */

template<typename Iterator>
class safe_ctr_proxy:
  public safe_mode::safe_container<safe_ctr_proxy<Iterator> >
{
public:
  typedef safe_mode::safe_iterator<Iterator,safe_ctr_proxy> iterator;
  typedef iterator                                          const_iterator;

  iterator       begin(){return begin_impl();}
  const_iterator begin()const{return begin_impl();}
  iterator       end(){return end_impl();}
  const_iterator end()const{return end_impl();}

protected:
  virtual iterator       begin_impl()=0;
  virtual const_iterator begin_impl()const=0;
  virtual iterator       end_impl()=0;
  virtual const_iterator end_impl()const=0;
};

template<typename Iterator,typename Container>
class safe_ctr_proxy_impl:public safe_ctr_proxy<Iterator>
{
  typedef safe_ctr_proxy<Iterator> super;
  typedef Container                container_type;

public:
  typedef typename super::iterator       iterator;
  typedef typename super::const_iterator const_iterator;

  virtual iterator       begin_impl(){return container().begin();}
  virtual const_iterator begin_impl()const{return container().begin();}
  virtual iterator       end_impl(){return container().end();}
  virtual const_iterator end_impl()const{return container().end();}

private:
  container_type& container()
  {
    return *static_cast<container_type*>(this);
  }
  
  const container_type& container()const
  {
    return *static_cast<const container_type*>(this);
  }
};

} /* namespace multi_index::detail */

} /* namespace multi_index */

} /* namespace boost */

#endif /* workaround */

#endif /* BOOST_MULTI_INDEX_ENABLE_SAFE_MODE */

#endif

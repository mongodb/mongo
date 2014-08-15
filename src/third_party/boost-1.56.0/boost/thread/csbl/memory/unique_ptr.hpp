// Copyright (C) 2013 Vicente J. Botet Escriba
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// 2013/10 Vicente J. Botet Escriba
//   Creation.

#ifndef BOOST_CSBL_MEMORY_UNIQUE_PTR_HPP
#define BOOST_CSBL_MEMORY_UNIQUE_PTR_HPP

#include <boost/thread/csbl/memory/config.hpp>

// 20.8.1 class template unique_ptr:
#if defined BOOST_NO_CXX11_SMART_PTR
#include <boost/thread/csbl/memory/default_delete.hpp>

#include <boost/interprocess/smart_ptr/unique_ptr.hpp>

namespace boost
{
  namespace csbl
  {
    template <class T, class D = default_delete<T> > class unique_ptr :
    public ::boost::interprocess::unique_ptr<T,D>
    {
      typedef ::boost::interprocess::unique_ptr<T,D> base_type;
      BOOST_MOVABLE_BUT_NOT_COPYABLE(unique_ptr)
    protected:
      //typedef typename base_type::nat nat;
      //typedef typename base_type::nullptr_t nullptr_t;
      struct nat  {int for_bool;};
      struct nat2 {int for_nullptr;};
      typedef int nat2::*nullptr_t;

    public:
      typedef typename base_type::element_type element_type;
      typedef typename base_type::deleter_type deleter_type;
      typedef typename base_type::pointer pointer;

      unique_ptr() : base_type()
      {}
      explicit unique_ptr(pointer p): base_type(p)
      {}
      unique_ptr(pointer p
          ,typename interprocess::ipcdetail::if_<interprocess::ipcdetail::is_reference<D>
          ,D
          ,typename interprocess::ipcdetail::add_reference<const D>::type>::type d)
      : base_type(p, d)
      {}
      unique_ptr(BOOST_RV_REF(unique_ptr) u)
      : base_type(boost::move(static_cast<base_type&>(u)))
      {}
      template <class U, class E>
      unique_ptr(BOOST_RV_REF_BEG unique_ptr<U, E> BOOST_RV_REF_END u,
          typename interprocess::ipcdetail::enable_if_c<
          interprocess::ipcdetail::is_convertible<typename unique_ptr<U, E>::pointer, pointer>::value &&
          interprocess::ipcdetail::is_convertible<E, D>::value &&
          (
              !interprocess::ipcdetail::is_reference<D>::value ||
              interprocess::ipcdetail::is_same<D, E>::value
          )
          ,
          nat
          >::type = nat())
      : base_type(boost::move(static_cast< ::boost::interprocess::unique_ptr<U,E>&>(u)))
      {}
      unique_ptr& operator=(BOOST_RV_REF(unique_ptr) u)
      {
        this->base_type::operator=(boost::move(static_cast<base_type&>(u)));
        return *this;
      }
      template <class U, class E>
      unique_ptr& operator=(BOOST_RV_REF_BEG unique_ptr<U, E> BOOST_RV_REF_END u)
      {
        this->base_type::template operator=<U,E>(boost::move(static_cast< ::boost::interprocess::unique_ptr<U,E>&>(u)));
        return *this;
      }
      unique_ptr& operator=(nullptr_t t)
      {
        this->base_type::operator=(t);
        return *this;
      }
      void swap(unique_ptr& u)
      {
        this->base_type::swap(u);
      }
    };
    template <class T, class D>
    class unique_ptr<T[], D> :
      public ::boost::interprocess::unique_ptr<T[],D>
    {

    };
  }
}
#else
namespace boost
{
  namespace csbl
  {
    using ::std::unique_ptr;
  }
}
#endif // BOOST_NO_CXX11_SMART_PTR
#endif // header

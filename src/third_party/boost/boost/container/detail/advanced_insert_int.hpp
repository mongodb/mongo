//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_ADVANCED_INSERT_INT_HPP
#define BOOST_CONTAINER_ADVANCED_INSERT_INT_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

// container
#include <boost/container/allocator_traits.hpp>
// container/detail
#include <boost/container/detail/copy_move_algo.hpp>
#include <boost/container/detail/destroyers.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/iterator.hpp>
#include <boost/container/detail/iterators.hpp>
#include <boost/move/detail/iterator_to_raw_pointer.hpp>
#if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
#include <boost/move/detail/fwd_macros.hpp>
#endif
// move

#include <boost/move/utility_core.hpp>
#include <boost/move/detail/force_ptr.hpp>
#include <boost/move/detail/launder.hpp>
// other
#include <boost/assert.hpp>

namespace boost { namespace container { namespace dtl {

template<class Allocator, class FwdIt>
struct move_insert_range_proxy
{
   typedef typename allocator_traits<Allocator>::value_type value_type;

   inline explicit move_insert_range_proxy(FwdIt first)
      :  first_(first)
   {}

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n)
   {
      this->first_ = ::boost::container::uninitialized_move_alloc_n_source
         (a, this->first_, n, p);
   }

   template<class Iterator>
   inline void copy_n_and_update(Allocator &, Iterator p, std::size_t n)
   {
      this->first_ = ::boost::container::move_n_source(this->first_, n, p);
   }

   FwdIt first_;
};


template<class Allocator, class FwdIt>
struct insert_range_proxy
{
   typedef typename allocator_traits<Allocator>::value_type value_type;

   inline explicit insert_range_proxy(FwdIt first)
      :  first_(first)
   {}

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n)
   {
      this->first_ = ::boost::container::uninitialized_copy_alloc_n_source(a, this->first_, n, p);
   }

   template<class Iterator>
   inline void copy_n_and_update(Allocator &, Iterator p, std::size_t n)
   {
      this->first_ = ::boost::container::copy_n_source(this->first_, n, p);
   }

   FwdIt first_;
};


template<class Allocator>
struct insert_n_copies_proxy
{
   typedef typename allocator_traits<Allocator>::value_type value_type;

   inline explicit insert_n_copies_proxy(const value_type &v)
      :  v_(v)
   {}

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n) const
   {  boost::container::uninitialized_fill_alloc_n(a, v_, n, p);  }

   template<class Iterator>
   inline void copy_n_and_update(Allocator &, Iterator p, std::size_t n) const
   {
      while (n){
         --n;
         *p = v_;
         ++p;
      }
   }

   const value_type &v_;
};

template<class Allocator>
struct insert_value_initialized_n_proxy
{
   typedef ::boost::container::allocator_traits<Allocator> alloc_traits;
   typedef typename allocator_traits<Allocator>::value_type value_type;
   typedef typename dtl::aligned_storage<sizeof(value_type), dtl::alignment_of<value_type>::value>::type storage_t;

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n) const
   {  boost::container::uninitialized_value_init_alloc_n(a, n, p);  }

   template<class Iterator>
   void copy_n_and_update(Allocator &a, Iterator p, std::size_t n) const
   {
      while (n){
         --n;
         storage_t v;
         alloc_traits::construct(a, (value_type*)&v);
         value_type *vp = move_detail::launder_cast<value_type *>(&v);
         value_destructor<Allocator> on_exit(a, *vp); (void)on_exit;
         *p = ::boost::move(*vp);
         ++p;
      }
   }
};

template<class Allocator>
struct insert_default_initialized_n_proxy
{
   typedef ::boost::container::allocator_traits<Allocator> alloc_traits;
   typedef typename allocator_traits<Allocator>::value_type value_type;
   typedef typename dtl::aligned_storage<sizeof(value_type), dtl::alignment_of<value_type>::value>::type storage_t;

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n) const
   {  boost::container::uninitialized_default_init_alloc_n(a, n, p);  }

   template<class Iterator>
   void copy_n_and_update(Allocator &a, Iterator p, std::size_t n) const
   {
      if(!is_pod<value_type>::value){
         while (n){
            --n;
            typename dtl::aligned_storage<sizeof(value_type), dtl::alignment_of<value_type>::value>::type v;
            alloc_traits::construct(a, (value_type*)&v, default_init);
            value_type *vp = move_detail::launder_cast<value_type *>(&v);
            value_destructor<Allocator> on_exit(a, *vp); (void)on_exit;
            *p = ::boost::move(*vp);
            ++p;
         }
      }
   }
};

template<class Allocator>
struct insert_copy_proxy
{
   typedef boost::container::allocator_traits<Allocator> alloc_traits;
   typedef typename alloc_traits::value_type value_type;

   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_copy_proxy(const value_type &v)
      :  v_(v)
   {}

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n) const
   {
      BOOST_ASSERT(n == 1);  (void)n;
      alloc_traits::construct( a, boost::movelib::iterator_to_raw_pointer(p), v_);
   }

   template<class Iterator>
   inline void copy_n_and_update(Allocator &, Iterator p, std::size_t n) const
   {
      BOOST_ASSERT(n == 1);  (void)n;
      *p = v_;
   }

   const value_type &v_;
};


template<class Allocator>
struct insert_move_proxy
{
   typedef boost::container::allocator_traits<Allocator> alloc_traits;
   typedef typename alloc_traits::value_type value_type;

   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_move_proxy(value_type &v)
      :  v_(v)
   {}

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n) const
   {
      BOOST_ASSERT(n == 1);  (void)n;
      alloc_traits::construct( a, boost::movelib::iterator_to_raw_pointer(p), ::boost::move(v_) );
   }

   template<class Iterator>
   inline void copy_n_and_update(Allocator &, Iterator p, std::size_t n) const
   {
      BOOST_ASSERT(n == 1);  (void)n;
      *p = ::boost::move(v_);
   }

   value_type &v_;
};

template<class It, class Allocator>
inline insert_move_proxy<Allocator> get_insert_value_proxy(BOOST_RV_REF(typename boost::container::iterator_traits<It>::value_type) v)
{
   return insert_move_proxy<Allocator>(v);
}

template<class It, class Allocator>
inline insert_copy_proxy<Allocator> get_insert_value_proxy(const typename boost::container::iterator_traits<It>::value_type &v)
{
   return insert_copy_proxy<Allocator>(v);
}

}}}   //namespace boost { namespace container { namespace dtl {

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

#include <boost/container/detail/variadic_templates_tools.hpp>
#include <boost/move/utility_core.hpp>

namespace boost {
namespace container {
namespace dtl {

template<class Allocator, class ...Args>
struct insert_nonmovable_emplace_proxy
{
   typedef boost::container::allocator_traits<Allocator>   alloc_traits;
   typedef typename alloc_traits::value_type       value_type;
   typedef typename build_number_seq<sizeof...(Args)>::type index_tuple_t;

   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_nonmovable_emplace_proxy(BOOST_FWD_REF(Args)... args)
      : args_(args...)
   {}

   template<class Iterator>
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n)
   {  this->priv_uninitialized_copy_some_and_update(a, index_tuple_t(), p, n);  }

   private:
   template<std::size_t ...IdxPack, class Iterator>
   inline void priv_uninitialized_copy_some_and_update(Allocator &a, const index_tuple<IdxPack...>&, Iterator p, std::size_t n)
   {
      BOOST_ASSERT(n == 1); (void)n;
      alloc_traits::construct( a, boost::movelib::iterator_to_raw_pointer(p), ::boost::forward<Args>(get<IdxPack>(this->args_))... );
   }

   protected:
   tuple<Args&...> args_;
};

template<class Allocator, class ...Args>
struct insert_emplace_proxy
   :  public insert_nonmovable_emplace_proxy<Allocator, Args...>
{
   typedef insert_nonmovable_emplace_proxy<Allocator, Args...> base_t;
   typedef boost::container::allocator_traits<Allocator>   alloc_traits;
   typedef typename base_t::value_type             value_type;
   typedef typename base_t::index_tuple_t          index_tuple_t;

   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy(BOOST_FWD_REF(Args)... args)
      : base_t(::boost::forward<Args>(args)...)
   {}

   template<class Iterator>
   inline void copy_n_and_update(Allocator &a, Iterator p, std::size_t n)
   {  this->priv_copy_some_and_update(a, index_tuple_t(), p, n);  }

   private:

   template<std::size_t ...IdxPack, class Iterator>
   inline void priv_copy_some_and_update(Allocator &a, const index_tuple<IdxPack...>&, Iterator p, std::size_t n)
   {
      BOOST_ASSERT(n ==1); (void)n;
      typename dtl::aligned_storage<sizeof(value_type), dtl::alignment_of<value_type>::value>::type v;
      alloc_traits::construct(a, (value_type*)&v, ::boost::forward<Args>(get<IdxPack>(this->args_))...);
      value_type *vp = move_detail::launder_cast<value_type *>(&v);
      BOOST_CONTAINER_TRY{
         *p = ::boost::move(*vp);
      }
      BOOST_CONTAINER_CATCH(...){
         alloc_traits::destroy(a, vp);
         BOOST_CONTAINER_RETHROW
      }
      BOOST_CONTAINER_CATCH_END
      alloc_traits::destroy(a, vp);
   }
};

//Specializations to avoid an unneeded temporary when emplacing from a single argument o type value_type
template<class Allocator>
struct insert_emplace_proxy<Allocator, typename boost::container::allocator_traits<Allocator>::value_type>
   : public insert_move_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy(typename boost::container::allocator_traits<Allocator>::value_type &&v)
   : insert_move_proxy<Allocator>(v)
   {}
};

//We use "add_const" here as adding "const" only confuses MSVC12(and maybe later) provoking
//compiler error C2752 ("more than one partial specialization matches").
//Any problem is solvable with an extra layer of indirection? ;-)
template<class Allocator>
struct insert_emplace_proxy<Allocator
   , typename boost::container::dtl::add_const<typename boost::container::allocator_traits<Allocator>::value_type>::type
   >
   : public insert_copy_proxy<Allocator>
{

   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy(const typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_copy_proxy<Allocator>(v)
   {}
};

template<class Allocator>
struct insert_emplace_proxy<Allocator, typename boost::container::allocator_traits<Allocator>::value_type &>
   : public insert_copy_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy(const typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_copy_proxy<Allocator>(v)
   {}
};

template<class Allocator>
struct insert_emplace_proxy<Allocator
   , typename boost::container::dtl::add_const<typename boost::container::allocator_traits<Allocator>::value_type>::type &
   >
   : public insert_copy_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy(const typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_copy_proxy<Allocator>(v)
   {}
};

}}}   //namespace boost { namespace container { namespace dtl {

#else // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

#include <boost/container/detail/value_init.hpp>

namespace boost {
namespace container {
namespace dtl {

#define BOOST_CONTAINER_ADVANCED_INSERT_INT_CODE(N) \
template< class Allocator BOOST_MOVE_I##N BOOST_MOVE_CLASS##N >\
struct insert_nonmovable_emplace_proxy##N\
{\
   typedef boost::container::allocator_traits<Allocator> alloc_traits;\
   typedef typename alloc_traits::value_type value_type;\
   \
   BOOST_STATIC_CONSTEXPR bool single_value = true;\
   \
   inline explicit insert_nonmovable_emplace_proxy##N(BOOST_MOVE_UREF##N)\
      BOOST_MOVE_COLON##N BOOST_MOVE_FWD_INIT##N {}\
   \
   template<class Iterator>\
   inline void uninitialized_copy_n_and_update(Allocator &a, Iterator p, std::size_t n)\
   {\
      BOOST_ASSERT(n == 1); (void)n;\
      alloc_traits::construct(a, boost::movelib::iterator_to_raw_pointer(p) BOOST_MOVE_I##N BOOST_MOVE_MFWD##N);\
   }\
   \
   template<class Iterator>\
   inline void copy_n_and_update(Allocator &, Iterator, std::size_t)\
   {  BOOST_ASSERT(false);   }\
   \
   protected:\
   BOOST_MOVE_MREF##N\
};\
\
template< class Allocator BOOST_MOVE_I##N BOOST_MOVE_CLASS##N >\
struct insert_emplace_proxy_arg##N\
   : insert_nonmovable_emplace_proxy##N< Allocator BOOST_MOVE_I##N BOOST_MOVE_TARG##N >\
{\
   typedef insert_nonmovable_emplace_proxy##N\
      < Allocator BOOST_MOVE_I##N BOOST_MOVE_TARG##N > base_t;\
   typedef typename base_t::value_type value_type;\
   typedef boost::container::allocator_traits<Allocator> alloc_traits;\
   \
   BOOST_STATIC_CONSTEXPR bool single_value = true;\
   \
   inline explicit insert_emplace_proxy_arg##N(BOOST_MOVE_UREF##N)\
      : base_t(BOOST_MOVE_FWD##N){}\
   \
   template<class Iterator>\
   inline void copy_n_and_update(Allocator &a, Iterator p, std::size_t n)\
   {\
      BOOST_ASSERT(n == 1); (void)n;\
      typename dtl::aligned_storage<sizeof(value_type), dtl::alignment_of<value_type>::value>::type v;\
      alloc_traits::construct(a, (value_type*)&v BOOST_MOVE_I##N BOOST_MOVE_MFWD##N);\
      value_type *vp = move_detail::launder_cast<value_type *>(&v);\
      BOOST_CONTAINER_TRY{\
         *p = ::boost::move(*vp);\
      }\
      BOOST_CONTAINER_CATCH(...){\
         alloc_traits::destroy(a, vp);\
         BOOST_CONTAINER_RETHROW\
      }\
      BOOST_CONTAINER_CATCH_END\
      alloc_traits::destroy(a, vp);\
   }\
};\
//
BOOST_MOVE_ITERATE_0TO9(BOOST_CONTAINER_ADVANCED_INSERT_INT_CODE)
#undef BOOST_CONTAINER_ADVANCED_INSERT_INT_CODE

#if defined(BOOST_NO_CXX11_RVALUE_REFERENCES)

//Specializations to avoid an unneeded temporary when emplacing from a single argument o type value_type
template<class Allocator>
struct insert_emplace_proxy_arg1<Allocator, ::boost::rv<typename boost::container::allocator_traits<Allocator>::value_type> >
   : public insert_move_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy_arg1(typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_move_proxy<Allocator>(v)
   {}
};

template<class Allocator>
struct insert_emplace_proxy_arg1<Allocator, typename boost::container::allocator_traits<Allocator>::value_type>
   : public insert_copy_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy_arg1(const typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_copy_proxy<Allocator>(v)
   {}
};

#else //e.g. MSVC10 & MSVC11

//Specializations to avoid an unneeded temporary when emplacing from a single argument o type value_type
template<class Allocator>
struct insert_emplace_proxy_arg1<Allocator, typename boost::container::allocator_traits<Allocator>::value_type>
   : public insert_move_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy_arg1(typename boost::container::allocator_traits<Allocator>::value_type &&v)
   : insert_move_proxy<Allocator>(v)
   {}
};

//We use "add_const" here as adding "const" only confuses MSVC10&11 provoking
//compiler error C2752 ("more than one partial specialization matches").
//Any problem is solvable with an extra layer of indirection? ;-)
template<class Allocator>
struct insert_emplace_proxy_arg1<Allocator
   , typename boost::container::dtl::add_const<typename boost::container::allocator_traits<Allocator>::value_type>::type
   >
   : public insert_copy_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy_arg1(const typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_copy_proxy<Allocator>(v)
   {}
};

template<class Allocator>
struct insert_emplace_proxy_arg1<Allocator, typename boost::container::allocator_traits<Allocator>::value_type &>
   : public insert_copy_proxy<Allocator>
{
   BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy_arg1(const typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_copy_proxy<Allocator>(v)
   {}
};

template<class Allocator>
struct insert_emplace_proxy_arg1<Allocator
   , typename boost::container::dtl::add_const<typename boost::container::allocator_traits<Allocator>::value_type>::type &
   >
   : public insert_copy_proxy<Allocator>
{
  BOOST_STATIC_CONSTEXPR bool single_value = true;

   inline explicit insert_emplace_proxy_arg1(const typename boost::container::allocator_traits<Allocator>::value_type &v)
   : insert_copy_proxy<Allocator>(v)
   {}
};

#endif

}}}   //namespace boost { namespace container { namespace dtl {

#endif   // !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)

#include <boost/container/detail/config_end.hpp>

#endif //#ifndef BOOST_CONTAINER_ADVANCED_INSERT_INT_HPP

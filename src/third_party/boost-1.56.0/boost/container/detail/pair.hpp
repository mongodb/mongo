//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013.
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_CONTAINER_DETAIL_PAIR_HPP
#define BOOST_CONTAINER_CONTAINER_DETAIL_PAIR_HPP

#if defined(_MSC_VER)
#  pragma once
#endif

#include <boost/container/detail/config_begin.hpp>
#include <boost/container/detail/workaround.hpp>

#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/type_traits.hpp>
#include <boost/container/detail/mpl.hpp>
#include <boost/container/detail/type_traits.hpp>

#include <utility>   //std::pair
#include <algorithm> //std::swap

#include <boost/move/utility.hpp>
#include <boost/type_traits/is_class.hpp>

#ifndef BOOST_CONTAINER_PERFECT_FORWARDING
#include <boost/container/detail/preprocessor.hpp>
#endif

namespace boost {
namespace container {
namespace container_detail {

template <class T1, class T2>
struct pair;

template <class T>
struct is_pair
{
   static const bool value = false;
};

template <class T1, class T2>
struct is_pair< pair<T1, T2> >
{
   static const bool value = true;
};

template <class T1, class T2>
struct is_pair< std::pair<T1, T2> >
{
   static const bool value = true;
};

struct pair_nat;

struct piecewise_construct_t { };
static const piecewise_construct_t piecewise_construct = piecewise_construct_t();

/*
template <class T1, class T2>
struct pair
{
    template <class U, class V> pair(pair<U, V>&& p);
    template <class... Args1, class... Args2>
        pair(piecewise_construct_t, tuple<Args1...> first_args,
             tuple<Args2...> second_args);

    template <class U, class V> pair& operator=(const pair<U, V>& p);
    pair& operator=(pair&& p) noexcept(is_nothrow_move_assignable<T1>::value &&
                                       is_nothrow_move_assignable<T2>::value);
    template <class U, class V> pair& operator=(pair<U, V>&& p);

    void swap(pair& p) noexcept(noexcept(swap(first, p.first)) &&
                                noexcept(swap(second, p.second)));
};

template <class T1, class T2> bool operator==(const pair<T1,T2>&, const pair<T1,T2>&);
template <class T1, class T2> bool operator!=(const pair<T1,T2>&, const pair<T1,T2>&);
template <class T1, class T2> bool operator< (const pair<T1,T2>&, const pair<T1,T2>&);
template <class T1, class T2> bool operator> (const pair<T1,T2>&, const pair<T1,T2>&);
template <class T1, class T2> bool operator>=(const pair<T1,T2>&, const pair<T1,T2>&);
template <class T1, class T2> bool operator<=(const pair<T1,T2>&, const pair<T1,T2>&);
*/


template <class T1, class T2>
struct pair
{
   private:
   BOOST_COPYABLE_AND_MOVABLE(pair)

   public:
   typedef T1 first_type;
   typedef T2 second_type;

   T1 first;
   T2 second;

   //Default constructor
   pair()
      : first(), second()
   {}

   //pair copy assignment
   pair(const pair& x)
      : first(x.first), second(x.second)
   {}

   //pair move constructor
   pair(BOOST_RV_REF(pair) p)
      : first(::boost::move(p.first)), second(::boost::move(p.second))
   {}

   template <class D, class S>
   pair(const pair<D, S> &p)
      : first(p.first), second(p.second)
   {}

   template <class D, class S>
   pair(BOOST_RV_REF_BEG pair<D, S> BOOST_RV_REF_END p)
      : first(::boost::move(p.first)), second(::boost::move(p.second))
   {}

   //pair from two values
   pair(const T1 &t1, const T2 &t2)
      : first(t1)
      , second(t2)
   {}

   template<class U, class V>
   pair(BOOST_FWD_REF(U) u, BOOST_FWD_REF(V) v)
      : first(::boost::forward<U>(u))
      , second(::boost::forward<V>(v))
   {}

   //And now compatibility with std::pair
   pair(const std::pair<T1, T2>& x)
      : first(x.first), second(x.second)
   {}

   template <class D, class S>
   pair(const std::pair<D, S>& p)
      : first(p.first), second(p.second)
   {}

   pair(BOOST_RV_REF_BEG std::pair<T1, T2> BOOST_RV_REF_END p)
      : first(::boost::move(p.first)), second(::boost::move(p.second))
   {}

   template <class D, class S>
   pair(BOOST_RV_REF_BEG std::pair<D, S> BOOST_RV_REF_END p)
      : first(::boost::move(p.first)), second(::boost::move(p.second))
   {}

   //piecewise_construct missing
   //template <class U, class V> pair(pair<U, V>&& p);
   //template <class... Args1, class... Args2>
   //   pair(piecewise_construct_t, tuple<Args1...> first_args,
   //        tuple<Args2...> second_args);
/*
   //Variadic versions
   template<class U>
   pair(BOOST_CONTAINER_PP_PARAM(U, u), typename container_detail::disable_if
         < container_detail::is_pair< typename container_detail::remove_ref_const<U>::type >, pair_nat>::type* = 0)
      : first(::boost::forward<U>(u))
      , second()
   {}

   #ifdef BOOST_CONTAINER_PERFECT_FORWARDING

   template<class U, class V, class ...Args>
   pair(U &&u, V &&v)
      : first(::boost::forward<U>(u))
      , second(::boost::forward<V>(v), ::boost::forward<Args>(args)...)
   {}

   #else

   #define BOOST_PP_LOCAL_MACRO(n)                                                            \
   template<class U, BOOST_PP_ENUM_PARAMS(n, class P)>                                        \
   pair(BOOST_CONTAINER_PP_PARAM(U, u)                                                          \
       ,BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_LIST, _))                                  \
      : first(::boost::forward<U>(u))                             \
      , second(BOOST_PP_ENUM(n, BOOST_CONTAINER_PP_PARAM_FORWARD, _))                        \
   {}                                                                                         \
   //!
   #define BOOST_PP_LOCAL_LIMITS (1, BOOST_CONTAINER_MAX_CONSTRUCTOR_PARAMETERS)
   #include BOOST_PP_LOCAL_ITERATE()
   #endif
*/
   //pair copy assignment
   pair& operator=(BOOST_COPY_ASSIGN_REF(pair) p)
   {
      first  = p.first;
      second = p.second;
      return *this;
   }

   //pair move assignment
   pair& operator=(BOOST_RV_REF(pair) p)
   {
      first  = ::boost::move(p.first);
      second = ::boost::move(p.second);
      return *this;
   }

   template <class D, class S>
   typename ::boost::container::container_detail::enable_if_c
      < !(::boost::container::container_detail::is_same<T1, D>::value &&
          ::boost::container::container_detail::is_same<T2, S>::value)
      , pair &>::type
      operator=(const pair<D, S>&p)
   {
      first  = p.first;
      second = p.second;
      return *this;
   }

   template <class D, class S>
   typename ::boost::container::container_detail::enable_if_c
      < !(::boost::container::container_detail::is_same<T1, D>::value &&
          ::boost::container::container_detail::is_same<T2, S>::value)
      , pair &>::type
      operator=(BOOST_RV_REF_BEG pair<D, S> BOOST_RV_REF_END p)
   {
      first  = ::boost::move(p.first);
      second = ::boost::move(p.second);
      return *this;
   }

   //std::pair copy assignment
   pair& operator=(const std::pair<T1, T2> &p)
   {
      first  = p.first;
      second = p.second;
      return *this;
   }

   template <class D, class S>
   pair& operator=(const std::pair<D, S> &p)
   {
      first  = ::boost::move(p.first);
      second = ::boost::move(p.second);
      return *this;
   }

   //std::pair move assignment
   pair& operator=(BOOST_RV_REF_BEG std::pair<T1, T2> BOOST_RV_REF_END p)
   {
      first  = ::boost::move(p.first);
      second = ::boost::move(p.second);
      return *this;
   }

   template <class D, class S>
   pair& operator=(BOOST_RV_REF_BEG std::pair<D, S> BOOST_RV_REF_END p)
   {
      first  = ::boost::move(p.first);
      second = ::boost::move(p.second);
      return *this;
   }

   //swap
   void swap(pair& p)
   {
      using std::swap;
      swap(this->first, p.first);
      swap(this->second, p.second);
   }
};

template <class T1, class T2>
inline bool operator==(const pair<T1,T2>& x, const pair<T1,T2>& y)
{  return static_cast<bool>(x.first == y.first && x.second == y.second);  }

template <class T1, class T2>
inline bool operator< (const pair<T1,T2>& x, const pair<T1,T2>& y)
{  return static_cast<bool>(x.first < y.first ||
                         (!(y.first < x.first) && x.second < y.second)); }

template <class T1, class T2>
inline bool operator!=(const pair<T1,T2>& x, const pair<T1,T2>& y)
{  return static_cast<bool>(!(x == y));  }

template <class T1, class T2>
inline bool operator> (const pair<T1,T2>& x, const pair<T1,T2>& y)
{  return y < x;  }

template <class T1, class T2>
inline bool operator>=(const pair<T1,T2>& x, const pair<T1,T2>& y)
{  return static_cast<bool>(!(x < y)); }

template <class T1, class T2>
inline bool operator<=(const pair<T1,T2>& x, const pair<T1,T2>& y)
{  return static_cast<bool>(!(y < x)); }

template <class T1, class T2>
inline pair<T1, T2> make_pair(T1 x, T2 y)
{  return pair<T1, T2>(x, y); }

template <class T1, class T2>
inline void swap(pair<T1, T2>& x, pair<T1, T2>& y)
{
   swap(x.first, y.first);
   swap(x.second, y.second);
}

}  //namespace container_detail {
}  //namespace container {


//Without this specialization recursive flat_(multi)map instantiation fails
//because is_enum needs to instantiate the recursive pair, leading to a compilation error).
//This breaks the cycle clearly stating that pair is not an enum avoiding any instantiation.
template<class T>
struct is_enum;

template<class T, class U>
struct is_enum< ::boost::container::container_detail::pair<T, U> >
{
   static const bool value = false;
};

//This specialization is needed to avoid instantiation of pair in
//is_class, and allow recursive maps.
template <class T1, class T2>
struct is_class< ::boost::container::container_detail::pair<T1, T2> >
   : public ::boost::true_type
{};

#ifdef BOOST_NO_CXX11_RVALUE_REFERENCES

template<class T1, class T2>
struct has_move_emulation_enabled< ::boost::container::container_detail::pair<T1, T2> >
   : ::boost::true_type
{};

#endif


}  //namespace boost {

#include <boost/container/detail/config_end.hpp>

#endif   //#ifndef BOOST_CONTAINER_DETAIL_PAIR_HPP

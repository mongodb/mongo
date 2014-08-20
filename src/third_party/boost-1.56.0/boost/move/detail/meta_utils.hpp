//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2012-2012.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/move for documentation.
//
//////////////////////////////////////////////////////////////////////////////

//! \file

#ifndef BOOST_MOVE_DETAIL_META_UTILS_HPP
#define BOOST_MOVE_DETAIL_META_UTILS_HPP

#include <boost/move/detail/config_begin.hpp>

//Small meta-typetraits to support move

namespace boost {
namespace move_detail {

//if_
template<bool C, typename T1, typename T2>
struct if_c
{
   typedef T1 type;
};

template<typename T1, typename T2>
struct if_c<false,T1,T2>
{
   typedef T2 type;
};

template<typename T1, typename T2, typename T3>
struct if_
{
   typedef typename if_c<0 != T1::value, T2, T3>::type type;
};

//enable_if_
template <bool B, class T = void>
struct enable_if_c
{
   typedef T type;
};

template <class T>
struct enable_if_c<false, T> {};

template <class Cond, class T = void>
struct enable_if : public enable_if_c<Cond::value, T> {};

template <class Cond, class T = void>
struct disable_if : public enable_if_c<!Cond::value, T> {};

//integral_constant
template<class T, T v>
struct integral_constant
{
   static const T value = v;
   typedef T value_type;
   typedef integral_constant<T, v> type;
};

//identity
template <class T>
struct identity
{
   typedef T type;
};

#if defined(_MSC_VER) && (_MSC_VER >= 1400)

//use intrinsic since in MSVC
//overaligned types can't go through ellipsis
template <class T, class U>
struct is_convertible
{
   static const bool value = __is_convertible_to(T, U);
};

#else

template <class T, class U>
class is_convertible
{
   typedef char true_t;
   class false_t { char dummy[2]; };
   static false_t dispatch(...);
   static true_t  dispatch(U);
   static T &trigger();
   public:
   static const bool value = sizeof(dispatch(trigger())) == sizeof(true_t);
};

#endif

//and_ not_
template <typename Condition1, typename Condition2, typename Condition3 = integral_constant<bool, true> >
struct and_
   : public integral_constant<bool, Condition1::value && Condition2::value && Condition3::value>
{};

template <typename Boolean>
struct not_
   : public integral_constant<bool, !Boolean::value>
{};

//is_lvalue_reference
template<class T>
struct is_lvalue_reference
   : public integral_constant<bool, false>
{};

template<class T>
struct is_lvalue_reference<T&>
   : public integral_constant<bool, true>
{};

template<class T>
struct is_class_or_union
{
   struct twochar { char _[2]; };
   template <class U>
   static char is_class_or_union_tester(void(U::*)(void));
   template <class U>
   static twochar is_class_or_union_tester(...);
   static const bool value = sizeof(is_class_or_union_tester<T>(0)) == sizeof(char);
};

struct empty{};

//addressof
template<class T> struct addr_impl_ref
{
   T & v_;
   inline addr_impl_ref( T & v ): v_( v ) {}
   inline operator T& () const { return v_; }

   private:
   addr_impl_ref & operator=(const addr_impl_ref &);
};

template<class T> struct addressof_impl
{
   static inline T * f( T & v, long )
   {
      return reinterpret_cast<T*>(
         &const_cast<char&>(reinterpret_cast<const volatile char &>(v)));
   }

   static inline T * f( T * v, int )
   {  return v;  }
};

template<class T>
inline T * addressof( T & v )
{
   return ::boost::move_detail::addressof_impl<T>::f
      ( ::boost::move_detail::addr_impl_ref<T>( v ), 0 );
}

}  //namespace move_detail {
}  //namespace boost {

#include <boost/move/detail/config_end.hpp>

#endif //#ifndef BOOST_MOVE_DETAIL_META_UTILS_HPP

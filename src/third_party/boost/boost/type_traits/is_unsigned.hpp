
//  (C) Copyright John Maddock 2005.  
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.


#ifndef BOOST_TT_IS_UNSIGNED_HPP_INCLUDED
#define BOOST_TT_IS_UNSIGNED_HPP_INCLUDED

#include <boost/type_traits/is_integral.hpp>
#include <boost/type_traits/is_enum.hpp>
#include <boost/type_traits/remove_cv.hpp>
#include <boost/type_traits/detail/ice_or.hpp>

// should be the last #include
#include <boost/type_traits/detail/bool_trait_def.hpp>

namespace boost {

#if !defined( __CODEGEARC__ )

namespace detail{

#if !(defined(__EDG_VERSION__) && __EDG_VERSION__ <= 238) && !defined(BOOST_NO_INCLASS_MEMBER_INITIALIZATION)

template <class T>
struct is_unsigned_values
{
   //
   // Note that we cannot use BOOST_STATIC_CONSTANT here, using enum's
   // rather than "real" static constants simply doesn't work or give
   // the correct answer.
   //
   typedef typename remove_cv<T>::type no_cv_t;
   static const no_cv_t minus_one = (static_cast<no_cv_t>(-1));
   static const no_cv_t zero = (static_cast<no_cv_t>(0));
};

template <class T>
struct is_ununsigned_helper
{
   BOOST_STATIC_CONSTANT(bool, value = (::boost::detail::is_unsigned_values<T>::minus_one > ::boost::detail::is_unsigned_values<T>::zero));
};

template <bool integral_type>
struct is_ununsigned_select_helper
{
   template <class T>
   struct rebind
   {
      typedef is_ununsigned_helper<T> type;
   };
};

template <>
struct is_ununsigned_select_helper<false>
{
   template <class T>
   struct rebind
   {
      typedef false_type type;
   };
};

template <class T>
struct is_unsigned_imp
{
   typedef is_ununsigned_select_helper< 
      ::boost::type_traits::ice_or<
         ::boost::is_integral<T>::value,
         ::boost::is_enum<T>::value>::value 
   > selector;
   typedef typename selector::template rebind<T> binder;
   typedef typename binder::type type;
   BOOST_STATIC_CONSTANT(bool, value = type::value);
};

#else

template <class T> struct is_unsigned_imp : public false_type{};
template <> struct is_unsigned_imp<unsigned char> : public true_type{};
template <> struct is_unsigned_imp<const unsigned char> : public true_type{};
template <> struct is_unsigned_imp<volatile unsigned char> : public true_type{};
template <> struct is_unsigned_imp<const volatile unsigned char> : public true_type{};
template <> struct is_unsigned_imp<unsigned short> : public true_type{};
template <> struct is_unsigned_imp<const unsigned short> : public true_type{};
template <> struct is_unsigned_imp<volatile unsigned short> : public true_type{};
template <> struct is_unsigned_imp<const volatile unsigned short> : public true_type{};
template <> struct is_unsigned_imp<unsigned int> : public true_type{};
template <> struct is_unsigned_imp<const unsigned int> : public true_type{};
template <> struct is_unsigned_imp<volatile unsigned int> : public true_type{};
template <> struct is_unsigned_imp<const volatile unsigned int> : public true_type{};
template <> struct is_unsigned_imp<unsigned long> : public true_type{};
template <> struct is_unsigned_imp<const unsigned long> : public true_type{};
template <> struct is_unsigned_imp<volatile unsigned long> : public true_type{};
template <> struct is_unsigned_imp<const volatile unsigned long> : public true_type{};
#ifdef BOOST_HAS_LONG_LONG
template <> struct is_unsigned_imp<unsigned long long> : public true_type{};
template <> struct is_unsigned_imp<const unsigned long long> : public true_type{};
template <> struct is_unsigned_imp<volatile unsigned long long> : public true_type{};
template <> struct is_unsigned_imp<const volatile unsigned long long> : public true_type{};
#endif
#if defined(CHAR_MIN) && (CHAR_MIN == 0)
template <> struct is_unsigned_imp<char> : public true_type{};
template <> struct is_unsigned_imp<const char> : public true_type{};
template <> struct is_unsigned_imp<volatile char> : public true_type{};
template <> struct is_unsigned_imp<const volatile char> : public true_type{};
#endif
#if defined(WCHAR_MIN) && (WCHAR_MIN == 0) && !defined(BOOST_NO_INTRINSIC_WCHAR_T)
template <> struct is_unsigned_imp<wchar_t> : public true_type{};
template <> struct is_unsigned_imp<const wchar_t> : public true_type{};
template <> struct is_unsigned_imp<volatile wchar_t> : public true_type{};
template <> struct is_unsigned_imp<const volatile wchar_t> : public true_type{};
#endif

#endif

}

#endif // !defined( __CODEGEARC__ )

#if defined( __CODEGEARC__ )
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_unsigned,T,__is_unsigned(T))
#else
BOOST_TT_AUX_BOOL_TRAIT_DEF1(is_unsigned,T,::boost::detail::is_unsigned_imp<T>::value)
#endif

} // namespace boost

#include <boost/type_traits/detail/bool_trait_undef.hpp>

#endif // BOOST_TT_IS_MEMBER_FUNCTION_POINTER_HPP_INCLUDED

//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/utility for most recent version including documentation.
//
//  Crippled version for crippled compilers:
//  see libs/utility/call_traits.htm
//

/* Release notes:
   01st October 2000:
      Fixed call_traits on VC6, using "poor man's partial specialisation",
      using ideas taken from "Generative programming" by Krzysztof Czarnecki 
      & Ulrich Eisenecker.
*/

#ifndef BOOST_OB_CALL_TRAITS_HPP
#define BOOST_OB_CALL_TRAITS_HPP

#ifndef BOOST_CONFIG_HPP
#include <boost/config.hpp>
#endif

#ifndef BOOST_ARITHMETIC_TYPE_TRAITS_HPP
#include <boost/type_traits/arithmetic_traits.hpp>
#endif
#ifndef BOOST_COMPOSITE_TYPE_TRAITS_HPP
#include <boost/type_traits/composite_traits.hpp>
#endif

namespace boost{

#ifdef BOOST_MSVC6_MEMBER_TEMPLATES
//
// use member templates to emulate
// partial specialisation:
//
namespace detail{

template <class T>
struct standard_call_traits
{
   typedef T value_type;
   typedef T& reference;
   typedef const T& const_reference;
   typedef const T& param_type;
};
template <class T>
struct simple_call_traits
{
   typedef T value_type;
   typedef T& reference;
   typedef const T& const_reference;
   typedef const T param_type;
};
template <class T>
struct reference_call_traits
{
   typedef T value_type;
   typedef T reference;
   typedef T const_reference;
   typedef T param_type;
};

template <bool pointer, bool arithmetic, bool reference>
struct call_traits_chooser
{
   template <class T>
   struct rebind
   {
      typedef standard_call_traits<T> type;
   };
};

template <>
struct call_traits_chooser<true, false, false>
{
   template <class T>
   struct rebind
   {
      typedef simple_call_traits<T> type;
   };
};

template <>
struct call_traits_chooser<false, false, true>
{
   template <class T>
   struct rebind
   {
      typedef reference_call_traits<T> type;
   };
};

template <bool size_is_small> 
struct call_traits_sizeof_chooser2
{
   template <class T>
   struct small_rebind
   {
      typedef simple_call_traits<T> small_type;
   };
};

template<> 
struct call_traits_sizeof_chooser2<false>
{
   template <class T>
   struct small_rebind
   {
      typedef standard_call_traits<T> small_type;
   };
};

template <>
struct call_traits_chooser<false, true, false>
{
   template <class T>
   struct rebind
   {
      enum { sizeof_choice = (sizeof(T) <= sizeof(void*)) };
      typedef call_traits_sizeof_chooser2<(sizeof(T) <= sizeof(void*))> chooser;
      typedef typename chooser::template small_rebind<T> bound_type;
      typedef typename bound_type::small_type type;
   };
};

} // namespace detail
template <typename T>
struct call_traits
{
private:
    typedef detail::call_traits_chooser<
         ::boost::is_pointer<T>::value,
         ::boost::is_arithmetic<T>::value, 
         ::boost::is_reference<T>::value
      > chooser;
   typedef typename chooser::template rebind<T> bound_type;
   typedef typename bound_type::type call_traits_type;
public:
   typedef typename call_traits_type::value_type       value_type;
   typedef typename call_traits_type::reference        reference;
   typedef typename call_traits_type::const_reference  const_reference;
   typedef typename call_traits_type::param_type       param_type;
};

#else
//
// sorry call_traits is completely non-functional
// blame your broken compiler:
//

template <typename T>
struct call_traits
{
   typedef T value_type;
   typedef T& reference;
   typedef const T& const_reference;
   typedef const T& param_type;
};

#endif // member templates

}

#endif // BOOST_OB_CALL_TRAITS_HPP

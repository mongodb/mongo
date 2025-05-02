///////////////////////////////////////////////////////////////////////////////
//  Copyright Vicente J. Botet Escriba 2009-2011
//  Copyright 2012 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_EXPLICIT_CONVERSION_HPP
#define BOOST_MP_EXPLICIT_CONVERSION_HPP

#include <type_traits>
#include <boost/multiprecision/detail/standalone_config.hpp>
#include <boost/multiprecision/detail/number_base.hpp> // number_category

namespace boost {
namespace multiprecision {
namespace detail {

template <unsigned int N>
struct dummy_size
{};

template <typename S, typename T>
struct has_generic_interconversion
{
   using type = typename std::conditional<
       is_number<S>::value && is_number<T>::value,
       typename std::conditional<
           number_category<S>::value == number_kind_integer,
           typename std::conditional<
               number_category<T>::value == number_kind_integer || number_category<T>::value == number_kind_floating_point || number_category<T>::value == number_kind_rational || number_category<T>::value == number_kind_fixed_point,
               std::true_type,
               std::false_type >::type,
           typename std::conditional<
               number_category<S>::value == number_kind_rational,
               typename std::conditional<
                   number_category<T>::value == number_kind_rational || number_category<T>::value == number_kind_rational,
                   std::true_type,
                   std::false_type >::type,
               typename std::conditional<
                   number_category<T>::value == number_kind_floating_point,
                   std::true_type,
                   std::false_type >::type>::type>::type,
       std::false_type >::type;
};

template <typename S, typename T>
struct is_explicitly_convertible_imp
{
   template <typename S1, typename T1>
   static int selector(dummy_size<static_cast<unsigned int>(sizeof(new T1(std::declval<S1>())))>*);

   template <typename S1, typename T1>
   static char selector(...);

   static constexpr bool value = sizeof(selector<S, T>(nullptr)) == sizeof(int);

   using type = std::integral_constant<bool, value>;
};

template <typename From, typename To>
struct is_explicitly_convertible : public is_explicitly_convertible_imp<From, To>::type
{
};

}}} // namespace boost::multiprecision::detail

#endif

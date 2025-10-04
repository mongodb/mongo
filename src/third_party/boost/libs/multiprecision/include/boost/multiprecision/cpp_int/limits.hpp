///////////////////////////////////////////////////////////////
//  Copyright 2012 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt
//
// Comparison operators for cpp_int_backend:
//
#ifndef BOOST_MP_CPP_INT_LIMITS_HPP
#define BOOST_MP_CPP_INT_LIMITS_HPP

#include <boost/multiprecision/traits/max_digits10.hpp>

namespace std {

namespace detail {

#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable : 4307)
#endif

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline BOOST_CXX14_CONSTEXPR_IF_DETECTION boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_min(const std::integral_constant<bool, true>&, const std::integral_constant<bool, true>&, const std::integral_constant<bool, true>&)
{
   // Bounded, signed, and no allocator.
   using result_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>                                               ;
   using ui_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MaxBits, MaxBits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked>, ExpressionTemplates>;
#ifdef BOOST_MP_NO_CONSTEXPR_DETECTION
   static
#else
   constexpr
#endif
   const result_type                                                                                                                                                                          val = -result_type(~ui_type(0));
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_min(const std::integral_constant<bool, true>&, const std::integral_constant<bool, true>&, const std::integral_constant<bool, false>&)
{
   // Bounded, signed, and an allocator (can't be constexpr).
   using result_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>                                               ;
   using ui_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MaxBits, MaxBits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked>, ExpressionTemplates>;
   static const result_type                                                                                                                                                                          val = -result_type(~ui_type(0));
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline BOOST_CXX14_CONSTEXPR_IF_DETECTION boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_min(const std::integral_constant<bool, true>&, const std::integral_constant<bool, false>&, const std::integral_constant<bool, true>&)
{
   // Bounded, unsigned, no allocator (can be constexpr):
#ifdef BOOST_MP_NO_CONSTEXPR_DETECTION
   static
#else
   constexpr
#endif
   const boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> val(0u);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_min(const std::integral_constant<bool, true>&, const std::integral_constant<bool, false>&, const std::integral_constant<bool, false>&)
{
   // Bounded and std::size_t with allocator (no constexpr):
   static const boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> val(0u);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates, bool has_allocator>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_min(const std::integral_constant<bool, false>&, const std::integral_constant<bool, true>&, const std::integral_constant<bool, has_allocator>&)
{
   // Unbounded and signed, never constexpr because there must be an allocator.
   // There is no minimum value, just return 0:
   static const boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> val(0u);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates, bool has_allocator>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_min(const std::integral_constant<bool, false>&, const std::integral_constant<bool, false>&, const std::integral_constant<bool, has_allocator>&)
{
   // Unbound and unsigned, never constexpr because there must be an allocator.
   static const boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> val(0u);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline BOOST_CXX14_CONSTEXPR_IF_DETECTION boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_max(const std::integral_constant<bool, true>&, const std::integral_constant<bool, true>&, const std::integral_constant<bool, true>&)
{
   // Bounded and signed, no allocator, can be constexpr.
   using result_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>                                               ;
   using ui_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MaxBits, MaxBits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked>, ExpressionTemplates>;
#ifdef BOOST_MP_NO_CONSTEXPR_DETECTION
   static
#else
   constexpr
#endif
   const result_type                                                                                                                                                                          val = ~ui_type(0);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_max(const std::integral_constant<bool, true>&, const std::integral_constant<bool, true>&, const std::integral_constant<bool, false>&)
{
   // Bounded and signed, has an allocator, never constexpr.
   using result_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>                                               ;
   using ui_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MaxBits, MaxBits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked>, ExpressionTemplates>;
   static const result_type                                                                                                                                                                          val = ~ui_type(0);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline BOOST_CXX14_CONSTEXPR_IF_DETECTION boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_max(const std::integral_constant<bool, true>&, const std::integral_constant<bool, false>&, const std::integral_constant<bool, true>&)
{
   // Bound and unsigned, no allocator so can be constexpr:
   using result_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>                                                          ;
   using ui_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, Allocator>, ExpressionTemplates>;
#ifdef BOOST_MP_NO_CONSTEXPR_DETECTION
   static
#else
   constexpr
#endif
   const result_type                                                                                                                                                                                     val = ~ui_type(0);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_max(const std::integral_constant<bool, true>&, const std::integral_constant<bool, false>&, const std::integral_constant<bool, false>&)
{
   // Bound and unsigned, has an allocator so can never be constexpr:
   using result_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>                                                          ;
   using ui_type = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, Allocator>, ExpressionTemplates>;
   static const result_type                                                                                                                                                                                     val = ~ui_type(0);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates, bool has_allocator>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_max(const std::integral_constant<bool, false>&, const std::integral_constant<bool, true>&, const std::integral_constant<bool, has_allocator>&)
{
   // Unbounded and signed.
   // There is no maximum value, just return 0:
   static const boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> val(0u);
   return val;
}

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates, bool has_allocator>
inline boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates>
get_max(const std::integral_constant<bool, false>&, const std::integral_constant<bool, false>&, const std::integral_constant<bool, has_allocator>&)
{
   // Unbound and unsigned:
   static const boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> val(0u);
   return val;
}

} // namespace detail

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
class numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >
{
   using backend_type = boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>;
   using number_type = boost::multiprecision::number<backend_type, ExpressionTemplates>                      ;

 public:
   static constexpr bool is_specialized = true;
   //
   // Largest and smallest numbers are bounded only by available memory, set
   // to zero:
   //
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type(min)()
   {
      return detail::get_min<MinBits, MaxBits, SignType, Checked, Allocator, ExpressionTemplates>(boost::multiprecision::backends::is_fixed_precision<backend_type>(), boost::multiprecision::is_signed_number<backend_type>(), std::integral_constant<bool, std::is_void<Allocator>::value>());
   }
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type(max)()
   {
      return detail::get_max<MinBits, MaxBits, SignType, Checked, Allocator, ExpressionTemplates>(boost::multiprecision::backends::is_fixed_precision<backend_type>(), boost::multiprecision::is_signed_number<backend_type>(), std::integral_constant<bool, std::is_void<Allocator>::value>());
   }
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type          lowest() { return (min)(); }
   static constexpr int  digits       = boost::multiprecision::backends::max_precision<backend_type>::value == SIZE_MAX ? INT_MAX : boost::multiprecision::backends::max_precision<backend_type>::value;
   static constexpr int  digits10     = static_cast<int>(boost::multiprecision::detail::calc_digits10_s<static_cast<std::size_t>(digits)>::value);
   static constexpr int  max_digits10 = static_cast<int>(boost::multiprecision::detail::calc_max_digits10_s<static_cast<std::size_t>(digits)>::value);
   static constexpr bool is_signed    = boost::multiprecision::is_signed_number<backend_type>::value;
   static constexpr bool is_integer   = true;
   static constexpr bool is_exact     = true;
   static constexpr int  radix        = 2;
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type          epsilon() { return 0; }
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type          round_error() { return 0; }
   static constexpr int  min_exponent                  = 0;
   static constexpr int  min_exponent10                = 0;
   static constexpr int  max_exponent                  = 0;
   static constexpr int  max_exponent10                = 0;
   static constexpr bool has_infinity                  = false;
   static constexpr bool has_quiet_NaN                 = false;
   static constexpr bool has_signaling_NaN             = false;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4996)
#endif
   static constexpr float_denorm_style has_denorm      = denorm_absent;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
   static constexpr bool               has_denorm_loss = false;
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type                        infinity() { return 0; }
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type                        quiet_NaN() { return 0; }
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type                        signaling_NaN() { return 0; }
   static BOOST_CXX14_CONSTEXPR_IF_DETECTION number_type                        denorm_min() { return 0; }
   static constexpr bool               is_iec559       = false;
   static constexpr bool               is_bounded      = boost::multiprecision::backends::is_fixed_precision<backend_type>::value;
   static constexpr bool               is_modulo       = (boost::multiprecision::backends::is_fixed_precision<backend_type>::value && (Checked == boost::multiprecision::unchecked));
   static constexpr bool               traps           = false;
   static constexpr bool               tinyness_before = false;
   static constexpr float_round_style round_style      = round_toward_zero;
};

template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::digits;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::digits10;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::max_digits10;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::is_signed;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::is_integer;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::is_exact;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::radix;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::min_exponent;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::min_exponent10;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::max_exponent;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr int numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::max_exponent10;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::has_infinity;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::has_quiet_NaN;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::has_signaling_NaN;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4996)
#endif
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr float_denorm_style numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::has_denorm;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::has_denorm_loss;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::is_iec559;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::is_bounded;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::is_modulo;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::traps;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::tinyness_before;
template <std::size_t MinBits, std::size_t MaxBits, boost::multiprecision::cpp_integer_type SignType, boost::multiprecision::cpp_int_check_type Checked, class Allocator, boost::multiprecision::expression_template_option ExpressionTemplates>
constexpr float_round_style numeric_limits<boost::multiprecision::number<boost::multiprecision::cpp_int_backend<MinBits, MaxBits, SignType, Checked, Allocator>, ExpressionTemplates> >::round_style;

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

} // namespace std

#endif

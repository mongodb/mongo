///////////////////////////////////////////////////////////////
//  Copyright 2013 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_CPP_INT_LITERALS_HPP
#define BOOST_MP_CPP_INT_LITERALS_HPP

#include <boost/multiprecision/cpp_int/cpp_int_config.hpp>

namespace boost { namespace multiprecision {

namespace literals {
namespace detail {

template <char>
struct hex_value;
template <>
struct hex_value<'0'>
{
   static constexpr limb_type value = 0;
};
template <>
struct hex_value<'1'>
{
   static constexpr limb_type value = 1;
};
template <>
struct hex_value<'2'>
{
   static constexpr limb_type value = 2;
};
template <>
struct hex_value<'3'>
{
   static constexpr limb_type value = 3;
};
template <>
struct hex_value<'4'>
{
   static constexpr limb_type value = 4;
};
template <>
struct hex_value<'5'>
{
   static constexpr limb_type value = 5;
};
template <>
struct hex_value<'6'>
{
   static constexpr limb_type value = 6;
};
template <>
struct hex_value<'7'>
{
   static constexpr limb_type value = 7;
};
template <>
struct hex_value<'8'>
{
   static constexpr limb_type value = 8;
};
template <>
struct hex_value<'9'>
{
   static constexpr limb_type value = 9;
};
template <>
struct hex_value<'a'>
{
   static constexpr limb_type value = 10;
};
template <>
struct hex_value<'b'>
{
   static constexpr limb_type value = 11;
};
template <>
struct hex_value<'c'>
{
   static constexpr limb_type value = 12;
};
template <>
struct hex_value<'d'>
{
   static constexpr limb_type value = 13;
};
template <>
struct hex_value<'e'>
{
   static constexpr limb_type value = 14;
};
template <>
struct hex_value<'f'>
{
   static constexpr limb_type value = 15;
};
template <>
struct hex_value<'A'>
{
   static constexpr limb_type value = 10;
};
template <>
struct hex_value<'B'>
{
   static constexpr limb_type value = 11;
};
template <>
struct hex_value<'C'>
{
   static constexpr limb_type value = 12;
};
template <>
struct hex_value<'D'>
{
   static constexpr limb_type value = 13;
};
template <>
struct hex_value<'E'>
{
   static constexpr limb_type value = 14;
};
template <>
struct hex_value<'F'>
{
   static constexpr limb_type value = 15;
};

template <class Pack, limb_type value>
struct combine_value_to_pack;
template <limb_type first, limb_type... ARGS, limb_type value>
struct combine_value_to_pack<value_pack<first, ARGS...>, value>
{
   using type = value_pack<first | value, ARGS...>;
};

template <char NextChar, char... CHARS>
struct pack_values
{
   static constexpr std::size_t chars_per_limb = sizeof(limb_type) * CHAR_BIT / 4;
   static constexpr std::size_t shift          = ((sizeof...(CHARS)) % chars_per_limb) * 4;
   static constexpr limb_type value_to_add  = shift ? hex_value<NextChar>::value << shift : hex_value<NextChar>::value;

   using recursive_packed_type = typename pack_values<CHARS...>::type                         ;
   using pack_type = typename std::conditional<shift == 0,
                                     typename recursive_packed_type::next_type,
                                     recursive_packed_type>::type;
   using type = typename combine_value_to_pack<pack_type, value_to_add>::type;
};
template <char NextChar>
struct pack_values<NextChar>
{
   static constexpr limb_type value_to_add = hex_value<NextChar>::value;

   using type = value_pack<value_to_add>;
};

template <class T>
struct strip_leading_zeros_from_pack;
template <limb_type... PACK>
struct strip_leading_zeros_from_pack<value_pack<PACK...> >
{
   using type = value_pack<PACK...>;
};
template <limb_type... PACK>
struct strip_leading_zeros_from_pack<value_pack<0u, PACK...> >
{
   using type = typename strip_leading_zeros_from_pack<value_pack<PACK...> >::type;
};

template <limb_type v, class PACK>
struct append_value_to_pack;
template <limb_type v, limb_type... PACK>
struct append_value_to_pack<v, value_pack<PACK...> >
{
   using type = value_pack<PACK..., v>;
};

template <class T>
struct reverse_value_pack;
template <limb_type v, limb_type... VALUES>
struct reverse_value_pack<value_pack<v, VALUES...> >
{
   using lead_values = typename reverse_value_pack<value_pack<VALUES...> >::type;
   using type = typename append_value_to_pack<v, lead_values>::type      ;
};
template <limb_type v>
struct reverse_value_pack<value_pack<v> >
{
   using type = value_pack<v>;
};
template <>
struct reverse_value_pack<value_pack<> >
{
   using type = value_pack<>;
};

template <char l1, char l2, char... STR>
struct make_packed_value_from_str
{
   static_assert(l1 == '0', "Multi-precision integer literals must be in hexadecimal notation.");
   static_assert((l2 == 'X') || (l2 == 'x'), "Multi-precision integer literals must be in hexadecimal notation.");
   using packed_type = typename pack_values<STR...>::type                       ;
   using stripped_type = typename strip_leading_zeros_from_pack<packed_type>::type;
   using type = typename reverse_value_pack<stripped_type>::type         ;
};

template <class Pack, class B>
struct make_backend_from_pack
{
   static constexpr Pack p  = {};
   static constexpr B value = p;
};

#if !defined(__cpp_inline_variables)
template <class Pack, class B>
constexpr B make_backend_from_pack<Pack, B>::value;
#endif

template <unsigned Digits>
struct signed_cpp_int_literal_result_type
{
   static constexpr unsigned                                                                               bits = Digits * 4;
   using backend_type = boost::multiprecision::backends::cpp_int_backend<bits, bits, signed_magnitude, unchecked, void>;
   using number_type = number<backend_type, et_off>                                                                   ;
};

template <unsigned Digits>
struct unsigned_cpp_int_literal_result_type
{
   static constexpr unsigned                                                                                 bits = Digits * 4;
   using backend_type = boost::multiprecision::backends::cpp_int_backend<bits, bits, unsigned_magnitude, unchecked, void>;
   using number_type = number<backend_type, et_off>                                                                     ;
};

} // namespace detail

template <char... STR>
constexpr typename boost::multiprecision::literals::detail::signed_cpp_int_literal_result_type<static_cast<unsigned>((sizeof...(STR)) - 2u)>::number_type operator ""_cppi()
{
   using pt = typename boost::multiprecision::literals::detail::make_packed_value_from_str<STR...>::type;
   return boost::multiprecision::literals::detail::make_backend_from_pack<pt, typename boost::multiprecision::literals::detail::signed_cpp_int_literal_result_type<static_cast<unsigned>((sizeof...(STR)) - 2u)>::backend_type>::value;
}

template <char... STR>
constexpr typename boost::multiprecision::literals::detail::unsigned_cpp_int_literal_result_type<static_cast<unsigned>((sizeof...(STR)) - 2u)>::number_type operator ""_cppui()
{
   using pt = typename boost::multiprecision::literals::detail::make_packed_value_from_str<STR...>::type;
   return boost::multiprecision::literals::detail::make_backend_from_pack<pt, typename boost::multiprecision::literals::detail::unsigned_cpp_int_literal_result_type<static_cast<unsigned>((sizeof...(STR)) - 2u)>::backend_type>::value;
}

#define BOOST_MP_DEFINE_SIZED_CPP_INT_LITERAL(Bits, ILiteral, ULiteral)                                                                                                                                                            \
   template <char... STR>                                                                                                                                                                                                          \
   constexpr boost::multiprecision::number<boost::multiprecision::backends::cpp_int_backend<Bits, Bits, boost::multiprecision::signed_magnitude, boost::multiprecision::unchecked, void> > operator ILiteral ()                    \
   {                                                                                                                                                                                                                               \
      using pt = typename boost::multiprecision::literals::detail::make_packed_value_from_str<STR...>::type;                                                                                                                       \
      return boost::multiprecision::literals::detail::make_backend_from_pack<                                                                                                                                                      \
          pt,                                                                                                                                                                                                                      \
          boost::multiprecision::backends::cpp_int_backend<Bits, Bits, boost::multiprecision::signed_magnitude, boost::multiprecision::unchecked, void> >::value;                                                                  \
   }                                                                                                                                                                                                                               \
   template <char... STR>                                                                                                                                                                                                          \
   constexpr boost::multiprecision::number<boost::multiprecision::backends::cpp_int_backend<Bits, Bits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, void> > operator ULiteral ()                  \
   {                                                                                                                                                                                                                               \
      using pt = typename boost::multiprecision::literals::detail::make_packed_value_from_str<STR...>::type;                                                                                                                       \
      return boost::multiprecision::literals::detail::make_backend_from_pack<                                                                                                                                                      \
          pt,                                                                                                                                                                                                                      \
          boost::multiprecision::backends::cpp_int_backend<Bits, Bits, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, void> >::value;                                                                \
   }

BOOST_MP_DEFINE_SIZED_CPP_INT_LITERAL(128, ""_cppi128, ""_cppui128)
BOOST_MP_DEFINE_SIZED_CPP_INT_LITERAL(256, ""_cppi256, ""_cppui256)
BOOST_MP_DEFINE_SIZED_CPP_INT_LITERAL(512, ""_cppi512, ""_cppui512)
BOOST_MP_DEFINE_SIZED_CPP_INT_LITERAL(1024, ""_cppi1024, ""_cppui1024)

} // namespace literals

//
// Overload unary minus operator for constexpr use:
//
template <std::size_t MinBits, cpp_int_check_type Checked>
constexpr number<cpp_int_backend<MinBits, MinBits, signed_magnitude, Checked, void>, et_off>
operator-(const number<cpp_int_backend<MinBits, MinBits, signed_magnitude, Checked, void>, et_off>& a)
{
   return cpp_int_backend<MinBits, MinBits, signed_magnitude, Checked, void>(a.backend(), boost::multiprecision::literals::detail::make_negate_tag());
}
template <std::size_t MinBits, cpp_int_check_type Checked>
constexpr number<cpp_int_backend<MinBits, MinBits, signed_magnitude, Checked, void>, et_off>
operator-(number<cpp_int_backend<MinBits, MinBits, signed_magnitude, Checked, void>, et_off>&& a)
{
   return cpp_int_backend<MinBits, MinBits, signed_magnitude, Checked, void>(static_cast<const number<cpp_int_backend<MinBits, MinBits, signed_magnitude, Checked, void>, et_off>&>(a).backend(), boost::multiprecision::literals::detail::make_negate_tag());
}

}} // namespace boost::multiprecision

#endif // BOOST_MP_CPP_INT_CORE_HPP

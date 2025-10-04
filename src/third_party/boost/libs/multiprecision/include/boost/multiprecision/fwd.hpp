///////////////////////////////////////////////////////////////////////////////
//  Copyright 2023 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_FWD_HPP
#define BOOST_MP_FWD_HPP

#include <boost/multiprecision/cpp_int/cpp_int_config.hpp>

namespace boost {
   namespace multiprecision {

      enum expression_template_option
      {
         et_off = 0,
         et_on = 1
      };

      template <class Backend>
      struct expression_template_default
      {
         static constexpr expression_template_option value = et_on;
      };

      template <class Backend, expression_template_option ExpressionTemplates = expression_template_default<Backend>::value>
      class number;

      template <class T>
      struct is_number : public std::integral_constant<bool, false>
      {};

      template <class Backend, expression_template_option ExpressionTemplates>
      struct is_number<number<Backend, ExpressionTemplates> > : public std::integral_constant<bool, true>
      {};

      namespace detail {

         // Forward-declare an expression wrapper
         template <class tag, class Arg1 = void, class Arg2 = void, class Arg3 = void, class Arg4 = void>
         struct expression;

      } // namespace detail

      enum cpp_integer_type
      {
         signed_magnitude = 1,
         unsigned_magnitude = 0,
         signed_packed = 3,
         unsigned_packed = 2
      };

      enum cpp_int_check_type
      {
         checked = 1,
         unchecked = 0
      };

      enum mpfr_allocation_type
      {
         allocate_stack,
         allocate_dynamic
      };

      template <class Backend>
      void log_postfix_event(const Backend&, const char* /*event_description*/);
      template <class Backend, class T>
      void log_postfix_event(const Backend&, const T&, const char* /*event_description*/);
      template <class Backend>
      void log_prefix_event(const Backend&, const char* /*event_description*/);
      template <class Backend, class T>
      void log_prefix_event(const Backend&, const T&, const char* /*event_description*/);
      template <class Backend, class T, class U>
      void log_prefix_event(const Backend&, const T&, const U&, const char* /*event_description*/);
      template <class Backend, class T, class U, class V>
      void log_prefix_event(const Backend&, const T&, const U&, const V&, const char* /*event_description*/);

      namespace backends {

         template <class Backend>
         struct debug_adaptor;

         template <class Backend>
         struct logged_adaptor;

         template <class Backend>
         struct complex_adaptor;

         enum digit_base_type
         {
            digit_base_2 = 2,
            digit_base_10 = 10
         };

         template <unsigned Digits, digit_base_type DigitBase = digit_base_10, class Allocator = void, class Exponent = int, Exponent MinExponent = 0, Exponent MaxExponent = 0>
         class cpp_bin_float;

         template <unsigned Digits10, class ExponentType = std::int32_t, class Allocator = void>
         class cpp_dec_float;

         template <std::size_t MinBits = 0, std::size_t MaxBits = 0, boost::multiprecision::cpp_integer_type SignType = signed_magnitude, cpp_int_check_type Checked = unchecked, class Allocator = typename std::conditional<MinBits && (MinBits == MaxBits), void, std::allocator<limb_type> >::type>
         struct cpp_int_backend;

         struct float128_backend;

         struct gmp_int;
         struct gmp_rational;

         template <unsigned digits10>
         struct gmp_float;

         template <unsigned digits10>
         struct mpc_complex_backend;

         template <unsigned digits10>
         struct mpfi_float_backend;

         template <unsigned digits10, mpfr_allocation_type AllocationType = allocate_dynamic>
         struct mpfr_float_backend;

         template <>
         struct mpfr_float_backend<0, allocate_stack>;

         template <class Backend>
         struct rational_adaptor;

         struct tommath_int;
      }

      using boost::multiprecision::backends::complex_adaptor;
      using boost::multiprecision::backends::debug_adaptor;
      using boost::multiprecision::backends::logged_adaptor;
      using backends::cpp_bin_float;
      using backends::digit_base_10;
      using backends::digit_base_2;
      using boost::multiprecision::backends::cpp_dec_float;
      using boost::multiprecision::backends::cpp_int_backend;
      using boost::multiprecision::backends::float128_backend;
      using boost::multiprecision::backends::gmp_float;
      using boost::multiprecision::backends::gmp_int;
      using boost::multiprecision::backends::gmp_rational;
      using boost::multiprecision::backends::mpc_complex_backend;
      using boost::multiprecision::backends::mpfi_float_backend;
      using boost::multiprecision::backends::mpfr_float_backend;
      using boost::multiprecision::backends::rational_adaptor;
      using boost::multiprecision::backends::tommath_int;

      template <unsigned Digits, backends::digit_base_type DigitBase, class Allocator, class Exponent, Exponent MinE, Exponent MaxE>
      struct expression_template_default<cpp_bin_float<Digits, DigitBase, Allocator, Exponent, MinE, MaxE> >
      {
         static constexpr expression_template_option value = std::is_void<Allocator>::value ? et_off : et_on;
      };

      template <std::size_t MinBits, std::size_t MaxBits, cpp_integer_type SignType, cpp_int_check_type Checked>
      struct expression_template_default<backends::cpp_int_backend<MinBits, MaxBits, SignType, Checked, void> >
      {
         static constexpr expression_template_option value = et_off;
      };

      template <class IntBackend>
      struct expression_template_default<backends::rational_adaptor<IntBackend> > : public expression_template_default<IntBackend>
      {};

      using complex128 = number<complex_adaptor<float128_backend>, et_off>;

      using cpp_bin_float_50 = number<backends::cpp_bin_float<50> >;
      using cpp_bin_float_100 = number<backends::cpp_bin_float<100> >;

      using cpp_bin_float_single = number<backends::cpp_bin_float<24, backends::digit_base_2, void, std::int16_t, -126, 127>, et_off>;
      using cpp_bin_float_double = number<backends::cpp_bin_float<53, backends::digit_base_2, void, std::int16_t, -1022, 1023>, et_off>;
      using cpp_bin_float_double_extended = number<backends::cpp_bin_float<64, backends::digit_base_2, void, std::int16_t, -16382, 16383>, et_off>;
      using cpp_bin_float_quad = number<backends::cpp_bin_float<113, backends::digit_base_2, void, std::int16_t, -16382, 16383>, et_off>;
      using cpp_bin_float_oct = number<backends::cpp_bin_float<237, backends::digit_base_2, void, std::int32_t, -262142, 262143>, et_off>;

      template <unsigned Digits, backends::digit_base_type DigitBase = backends::digit_base_10, class Allocator = void, class Exponent = int, Exponent MinExponent = 0, Exponent MaxExponent = 0>
      using cpp_complex_backend = complex_adaptor<cpp_bin_float<Digits, DigitBase, Allocator, Exponent, MinExponent, MaxExponent> >;

      template <unsigned Digits, backends::digit_base_type DigitBase = digit_base_10, class Allocator = void, class Exponent = int, Exponent MinExponent = 0, Exponent MaxExponent = 0, expression_template_option ExpressionTemplates = et_off>
      using cpp_complex = number<complex_adaptor<cpp_bin_float<Digits, DigitBase, Allocator, Exponent, MinExponent, MaxExponent> >, ExpressionTemplates>;

      using cpp_complex_50 = cpp_complex<50>;
      using cpp_complex_100 = cpp_complex<100>;

      using cpp_complex_single = cpp_complex<24, backends::digit_base_2, void, std::int16_t, -126, 127>;
      using cpp_complex_double = cpp_complex<53, backends::digit_base_2, void, std::int16_t, -1022, 1023>;
      using cpp_complex_extended = cpp_complex<64, backends::digit_base_2, void, std::int16_t, -16382, 16383>;
      using cpp_complex_quad = cpp_complex<113, backends::digit_base_2, void, std::int16_t, -16382, 16383>;
      using cpp_complex_oct = cpp_complex<237, backends::digit_base_2, void, std::int32_t, -262142, 262143>;

      using cpp_dec_float_50 = number<cpp_dec_float<50> >;
      using cpp_dec_float_100 = number<cpp_dec_float<100> >;

      using cpp_int = number<cpp_int_backend<> >;
      using cpp_rational_backend = rational_adaptor<cpp_int_backend<> >;
      using cpp_rational = number<cpp_rational_backend>;

      // Fixed precision unsigned types:
      using uint128_t = number<cpp_int_backend<128, 128, unsigned_magnitude, unchecked, void> >;
      using uint256_t = number<cpp_int_backend<256, 256, unsigned_magnitude, unchecked, void> >;
      using uint512_t = number<cpp_int_backend<512, 512, unsigned_magnitude, unchecked, void> >;
      using uint1024_t = number<cpp_int_backend<1024, 1024, unsigned_magnitude, unchecked, void> >;

      // Fixed precision signed types:
      using int128_t = number<cpp_int_backend<128, 128, signed_magnitude, unchecked, void> >;
      using int256_t = number<cpp_int_backend<256, 256, signed_magnitude, unchecked, void> >;
      using int512_t = number<cpp_int_backend<512, 512, signed_magnitude, unchecked, void> >;
      using int1024_t = number<cpp_int_backend<1024, 1024, signed_magnitude, unchecked, void> >;

      // Over again, but with checking enabled this time:
      using checked_cpp_int = number<cpp_int_backend<0, 0, signed_magnitude, checked> >;
      using checked_cpp_rational_backend = rational_adaptor<cpp_int_backend<0, 0, signed_magnitude, checked> >;
      using checked_cpp_rational = number<checked_cpp_rational_backend>;
      // Fixed precision unsigned types:
      using checked_uint128_t = number<cpp_int_backend<128, 128, unsigned_magnitude, checked, void> >;
      using checked_uint256_t = number<cpp_int_backend<256, 256, unsigned_magnitude, checked, void> >;
      using checked_uint512_t = number<cpp_int_backend<512, 512, unsigned_magnitude, checked, void> >;
      using checked_uint1024_t = number<cpp_int_backend<1024, 1024, unsigned_magnitude, checked, void> >;

      // Fixed precision signed types:
      using checked_int128_t = number<cpp_int_backend<128, 128, signed_magnitude, checked, void> >;
      using checked_int256_t = number<cpp_int_backend<256, 256, signed_magnitude, checked, void> >;
      using checked_int512_t = number<cpp_int_backend<512, 512, signed_magnitude, checked, void> >;
      using checked_int1024_t = number<cpp_int_backend<1024, 1024, signed_magnitude, checked, void> >;

      template <class Number>
      using debug_adaptor_t = number<debug_adaptor<typename Number::backend_type>, Number::et>;
      template <class Number>
      using logged_adaptor_t = number<logged_adaptor<typename Number::backend_type>, Number::et>;


      using float128 = number<float128_backend, et_off>;

      using mpf_float_50 = number<gmp_float<50> >;
      using mpf_float_100 = number<gmp_float<100> >;
      using mpf_float_500 = number<gmp_float<500> >;
      using mpf_float_1000 = number<gmp_float<1000> >;
      using mpf_float = number<gmp_float<0> >;
      using mpz_int = number<gmp_int>;
      using mpq_rational = number<gmp_rational>;

      using mpc_complex_50 = number<mpc_complex_backend<50> >;
      using mpc_complex_100 = number<mpc_complex_backend<100> >;
      using mpc_complex_500 = number<mpc_complex_backend<500> >;
      using mpc_complex_1000 = number<mpc_complex_backend<1000> >;
      using mpc_complex = number<mpc_complex_backend<0> >;

      using mpfi_float_50 = number<mpfi_float_backend<50> >;
      using mpfi_float_100 = number<mpfi_float_backend<100> >;
      using mpfi_float_500 = number<mpfi_float_backend<500> >;
      using mpfi_float_1000 = number<mpfi_float_backend<1000> >;
      using mpfi_float = number<mpfi_float_backend<0> >;

      using mpfr_float_50 = number<mpfr_float_backend<50> >;
      using mpfr_float_100 = number<mpfr_float_backend<100> >;
      using mpfr_float_500 = number<mpfr_float_backend<500> >;
      using mpfr_float_1000 = number<mpfr_float_backend<1000> >;
      using mpfr_float = number<mpfr_float_backend<0> >;

      using static_mpfr_float_50 = number<mpfr_float_backend<50, allocate_stack> >;
      using static_mpfr_float_100 = number<mpfr_float_backend<100, allocate_stack> >;

      using tom_int = number<tommath_int>;
      using tommath_rational = rational_adaptor<tommath_int>;
      using tom_rational = number<tommath_rational>;

} }

#endif

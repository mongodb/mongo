///////////////////////////////////////////////////////////////
//  Copyright 2020 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_MP_RATIONAL_ADAPTOR_HPP
#define BOOST_MP_RATIONAL_ADAPTOR_HPP

#include <boost/multiprecision/number.hpp>
#include <boost/multiprecision/detail/hash.hpp>
#include <boost/multiprecision/detail/float128_functions.hpp>
#include <boost/multiprecision/detail/no_exceptions_support.hpp>

namespace boost {
namespace multiprecision {
namespace backends {

template <class Backend>
struct rational_adaptor
{
   //
   // Each backend need to declare 3 type lists which declare the types
   // with which this can interoperate.  These lists must at least contain
   // the widest type in each category - so "long long" must be the final
   // type in the signed_types list for example.  Any narrower types if not
   // present in the list will get promoted to the next wider type that is
   // in the list whenever mixed arithmetic involving that type is encountered.
   //
   typedef typename Backend::signed_types    signed_types;
   typedef typename Backend::unsigned_types  unsigned_types;
   typedef typename Backend::float_types     float_types;

   typedef typename std::tuple_element<0, unsigned_types>::type ui_type;

   static Backend get_one()
   {
      Backend t;
      t = static_cast<ui_type>(1);
      return t;
   }
   static Backend get_zero()
   {
      Backend t;
      t = static_cast<ui_type>(0);
      return t;
   }

   static const Backend& one()
   {
      static const Backend result(get_one());
      return result;
   }
   static const Backend& zero()
   {
      static const Backend result(get_zero());
      return result;
   }

   void normalize()
   {
      using default_ops::eval_gcd;
      using default_ops::eval_eq;
      using default_ops::eval_divide;
      using default_ops::eval_get_sign;

      int s = eval_get_sign(m_denom);

      if(s == 0)
      {
         BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
      }
      else if (s < 0)
      {
         m_num.negate();
         m_denom.negate();
      }

      Backend g, t;
      eval_gcd(g, m_num, m_denom);
      if (!eval_eq(g, one()))
      {
         eval_divide(t, m_num, g);
         m_num.swap(t);
         eval_divide(t, m_denom, g);
         m_denom = std::move(t);
      }
   }

   // We must have a default constructor:
   rational_adaptor()
      : m_num(zero()), m_denom(one()) {}

   rational_adaptor(const rational_adaptor& o) : m_num(o.m_num), m_denom(o.m_denom) {}
   rational_adaptor(rational_adaptor&& o) = default;

   // Optional constructors, we can make this type slightly more efficient
   // by providing constructors from any type we can handle natively.
   // These will also cause number<> to be implicitly constructible
   // from these types unless we make such constructors explicit.
   //
   template <class Arithmetic>
   rational_adaptor(const Arithmetic& val, typename std::enable_if<std::is_constructible<Backend, Arithmetic>::value && !std::is_floating_point<Arithmetic>::value>::type const* = nullptr)
      : m_num(val), m_denom(one()) {}

   //
   // Pass-through 2-arg construction of components:
   //
   template <class T, class U>
   rational_adaptor(const T& a, const U& b, typename std::enable_if<std::is_constructible<Backend, T const&>::value && std::is_constructible<Backend, U const&>::value>::type const* = nullptr)
      : m_num(a), m_denom(b) 
   {
      normalize();
   }
   template <class T, class U>
   rational_adaptor(T&& a, const U& b, typename std::enable_if<std::is_constructible<Backend, T>::value && std::is_constructible<Backend, U>::value>::type const* = nullptr)
      : m_num(static_cast<T&&>(a)), m_denom(b) 
   {
      normalize();
   }
   template <class T, class U>
   rational_adaptor(T&& a, U&& b, typename std::enable_if<std::is_constructible<Backend, T>::value && std::is_constructible<Backend, U>::value>::type const* = nullptr)
      : m_num(static_cast<T&&>(a)), m_denom(static_cast<U&&>(b)) 
   {
      normalize();
   }
   template <class T, class U>
   rational_adaptor(const T& a, U&& b, typename std::enable_if<std::is_constructible<Backend, T>::value && std::is_constructible<Backend, U>::value>::type const* = nullptr)
      : m_num(a), m_denom(static_cast<U&&>(b)) 
   {
      normalize();
   }
   //
   // In the absense of converting constructors, operator= takes the strain.
   // In addition to the usual suspects, there must be one operator= for each type
   // listed in signed_types, unsigned_types, and float_types plus a string constructor.
   //
   rational_adaptor& operator=(const rational_adaptor& o) = default;
   rational_adaptor& operator=(rational_adaptor&& o) = default;
   template <class Arithmetic>
   inline typename std::enable_if<!std::is_floating_point<Arithmetic>::value, rational_adaptor&>::type operator=(const Arithmetic& i)
   {
      m_num = i;
      m_denom = one();
      return *this;
   }
   rational_adaptor& operator=(const char* s)
   {
      using default_ops::eval_eq;

      std::string                        s1;
      multiprecision::number<Backend>    v1, v2;
      char                               c;
      bool                               have_hex = false;
      const char* p = s; // saved for later

      while ((0 != (c = *s)) && (c == 'x' || c == 'X' || c == '-' || c == '+' || (c >= '0' && c <= '9') || (have_hex && (c >= 'a' && c <= 'f')) || (have_hex && (c >= 'A' && c <= 'F'))))
      {
         if (c == 'x' || c == 'X')
            have_hex = true;
         s1.append(1, c);
         ++s;
      }
      v1.assign(s1);
      s1.erase();
      if (c == '/')
      {
         ++s;
         while ((0 != (c = *s)) && (c == 'x' || c == 'X' || c == '-' || c == '+' || (c >= '0' && c <= '9') || (have_hex && (c >= 'a' && c <= 'f')) || (have_hex && (c >= 'A' && c <= 'F'))))
         {
            if (c == 'x' || c == 'X')
               have_hex = true;
            s1.append(1, c);
            ++s;
         }
         v2.assign(s1);
      }
      else
         v2 = 1;
      if (*s)
      {
         BOOST_MP_THROW_EXCEPTION(std::runtime_error(std::string("Could not parse the string \"") + p + std::string("\" as a valid rational number.")));
      }
      multiprecision::number<Backend> gcd;
      eval_gcd(gcd.backend(), v1.backend(), v2.backend());
      if (!eval_eq(gcd.backend(), one()))
      {
         v1 /= gcd;
         v2 /= gcd;
      }
      num() = std::move(std::move(v1).backend());
      denom() = std::move(std::move(v2).backend());
      return *this;
   }
   template <class Float>
   typename std::enable_if<std::is_floating_point<Float>::value, rational_adaptor&>::type operator=(Float i)
   {
      using default_ops::eval_eq;
      BOOST_MP_FLOAT128_USING using std::floor; using std::frexp; using std::ldexp;

      int   e;
      Float f = frexp(i, &e);
#ifdef BOOST_HAS_FLOAT128
      f = ldexp(f, std::is_same<float128_type, Float>::value ? 113 : std::numeric_limits<Float>::digits);
      e -= std::is_same<float128_type, Float>::value ? 113 : std::numeric_limits<Float>::digits;
#else
      f = ldexp(f, std::numeric_limits<Float>::digits);
      e -= std::numeric_limits<Float>::digits;
#endif
      number<Backend> num(f);
      number<Backend> denom(1u);
      if (e > 0)
      {
         num <<= e;
      }
      else if (e < 0)
      {
         denom <<= -e;
      }
      number<Backend> gcd;
      eval_gcd(gcd.backend(), num.backend(), denom.backend());
      if (!eval_eq(gcd.backend(), one()))
      {
         num /= gcd;
         denom /= gcd;
      }
      this->num() = std::move(std::move(num).backend());
      this->denom() = std::move(std::move(denom).backend());
      return *this;
   }

   void swap(rational_adaptor& o)
   {
      m_num.swap(o.m_num);
      m_denom.swap(o.m_denom);
   }
   std::string str(std::streamsize digits, std::ios_base::fmtflags f) const
   {
      using default_ops::eval_eq;
      //
      // We format the string ourselves so we can match what GMP's mpq type does:
      //
      std::string result = num().str(digits, f);
      if (!eval_eq(denom(), one()))
      {
         result.append(1, '/');
         result.append(denom().str(digits, f));
      }
      return result;
   }
   void negate()
   {
      m_num.negate();
   }
   int compare(const rational_adaptor& o) const
   {
      std::ptrdiff_t s1 = eval_get_sign(*this);
      std::ptrdiff_t s2 = eval_get_sign(o);
      if (s1 != s2)
      {
         return s1 < s2 ? -1 : 1;
      }
      else if (s1 == 0)
         return 0; // both zero.

      bool neg = false;
      if (s1 >= 0)
      {
         s1 = eval_msb(num()) + eval_msb(o.denom());
         s2 = eval_msb(o.num()) + eval_msb(denom());
      }
      else
      {
         Backend t(num());
         t.negate();
         s1 = eval_msb(t) + eval_msb(o.denom());
         t = o.num();
         t.negate();
         s2 = eval_msb(t) + eval_msb(denom());
         neg = true;
      }
      s1 -= s2;
      if (s1 < -1)
         return neg ? 1 : -1;
      else if (s1 > 1)
         return neg ? -1 : 1;

      Backend t1, t2;
      eval_multiply(t1, num(), o.denom());
      eval_multiply(t2, o.num(), denom());
      return t1.compare(t2);
   }
   //
   // Comparison with arithmetic types, default just constructs a temporary:
   //
   template <class A>
   typename std::enable_if<boost::multiprecision::detail::is_arithmetic<A>::value, int>::type compare(A i) const
   {
      rational_adaptor t;
      t = i;  //  Note: construct directly from i if supported.
      return compare(t);
   }

   Backend& num() { return m_num; }
   const Backend& num()const { return m_num; }
   Backend& denom() { return m_denom; }
   const Backend& denom()const { return m_denom; }

   #ifndef BOOST_MP_STANDALONE
   template <class Archive>
   void serialize(Archive& ar, const std::integral_constant<bool, true>&)
   {
      // Saving
      number<Backend> n(num()), d(denom());
      ar& boost::make_nvp("numerator", n);
      ar& boost::make_nvp("denominator", d);
   }
   template <class Archive>
   void serialize(Archive& ar, const std::integral_constant<bool, false>&)
   {
      // Loading
      number<Backend> n, d;
      ar& boost::make_nvp("numerator", n);
      ar& boost::make_nvp("denominator", d);
      num() = n.backend();
      denom() = d.backend();
   }
   template <class Archive>
   void serialize(Archive& ar, const unsigned int /*version*/)
   {
      using tag = typename Archive::is_saving;
      using saving_tag = std::integral_constant<bool, tag::value>;
      serialize(ar, saving_tag());
   }
   #endif // BOOST_MP_STANDALONE
   
 private:
   Backend m_num, m_denom;
};

//
// Helpers:
//
template <class T>
inline constexpr typename std::enable_if<std::numeric_limits<T>::is_specialized && !std::numeric_limits<T>::is_signed, bool>::type
is_minus_one(const T&)
{
   return false;
}
template <class T>
inline constexpr typename std::enable_if<!std::numeric_limits<T>::is_specialized || std::numeric_limits<T>::is_signed, bool>::type
is_minus_one(const T& val)
{
   return val == -1;
}

//
// Required non-members:
//
template <class Backend> 
inline void eval_add(rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   eval_add_subtract_imp(a, a, b, true);
}
template <class Backend> 
inline void eval_subtract(rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   eval_add_subtract_imp(a, a, b, false);
}

template <class Backend> 
inline void eval_multiply(rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   eval_multiply_imp(a, a, b.num(), b.denom());
}

template <class Backend> 
void eval_divide(rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   using default_ops::eval_divide;
   rational_adaptor<Backend> t;
   eval_divide(t, a, b);
   a = std::move(t);
}
//
// Conversions:
//
template <class R, class IntBackend>
inline typename std::enable_if<number_category<R>::value == number_kind_floating_point>::type eval_convert_to(R* result, const rational_adaptor<IntBackend>& backend)
{
   //
   // The generic conversion is as good as anything we can write here:
   //
   ::boost::multiprecision::detail::generic_convert_rational_to_float(*result, backend);
}

template <class R, class IntBackend>
inline typename std::enable_if<(number_category<R>::value != number_kind_integer) && (number_category<R>::value != number_kind_floating_point) && !std::is_enum<R>::value>::type eval_convert_to(R* result, const rational_adaptor<IntBackend>& backend)
{
   using default_ops::eval_convert_to;
   R d;
   eval_convert_to(result, backend.num());
   eval_convert_to(&d, backend.denom());
   *result /= d;
}

template <class R, class Backend>
inline typename std::enable_if<number_category<R>::value == number_kind_integer>::type eval_convert_to(R* result, const rational_adaptor<Backend>& backend)
{
   using default_ops::eval_divide;
   using default_ops::eval_convert_to;
   Backend t;
   eval_divide(t, backend.num(), backend.denom());
   eval_convert_to(result, t);
}

//
// Hashing support, not strictly required, but it is used in our tests:
//
template <class Backend>
inline std::size_t hash_value(const rational_adaptor<Backend>& arg)
{
   std::size_t result = hash_value(arg.num());
   std::size_t result2 = hash_value(arg.denom());
   boost::multiprecision::detail::hash_combine(result, result2);
   return result;
}
//
// assign_components:
//
template <class Backend>
void assign_components(rational_adaptor<Backend>& result, Backend const& a, Backend const& b)
{
   using default_ops::eval_gcd;
   using default_ops::eval_divide;
   using default_ops::eval_eq;
   using default_ops::eval_is_zero;
   using default_ops::eval_get_sign;

   if (eval_is_zero(b))
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
   }
   Backend g;
   eval_gcd(g, a, b);
   if (eval_eq(g, rational_adaptor<Backend>::one()))
   {
      result.num() = a;
      result.denom() = b;
   }
   else
   {
      eval_divide(result.num(), a, g);
      eval_divide(result.denom(), b, g);
   }
   if (eval_get_sign(result.denom()) < 0)
   {
      result.num().negate();
      result.denom().negate();
   }
}
//
// Again for arithmetic types, overload for whatever arithmetic types are directly supported:
//
template <class Backend, class Arithmetic1, class Arithmetic2>
inline void assign_components(rational_adaptor<Backend>& result, const Arithmetic1& a, typename std::enable_if<std::is_arithmetic<Arithmetic1>::value && std::is_arithmetic<Arithmetic2>::value, const Arithmetic2&>::type b)
{
   using default_ops::eval_gcd;
   using default_ops::eval_divide;
   using default_ops::eval_eq;

   if (b == 0)
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
   }

   Backend g;
   result.num()   = a;
   eval_gcd(g, result.num(), b);
   if (eval_eq(g, rational_adaptor<Backend>::one()))
   {
      result.denom() = b;
   }
   else
   {
      eval_divide(result.num(), g);
      eval_divide(result.denom(), b, g);
   }
   if (eval_get_sign(result.denom()) < 0)
   {
      result.num().negate();
      result.denom().negate();
   }
}
template <class Backend, class Arithmetic1, class Arithmetic2>
inline void assign_components(rational_adaptor<Backend>& result, const Arithmetic1& a, typename std::enable_if<!std::is_arithmetic<Arithmetic1>::value || !std::is_arithmetic<Arithmetic2>::value, const Arithmetic2&>::type b)
{
   using default_ops::eval_gcd;
   using default_ops::eval_divide;
   using default_ops::eval_eq;

   Backend g;
   result.num()   = a;
   result.denom() = b;

   if (eval_get_sign(result.denom()) == 0)
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
   }

   eval_gcd(g, result.num(), result.denom());
   if (!eval_eq(g, rational_adaptor<Backend>::one()))
   {
      eval_divide(result.num(), g);
      eval_divide(result.denom(), g);
   }
   if (eval_get_sign(result.denom()) < 0)
   {
      result.num().negate();
      result.denom().negate();
   }
}
//
// Optional comparison operators:
//
template <class Backend>
inline bool eval_is_zero(const rational_adaptor<Backend>& arg)
{
   using default_ops::eval_is_zero;
   return eval_is_zero(arg.num());
}

template <class Backend>
inline int eval_get_sign(const rational_adaptor<Backend>& arg)
{
   using default_ops::eval_get_sign;
   return eval_get_sign(arg.num());
}

template <class Backend>
inline bool eval_eq(const rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   using default_ops::eval_eq;
   return eval_eq(a.num(), b.num()) && eval_eq(a.denom(), b.denom());
}

template <class Backend, class Arithmetic>
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value&& std::is_integral<Arithmetic>::value, bool>::type 
   eval_eq(const rational_adaptor<Backend>& a, Arithmetic b)
{
   using default_ops::eval_eq;
   return eval_eq(a.denom(), rational_adaptor<Backend>::one()) && eval_eq(a.num(), b);
}

template <class Backend, class Arithmetic>
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value&& std::is_integral<Arithmetic>::value, bool>::type 
   eval_eq(Arithmetic b, const rational_adaptor<Backend>& a)
{
   using default_ops::eval_eq;
   return eval_eq(a.denom(), rational_adaptor<Backend>::one()) && eval_eq(a.num(), b);
}

//
// Arithmetic operations, starting with addition:
//
template <class Backend, class Arithmetic> 
void eval_add_subtract_imp(rational_adaptor<Backend>& result, const Arithmetic& arg, bool isaddition)
{
   using default_ops::eval_multiply;
   using default_ops::eval_divide;
   using default_ops::eval_add;
   using default_ops::eval_gcd;
   Backend t;
   eval_multiply(t, result.denom(), arg);
   if (isaddition)
      eval_add(result.num(), t);
   else
      eval_subtract(result.num(), t);
   //
   // There is no need to re-normalize here, we have 
   // (a + bm) / b
   // and gcd(a + bm, b) = gcd(a, b) = 1
   //
   /*
   eval_gcd(t, result.num(), result.denom());
   if (!eval_eq(t, rational_adaptor<Backend>::one()) != 0)
   {
      Backend t2;
      eval_divide(t2, result.num(), t);
      t2.swap(result.num());
      eval_divide(t2, result.denom(), t);
      t2.swap(result.denom());
   }
   */
}

template <class Backend, class Arithmetic> 
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_add(rational_adaptor<Backend>& result, const Arithmetic& arg)
{
   eval_add_subtract_imp(result, arg, true);
}

template <class Backend, class Arithmetic> 
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_subtract(rational_adaptor<Backend>& result, const Arithmetic& arg)
{
   eval_add_subtract_imp(result, arg, false);
}

template <class Backend>
void eval_add_subtract_imp(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b, bool isaddition)
{
   using default_ops::eval_eq;
   using default_ops::eval_multiply;
   using default_ops::eval_divide;
   using default_ops::eval_add;
   using default_ops::eval_subtract;
   //
   // Let  a = an/ad
   //      b = bn/bd
   //      g = gcd(ad, bd)
   // result = rn/rd
   //
   // Then:
   // rn = an * (bd/g) + bn * (ad/g)
   // rd = ad * (bd/g)
   //    = (ad/g) * (bd/g) * g
   //
   // And the whole thing can then be rescaled by
   //      gcd(rn, g)
   //
   Backend gcd, t1, t2, t3, t4;
   //
   // Begin by getting the gcd of the 2 denominators:
   //
   eval_gcd(gcd, a.denom(), b.denom());
   //
   // Do we have gcd > 1:
   //
   if (!eval_eq(gcd, rational_adaptor<Backend>::one()))
   {
      //
      // Scale the denominators by gcd, and put the results in t1 and t2:
      //
      eval_divide(t1, b.denom(), gcd);
      eval_divide(t2, a.denom(), gcd);
      //
      // multiply the numerators by the scale denominators and put the results in t3, t4:
      //
      eval_multiply(t3, a.num(), t1);
      eval_multiply(t4, b.num(), t2);
      //
      // Add them up:
      //
      if (isaddition)
         eval_add(t3, t4);
      else
         eval_subtract(t3, t4);
      //
      // Get the gcd of gcd and our numerator (t3):
      //
      eval_gcd(t4, t3, gcd);
      if (eval_eq(t4, rational_adaptor<Backend>::one()))
      {
         result.num() = t3;
         eval_multiply(result.denom(), t1, a.denom());
      }
      else
      {
         //
         // Uncommon case where gcd is not 1, divide the numerator
         // and the denominator terms by the new gcd.  Note we perform division
         // on the existing gcd value as this is the smallest of the 3 denominator
         // terms we'll be multiplying together, so there's a good chance it's a
         // single limb value already:
         //
         eval_divide(result.num(), t3, t4);
         eval_divide(t3, gcd, t4);
         eval_multiply(t4, t1, t2);
         eval_multiply(result.denom(), t4, t3);
      }
   }
   else
   {
      //
      // Most common case (approx 60%) where gcd is one:
      //
      eval_multiply(t1, a.num(), b.denom());
      eval_multiply(t2, a.denom(), b.num());
      if (isaddition)
         eval_add(result.num(), t1, t2);
      else
         eval_subtract(result.num(), t1, t2);
      eval_multiply(result.denom(), a.denom(), b.denom());
   }
}


template <class Backend>
inline void eval_add(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   eval_add_subtract_imp(result, a, b, true);
}
template <class Backend>
inline void eval_subtract(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   eval_add_subtract_imp(result, a, b, false);
}

template <class Backend, class Arithmetic>
void eval_add_subtract_imp(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const Arithmetic& b, bool isaddition)
{
   using default_ops::eval_add;
   using default_ops::eval_subtract;
   using default_ops::eval_multiply;

   if (&result == &a)
      return eval_add_subtract_imp(result, b, isaddition);

   eval_multiply(result.num(), a.denom(), b);
   if (isaddition)
      eval_add(result.num(), a.num());
   else
      BOOST_IF_CONSTEXPR(std::numeric_limits<Backend>::is_signed == false)
   {
      Backend t;
      eval_subtract(t, a.num(), result.num());
      result.num() = std::move(t);
   }
   else
   {
      eval_subtract(result.num(), a.num());
      result.negate();
   }
   result.denom() = a.denom();
   //
   // There is no need to re-normalize here, we have 
   // (a + bm) / b
   // and gcd(a + bm, b) = gcd(a, b) = 1
   //
}
template <class Backend, class Arithmetic>
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_add(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const Arithmetic& b)
{
   eval_add_subtract_imp(result, a, b, true);
}
template <class Backend, class Arithmetic>
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_subtract(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const Arithmetic& b)
{
   eval_add_subtract_imp(result, a, b, false);
}

//
// Multiplication:
//
template <class Backend> 
void eval_multiply_imp(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const Backend& b_num, const Backend& b_denom)
{
   using default_ops::eval_multiply;
   using default_ops::eval_divide;
   using default_ops::eval_gcd;
   using default_ops::eval_get_sign;
   using default_ops::eval_eq;

   Backend gcd_left, gcd_right, t1, t2;
   eval_gcd(gcd_left, a.num(), b_denom);
   eval_gcd(gcd_right, b_num, a.denom());
   //
   // Unit gcd's are the most likely case:
   //
   bool b_left = eval_eq(gcd_left, rational_adaptor<Backend>::one());
   bool b_right = eval_eq(gcd_right, rational_adaptor<Backend>::one());

   if (b_left && b_right)
   {
      eval_multiply(result.num(), a.num(), b_num);
      eval_multiply(result.denom(), a.denom(), b_denom);
   }
   else if (b_left)
   {
      eval_divide(t2, b_num, gcd_right);
      eval_multiply(result.num(), a.num(), t2);
      eval_divide(t1, a.denom(), gcd_right);
      eval_multiply(result.denom(), t1, b_denom);
   }
   else if (b_right)
   {
      eval_divide(t1, a.num(), gcd_left);
      eval_multiply(result.num(), t1, b_num);
      eval_divide(t2, b_denom, gcd_left);
      eval_multiply(result.denom(), a.denom(), t2);
   }
   else
   {
      eval_divide(t1, a.num(), gcd_left);
      eval_divide(t2, b_num, gcd_right);
      eval_multiply(result.num(), t1, t2);
      eval_divide(t1, a.denom(), gcd_right);
      eval_divide(t2, b_denom, gcd_left);
      eval_multiply(result.denom(), t1, t2);
   }
   //
   // We may have b_denom negative if this is actually division, if so just correct things now:
   //
   if (eval_get_sign(b_denom) < 0)
   {
      result.num().negate();
      result.denom().negate();
   }
}

template <class Backend> 
void eval_multiply(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   using default_ops::eval_multiply;

   if (&a == &b)
   {
      // squaring, gcd's are 1:
      eval_multiply(result.num(), a.num(), b.num());
      eval_multiply(result.denom(), a.denom(), b.denom());
      return;
   }
   eval_multiply_imp(result, a, b.num(), b.denom());
}

template <class Backend, class Arithmetic> 
void eval_multiply_imp(Backend& result_num, Backend& result_denom, Arithmetic arg)
{
   if (arg == 0)
   {
      result_num = rational_adaptor<Backend>::zero();
      result_denom = rational_adaptor<Backend>::one();
      return;
   }
   else if (arg == 1)
      return;

   using default_ops::eval_multiply;
   using default_ops::eval_divide;
   using default_ops::eval_gcd;
   using default_ops::eval_convert_to;

   Backend gcd, t;
   Arithmetic integer_gcd;
   eval_gcd(gcd, result_denom, arg);
   eval_convert_to(&integer_gcd, gcd);
   arg /= integer_gcd;
   if (boost::multiprecision::detail::unsigned_abs(arg) > 1)
   {
      eval_multiply(t, result_num, arg);
      result_num = std::move(t);
   }
   else if (is_minus_one(arg))
      result_num.negate();
   if (integer_gcd > 1)
   {
      eval_divide(t, result_denom, integer_gcd);
      result_denom = std::move(t);
   }
}
template <class Backend> 
void eval_multiply_imp(Backend& result_num, Backend& result_denom, Backend arg)
{
   using default_ops::eval_multiply;
   using default_ops::eval_divide;
   using default_ops::eval_gcd;
   using default_ops::eval_convert_to;
   using default_ops::eval_is_zero;
   using default_ops::eval_eq;
   using default_ops::eval_get_sign;

   if (eval_is_zero(arg))
   {
      result_num = rational_adaptor<Backend>::zero();
      result_denom = rational_adaptor<Backend>::one();
      return;
   }
   else if (eval_eq(arg, rational_adaptor<Backend>::one()))
      return;

   Backend gcd, t;
   eval_gcd(gcd, result_denom, arg);
   if (!eval_eq(gcd, rational_adaptor<Backend>::one()))
   {
      eval_divide(t, arg, gcd);
      arg = t;
   }
   else
      t = arg;
   if (eval_get_sign(arg) < 0)
      t.negate();

   if (!eval_eq(t, rational_adaptor<Backend>::one()))
   {
      eval_multiply(t, result_num, arg);
      result_num = std::move(t);
   }
   else if (eval_get_sign(arg) < 0)
      result_num.negate();
   if (!eval_eq(gcd, rational_adaptor<Backend>::one()))
   {
      eval_divide(t, result_denom, gcd);
      result_denom = std::move(t);
   }
}

template <class Backend, class Arithmetic> 
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_multiply(rational_adaptor<Backend>& result, const Arithmetic& arg)
{
   eval_multiply_imp(result.num(), result.denom(), arg);
}

template <class Backend, class Arithmetic> 
typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && std::is_integral<Arithmetic>::value>::type
   eval_multiply_imp(rational_adaptor<Backend>& result, const Backend& a_num, const Backend& a_denom, Arithmetic b)
{
   if (b == 0)
   {
      result.num() = rational_adaptor<Backend>::zero();
      result.denom() = rational_adaptor<Backend>::one();
      return;
   }
   else if (b == 1)
   {
      result.num() = a_num;
      result.denom() = a_denom;
      return;
   }

   using default_ops::eval_multiply;
   using default_ops::eval_divide;
   using default_ops::eval_gcd;
   using default_ops::eval_convert_to;

   Backend gcd;
   Arithmetic integer_gcd;
   eval_gcd(gcd, a_denom, b);
   eval_convert_to(&integer_gcd, gcd);
   b /= integer_gcd;
   if (boost::multiprecision::detail::unsigned_abs(b) > 1)
      eval_multiply(result.num(), a_num, b);
   else if (is_minus_one(b))
   {
      result.num() = a_num;
      result.num().negate();
   }
   else
      result.num() = a_num;
   if (integer_gcd > 1)
      eval_divide(result.denom(), a_denom, integer_gcd);
   else
      result.denom() = a_denom;
}
template <class Backend> 
inline void eval_multiply_imp(rational_adaptor<Backend>& result, const Backend& a_num, const Backend& a_denom, const Backend& b)
{
   result.num() = a_num;
   result.denom() = a_denom;
   eval_multiply_imp(result.num(), result.denom(), b);
}

template <class Backend, class Arithmetic> 
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_multiply(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const Arithmetic& b)
{
   if (&result == &a)
      return eval_multiply(result, b);

   eval_multiply_imp(result, a.num(), a.denom(), b);
}

template <class Backend, class Arithmetic> 
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_multiply(rational_adaptor<Backend>& result, const Arithmetic& b, const rational_adaptor<Backend>& a)
{
   return eval_multiply(result, a, b);
}

//
// Division:
//
template <class Backend>
inline void eval_divide(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, const rational_adaptor<Backend>& b)
{
   using default_ops::eval_multiply;
   using default_ops::eval_get_sign;

   if (eval_get_sign(b.num()) == 0)
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
      return;
   }
   if (&a == &b)
   {
      // Huh? Really?
      result.num() = result.denom() = rational_adaptor<Backend>::one();
      return;
   }
   if (&result == &b)
   {
      rational_adaptor<Backend> t(b);
      return eval_divide(result, a, t);
   }
   eval_multiply_imp(result, a, b.denom(), b.num());
}

template <class Backend, class Arithmetic> 
inline typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && (std::is_integral<Arithmetic>::value || std::is_same<Arithmetic, Backend>::value)>::type
   eval_divide(rational_adaptor<Backend>& result, const Arithmetic& b, const rational_adaptor<Backend>& a)
{
   using default_ops::eval_get_sign;

   if (eval_get_sign(a.num()) == 0)
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
      return;
   }
   if (&a == &result)
   {
      eval_multiply_imp(result.denom(), result.num(), b);
      result.num().swap(result.denom());
   }
   else
      eval_multiply_imp(result, a.denom(), a.num(), b);

   if (eval_get_sign(result.denom()) < 0)
   {
      result.num().negate();
      result.denom().negate();
   }
}

template <class Backend, class Arithmetic>
typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && std::is_integral<Arithmetic>::value>::type
eval_divide(rational_adaptor<Backend>& result, Arithmetic arg)
{
   if (arg == 0)
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
      return;
   }
   else if (arg == 1)
      return;
   else if (is_minus_one(arg))
   {
      result.negate();
      return;
   }
   if (eval_get_sign(result) == 0)
   {
      return;
   }


   using default_ops::eval_multiply;
   using default_ops::eval_gcd;
   using default_ops::eval_convert_to;
   using default_ops::eval_divide;

   Backend gcd, t;
   Arithmetic integer_gcd;
   eval_gcd(gcd, result.num(), arg);
   eval_convert_to(&integer_gcd, gcd);
   arg /= integer_gcd;

   eval_multiply(t, result.denom(), boost::multiprecision::detail::unsigned_abs(arg));
   result.denom() = std::move(t);
   if (arg < 0)
   {
      result.num().negate();
   }
   if (integer_gcd > 1)
   {
      eval_divide(t, result.num(), integer_gcd);
      result.num() = std::move(t);
   }
}
template <class Backend>
void eval_divide(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, Backend arg)
{
   using default_ops::eval_multiply;
   using default_ops::eval_gcd;
   using default_ops::eval_convert_to;
   using default_ops::eval_divide;
   using default_ops::eval_is_zero;
   using default_ops::eval_eq;
   using default_ops::eval_get_sign;

   if (eval_is_zero(arg))
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
      return;
   }
   else if (eval_eq(a, rational_adaptor<Backend>::one()) || (eval_get_sign(a) == 0))
   {
      if (&result != &a)
         result = a;
      result.denom() = arg;
      return;
   }

   Backend gcd, u_arg, t;
   eval_gcd(gcd, a.num(), arg);
   bool has_unit_gcd = eval_eq(gcd, rational_adaptor<Backend>::one());
   if (!has_unit_gcd)
   {
      eval_divide(u_arg, arg, gcd);
      arg = u_arg;
   }
   else
      u_arg = arg;
   if (eval_get_sign(u_arg) < 0)
      u_arg.negate();

   eval_multiply(t, a.denom(), u_arg);
   result.denom() = std::move(t);
   
   if (!has_unit_gcd)
   {
      eval_divide(t, a.num(), gcd);
      result.num() = std::move(t);
   }
   else if (&result != &a)
      result.num() = a.num();

   if (eval_get_sign(arg) < 0)
   {
      result.num().negate();
   }
}
template <class Backend>
void eval_divide(rational_adaptor<Backend>& result, const Backend& arg)
{
   eval_divide(result, result, arg);
}

template <class Backend, class Arithmetic>
typename std::enable_if<std::is_convertible<Arithmetic, Backend>::value && std::is_integral<Arithmetic>::value>::type
   eval_divide(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& a, Arithmetic arg)
{
   if (&result == &a)
      return eval_divide(result, arg);
   if (arg == 0)
   {
      BOOST_MP_THROW_EXCEPTION(std::overflow_error("Integer division by zero"));
      return;
   }
   else if (arg == 1)
   {
      result = a;
      return;
   }
   else if (is_minus_one(arg))
   {
      result = a;
      result.num().negate();
      return;
   }

   if (eval_get_sign(a) == 0)
   {
      result = a;
      return;
   }

   using default_ops::eval_multiply;
   using default_ops::eval_divide;
   using default_ops::eval_gcd;
   using default_ops::eval_convert_to;

   Backend gcd;
   Arithmetic integer_gcd;
   eval_gcd(gcd, a.num(), arg);
   eval_convert_to(&integer_gcd, gcd);
   arg /= integer_gcd;
   eval_multiply(result.denom(), a.denom(), boost::multiprecision::detail::unsigned_abs(arg));

   if (integer_gcd > 1)
   {
      eval_divide(result.num(), a.num(), integer_gcd);
   }
   else
      result.num() = a.num();
   if (arg < 0)
   {
      result.num().negate();
   }
}

//
// Increment and decrement:
//
template <class Backend> 
inline void eval_increment(rational_adaptor<Backend>& arg)
{
   using default_ops::eval_add;
   eval_add(arg.num(), arg.denom());
}
template <class Backend> 
inline void eval_decrement(rational_adaptor<Backend>& arg)
{
   using default_ops::eval_subtract;
   eval_subtract(arg.num(), arg.denom());
}

//
// abs:
//
template <class Backend> 
inline void eval_abs(rational_adaptor<Backend>& result, const rational_adaptor<Backend>& arg)
{
   using default_ops::eval_abs;
   eval_abs(result.num(), arg.num());
   result.denom() = arg.denom();
}

} // namespace backends

//
// Define a category for this number type, one of:
// 
//    number_kind_integer
//    number_kind_floating_point
//    number_kind_rational
//    number_kind_fixed_point
//    number_kind_complex
//
template<class Backend>
struct number_category<rational_adaptor<Backend> > : public std::integral_constant<int, number_kind_rational>
{};

template <class Backend, expression_template_option ExpressionTemplates>
struct component_type<number<rational_adaptor<Backend>, ExpressionTemplates> >
{
   typedef number<Backend, ExpressionTemplates> type;
};

template <class IntBackend, expression_template_option ET>
inline number<IntBackend, ET> numerator(const number<rational_adaptor<IntBackend>, ET>& val)
{
   return val.backend().num();
}
template <class IntBackend, expression_template_option ET>
inline number<IntBackend, ET> denominator(const number<rational_adaptor<IntBackend>, ET>& val)
{
   return val.backend().denom();
}

template <class Backend>
struct is_unsigned_number<rational_adaptor<Backend> > : public is_unsigned_number<Backend>
{};


}} // namespace boost::multiprecision

namespace std {

   template <class IntBackend, boost::multiprecision::expression_template_option ExpressionTemplates>
   class numeric_limits<boost::multiprecision::number<boost::multiprecision::rational_adaptor<IntBackend>, ExpressionTemplates> > : public std::numeric_limits<boost::multiprecision::number<IntBackend, ExpressionTemplates> >
   {
      using base_type = std::numeric_limits<boost::multiprecision::number<IntBackend> >;
      using number_type = boost::multiprecision::number<boost::multiprecision::rational_adaptor<IntBackend> >;

   public:
      static constexpr bool is_integer = false;
      static constexpr bool is_exact = true;
      static constexpr      number_type(min)() { return (base_type::min)(); }
      static constexpr      number_type(max)() { return (base_type::max)(); }
      static constexpr number_type lowest() { return -(max)(); }
      static constexpr number_type epsilon() { return base_type::epsilon(); }
      static constexpr number_type round_error() { return epsilon() / 2; }
      static constexpr number_type infinity() { return base_type::infinity(); }
      static constexpr number_type quiet_NaN() { return base_type::quiet_NaN(); }
      static constexpr number_type signaling_NaN() { return base_type::signaling_NaN(); }
      static constexpr number_type denorm_min() { return base_type::denorm_min(); }
   };

   template <class IntBackend, boost::multiprecision::expression_template_option ExpressionTemplates>
   constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::rational_adaptor<IntBackend>, ExpressionTemplates> >::is_integer;
   template <class IntBackend, boost::multiprecision::expression_template_option ExpressionTemplates>
   constexpr bool numeric_limits<boost::multiprecision::number<boost::multiprecision::rational_adaptor<IntBackend>, ExpressionTemplates> >::is_exact;

} // namespace std

#endif

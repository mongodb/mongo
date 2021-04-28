///////////////////////////////////////////////////////////////////////////////
//  Copyright 2018 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_PRECISION_HPP
#define BOOST_MP_PRECISION_HPP

#include <boost/multiprecision/traits/is_variable_precision.hpp>
#include <boost/multiprecision/detail/number_base.hpp>
#include <boost/multiprecision/detail/digits.hpp>

namespace boost { namespace multiprecision { namespace detail {

template <class B, boost::multiprecision::expression_template_option ET>
inline constexpr unsigned current_precision_of_last_chance_imp(const boost::multiprecision::number<B, ET>&, const std::integral_constant<bool, false>&)
{
   return std::numeric_limits<boost::multiprecision::number<B, ET> >::digits10;
}
template <class B, boost::multiprecision::expression_template_option ET>
inline BOOST_MP_CXX14_CONSTEXPR unsigned current_precision_of_last_chance_imp(const boost::multiprecision::number<B, ET>& val, const std::integral_constant<bool, true>&)
{
   //
   // We have an arbitrary precision integer, take it's "precision" as the
   // location of the most-significant-bit less the location of the
   // least-significant-bit, ie the number of bits required to represent the
   // the value assuming we will have an exponent to shift things by:
   //
   return val.is_zero() ? 1 : digits2_2_10(msb(abs(val)) - lsb(abs(val)) + 1);
}

template <class B, boost::multiprecision::expression_template_option ET>
inline BOOST_MP_CXX14_CONSTEXPR unsigned current_precision_of_imp(const boost::multiprecision::number<B, ET>& n, const std::integral_constant<bool, true>&)
{
   return n.precision();
}
template <class B, boost::multiprecision::expression_template_option ET>
inline constexpr unsigned current_precision_of_imp(const boost::multiprecision::number<B, ET>& val, const std::integral_constant<bool, false>&)
{
   return current_precision_of_last_chance_imp(val,
                                               std::integral_constant<bool, 
                                                       std::numeric_limits<boost::multiprecision::number<B, ET> >::is_specialized &&
                                                   std::numeric_limits<boost::multiprecision::number<B, ET> >::is_integer && std::numeric_limits<boost::multiprecision::number<B, ET> >::is_exact && !std::numeric_limits<boost::multiprecision::number<B, ET> >::is_modulo > ());
}

template <class Terminal>
inline constexpr unsigned current_precision_of(const Terminal&)
{
   return std::numeric_limits<Terminal>::digits10;
}

template <class Terminal, std::size_t N>
inline constexpr unsigned current_precision_of(const Terminal (&)[N])
{ // For string literals:
   return 0;
}

template <class B, boost::multiprecision::expression_template_option ET>
inline constexpr unsigned current_precision_of(const boost::multiprecision::number<B, ET>& n)
{
   return current_precision_of_imp(n, boost::multiprecision::detail::is_variable_precision<boost::multiprecision::number<B, ET> >());
}

template <class tag, class Arg1>
inline constexpr unsigned current_precision_of(const expression<tag, Arg1, void, void, void>& expr)
{
   return current_precision_of(expr.left_ref());
}

template <class Arg1>
inline constexpr unsigned current_precision_of(const expression<terminal, Arg1, void, void, void>& expr)
{
   return current_precision_of(expr.value());
}

template <class tag, class Arg1, class Arg2>
inline constexpr unsigned current_precision_of(const expression<tag, Arg1, Arg2, void, void>& expr)
{
   return (std::max)(current_precision_of(expr.left_ref()), current_precision_of(expr.right_ref()));
}

template <class tag, class Arg1, class Arg2, class Arg3>
inline constexpr unsigned current_precision_of(const expression<tag, Arg1, Arg2, Arg3, void>& expr)
{
   return (std::max)((std::max)(current_precision_of(expr.left_ref()), current_precision_of(expr.right_ref())), current_precision_of(expr.middle_ref()));
}

#ifdef BOOST_MSVC
#pragma warning(push)
#pragma warning(disable : 4130)
#endif

template <class R, bool = boost::multiprecision::detail::is_variable_precision<R>::value>
struct scoped_default_precision
{
   template <class T>
   constexpr scoped_default_precision(const T&) {}
   template <class T, class U>
   constexpr scoped_default_precision(const T&, const U&) {}
   template <class T, class U, class V>
   constexpr scoped_default_precision(const T&, const U&, const V&) {}

   //
   // This function is never called: in C++17 it won't be compiled either:
   //
   unsigned precision() const
   {
      BOOST_ASSERT("This function should never be called!!" == 0);
      return 0;
   }
};

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

template <class R>
struct scoped_default_precision<R, true>
{
   template <class T>
   BOOST_MP_CXX14_CONSTEXPR scoped_default_precision(const T& a)
   {
      init(current_precision_of(a));
   }
   template <class T, class U>
   BOOST_MP_CXX14_CONSTEXPR scoped_default_precision(const T& a, const U& b)
   {
      init((std::max)(current_precision_of(a), current_precision_of(b)));
   }
   template <class T, class U, class V>
   BOOST_MP_CXX14_CONSTEXPR scoped_default_precision(const T& a, const U& b, const V& c)
   {
      init((std::max)((std::max)(current_precision_of(a), current_precision_of(b)), current_precision_of(c)));
   }
   ~scoped_default_precision()
   {
      if(m_new_prec != m_old_prec)
         R::default_precision(m_old_prec);
   }
   BOOST_MP_CXX14_CONSTEXPR unsigned precision() const
   {
      return m_new_prec;
   }

 private:
   BOOST_MP_CXX14_CONSTEXPR void init(unsigned p)
   {
      m_old_prec = R::default_precision();
      if (p && (p != m_old_prec))
      {
         R::default_precision(p);
         m_new_prec = p;
      }
      else
         m_new_prec = m_old_prec;
   }
   unsigned m_old_prec, m_new_prec;
};

template <class T>
inline BOOST_MP_CXX14_CONSTEXPR void maybe_promote_precision(T*, const std::integral_constant<bool, false>&) {}

template <class T>
inline BOOST_MP_CXX14_CONSTEXPR void maybe_promote_precision(T* obj, const std::integral_constant<bool, true>&)
{
   if (obj->precision() != T::default_precision())
   {
      obj->precision(T::default_precision());
   }
}

template <class T>
inline BOOST_MP_CXX14_CONSTEXPR void maybe_promote_precision(T* obj)
{
   maybe_promote_precision(obj, std::integral_constant<bool, boost::multiprecision::detail::is_variable_precision<T>::value>());
}

#ifndef BOOST_NO_CXX17_IF_CONSTEXPR
#define BOOST_MP_CONSTEXPR_IF_VARIABLE_PRECISION(T) \
   if                                               \
   constexpr(boost::multiprecision::detail::is_variable_precision<T>::value)
#else
#define BOOST_MP_CONSTEXPR_IF_VARIABLE_PRECISION(T) if (boost::multiprecision::detail::is_variable_precision<T>::value)
#endif

}
}
} // namespace boost::multiprecision::detail

#endif // BOOST_MP_IS_BACKEND_HPP

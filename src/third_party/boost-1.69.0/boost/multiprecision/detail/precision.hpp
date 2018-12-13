///////////////////////////////////////////////////////////////////////////////
//  Copyright 2018 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_PRECISION_HPP
#define BOOST_MP_PRECISION_HPP

#include <boost/multiprecision/traits/is_variable_precision.hpp>
#include <boost/multiprecision/detail/number_base.hpp>

namespace boost{ namespace multiprecision{  namespace detail{

   template <class B, boost::multiprecision::expression_template_option ET>
   inline unsigned current_precision_of_imp(const boost::multiprecision::number<B, ET>& n, const mpl::true_&)
   {
      return n.precision();
   }
   template <class B, boost::multiprecision::expression_template_option ET>
   inline BOOST_CONSTEXPR unsigned current_precision_of_imp(const boost::multiprecision::number<B, ET>&, const mpl::false_&)
   {
      return std::numeric_limits<boost::multiprecision::number<B, ET> >::digits10;
   }

   template <class Terminal>
   inline BOOST_CONSTEXPR unsigned current_precision_of(const Terminal&)
   {
      return std::numeric_limits<Terminal>::digits10;
   }

   template <class Terminal, std::size_t N>
   inline BOOST_CONSTEXPR unsigned current_precision_of(const Terminal(&)[N])
   { // For string literals:
      return 0;
   }

   template <class B, boost::multiprecision::expression_template_option ET>
   inline BOOST_CONSTEXPR unsigned current_precision_of(const boost::multiprecision::number<B, ET>& n)
   {
      return current_precision_of_imp(n, boost::multiprecision::detail::is_variable_precision<boost::multiprecision::number<B, ET> >());
   }

   template<class tag, class Arg1>
   inline BOOST_CONSTEXPR unsigned current_precision_of(const expression<tag, Arg1, void, void, void>& expr)
   {
      return current_precision_of(expr.left_ref());
   }

   template<class Arg1>
   inline BOOST_CONSTEXPR unsigned current_precision_of(const expression<terminal, Arg1, void, void, void>& expr)
   {
      return current_precision_of(expr.value());
   }

   template <class tag, class Arg1, class Arg2>
   inline BOOST_CONSTEXPR unsigned current_precision_of(const expression<tag, Arg1, Arg2, void, void>& expr)
   {
      return (std::max)(current_precision_of(expr.left_ref()), current_precision_of(expr.right_ref()));
   }

   template <class tag, class Arg1, class Arg2, class Arg3>
   inline BOOST_CONSTEXPR unsigned current_precision_of(const expression<tag, Arg1, Arg2, Arg3, void>& expr)
   {
      return (std::max)((std::max)(current_precision_of(expr.left_ref()), current_precision_of(expr.right_ref())), current_precision_of(expr.middle_ref()));
   }

   template <class R, bool = boost::multiprecision::detail::is_variable_precision<R>::value>
   struct scoped_default_precision
   {
      template <class T>
      scoped_default_precision(const T&) {}
      template <class T, class U>
      scoped_default_precision(const T&, const U&) {}
      template <class T, class U, class V>
      scoped_default_precision(const T&, const U&, const V&) {}
   };

   template <class R>
   struct scoped_default_precision<R, true>
   {
      template <class T>
      scoped_default_precision(const T& a) 
      {
         init(current_precision_of(a));
      }
      template <class T, class U>
      scoped_default_precision(const T& a, const U& b)
      {
         init((std::max)(current_precision_of(a), current_precision_of(b)));
      }
      template <class T, class U, class V>
      scoped_default_precision(const T& a, const U& b, const V& c)
      {
         init((std::max)((std::max)(current_precision_of(a), current_precision_of(b)), current_precision_of(c)));
      }
      ~scoped_default_precision()
      {
         R::default_precision(m_prec);
      }
   private:
      void init(unsigned p)
      {
         m_prec = R::default_precision();
         if (p)
            R::default_precision(p);
      }
      unsigned m_prec;
   };

   template <class T>
   inline void maybe_promote_precision(T*, const mpl::false_&){}

   template <class T>
   inline void maybe_promote_precision(T* obj, const mpl::true_&)
   {
      if (obj->precision() != T::default_precision())
      {
         obj->precision(T::default_precision());
      }
   }

   template <class T>
   inline void maybe_promote_precision(T* obj)
   {
      maybe_promote_precision(obj, boost::multiprecision::detail::is_variable_precision<T>());
   }

#ifndef BOOST_NO_CXX17_IF_CONSTEXPR
#  define BOOST_MP_CONSTEXPR_IF_VARIABLE_PRECISION(T) if constexpr (boost::multiprecision::detail::is_variable_precision<T>::value)
#else
#  define BOOST_MP_CONSTEXPR_IF_VARIABLE_PRECISION(T) if(boost::multiprecision::detail::is_variable_precision<T>::value)
#endif


}
}
}

#endif // BOOST_MP_IS_BACKEND_HPP

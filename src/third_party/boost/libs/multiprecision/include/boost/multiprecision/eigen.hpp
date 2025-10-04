///////////////////////////////////////////////////////////////////////////////
//  Copyright 2018 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_EIGEN_HPP
#define BOOST_MP_EIGEN_HPP

#include <boost/multiprecision/number.hpp>
#include <Eigen/Core>

//
// Generic Eigen support code:
//
namespace Eigen {

template <class B1, class B2>
struct NumTraitsImp;

template <class B1>
struct NumTraitsImp<B1, B1>
{
   using self_type  = B1;
   using Real       = typename boost::multiprecision::scalar_result_from_possible_complex<self_type>::type;
   using NonInteger = self_type; // Not correct but we can't do much better??
   using Literal    = double;
   using Nested     = self_type;
   enum
   {
      IsComplex             = boost::multiprecision::number_category<self_type>::value == boost::multiprecision::number_kind_complex,
      IsInteger             = boost::multiprecision::number_category<self_type>::value == boost::multiprecision::number_kind_integer,
      ReadCost              = 1,
      AddCost               = 4,
      MulCost               = 8,
      IsSigned              = std::numeric_limits<self_type>::is_specialized ? std::numeric_limits<self_type>::is_signed : true,
      RequireInitialization = 1,
   };
   static Real epsilon()
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return std::numeric_limits<Real>::epsilon();
   }
   static Real dummy_precision()
   {
      return 1000 * epsilon();
   }
   static Real highest()
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return (std::numeric_limits<Real>::max)();
   }
   static Real lowest()
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return (std::numeric_limits<Real>::min)();
   }
   static int digits10_imp(const std::integral_constant<bool, true>&)
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return std::numeric_limits<Real>::digits10;
   }
   template <bool B>
   static int digits10_imp(const std::integral_constant<bool, B>&)
   {
      return Real::thread_default_precision();
   }
   static int digits10()
   {
      return digits10_imp(std::integral_constant < bool, std::numeric_limits<Real>::digits10 && (std::numeric_limits<Real>::digits10 != INT_MAX) ? true : false > ());
   }
   static int digits()
   {
      // return the number of digits in the component type in case Real is complex
      // and we have no numeric_limits specialization.
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return std::numeric_limits<Real>::digits;
   }
   static int min_exponent()
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return std::numeric_limits<Real>::min_exponent;
   }
   static int max_exponent()
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return std::numeric_limits<Real>::max_exponent;
   }
   static Real infinity()
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return std::numeric_limits<Real>::infinity();
   }
   static Real quiet_NaN()
   {
      static_assert(std::numeric_limits<Real>::is_specialized, "Eigen's NumTraits instantiated on a type with no numeric_limits support.  Are you using a variable precision type?");
      return std::numeric_limits<Real>::quiet_NaN();
   }
};   
   
template <class B1, class B2>
struct NumTraitsImp : public NumTraitsImp<B2, B2>
{
   //
   // This version is instantiated when B1 and B2 are different types, this happens for rational/complex/interval
   // types, in which case many methods defer to those of the "component type" B2.
   //
   using self_type  = B1;
   using Real       = typename boost::multiprecision::scalar_result_from_possible_complex<self_type>::type;
   using NonInteger = self_type; // Not correct but we can't do much better??
   using Literal    = double;
   using Nested     = self_type;
   enum
   {
      IsComplex             = boost::multiprecision::number_category<self_type>::value == boost::multiprecision::number_kind_complex,
      IsInteger             = boost::multiprecision::number_category<self_type>::value == boost::multiprecision::number_kind_integer,
      ReadCost              = 1,
      AddCost               = 4,
      MulCost               = 8,
      IsSigned              = std::numeric_limits<self_type>::is_specialized ? std::numeric_limits<self_type>::is_signed : true,
      RequireInitialization = 1,
   };
   static B2 epsilon()
   {
      return NumTraitsImp<B2, B2>::epsilon();
   }
   static B2 dummy_precision()
   {
      return 1000 * epsilon();
   }
   static B2 highest()
   {
      return NumTraitsImp<B2, B2>::highest();
   }
   static B2 lowest()
   {
      return NumTraitsImp<B2, B2>::lowest();
   }
   static int digits10()
   {
      return NumTraitsImp<B2, B2>::digits10();
   }
   static int digits()
   {
      return NumTraitsImp<B2, B2>::digits();
   }
   static int min_exponent()
   {
      return NumTraitsImp<B2, B2>::min_exponent();
   }
   static int max_exponent()
   {
      return NumTraitsImp<B2, B2>::max_exponent();
   }
   static B2 infinity()
   {
      return NumTraitsImp<B2, B2>::infinity();
   }
   static B2 quiet_NaN()
   {
      return NumTraitsImp<B2, B2>::quiet_NaN();
   }
};

template <class Backend, boost::multiprecision::expression_template_option ExpressionTemplates>
struct NumTraits<boost::multiprecision::number<Backend, ExpressionTemplates> > : public NumTraitsImp<boost::multiprecision::number<Backend, ExpressionTemplates>, typename boost::multiprecision::number<Backend, ExpressionTemplates>::value_type>
{};
template <class tag, class Arg1, class Arg2, class Arg3, class Arg4>
struct NumTraits<boost::multiprecision::detail::expression<tag, Arg1, Arg2, Arg3, Arg4> > : public NumTraits<typename boost::multiprecision::detail::expression<tag, Arg1, Arg2, Arg3, Arg4>::result_type>
{};

#define BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(A)                                                                                                                                                                           \
   template <class Backend, boost::multiprecision::expression_template_option ExpressionTemplates, typename BinaryOp>                                                                                                  \
   struct ScalarBinaryOpTraits<boost::multiprecision::number<Backend, ExpressionTemplates>, A, BinaryOp>                                                                                                               \
   {                                                                                                                                                                                                                   \
      /*static_assert(boost::multiprecision::is_compatible_arithmetic_type<A, boost::multiprecision::number<Backend, ExpressionTemplates> >::value, "Interoperability with this arithmetic type is not supported.");*/ \
      using ReturnType = boost::multiprecision::number<Backend, ExpressionTemplates>;                                                                                                                                  \
   };                                                                                                                                                                                                                  \
   template <class Backend, boost::multiprecision::expression_template_option ExpressionTemplates, typename BinaryOp>                                                                                                  \
   struct ScalarBinaryOpTraits<A, boost::multiprecision::number<Backend, ExpressionTemplates>, BinaryOp>                                                                                                               \
   {                                                                                                                                                                                                                   \
      /*static_assert(boost::multiprecision::is_compatible_arithmetic_type<A, boost::multiprecision::number<Backend, ExpressionTemplates> >::value, "Interoperability with this arithmetic type is not supported.");*/ \
      using ReturnType = boost::multiprecision::number<Backend, ExpressionTemplates>;                                                                                                                                  \
   };

BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(float)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(double)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(long double)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(char)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(unsigned char)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(signed char)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(short)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(unsigned short)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(int)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(unsigned int)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(long)
BOOST_MP_EIGEN_SCALAR_TRAITS_DECL(unsigned long)

#if 0    
      template<class Backend, boost::multiprecision::expression_template_option ExpressionTemplates, class Backend2, boost::multiprecision::expression_template_option ExpressionTemplates2, typename BinaryOp>
   struct ScalarBinaryOpTraits<boost::multiprecision::number<Backend, ExpressionTemplates>, boost::multiprecision::number<Backend2, ExpressionTemplates2>, BinaryOp>
   {
      static_assert(
         boost::multiprecision::is_compatible_arithmetic_type<boost::multiprecision::number<Backend2, ExpressionTemplates2>, boost::multiprecision::number<Backend, ExpressionTemplates> >::value
         || boost::multiprecision::is_compatible_arithmetic_type<boost::multiprecision::number<Backend, ExpressionTemplates>, boost::multiprecision::number<Backend2, ExpressionTemplates2> >::value, "Interoperability with this arithmetic type is not supported.");
      using ReturnType = typename std::conditional<std::is_convertible<boost::multiprecision::number<Backend2, ExpressionTemplates2>, boost::multiprecision::number<Backend, ExpressionTemplates> >::value,
         boost::multiprecision::number<Backend, ExpressionTemplates>, boost::multiprecision::number<Backend2, ExpressionTemplates2> >::type;
   };

   template<unsigned D, typename BinaryOp>
   struct ScalarBinaryOpTraits<boost::multiprecision::number<boost::multiprecision::backends::mpc_complex_backend<D>, boost::multiprecision::et_on>, boost::multiprecision::mpfr_float, BinaryOp>
   {
      using ReturnType = boost::multiprecision::number<boost::multiprecision::backends::mpc_complex_backend<D>, boost::multiprecision::et_on>;
   };

   template<typename BinaryOp>
   struct ScalarBinaryOpTraits<boost::multiprecision::mpfr_float, boost::multiprecision::mpc_complex, BinaryOp>
   {
      using ReturnType = boost::multiprecision::number<boost::multiprecision::backends::mpc_complex_backend<0>, boost::multiprecision::et_on>;
   };

   template<class Backend, boost::multiprecision::expression_template_option ExpressionTemplates, typename BinaryOp>
   struct ScalarBinaryOpTraits<boost::multiprecision::number<Backend, ExpressionTemplates>, boost::multiprecision::number<Backend, ExpressionTemplates>, BinaryOp>
   {
      using ReturnType = boost::multiprecision::number<Backend, ExpressionTemplates>;
   };
#endif

template <class Backend, boost::multiprecision::expression_template_option ExpressionTemplates, class tag, class Arg1, class Arg2, class Arg3, class Arg4, typename BinaryOp>
struct ScalarBinaryOpTraits<boost::multiprecision::number<Backend, ExpressionTemplates>, boost::multiprecision::detail::expression<tag, Arg1, Arg2, Arg3, Arg4>, BinaryOp>
{
   static_assert(std::is_convertible<typename boost::multiprecision::detail::expression<tag, Arg1, Arg2, Arg3, Arg4>::result_type, boost::multiprecision::number<Backend, ExpressionTemplates> >::value, "Interoperability with this arithmetic type is not supported.");
   using ReturnType = boost::multiprecision::number<Backend, ExpressionTemplates>;
};

template <class tag, class Arg1, class Arg2, class Arg3, class Arg4, class Backend, boost::multiprecision::expression_template_option ExpressionTemplates, typename BinaryOp>
struct ScalarBinaryOpTraits<boost::multiprecision::detail::expression<tag, Arg1, Arg2, Arg3, Arg4>, boost::multiprecision::number<Backend, ExpressionTemplates>, BinaryOp>
{
   static_assert(std::is_convertible<typename boost::multiprecision::detail::expression<tag, Arg1, Arg2, Arg3, Arg4>::result_type, boost::multiprecision::number<Backend, ExpressionTemplates> >::value, "Interoperability with this arithmetic type is not supported.");
   using ReturnType = boost::multiprecision::number<Backend, ExpressionTemplates>;
};

namespace numext {
   using boost::multiprecision::conj;
}

} // namespace Eigen

#endif

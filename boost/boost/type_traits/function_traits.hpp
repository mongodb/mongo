
//  Copyright 2000 John Maddock (john@johnmaddock.co.uk)
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_FUNCTION_TRAITS_HPP_INCLUDED
#define BOOST_TT_FUNCTION_TRAITS_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/type_traits/is_function.hpp>
#include <boost/type_traits/add_pointer.hpp>

namespace boost {

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
namespace detail {

template<typename Function> struct function_traits_helper;

template<typename R>
struct function_traits_helper<R (*)(void)>
{
  BOOST_STATIC_CONSTANT(int, arity = 0);
  typedef R result_type;
};

template<typename R, typename T1>
struct function_traits_helper<R (*)(T1)>
{
  BOOST_STATIC_CONSTANT(int, arity = 1);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T1 argument_type;
};

template<typename R, typename T1, typename T2>
struct function_traits_helper<R (*)(T1, T2)>
{
  BOOST_STATIC_CONSTANT(int, arity = 2);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T1 first_argument_type;
  typedef T2 second_argument_type;
};

template<typename R, typename T1, typename T2, typename T3>
struct function_traits_helper<R (*)(T1, T2, T3)>
{
  BOOST_STATIC_CONSTANT(int, arity = 3);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
};

template<typename R, typename T1, typename T2, typename T3, typename T4>
struct function_traits_helper<R (*)(T1, T2, T3, T4)>
{
  BOOST_STATIC_CONSTANT(int, arity = 4);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
  typedef T4 arg4_type;
};

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5>
struct function_traits_helper<R (*)(T1, T2, T3, T4, T5)>
{
  BOOST_STATIC_CONSTANT(int, arity = 5);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
  typedef T4 arg4_type;
  typedef T5 arg5_type;
};

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6>
struct function_traits_helper<R (*)(T1, T2, T3, T4, T5, T6)>
{
  BOOST_STATIC_CONSTANT(int, arity = 6);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
  typedef T4 arg4_type;
  typedef T5 arg5_type;
  typedef T6 arg6_type;
};

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7>
struct function_traits_helper<R (*)(T1, T2, T3, T4, T5, T6, T7)>
{
  BOOST_STATIC_CONSTANT(int, arity = 7);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
  typedef T4 arg4_type;
  typedef T5 arg5_type;
  typedef T6 arg6_type;
  typedef T7 arg7_type;
};

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7, typename T8>
struct function_traits_helper<R (*)(T1, T2, T3, T4, T5, T6, T7, T8)>
{
  BOOST_STATIC_CONSTANT(int, arity = 8);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
  typedef T4 arg4_type;
  typedef T5 arg5_type;
  typedef T6 arg6_type;
  typedef T7 arg7_type;
  typedef T8 arg8_type;
};

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7, typename T8, typename T9>
struct function_traits_helper<R (*)(T1, T2, T3, T4, T5, T6, T7, T8, T9)>
{
  BOOST_STATIC_CONSTANT(int, arity = 9);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
  typedef T4 arg4_type;
  typedef T5 arg5_type;
  typedef T6 arg6_type;
  typedef T7 arg7_type;
  typedef T8 arg8_type;
  typedef T9 arg9_type;
};

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7, typename T8, typename T9,
         typename T10>
struct function_traits_helper<R (*)(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10)>
{
  BOOST_STATIC_CONSTANT(int, arity = 10);
  typedef R result_type;
  typedef T1 arg1_type;
  typedef T2 arg2_type;
  typedef T3 arg3_type;
  typedef T4 arg4_type;
  typedef T5 arg5_type;
  typedef T6 arg6_type;
  typedef T7 arg7_type;
  typedef T8 arg8_type;
  typedef T9 arg9_type;
  typedef T10 arg10_type;
};

} // end namespace detail

template<typename Function>
struct function_traits : 
    public detail::function_traits_helper<typename boost::add_pointer<Function>::type>
{
};

#else

namespace detail {

template<int N> 
struct type_of_size
{
  char elements[N];
};

template<typename R>
type_of_size<1> function_arity_helper(R (*f)());

template<typename R, typename T1>
type_of_size<2> function_arity_helper(R (*f)(T1));

template<typename R, typename T1, typename T2>
type_of_size<3> function_arity_helper(R (*f)(T1, T2));

template<typename R, typename T1, typename T2, typename T3>
type_of_size<4> function_arity_helper(R (*f)(T1, T2, T3));

template<typename R, typename T1, typename T2, typename T3, typename T4>
type_of_size<5> function_arity_helper(R (*f)(T1, T2, T3, T4));

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5>
type_of_size<6> function_arity_helper(R (*f)(T1, T2, T3, T4, T5));

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6>
type_of_size<7> function_arity_helper(R (*f)(T1, T2, T3, T4, T5, T6));

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7>
type_of_size<8> function_arity_helper(R (*f)(T1, T2, T3, T4, T5, T6, T7));

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7, typename T8>
type_of_size<9> function_arity_helper(R (*f)(T1, T2, T3, T4, T5, T6, T7, T8));

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7, typename T8, typename T9>
type_of_size<10> function_arity_helper(R (*f)(T1, T2, T3, T4, T5, T6, T7, T8, 
                                              T9));

template<typename R, typename T1, typename T2, typename T3, typename T4,
         typename T5, typename T6, typename T7, typename T8, typename T9,
         typename T10>
type_of_size<11> function_arity_helper(R (*f)(T1, T2, T3, T4, T5, T6, T7, T8, 
                                              T9, T10));
} // end namespace detail

// Won't work with references
template<typename Function>
struct function_traits
{
  BOOST_STATIC_CONSTANT(int, arity = (sizeof(detail::function_arity_helper((Function*)0))-1));
};

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
}

#endif // BOOST_TT_FUNCTION_TRAITS_HPP_INCLUDED

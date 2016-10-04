///////////////////////////////////////////////////////////////////////////////
//  Copyright 2011 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MATH_BIG_NUM_DEF_OPS
#define BOOST_MATH_BIG_NUM_DEF_OPS

#include <boost/math/policies/error_handling.hpp>
#include <boost/multiprecision/detail/number_base.hpp>
#include <boost/math/special_functions/fpclassify.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/mpl/front.hpp>
#include <boost/mpl/fold.hpp>
#include <boost/cstdint.hpp>
#include <boost/type_traits/make_unsigned.hpp>

#ifndef INSTRUMENT_BACKEND
#ifndef BOOST_MP_INSTRUMENT
#define INSTRUMENT_BACKEND(x)
#else
#define INSTRUMENT_BACKEND(x)\
   std::cout << BOOST_STRINGIZE(x) << " = " << x.str(0, std::ios_base::scientific) << std::endl;
#endif
#endif


namespace boost{ namespace multiprecision{ namespace default_ops{

#ifdef BOOST_MSVC
// warning C4127: conditional expression is constant
#pragma warning(push)
#pragma warning(disable:4127)
#endif
//
// Default versions of mixed arithmetic, these just construct a temporary
// from the arithmetic value and then do the arithmetic on that, two versions
// of each depending on whether the backend can be directly constructed from type V.
//
// Note that we have to provide *all* the template parameters to class number when used in
// enable_if as MSVC-10 won't compile the code if we rely on a computed-default parameter.
// Since the result of the test doesn't depend on whether expression templates are on or off
// we just use et_on everywhere.  We could use a BOOST_WORKAROUND but that just obfuscates the
// code even more....
//
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value >::type 
   eval_add(T& result, V const& v)
{
   T t;
   t = v;
   eval_add(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value >::type 
   eval_add(T& result, V const& v)
{
   T t(v);
   eval_add(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_subtract(T& result, V const& v)
{
   T t;
   t = v;
   eval_subtract(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value>::type
   eval_subtract(T& result, V const& v)
{
   T t(v);
   eval_subtract(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_multiply(T& result, V const& v)
{
   T t;
   t = v;
   eval_multiply(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value>::type
   eval_multiply(T& result, V const& v)
{
   T t(v);
   eval_multiply(result, t);
}

template <class T, class U, class V>
void eval_multiply(T& t, const U& u, const V& v);

template <class T, class U, class V>
inline typename disable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_add(T& t, const U& u, const V& v)
{
   T z;
   eval_multiply(z, u, v);
   eval_add(t, z);
}
template <class T, class U, class V>
inline typename enable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_add(T& t, const U& u, const V& v)
{
   eval_multiply_add(t, v, u);
}
template <class T, class U, class V>
inline typename disable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_subtract(T& t, const U& u, const V& v)
{
   T z;
   eval_multiply(z, u, v);
   eval_subtract(t, z);
}
template <class T, class U, class V>
inline typename enable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_subtract(T& t, const U& u, const V& v)
{
   eval_multiply_subtract(t, v, u);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_divide(T& result, V const& v)
{
   T t;
   t = v;
   eval_divide(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value>::type
   eval_divide(T& result, V const& v)
{
   T t(v);
   eval_divide(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_modulus(T& result, V const& v)
{
   T t;
   t = v;
   eval_modulus(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value&& is_convertible<V, T>::value>::type
   eval_modulus(T& result, V const& v)
{
   T t(v);
   eval_modulus(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_bitwise_and(T& result, V const& v)
{
   T t;
   t = v;
   eval_bitwise_and(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value>::type
   eval_bitwise_and(T& result, V const& v)
{
   T t(v);
   eval_bitwise_and(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_bitwise_or(T& result, V const& v)
{
   T t;
   t = v;
   eval_bitwise_or(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value>::type
   eval_bitwise_or(T& result, V const& v)
{
   T t(v);
   eval_bitwise_or(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_bitwise_xor(T& result, V const& v)
{
   T t;
   t = v;
   eval_bitwise_xor(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value>::type
   eval_bitwise_xor(T& result, V const& v)
{
   T t(v);
   eval_bitwise_xor(result, t);
}

template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && !is_convertible<V, T>::value>::type
   eval_complement(T& result, V const& v)
{
   T t;
   t = v;
   eval_complement(result, t);
}
template <class T, class V>
inline typename enable_if_c<is_convertible<V, number<T, et_on> >::value && is_convertible<V, T>::value>::type
   eval_complement(T& result, V const& v)
{
   T t(v);
   eval_complement(result, t);
}

//
// Default versions of 3-arg arithmetic functions, these mostly just forward to the 2 arg versions:
//
template <class T, class U, class V>
void eval_add(T& t, const U& u, const V& v);

template <class T>
inline void eval_add_default(T& t, const T& u, const T& v)
{
   if(&t == &v)
   {
      eval_add(t, u);
   }
   else if(&t == &u)
   {
      eval_add(t, v);
   }
   else
   {
      t = u;
      eval_add(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_add_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_add(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_add_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_add(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value>::type eval_add_default(T& t, const U& u, const T& v)
{
   eval_add(t, v, u);
}
template <class T, class U, class V>
inline void eval_add_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      eval_add(t, u);
   }
   else
   {
      t = u;
      eval_add(t, v);
   }
}
template <class T, class U, class V>
inline void eval_add(T& t, const U& u, const V& v)
{
   eval_add_default(t, u, v);
}

template <class T, class U, class V>
void eval_subtract(T& t, const U& u, const V& v);

template <class T>
inline void eval_subtract_default(T& t, const T& u, const T& v)
{
   if((&t == &v) && is_signed_number<T>::value)
   {
      eval_subtract(t, u);
      t.negate();
   }
   else if(&t == &u)
   {
      eval_subtract(t, v);
   }
   else
   {
      t = u;
      eval_subtract(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_subtract_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_subtract(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_subtract_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_subtract(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_signed_number<T>::value>::type eval_subtract_default(T& t, const U& u, const T& v)
{
   eval_subtract(t, v, u);
   t.negate();
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_unsigned_number<T>::value>::type eval_subtract_default(T& t, const U& u, const T& v)
{
   T temp(u);
   eval_subtract(t, temp, v);
}
template <class T, class U, class V>
inline void eval_subtract_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      eval_subtract(t, u);
      t.negate();
   }
   else
   {
      t = u;
      eval_subtract(t, v);
   }
}
template <class T, class U, class V>
inline void eval_subtract(T& t, const U& u, const V& v)
{
   eval_subtract_default(t, u, v);
}

template <class T>
inline void eval_multiply_default(T& t, const T& u, const T& v)
{
   if(&t == &v)
   {
      eval_multiply(t, u);
   }
   else if(&t == &u)
   {
      eval_multiply(t, v);
   }
   else
   {
      t = u;
      eval_multiply(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_multiply_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_multiply(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_multiply_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_multiply(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value>::type eval_multiply_default(T& t, const U& u, const T& v)
{
   eval_multiply(t, v, u);
}
template <class T, class U, class V>
inline void eval_multiply_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      eval_multiply(t, u);
   }
   else
   {
      t = u;
      eval_multiply(t, v);
   }
}
template <class T, class U, class V>
inline void eval_multiply(T& t, const U& u, const V& v)
{
   eval_multiply_default(t, u, v);
}

template <class T, class U, class V, class X>
inline typename disable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_add(T& t, const U& u, const V& v, const X& x)
{
   if((void*)&x == (void*)&t)
   {
      T z;
      z = x;
      eval_multiply_add(t, u, v, z);
   }
   else
   {
      eval_multiply(t, u, v);
      eval_add(t, x);
   }
}
template <class T, class U, class V, class X>
inline typename enable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_add(T& t, const U& u, const V& v, const X& x)
{
   eval_multiply_add(t, v, u, x);
}
template <class T, class U, class V, class X>
inline typename disable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_subtract(T& t, const U& u, const V& v, const X& x)
{
   if((void*)&x == (void*)&t)
   {
      T z;
      z = x;
      eval_multiply_subtract(t, u, v, z);
   }
   else
   {
      eval_multiply(t, u, v);
      eval_subtract(t, x);
   }
}
template <class T, class U, class V, class X>
inline typename enable_if_c<!is_same<T, U>::value && is_same<T, V>::value>::type eval_multiply_subtract(T& t, const U& u, const V& v, const X& x)
{
   eval_multiply_subtract(t, v, u, x);
}

template <class T, class U, class V>
void eval_divide(T& t, const U& u, const V& v);

template <class T>
inline void eval_divide_default(T& t, const T& u, const T& v)
{
   if(&t == &u)
      eval_divide(t, v);
   else if(&t == &v)
   {
      T temp;
      eval_divide(temp, u, v);
      temp.swap(t);
   }
   else
   {
      t = u;
      eval_divide(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_divide_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_divide(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_divide_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_divide(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_divide_default(T& t, const U& u, const T& v)
{
   T uu;
   uu = u;
   eval_divide(t, uu, v);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_divide_default(T& t, const U& u, const T& v)
{
   T uu(u);
   eval_divide(t, uu, v);
}
template <class T, class U, class V>
inline void eval_divide_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      T temp(u);
      eval_divide(temp, v);
      t = temp;
   }
   else
   {
      t = u;
      eval_divide(t, v);
   }
}
template <class T, class U, class V>
inline void eval_divide(T& t, const U& u, const V& v)
{
   eval_divide_default(t, u, v);
}

template <class T, class U, class V>
void eval_modulus(T& t, const U& u, const V& v);

template <class T>
inline void eval_modulus_default(T& t, const T& u, const T& v)
{
   if(&t == &u)
      eval_modulus(t, v);
   else if(&t == &v)
   {
      T temp;
      eval_modulus(temp, u, v);
      temp.swap(t);
   }
   else
   {
      t = u;
      eval_modulus(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_modulus_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_modulus(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_modulus_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_modulus(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_modulus_default(T& t, const U& u, const T& v)
{
   T uu;
   uu = u;
   eval_modulus(t, uu, v);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_modulus_default(T& t, const U& u, const T& v)
{
   T uu(u);
   eval_modulus(t, uu, v);
}
template <class T, class U, class V>
inline void eval_modulus_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      T temp(u);
      eval_modulus(temp, v);
      t = temp;
   }
   else
   {
      t = u;
      eval_modulus(t, v);
   }
}
template <class T, class U, class V>
inline void eval_modulus(T& t, const U& u, const V& v)
{
   eval_modulus_default(t, u, v);
}

template <class T, class U, class V>
void eval_bitwise_and(T& t, const U& u, const V& v);

template <class T>
inline void eval_bitwise_and_default(T& t, const T& u, const T& v)
{
   if(&t == &v)
   {
      eval_bitwise_and(t, u);
   }
   else if(&t == &u)
   {
      eval_bitwise_and(t, v);
   }
   else
   {
      t = u;
      eval_bitwise_and(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_bitwise_and_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_bitwise_and(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_bitwise_and_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_bitwise_and(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value>::type eval_bitwise_and_default(T& t, const U& u, const T& v)
{
   eval_bitwise_and(t, v, u);
}
template <class T, class U, class V>
inline void eval_bitwise_and_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      eval_bitwise_and(t, u);
   }
   else
   {
      t = u;
      eval_bitwise_and(t, v);
   }
}
template <class T, class U, class V>
inline void eval_bitwise_and(T& t, const U& u, const V& v)
{
   eval_bitwise_and_default(t, u, v);
}

template <class T, class U, class V>
void eval_bitwise_or(T& t, const U& u, const V& v);

template <class T>
inline void eval_bitwise_or_default(T& t, const T& u, const T& v)
{
   if(&t == &v)
   {
      eval_bitwise_or(t, u);
   }
   else if(&t == &u)
   {
      eval_bitwise_or(t, v);
   }
   else
   {
      t = u;
      eval_bitwise_or(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_bitwise_or_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_bitwise_or(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_bitwise_or_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_bitwise_or(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value>::type eval_bitwise_or_default(T& t, const U& u, const T& v)
{
   eval_bitwise_or(t, v, u);
}
template <class T, class U, class V>
inline void eval_bitwise_or_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      eval_bitwise_or(t, u);
   }
   else
   {
      t = u;
      eval_bitwise_or(t, v);
   }
}
template <class T, class U, class V>
inline void eval_bitwise_or(T& t, const U& u, const V& v)
{
   eval_bitwise_or_default(t, u, v);
}

template <class T, class U, class V>
void eval_bitwise_xor(T& t, const U& u, const V& v);

template <class T>
inline void eval_bitwise_xor_default(T& t, const T& u, const T& v)
{
   if(&t == &v)
   {
      eval_bitwise_xor(t, u);
   }
   else if(&t == &u)
   {
      eval_bitwise_xor(t, v);
   }
   else
   {
      t = u;
      eval_bitwise_xor(t, v);
   }
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && !is_convertible<U, T>::value>::type eval_bitwise_xor_default(T& t, const T& u, const U& v)
{
   T vv;
   vv = v;
   eval_bitwise_xor(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value && is_convertible<U, T>::value>::type eval_bitwise_xor_default(T& t, const T& u, const U& v)
{
   T vv(v);
   eval_bitwise_xor(t, u, vv);
}
template <class T, class U>
inline typename enable_if_c<is_convertible<U, number<T, et_on> >::value>::type eval_bitwise_xor_default(T& t, const U& u, const T& v)
{
   eval_bitwise_xor(t, v, u);
}
template <class T, class U, class V>
inline void eval_bitwise_xor_default(T& t, const U& u, const V& v)
{
   if(is_same<T, V>::value && ((void*)&t == (void*)&v))
   {
      eval_bitwise_xor(t, u);
   }
   else
   {
      t = u;
      eval_bitwise_xor(t, v);
   }
}
template <class T, class U, class V>
inline void eval_bitwise_xor(T& t, const U& u, const V& v)
{
   eval_bitwise_xor_default(t, u, v);
}

template <class T>
inline void eval_increment(T& val)
{
   typedef typename mpl::front<typename T::unsigned_types>::type ui_type;
   eval_add(val, static_cast<ui_type>(1u));
}
template <class T>
inline void eval_decrement(T& val)
{
   typedef typename mpl::front<typename T::unsigned_types>::type ui_type;
   eval_subtract(val, static_cast<ui_type>(1u));
}

template <class T, class V>
inline void eval_left_shift(T& result, const T& arg, const V val)
{
   result = arg;
   eval_left_shift(result, val);
}

template <class T, class V>
inline void eval_right_shift(T& result, const T& arg, const V val)
{
   result = arg;
   eval_right_shift(result, val);
}

template <class T>
inline bool eval_is_zero(const T& val)
{
   typedef typename mpl::front<typename T::unsigned_types>::type ui_type;
   return val.compare(static_cast<ui_type>(0)) == 0;
}
template <class T>
inline int eval_get_sign(const T& val)
{
   typedef typename mpl::front<typename T::unsigned_types>::type ui_type;
   return val.compare(static_cast<ui_type>(0));
}

template <class T, class V>
inline void assign_components_imp(T& result, const V& v1, const V& v2, const mpl::int_<number_kind_rational>&)
{
   result = v1;
   T t;
   t = v2;
   eval_divide(result, t);
}

template <class T, class V>
inline void assign_components(T& result, const V& v1, const V& v2)
{
   return assign_components_imp(result, v1, v2, typename number_category<T>::type());
}

template <class R, int b>
struct has_enough_bits
{
   template <class T>
   struct type : public mpl::and_<mpl::not_<is_same<R, T> >, mpl::bool_<std::numeric_limits<T>::digits >= b> >{};
};

template <class R>
struct terminal
{
   terminal(const R& v) : value(v){}
   terminal(){}
   terminal& operator = (R val) { value = val;  return *this; }
   R value;
   operator R()const {  return value;  }
};

template<class R, class B>
struct calculate_next_larger_type
{
   // Find which list we're looking through:
   typedef typename mpl::if_<
      is_signed<R>,
      typename B::signed_types,
      typename mpl::if_<
         is_unsigned<R>,
         typename B::unsigned_types,
         typename B::float_types
      >::type
   >::type list_type;
   // A predicate to find a type with enough bits:
   typedef typename has_enough_bits<R, std::numeric_limits<R>::digits>::template type<mpl::_> pred_type;
   // See if the last type is in the list, if so we have to start after this:
   typedef typename mpl::find_if<
      list_type,
      is_same<R, mpl::_>
   >::type start_last;
   // Where we're starting from, either the start of the sequence or the last type found:
   typedef typename mpl::if_<is_same<start_last, typename mpl::end<list_type>::type>, typename mpl::begin<list_type>::type, start_last>::type start_seq;
   // The range we're searching:
   typedef mpl::iterator_range<start_seq, typename mpl::end<list_type>::type> range;
   // Find the next type:
   typedef typename mpl::find_if<
      range,
      pred_type
   >::type iter_type;
   // Either the next type, or a "terminal" to indicate we've run out of types to search:
   typedef typename mpl::eval_if<
      is_same<typename mpl::end<list_type>::type, iter_type>,
      mpl::identity<terminal<R> >,
      mpl::deref<iter_type>
      >::type type;
};

template <class R, class T>
inline bool check_in_range(const T& t)
{
   // Can t fit in an R?
   if(std::numeric_limits<R>::is_specialized && std::numeric_limits<R>::is_bounded && (t > (std::numeric_limits<R>::max)()))
      return true;
   return false;
}

template <class R, class T>
inline bool check_in_range(const terminal<T>&)
{
   return false;
}

template <class R, class B>
inline void eval_convert_to(R* result, const B& backend)
{
   typedef typename calculate_next_larger_type<R, B>::type next_type;
   next_type n;
   eval_convert_to(&n, backend);
   if(check_in_range<R>(n))
   {
      *result = (std::numeric_limits<R>::max)();
   }
   else
      *result = static_cast<R>(n);
}

template <class R, class B>
inline void eval_convert_to(terminal<R>* result, const B& backend)
{
   //
   // We ran out of types to try for the conversion, try
   // a lexical_cast and hope for the best:
   //
   result->value = boost::lexical_cast<R>(backend.str(0, std::ios_base::fmtflags(0)));
}

template <class B>
inline void eval_convert_to(std::string* result, const B& backend)
{
   *result = backend.str(0, std::ios_base::fmtflags(0));
}
//
// Functions:
//
template <class T>
void eval_abs(T& result, const T& arg)
{
   typedef typename T::signed_types type_list;
   typedef typename mpl::front<type_list>::type front;
   result = arg;
   if(arg.compare(front(0)) < 0)
      result.negate();
}
template <class T>
void eval_fabs(T& result, const T& arg)
{
   BOOST_STATIC_ASSERT_MSG(number_category<T>::value == number_kind_floating_point, "The fabs function is only valid for floating point types.");
   typedef typename T::signed_types type_list;
   typedef typename mpl::front<type_list>::type front;
   result = arg;
   if(arg.compare(front(0)) < 0)
      result.negate();
}

template <class Backend>
inline int eval_fpclassify(const Backend& arg)
{
   BOOST_STATIC_ASSERT_MSG(number_category<Backend>::value == number_kind_floating_point, "The fpclassify function is only valid for floating point types.");
   return eval_is_zero(arg) ? FP_ZERO : FP_NORMAL;
}

template <class T>
inline void eval_fmod(T& result, const T& a, const T& b)
{
   BOOST_STATIC_ASSERT_MSG(number_category<T>::value == number_kind_floating_point, "The fmod function is only valid for floating point types.");
   if((&result == &a) || (&result == &b))
   {
      T temp;
      eval_fmod(temp, a, b);
      result = temp;
      return;
   }
   T n;
   eval_divide(result, a, b);
   if(eval_get_sign(result) < 0)
      eval_ceil(n, result);
   else
      eval_floor(n, result);
   eval_multiply(n, b);
   eval_subtract(result, a, n);
}
template<class T, class A> 
inline typename enable_if<is_arithmetic<A>, void>::type eval_fmod(T& result, const T& x, const A& a)
{
   typedef typename boost::multiprecision::detail::canonical<A, T>::type canonical_type;
   typedef typename mpl::if_<is_same<A, canonical_type>, T, canonical_type>::type cast_type;
   cast_type c;
   c = a;
   eval_fmod(result, x, c);
}

template<class T, class A> 
inline typename enable_if<is_arithmetic<A>, void>::type eval_fmod(T& result, const A& x, const T& a)
{
   typedef typename boost::multiprecision::detail::canonical<A, T>::type canonical_type;
   typedef typename mpl::if_<is_same<A, canonical_type>, T, canonical_type>::type cast_type;
   cast_type c;
   c = x;
   eval_fmod(result, c, a);
}

template <class T>
inline void eval_trunc(T& result, const T& a)
{
   BOOST_STATIC_ASSERT_MSG(number_category<T>::value == number_kind_floating_point, "The trunc function is only valid for floating point types.");
   int c = eval_fpclassify(a);
   if(c == (int)FP_NAN || c == (int)FP_INFINITE)
   {
      result = boost::math::policies::raise_rounding_error("boost::multiprecision::trunc<%1%>(%1%)", 0, number<T>(a), number<T>(a), boost::math::policies::policy<>()).backend();
      return;
   }
   if(eval_get_sign(a) < 0)
      eval_ceil(result, a);
   else
      eval_floor(result, a);
}

template <class T>
inline void eval_round(T& result, const T& a)
{
   BOOST_STATIC_ASSERT_MSG(number_category<T>::value == number_kind_floating_point, "The round function is only valid for floating point types.");
   typedef typename boost::multiprecision::detail::canonical<float, T>::type fp_type;
   int c = eval_fpclassify(a);
   if((c == (int)FP_NAN) || (c == (int)FP_INFINITE))
   {
      result = boost::math::policies::raise_rounding_error("boost::multiprecision::round<%1%>(%1%)", 0, number<T>(a), number<T>(a), boost::math::policies::policy<>()).backend();
      return;
   }
   if(eval_get_sign(a) < 0)
   {
      eval_subtract(result, a, fp_type(0.5f));
      eval_ceil(result, result);
   }
   else
   {
      eval_add(result, a, fp_type(0.5f));
      eval_floor(result, result);
   }
}

template <class B>
void eval_lcm(B& result, const B& a, const B& b);
template <class B>
void eval_gcd(B& result, const B& a, const B& b);

template <class T, class Arithmetic>
inline typename enable_if<is_integral<Arithmetic> >::type eval_gcd(T& result, const T& a, const Arithmetic& b)
{
   typedef typename boost::multiprecision::detail::canonical<Arithmetic, T>::type si_type;
   using default_ops::eval_gcd;
   T t;
   t = static_cast<si_type>(b);
   eval_gcd(result, a, t);
}
template <class T, class Arithmetic>
inline typename enable_if<is_integral<Arithmetic> >::type eval_gcd(T& result, const Arithmetic& a, const T& b)
{
   eval_gcd(result, b, a);
}
template <class T, class Arithmetic>
inline typename enable_if<is_integral<Arithmetic> >::type eval_lcm(T& result, const T& a, const Arithmetic& b)
{
   typedef typename boost::multiprecision::detail::canonical<Arithmetic, T>::type si_type;
   using default_ops::eval_lcm;
   T t;
   t = static_cast<si_type>(b);
   eval_lcm(result, a, t);
}
template <class T, class Arithmetic>
inline typename enable_if<is_integral<Arithmetic> >::type eval_lcm(T& result, const Arithmetic& a, const T& b)
{
   eval_lcm(result, b, a);
}

template <class T>
inline unsigned eval_lsb(const T& val)
{
   typedef typename boost::multiprecision::detail::canonical<unsigned, T>::type ui_type;
   int c = eval_get_sign(val);
   if(c == 0)
   {
      BOOST_THROW_EXCEPTION(std::range_error("No bits were set in the operand."));
   }
   if(c < 0)
   {
      BOOST_THROW_EXCEPTION(std::range_error("Testing individual bits in negative values is not supported - results are undefined."));
   }
   unsigned result = 0;
   T mask, t;
   mask = ui_type(1);
   do
   {
      eval_bitwise_and(t, mask, val);
      ++result;
      eval_left_shift(mask, 1);
   }
   while(eval_is_zero(t));
   
   return --result;
}

template <class T>
inline int eval_msb(const T& val)
{
   int c = eval_get_sign(val);
   if(c == 0)
   {
      BOOST_THROW_EXCEPTION(std::range_error("No bits were set in the operand."));
   }
   if(c < 0)
   {
      BOOST_THROW_EXCEPTION(std::range_error("Testing individual bits in negative values is not supported - results are undefined."));
   }
   //
   // This implementation is really really rubbish - it does
   // a linear scan for the most-significant-bit.  We should really
   // do a binary search, but as none of our backends actually needs
   // this implementation, we'll leave it for now.  In fact for most
   // backends it's likely that there will always be a more efficient
   // native implementation possible.
   //
   unsigned result = 0;
   T t(val);
   while(!eval_is_zero(t))
   {
      eval_right_shift(t, 1);
      ++result;
   }
   return --result;
}

template <class T>
inline bool eval_bit_test(const T& val, unsigned index)
{
   typedef typename boost::multiprecision::detail::canonical<unsigned, T>::type ui_type;
   T mask, t;
   mask = ui_type(1);
   eval_left_shift(mask, index);
   eval_bitwise_and(t, mask, val);
   return !eval_is_zero(t);
}

template <class T>
inline void eval_bit_set(T& val, unsigned index)
{
   typedef typename boost::multiprecision::detail::canonical<unsigned, T>::type ui_type;
   T mask;
   mask = ui_type(1);
   eval_left_shift(mask, index);
   eval_bitwise_or(val, mask);
}

template <class T>
inline void eval_bit_flip(T& val, unsigned index)
{
   typedef typename boost::multiprecision::detail::canonical<unsigned, T>::type ui_type;
   T mask;
   mask = ui_type(1);
   eval_left_shift(mask, index);
   eval_bitwise_xor(val, mask);
}

template <class T>
inline void eval_bit_unset(T& val, unsigned index)
{
   typedef typename boost::multiprecision::detail::canonical<unsigned, T>::type ui_type;
   T mask, t;
   mask = ui_type(1);
   eval_left_shift(mask, index);
   eval_bitwise_and(t, mask, val);
   if(!eval_is_zero(t))
      eval_bitwise_xor(val, mask);
}

template <class B>
void eval_integer_sqrt(B& s, B& r, const B& x)
{
   //
   // This is slow bit-by-bit integer square root, see for example
   // http://en.wikipedia.org/wiki/Methods_of_computing_square_roots#Binary_numeral_system_.28base_2.29
   // There are better methods such as http://hal.inria.fr/docs/00/07/28/54/PDF/RR-3805.pdf
   // and http://hal.inria.fr/docs/00/07/21/13/PDF/RR-4475.pdf which should be implemented
   // at some point.
   //
   typedef typename boost::multiprecision::detail::canonical<unsigned char, B>::type ui_type;

   s = ui_type(0u);
   if(eval_get_sign(x) == 0)
   {
      r = ui_type(0u);
      return;
   }
   int g = eval_msb(x);
   if(g == 0)
   {
      r = ui_type(1);
      return;
   }
   
   B t;
   r = x;
   g /= 2;
   int org_g = g;
   eval_bit_set(s, g);
   eval_bit_set(t, 2 * g);
   eval_subtract(r, x, t);
   --g;
   if(eval_get_sign(r) == 0)
      return;
   int msbr = eval_msb(r);
   do
   {
      if(msbr >= org_g + g + 1)
      {
         t = s;
         eval_left_shift(t, g + 1);
         eval_bit_set(t, 2 * g);
         if(t.compare(r) <= 0)
         {
            eval_bit_set(s, g);
            eval_subtract(r, t);
            if(eval_get_sign(r) == 0)
               return;
            msbr = eval_msb(r);
         }
      }
      --g;
   }
   while(g >= 0);
}

//
// These have to implemented by the backend, declared here so that our macro generated code compiles OK.
//
template <class T>
typename enable_if_c<sizeof(T) == 0>::type eval_floor();
template <class T>
typename enable_if_c<sizeof(T) == 0>::type eval_ceil();
template <class T>
typename enable_if_c<sizeof(T) == 0>::type eval_trunc();
template <class T>
typename enable_if_c<sizeof(T) == 0>::type eval_sqrt();
template <class T>
typename enable_if_c<sizeof(T) == 0>::type eval_ldexp();
template <class T>
typename enable_if_c<sizeof(T) == 0>::type eval_frexp();

//
// eval_logb and eval_scalbn simply assume base 2 and forward to
// eval_ldexp and eval_frexp:
//
template <class B>
inline typename B::exponent_type eval_ilogb(const B& val)
{
   BOOST_STATIC_ASSERT_MSG(!std::numeric_limits<number<B> >::is_specialized || (std::numeric_limits<number<B> >::radix == 2), "The default implementation of ilogb requires a base 2 number type");
   typename B::exponent_type e;
   B result;
   eval_frexp(result, val, &e);
   return e - 1;
}
template <class B>
inline void eval_logb(B& result, const B& val)
{
   typedef typename boost::mpl::if_c<boost::is_same<boost::intmax_t, long>::value, boost::long_long_type, boost::intmax_t>::type max_t;
   result = static_cast<max_t>(eval_ilogb(val));
}
template <class B, class A>
inline void eval_scalbn(B& result, const B& val, A e)
{
   BOOST_STATIC_ASSERT_MSG(!std::numeric_limits<number<B> >::is_specialized || (std::numeric_limits<number<B> >::radix == 2), "The default implementation of scalbn requires a base 2 number type");
   eval_ldexp(result, val, static_cast<typename B::exponent_type>(e));
}
//
// These functions are implemented in separate files, but expanded inline here,
// DO NOT CHANGE THE ORDER OF THESE INCLUDES:
//
#include <boost/multiprecision/detail/functions/constants.hpp>
#include <boost/multiprecision/detail/functions/pow.hpp>
#include <boost/multiprecision/detail/functions/trig.hpp>

}

} // namespace multiprecision
namespace math{
//
// Default versions of floating point classification routines:
//
template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline int fpclassify BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::number<Backend, ExpressionTemplates>& arg)
{
   using multiprecision::default_ops::eval_fpclassify;
   return eval_fpclassify(arg.backend());
}
template <class tag, class A1, class A2, class A3, class A4>
inline int fpclassify BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::detail::expression<tag, A1, A2, A3, A4>& arg)
{
   typedef typename multiprecision::detail::expression<tag, A1, A2, A3, A4>::result_type value_type;
   return (fpclassify)(value_type(arg));
}
template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline bool isfinite BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::number<Backend, ExpressionTemplates>& arg)
{
   int v = (fpclassify)(arg);
   return (v != (int)FP_INFINITE) && (v != (int)FP_NAN);
}
template <class tag, class A1, class A2, class A3, class A4>
inline bool isfinite BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::detail::expression<tag, A1, A2, A3, A4>& arg)
{
   typedef typename multiprecision::detail::expression<tag, A1, A2, A3, A4>::result_type value_type;
   return (isfinite)(value_type(arg));
}
template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline bool isnan BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::number<Backend, ExpressionTemplates>& arg)
{
   return (fpclassify)(arg) == (int)FP_NAN;
}
template <class tag, class A1, class A2, class A3, class A4>
inline bool isnan BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::detail::expression<tag, A1, A2, A3, A4>& arg)
{
   typedef typename multiprecision::detail::expression<tag, A1, A2, A3, A4>::result_type value_type;
   return (isnan)(value_type(arg));
}
template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline bool isinf BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::number<Backend, ExpressionTemplates>& arg)
{
   return (fpclassify)(arg) == (int)FP_INFINITE;
}
template <class tag, class A1, class A2, class A3, class A4>
inline bool isinf BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::detail::expression<tag, A1, A2, A3, A4>& arg)
{
   typedef typename multiprecision::detail::expression<tag, A1, A2, A3, A4>::result_type value_type;
   return (isinf)(value_type(arg));
}
template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline bool isnormal BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::number<Backend, ExpressionTemplates>& arg)
{
   return (fpclassify)(arg) == (int)FP_NORMAL;
}
template <class tag, class A1, class A2, class A3, class A4>
inline bool isnormal BOOST_PREVENT_MACRO_SUBSTITUTION(const multiprecision::detail::expression<tag, A1, A2, A3, A4>& arg)
{
   typedef typename multiprecision::detail::expression<tag, A1, A2, A3, A4>::result_type value_type;
   return (isnormal)(value_type(arg));
}

} // namespace math
namespace multiprecision{

template <class B1, class B2, class B3, expression_template_option ET1, expression_template_option ET2, expression_template_option ET3>
inline number<B1, ET1>& add(number<B1, ET1>& result, const number<B2, ET2>& a, const number<B3, ET3>& b)
{
   BOOST_STATIC_ASSERT_MSG((is_convertible<B2, B1>::value), "No conversion to the target of a mixed precision addition exists");
   BOOST_STATIC_ASSERT_MSG((is_convertible<B3, B1>::value), "No conversion to the target of a mixed precision addition exists");
   using default_ops::eval_add;
   eval_add(result.backend(), a.backend(), b.backend());
   return result;
}

template <class B1, class B2, class B3, expression_template_option ET1, expression_template_option ET2, expression_template_option ET3>
inline number<B1, ET1>& subtract(number<B1, ET1>& result, const number<B2, ET2>& a, const number<B3, ET3>& b)
{
   BOOST_STATIC_ASSERT_MSG((is_convertible<B2, B1>::value), "No conversion to the target of a mixed precision addition exists");
   BOOST_STATIC_ASSERT_MSG((is_convertible<B3, B1>::value), "No conversion to the target of a mixed precision addition exists");
   using default_ops::eval_subtract;
   eval_subtract(result.backend(), a.backend(), b.backend());
   return result;
}

template <class B1, class B2, class B3, expression_template_option ET1, expression_template_option ET2, expression_template_option ET3>
inline number<B1, ET1>& multiply(number<B1, ET1>& result, const number<B2, ET2>& a, const number<B3, ET3>& b)
{
   BOOST_STATIC_ASSERT_MSG((is_convertible<B2, B1>::value), "No conversion to the target of a mixed precision addition exists");
   BOOST_STATIC_ASSERT_MSG((is_convertible<B3, B1>::value), "No conversion to the target of a mixed precision addition exists");
   using default_ops::eval_multiply;
   eval_multiply(result.backend(), a.backend(), b.backend());
   return result;
}

template <class B, expression_template_option ET, class I>
inline typename enable_if_c<is_integral<I>::value, number<B, ET>&>::type 
   add(number<B, ET>& result, const I& a, const I& b)
{
   using default_ops::eval_add;
   typedef typename detail::canonical<I, B>::type canonical_type;
   eval_add(result.backend(), static_cast<canonical_type>(a), static_cast<canonical_type>(b));
   return result;
}

template <class B, expression_template_option ET, class I>
inline typename enable_if_c<is_integral<I>::value, number<B, ET>&>::type 
   subtract(number<B, ET>& result, const I& a, const I& b)
{
   using default_ops::eval_subtract;
   typedef typename detail::canonical<I, B>::type canonical_type;
   eval_subtract(result.backend(), static_cast<canonical_type>(a), static_cast<canonical_type>(b));
   return result;
}

template <class B, expression_template_option ET, class I>
inline typename enable_if_c<is_integral<I>::value, number<B, ET>&>::type 
   multiply(number<B, ET>& result, const I& a, const I& b)
{
   using default_ops::eval_multiply;
   typedef typename detail::canonical<I, B>::type canonical_type;
   eval_multiply(result.backend(), static_cast<canonical_type>(a), static_cast<canonical_type>(b));
   return result;
}

template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline typename detail::expression<tag, A1, A2, A3, A4>::result_type trunc(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   return BOOST_MP_MOVE(trunc(number_type(v), pol));
}

template <class Backend, expression_template_option ExpressionTemplates, class Policy>
inline number<Backend, ExpressionTemplates> trunc(const number<Backend, ExpressionTemplates>& v, const Policy&)
{
   using default_ops::eval_trunc;
   number<Backend, ExpressionTemplates> result;
   eval_trunc(result.backend(), v.backend());
   return BOOST_MP_MOVE(result);
}

template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline int itrunc(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   number_type r = trunc(v, pol);
   if((r > (std::numeric_limits<int>::max)()) || r < (std::numeric_limits<int>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::itrunc<%1%>(%1%)", 0, number_type(v), 0, pol);
   return r.template convert_to<int>();
}
template <class tag, class A1, class A2, class A3, class A4>
inline int itrunc(const detail::expression<tag, A1, A2, A3, A4>& v)
{
   return itrunc(v, boost::math::policies::policy<>());
}
template <class Backend, expression_template_option ExpressionTemplates, class Policy>
inline int itrunc(const number<Backend, ExpressionTemplates>& v, const Policy& pol)
{
   number<Backend, ExpressionTemplates> r = trunc(v, pol);
   if((r > (std::numeric_limits<int>::max)()) || r < (std::numeric_limits<int>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::itrunc<%1%>(%1%)", 0, v, 0, pol);
   return r.template convert_to<int>();
}
template <class Backend, expression_template_option ExpressionTemplates>
inline int itrunc(const number<Backend, ExpressionTemplates>& v)
{
   return itrunc(v, boost::math::policies::policy<>());
}
template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline long ltrunc(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   number_type r = trunc(v, pol);
   if((r > (std::numeric_limits<long>::max)()) || r < (std::numeric_limits<long>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::ltrunc<%1%>(%1%)", 0, number_type(v), 0L, pol);
   return r.template convert_to<long>();
}
template <class tag, class A1, class A2, class A3, class A4>
inline long ltrunc(const detail::expression<tag, A1, A2, A3, A4>& v)
{
   return ltrunc(v, boost::math::policies::policy<>());
}
template <class T, expression_template_option ExpressionTemplates, class Policy>
inline long ltrunc(const number<T, ExpressionTemplates>& v, const Policy& pol)
{
   number<T, ExpressionTemplates> r = trunc(v, pol);
   if((r > (std::numeric_limits<long>::max)()) || r < (std::numeric_limits<long>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::ltrunc<%1%>(%1%)", 0, v, 0L, pol);
   return r.template convert_to<long>();
}
template <class T, expression_template_option ExpressionTemplates>
inline long ltrunc(const number<T, ExpressionTemplates>& v)
{
   return ltrunc(v, boost::math::policies::policy<>());
}
#ifndef BOOST_NO_LONG_LONG
template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline boost::long_long_type lltrunc(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   number_type r = trunc(v, pol);
   if((r > (std::numeric_limits<boost::long_long_type>::max)()) || r < (std::numeric_limits<boost::long_long_type>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::lltrunc<%1%>(%1%)", 0, number_type(v), 0LL, pol);
   return r.template convert_to<boost::long_long_type>();
}
template <class tag, class A1, class A2, class A3, class A4>
inline boost::long_long_type lltrunc(const detail::expression<tag, A1, A2, A3, A4>& v)
{
   return lltrunc(v, boost::math::policies::policy<>());
}
template <class T, expression_template_option ExpressionTemplates, class Policy>
inline boost::long_long_type lltrunc(const number<T, ExpressionTemplates>& v, const Policy& pol)
{
   number<T, ExpressionTemplates> r = trunc(v, pol);
   if((r > (std::numeric_limits<boost::long_long_type>::max)()) || r < (std::numeric_limits<boost::long_long_type>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::lltrunc<%1%>(%1%)", 0, v, 0LL, pol);
   return r.template convert_to<boost::long_long_type>();
}
template <class T, expression_template_option ExpressionTemplates>
inline boost::long_long_type lltrunc(const number<T, ExpressionTemplates>& v)
{
   return lltrunc(v, boost::math::policies::policy<>());
}
#endif
template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline typename detail::expression<tag, A1, A2, A3, A4>::result_type round(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   return BOOST_MP_MOVE(round(static_cast<number_type>(v), pol));
}
template <class T, expression_template_option ExpressionTemplates, class Policy>
inline number<T, ExpressionTemplates> round(const number<T, ExpressionTemplates>& v, const Policy&)
{
   using default_ops::eval_round;
   number<T, ExpressionTemplates> result;
   eval_round(result.backend(), v.backend());
   return BOOST_MP_MOVE(result);
}

template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline int iround(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   number_type r = round(v, pol);
   if((r > (std::numeric_limits<int>::max)()) || r < (std::numeric_limits<int>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::iround<%1%>(%1%)", 0, number_type(v), 0, pol);
   return r.template convert_to<int>();
}
template <class tag, class A1, class A2, class A3, class A4>
inline int iround(const detail::expression<tag, A1, A2, A3, A4>& v)
{
   return iround(v, boost::math::policies::policy<>());
}
template <class T, expression_template_option ExpressionTemplates, class Policy>
inline int iround(const number<T, ExpressionTemplates>& v, const Policy& pol)
{
   number<T, ExpressionTemplates> r = round(v, pol);
   if((r > (std::numeric_limits<int>::max)()) || r < (std::numeric_limits<int>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::iround<%1%>(%1%)", 0, v, 0, pol);
   return r.template convert_to<int>();
}
template <class T, expression_template_option ExpressionTemplates>
inline int iround(const number<T, ExpressionTemplates>& v)
{
   return iround(v, boost::math::policies::policy<>());
}
template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline long lround(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   number_type r = round(v, pol);
   if((r > (std::numeric_limits<long>::max)()) || r < (std::numeric_limits<long>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::lround<%1%>(%1%)", 0, number_type(v), 0L, pol);
   return r.template convert_to<long>();
}
template <class tag, class A1, class A2, class A3, class A4>
inline long lround(const detail::expression<tag, A1, A2, A3, A4>& v)
{
   return lround(v, boost::math::policies::policy<>());
}
template <class T, expression_template_option ExpressionTemplates, class Policy>
inline long lround(const number<T, ExpressionTemplates>& v, const Policy& pol)
{
   number<T, ExpressionTemplates> r = round(v, pol);
   if((r > (std::numeric_limits<long>::max)()) || r < (std::numeric_limits<long>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::lround<%1%>(%1%)", 0, v, 0L, pol);
   return r.template convert_to<long>();
}
template <class T, expression_template_option ExpressionTemplates>
inline long lround(const number<T, ExpressionTemplates>& v)
{
   return lround(v, boost::math::policies::policy<>());
}
#ifndef BOOST_NO_LONG_LONG
template <class tag, class A1, class A2, class A3, class A4, class Policy>
inline boost::long_long_type llround(const detail::expression<tag, A1, A2, A3, A4>& v, const Policy& pol)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   number_type r = round(v, pol);
   if((r > (std::numeric_limits<boost::long_long_type>::max)()) || r < (std::numeric_limits<boost::long_long_type>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::iround<%1%>(%1%)", 0, number_type(v), 0LL, pol);
   return r.template convert_to<boost::long_long_type>();
}
template <class tag, class A1, class A2, class A3, class A4>
inline boost::long_long_type llround(const detail::expression<tag, A1, A2, A3, A4>& v)
{
   return llround(v, boost::math::policies::policy<>());
}
template <class T, expression_template_option ExpressionTemplates, class Policy>
inline boost::long_long_type llround(const number<T, ExpressionTemplates>& v, const Policy& pol)
{
   number<T, ExpressionTemplates> r = round(v, pol);
   if((r > (std::numeric_limits<boost::long_long_type>::max)()) || r < (std::numeric_limits<boost::long_long_type>::min)() || !(boost::math::isfinite)(v))
      return boost::math::policies::raise_rounding_error("boost::multiprecision::iround<%1%>(%1%)", 0, v, 0LL, pol);
   return r.template convert_to<boost::long_long_type>();
}
template <class T, expression_template_option ExpressionTemplates>
inline boost::long_long_type llround(const number<T, ExpressionTemplates>& v)
{
   return llround(v, boost::math::policies::policy<>());
}
#endif
//
// frexp does not return an expression template since we require the
// integer argument to be evaluated even if the returned value is
// not assigned to anything...
//
template <class T, expression_template_option ExpressionTemplates>
inline typename enable_if_c<number_category<T>::value == number_kind_floating_point, number<T, ExpressionTemplates> >::type frexp(const number<T, ExpressionTemplates>& v, short* pint)
{
   using default_ops::eval_frexp;
   number<T, ExpressionTemplates> result;
   eval_frexp(result.backend(), v.backend(), pint);
   return BOOST_MP_MOVE(result);
}
template <class tag, class A1, class A2, class A3, class A4>
inline typename enable_if_c<number_category<typename detail::expression<tag, A1, A2, A3, A4>::result_type>::value == number_kind_floating_point, typename detail::expression<tag, A1, A2, A3, A4>::result_type>::type 
   frexp(const detail::expression<tag, A1, A2, A3, A4>& v, short* pint)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   return BOOST_MP_MOVE(frexp(static_cast<number_type>(v), pint));
}
template <class T, expression_template_option ExpressionTemplates>
inline typename enable_if_c<number_category<T>::value == number_kind_floating_point, number<T, ExpressionTemplates> >::type frexp(const number<T, ExpressionTemplates>& v, int* pint)
{
   using default_ops::eval_frexp;
   number<T, ExpressionTemplates> result;
   eval_frexp(result.backend(), v.backend(), pint);
   return BOOST_MP_MOVE(result);
}
template <class tag, class A1, class A2, class A3, class A4>
inline typename enable_if_c<number_category<typename detail::expression<tag, A1, A2, A3, A4>::result_type>::value == number_kind_floating_point, typename detail::expression<tag, A1, A2, A3, A4>::result_type>::type
frexp(const detail::expression<tag, A1, A2, A3, A4>& v, int* pint)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   return BOOST_MP_MOVE(frexp(static_cast<number_type>(v), pint));
}
template <class T, expression_template_option ExpressionTemplates>
inline typename enable_if_c<number_category<T>::value == number_kind_floating_point, number<T, ExpressionTemplates> >::type frexp(const number<T, ExpressionTemplates>& v, long* pint)
{
   using default_ops::eval_frexp;
   number<T, ExpressionTemplates> result;
   eval_frexp(result.backend(), v.backend(), pint);
   return BOOST_MP_MOVE(result);
}
template <class tag, class A1, class A2, class A3, class A4>
inline typename enable_if_c<number_category<typename detail::expression<tag, A1, A2, A3, A4>::result_type>::value == number_kind_floating_point, typename detail::expression<tag, A1, A2, A3, A4>::result_type>::type
frexp(const detail::expression<tag, A1, A2, A3, A4>& v, long* pint)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   return BOOST_MP_MOVE(frexp(static_cast<number_type>(v), pint));
}
template <class T, expression_template_option ExpressionTemplates>
inline typename enable_if_c<number_category<T>::value == number_kind_floating_point, number<T, ExpressionTemplates> >::type frexp(const number<T, ExpressionTemplates>& v, boost::long_long_type* pint)
{
   using default_ops::eval_frexp;
   number<T, ExpressionTemplates> result;
   eval_frexp(result.backend(), v.backend(), pint);
   return BOOST_MP_MOVE(result);
}
template <class tag, class A1, class A2, class A3, class A4>
inline typename enable_if_c<number_category<typename detail::expression<tag, A1, A2, A3, A4>::result_type>::value == number_kind_floating_point, typename detail::expression<tag, A1, A2, A3, A4>::result_type>::type
frexp(const detail::expression<tag, A1, A2, A3, A4>& v, boost::long_long_type* pint)
{
   typedef typename detail::expression<tag, A1, A2, A3, A4>::result_type number_type;
   return BOOST_MP_MOVE(frexp(static_cast<number_type>(v), pint));
}

template <class B, expression_template_option ExpressionTemplates>
inline typename enable_if_c<number_category<B>::value == number_kind_integer, number<B, ExpressionTemplates> >::type
   sqrt(const number<B, ExpressionTemplates>& x)
{
   using default_ops::eval_integer_sqrt;
   number<B, ExpressionTemplates> s, r;
   eval_integer_sqrt(s.backend(), r.backend(), x.backend());
   return s;
}

template <class B, expression_template_option ExpressionTemplates>
inline typename enable_if_c<number_category<B>::value == number_kind_integer, number<B, ExpressionTemplates> >::type
   sqrt(const number<B, ExpressionTemplates>& x, number<B, ExpressionTemplates>& r)
{
   using default_ops::eval_integer_sqrt;
   number<B, ExpressionTemplates> s;
   eval_integer_sqrt(s.backend(), r.backend(), x.backend());
   return s;
}

#define UNARY_OP_FUNCTOR(func, category)\
namespace detail{\
template <class Backend> \
struct BOOST_JOIN(func, _funct)\
{\
   void operator()(Backend& result, const Backend& arg)const\
   {\
      using default_ops::BOOST_JOIN(eval_,func);\
      BOOST_JOIN(eval_,func)(result, arg);\
   }\
};\
\
}\
\
template <class tag, class A1, class A2, class A3, class A4> \
inline typename enable_if_c<number_category<detail::expression<tag, A1, A2, A3, A4> >::value == category,\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> > \
>::type \
func(const detail::expression<tag, A1, A2, A3, A4>& arg)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> \
> (\
        detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>() \
      , arg   \
    );\
}\
template <class Backend> \
inline typename enable_if_c<number_category<Backend>::value == category,\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> > \
>::type \
func(const number<Backend, et_on>& arg)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  >(\
        detail::BOOST_JOIN(func, _funct)<Backend>() \
      , arg   \
    );\
}\
template <class Backend> \
inline typename boost::enable_if_c<\
   boost::multiprecision::number_category<Backend>::value == category,\
   number<Backend, et_off> >::type \
func(const number<Backend, et_off>& arg)\
{\
   number<Backend, et_off> result;\
   using default_ops::BOOST_JOIN(eval_,func);\
   BOOST_JOIN(eval_,func)(result.backend(), arg.backend());\
   return BOOST_MP_MOVE(result);\
}

#define BINARY_OP_FUNCTOR(func, category)\
namespace detail{\
template <class Backend> \
struct BOOST_JOIN(func, _funct)\
{\
   void operator()(Backend& result, const Backend& arg, const Backend& a)const\
   {\
      using default_ops:: BOOST_JOIN(eval_,func);\
      BOOST_JOIN(eval_,func)(result, arg, a);\
   }\
   template <class Arithmetic> \
   void operator()(Backend& result, const Backend& arg, const Arithmetic& a)const\
   {\
      using default_ops:: BOOST_JOIN(eval_,func);\
      BOOST_JOIN(eval_,func)(result, arg, a);\
   }\
   template <class Arithmetic> \
   void operator()(Backend& result, const Arithmetic& arg, const Backend& a)const\
   {\
      using default_ops:: BOOST_JOIN(eval_,func);\
      BOOST_JOIN(eval_,func)(result, arg, a);\
   }\
};\
\
}\
template <class Backend> \
inline typename enable_if_c<number_category<Backend>::value == category,\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , number<Backend, et_on> > \
>::type \
func(const number<Backend, et_on>& arg, const number<Backend, et_on>& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , number<Backend, et_on> \
  >(\
        detail::BOOST_JOIN(func, _funct)<Backend>() \
      , arg,\
      a\
    );\
}\
template <class Backend, class tag, class A1, class A2, class A3, class A4> \
inline typename enable_if_c<\
   (number_category<Backend>::value == category) && (number_category<detail::expression<tag, A1, A2, A3, A4> >::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , detail::expression<tag, A1, A2, A3, A4> > \
>::type \
func(const number<Backend, et_on>& arg, const detail::expression<tag, A1, A2, A3, A4>& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , detail::expression<tag, A1, A2, A3, A4> \
  >(\
        detail::BOOST_JOIN(func, _funct)<Backend>() \
      , arg,\
      a\
    );\
}\
template <class tag, class A1, class A2, class A3, class A4, class Backend> \
inline typename enable_if_c<\
   (number_category<Backend>::value == category) && (number_category<detail::expression<tag, A1, A2, A3, A4> >::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , number<Backend, et_on> > \
>::type \
func(const detail::expression<tag, A1, A2, A3, A4>& arg, const number<Backend, et_on>& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , number<Backend, et_on> \
  >(\
        detail::BOOST_JOIN(func, _funct)<Backend>() \
      , arg,\
      a\
    );\
}\
template <class tag, class A1, class A2, class A3, class A4, class tagb, class A1b, class A2b, class A3b, class A4b> \
inline typename enable_if_c<\
      (number_category<detail::expression<tag, A1, A2, A3, A4> >::value == category) && (number_category<detail::expression<tagb, A1b, A2b, A3b, A4b> >::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , detail::expression<tagb, A1b, A2b, A3b, A4b> > \
>::type \
func(const detail::expression<tag, A1, A2, A3, A4>& arg, const detail::expression<tagb, A1b, A2b, A3b, A4b>& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , detail::expression<tagb, A1b, A2b, A3b, A4b> \
  >(\
        detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>() \
      , arg,\
      a\
    );\
}\
template <class Backend, class Arithmetic> \
inline typename enable_if_c<\
   is_arithmetic<Arithmetic>::value && (number_category<Backend>::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , Arithmetic\
  > \
>::type \
func(const number<Backend, et_on>& arg, const Arithmetic& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , Arithmetic\
  >(\
        detail::BOOST_JOIN(func, _funct)<Backend>() \
      , arg,\
      a\
    );\
}\
template <class tag, class A1, class A2, class A3, class A4, class Arithmetic> \
inline typename enable_if_c<\
   is_arithmetic<Arithmetic>::value && (number_category<detail::expression<tag, A1, A2, A3, A4> >::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , Arithmetic\
  > \
>::type \
func(const detail::expression<tag, A1, A2, A3, A4>& arg, const Arithmetic& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , Arithmetic\
   >(\
        detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>() \
      , arg,\
      a\
    );\
}\
template <class Backend, class Arithmetic> \
inline typename enable_if_c<\
   is_arithmetic<Arithmetic>::value && (number_category<Backend>::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , Arithmetic \
  , number<Backend, et_on> \
  > \
>::type \
func(const Arithmetic& arg, const number<Backend, et_on>& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , Arithmetic \
  , number<Backend, et_on> \
  >(\
        detail::BOOST_JOIN(func, _funct)<Backend>() \
      , arg,\
      a\
    );\
}\
template <class tag, class A1, class A2, class A3, class A4, class Arithmetic> \
inline typename enable_if_c<\
   is_arithmetic<Arithmetic>::value && (number_category<detail::expression<tag, A1, A2, A3, A4> >::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , Arithmetic \
  , detail::expression<tag, A1, A2, A3, A4> \
  > \
>::type \
func(const Arithmetic& arg, const detail::expression<tag, A1, A2, A3, A4>& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , Arithmetic \
  , detail::expression<tag, A1, A2, A3, A4> \
   >(\
        detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>() \
      , arg,\
      a\
    );\
}\
template <class Backend> \
inline typename enable_if_c<(number_category<Backend>::value == category),\
   number<Backend, et_off> >::type \
func(const number<Backend, et_off>& arg, const number<Backend, et_off>& a)\
{\
   number<Backend, et_off> result;\
   using default_ops:: BOOST_JOIN(eval_,func);\
   BOOST_JOIN(eval_,func)(result.backend(), arg.backend(), a.backend());\
   return BOOST_MP_MOVE(result);\
}\
template <class Backend, class Arithmetic> \
inline typename enable_if_c<\
   is_arithmetic<Arithmetic>::value && (number_category<Backend>::value == category),\
   number<Backend, et_off> \
>::type \
func(const number<Backend, et_off>& arg, const Arithmetic& a)\
{\
   typedef typename detail::canonical<Arithmetic, Backend>::type canonical_type;\
   number<Backend, et_off> result;\
   using default_ops:: BOOST_JOIN(eval_,func);\
   BOOST_JOIN(eval_,func)(result.backend(), arg.backend(), static_cast<canonical_type>(a));\
   return BOOST_MP_MOVE(result);\
}\
template <class Backend, class Arithmetic> \
inline typename enable_if_c<\
   is_arithmetic<Arithmetic>::value && (number_category<Backend>::value == category),\
   number<Backend, et_off> \
>::type \
func(const Arithmetic& a, const number<Backend, et_off>& arg)\
{\
   typedef typename detail::canonical<Arithmetic, Backend>::type canonical_type;\
   number<Backend, et_off> result;\
   using default_ops:: BOOST_JOIN(eval_,func);\
   BOOST_JOIN(eval_,func)(result.backend(), static_cast<canonical_type>(a), arg.backend());\
   return BOOST_MP_MOVE(result);\
}\


#define HETERO_BINARY_OP_FUNCTOR_B(func, Arg2, category)\
template <class tag, class A1, class A2, class A3, class A4> \
inline typename enable_if_c<\
   (number_category<detail::expression<tag, A1, A2, A3, A4> >::value == category),\
   detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , Arg2> \
>::type \
func(const detail::expression<tag, A1, A2, A3, A4>& arg, Arg2 const& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type> \
  , detail::expression<tag, A1, A2, A3, A4> \
  , Arg2\
   >(\
        detail::BOOST_JOIN(func, _funct)<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>() \
      , arg, a   \
    );\
}\
template <class Backend> \
inline typename enable_if_c<\
   (number_category<Backend>::value == category),\
  detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , Arg2> \
>::type \
func(const number<Backend, et_on>& arg, Arg2 const& a)\
{\
    return detail::expression<\
    detail::function\
  , detail::BOOST_JOIN(func, _funct)<Backend> \
  , number<Backend, et_on> \
  , Arg2\
  >(\
        detail::BOOST_JOIN(func, _funct)<Backend>() \
      , arg,\
      a\
    );\
}\
template <class Backend> \
inline typename enable_if_c<\
  (number_category<Backend>::value == category),\
  number<Backend, et_off> >::type \
func(const number<Backend, et_off>& arg, Arg2 const& a)\
{\
   number<Backend, et_off> result;\
   using default_ops:: BOOST_JOIN(eval_,func);\
   BOOST_JOIN(eval_,func)(result.backend(), arg.backend(), a);\
   return BOOST_MP_MOVE(result);\
}\

#define HETERO_BINARY_OP_FUNCTOR(func, Arg2, category)\
namespace detail{\
template <class Backend> \
struct BOOST_JOIN(func, _funct)\
{\
   template <class Arg>\
   void operator()(Backend& result, Backend const& arg, Arg a)const\
   {\
      using default_ops:: BOOST_JOIN(eval_,func);\
      BOOST_JOIN(eval_,func)(result, arg, a);\
   }\
};\
\
}\
\
HETERO_BINARY_OP_FUNCTOR_B(func, Arg2, category)

namespace detail{
template <class Backend>
struct abs_funct
{
   void operator()(Backend& result, const Backend& arg)const
   {
      using default_ops::eval_abs;
      eval_abs(result, arg);
   }
};

}

template <class tag, class A1, class A2, class A3, class A4>
inline detail::expression<
    detail::function
  , detail::abs_funct<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>
  , detail::expression<tag, A1, A2, A3, A4> >
abs(const detail::expression<tag, A1, A2, A3, A4>& arg)
{
    return detail::expression<
    detail::function
  , detail::abs_funct<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>
  , detail::expression<tag, A1, A2, A3, A4>
> (
        detail::abs_funct<typename detail::backend_type<detail::expression<tag, A1, A2, A3, A4> >::type>()
      , arg
    );
}
template <class Backend>
inline detail::expression<
    detail::function
  , detail::abs_funct<Backend>
  , number<Backend, et_on> >
abs(const number<Backend, et_on>& arg)
{
    return detail::expression<
    detail::function
  , detail::abs_funct<Backend>
  , number<Backend, et_on>
  >(
        detail::abs_funct<Backend>()
      , arg
    );
}
template <class Backend>
inline number<Backend, et_off>
abs(const number<Backend, et_off>& arg)
{
   number<Backend, et_off> result;
   using default_ops::eval_abs;
   eval_abs(result.backend(), arg.backend());
   return BOOST_MP_MOVE(result);
}

UNARY_OP_FUNCTOR(fabs, number_kind_floating_point)
UNARY_OP_FUNCTOR(sqrt, number_kind_floating_point)
UNARY_OP_FUNCTOR(floor, number_kind_floating_point)
UNARY_OP_FUNCTOR(ceil, number_kind_floating_point)
UNARY_OP_FUNCTOR(trunc, number_kind_floating_point)
UNARY_OP_FUNCTOR(round, number_kind_floating_point)
UNARY_OP_FUNCTOR(exp, number_kind_floating_point)
UNARY_OP_FUNCTOR(log, number_kind_floating_point)
UNARY_OP_FUNCTOR(log10, number_kind_floating_point)
UNARY_OP_FUNCTOR(cos, number_kind_floating_point)
UNARY_OP_FUNCTOR(sin, number_kind_floating_point)
UNARY_OP_FUNCTOR(tan, number_kind_floating_point)
UNARY_OP_FUNCTOR(asin, number_kind_floating_point)
UNARY_OP_FUNCTOR(acos, number_kind_floating_point)
UNARY_OP_FUNCTOR(atan, number_kind_floating_point)
UNARY_OP_FUNCTOR(cosh, number_kind_floating_point)
UNARY_OP_FUNCTOR(sinh, number_kind_floating_point)
UNARY_OP_FUNCTOR(tanh, number_kind_floating_point)

HETERO_BINARY_OP_FUNCTOR(ldexp, short, number_kind_floating_point)
//HETERO_BINARY_OP_FUNCTOR(frexp, short*, number_kind_floating_point)
HETERO_BINARY_OP_FUNCTOR_B(ldexp, int, number_kind_floating_point)
//HETERO_BINARY_OP_FUNCTOR_B(frexp, int*, number_kind_floating_point)
HETERO_BINARY_OP_FUNCTOR_B(ldexp, long, number_kind_floating_point)
//HETERO_BINARY_OP_FUNCTOR_B(frexp, long*, number_kind_floating_point)
HETERO_BINARY_OP_FUNCTOR_B(ldexp, boost::long_long_type, number_kind_floating_point)
//HETERO_BINARY_OP_FUNCTOR_B(frexp, boost::long_long_type*, number_kind_floating_point)
BINARY_OP_FUNCTOR(pow, number_kind_floating_point)
BINARY_OP_FUNCTOR(fmod, number_kind_floating_point)
BINARY_OP_FUNCTOR(atan2, number_kind_floating_point)

UNARY_OP_FUNCTOR(logb, number_kind_floating_point)
HETERO_BINARY_OP_FUNCTOR(scalbn, short, number_kind_floating_point)
HETERO_BINARY_OP_FUNCTOR_B(scalbn, int, number_kind_floating_point)
HETERO_BINARY_OP_FUNCTOR_B(scalbn, long, number_kind_floating_point)
HETERO_BINARY_OP_FUNCTOR_B(scalbn, boost::long_long_type, number_kind_floating_point)

//
// Integer functions:
//
BINARY_OP_FUNCTOR(gcd, number_kind_integer)
BINARY_OP_FUNCTOR(lcm, number_kind_integer)
HETERO_BINARY_OP_FUNCTOR_B(pow, unsigned, number_kind_integer)

#undef BINARY_OP_FUNCTOR
#undef UNARY_OP_FUNCTOR

//
// ilogb:
//
template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline typename enable_if_c<number_category<Backend>::value == number_kind_floating_point, typename Backend::exponent_type>::type 
   ilogb(const multiprecision::number<Backend, ExpressionTemplates>& val)
{
   using default_ops::eval_ilogb;
   return eval_ilogb(val.backend());
}

template <class tag, class A1, class A2, class A3, class A4>
inline typename enable_if_c<number_category<detail::expression<tag, A1, A2, A3, A4> >::value == number_kind_floating_point, typename multiprecision::detail::expression<tag, A1, A2, A3, A4>::result_type::backend_type::exponent_type>::type
ilogb(const detail::expression<tag, A1, A2, A3, A4>& val)
{
   using default_ops::eval_ilogb;
   typename multiprecision::detail::expression<tag, A1, A2, A3, A4>::result_type arg(val);
   return eval_ilogb(arg.backend());
}

} //namespace multiprecision

namespace math{
//
// Overload of Boost.Math functions that find the wrong overload when used with number:
//
namespace detail{
   template <class T> T sinc_pi_imp(T);
   template <class T> T sinhc_pi_imp(T);
}
template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline multiprecision::number<Backend, ExpressionTemplates> sinc_pi(const multiprecision::number<Backend, ExpressionTemplates>& x)
{
   return BOOST_MP_MOVE(detail::sinc_pi_imp(x));
}

template <class Backend, multiprecision::expression_template_option ExpressionTemplates, class Policy>
inline multiprecision::number<Backend, ExpressionTemplates> sinc_pi(const multiprecision::number<Backend, ExpressionTemplates>& x, const Policy&)
{
   return BOOST_MP_MOVE(detail::sinc_pi_imp(x));
}

template <class Backend, multiprecision::expression_template_option ExpressionTemplates>
inline multiprecision::number<Backend, ExpressionTemplates> sinhc_pi(const multiprecision::number<Backend, ExpressionTemplates>& x)
{
   return BOOST_MP_MOVE(detail::sinhc_pi_imp(x));
}

template <class Backend, multiprecision::expression_template_option ExpressionTemplates, class Policy>
inline multiprecision::number<Backend, ExpressionTemplates> sinhc_pi(const multiprecision::number<Backend, ExpressionTemplates>& x, const Policy&)
{
   return BOOST_MP_MOVE(boost::math::sinhc_pi(x));
}

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif
} // namespace math
} // namespace boost

//
// This has to come last of all:
//
#include <boost/multiprecision/detail/no_et_ops.hpp>
#include <boost/multiprecision/detail/et_ops.hpp>

#endif


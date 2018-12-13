
//  (C) Copyright Dave Abrahams, Steve Cleary, Beman Dawes, Howard
//  Hinnant & John Maddock 2000.  
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.


#ifndef BOOST_TT_IS_MEMBER_FUNCTION_POINTER_CXX_11_HPP_INCLUDED
#define BOOST_TT_IS_MEMBER_FUNCTION_POINTER_CXX_11_HPP_INCLUDED

#include <boost/type_traits/integral_constant.hpp>

namespace boost {

#ifdef _MSC_VER
#define BOOST_TT_DEF_CALL __thiscall
#else
#define BOOST_TT_DEF_CALL
#endif


   template <class T>
   struct is_member_function_pointer : public false_type {};
   template <class T>
   struct is_member_function_pointer<T const> : public is_member_function_pointer<T> {};
   template <class T>
   struct is_member_function_pointer<T volatile> : public is_member_function_pointer<T> {};
   template <class T>
   struct is_member_function_pointer<T const volatile> : public is_member_function_pointer<T> {};

#if defined(_MSVC_LANG) && (_MSVC_LANG >= 201703) 
   // MSVC can't handle noexcept(b) as a deduced template parameter 
   // so we will have to write everything out :(
#define BOOST_TT_NOEXCEPT_PARAM
#define BOOST_TT_NOEXCEPT_DECL
#elif defined(__cpp_noexcept_function_type)
#define BOOST_TT_NOEXCEPT_PARAM , bool NE
#define BOOST_TT_NOEXCEPT_DECL noexcept(NE)
#else
#define BOOST_TT_NOEXCEPT_PARAM
#define BOOST_TT_NOEXCEPT_DECL
#endif

   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (C::*)(Args..., ...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const qualified:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // Reference qualified:

   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)& BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)& BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const qualified:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)const & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)const volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // rvalue reference qualified:

   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const qualified:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)const && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (BOOST_TT_DEF_CALL C::*)(Args...)const volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};

#ifdef _MSC_VER
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // reference qualified:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // rvalue reference qualified:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__stdcall C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__fastcall C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret (__vectorcall C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif


#if defined(_MSVC_LANG) && (_MSVC_LANG >= 201703) 

#undef BOOST_TT_NOEXCEPT_DECL
#define BOOST_TT_NOEXCEPT_DECL noexcept

   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const qualified:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // Reference qualified:

   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)& BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)& BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const qualified:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)const & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)const volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const volatile & BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // rvalue reference qualified:

   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const qualified:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)const && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(BOOST_TT_DEF_CALL C::*)(Args...)const volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class ...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(C::*)(Args..., ...)const volatile && BOOST_TT_NOEXCEPT_DECL> : public true_type {};

#ifdef _MSC_VER
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)const BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)const volatile BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // reference qualified:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)const &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)const volatile &BOOST_TT_NOEXCEPT_DECL> : public true_type {};

   // rvalue reference qualified:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...) && BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)const &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   // const volatile:
#ifdef __CLR_VER
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret __clrcall(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
#ifndef _M_AMD64
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__stdcall C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__fastcall C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__cdecl C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif
   template <class Ret, class C, class...Args BOOST_TT_NOEXCEPT_PARAM>
   struct is_member_function_pointer<Ret(__vectorcall C::*)(Args...)const volatile &&BOOST_TT_NOEXCEPT_DECL> : public true_type {};
#endif


#endif

#undef BOOST_TT_NOEXCEPT_DECL
#undef BOOST_TT_NOEXCEPT_PARAM
#undef BOOST_TT_DEF_CALL
}

#endif // BOOST_TT_IS_MEMBER_FUNCTION_POINTER_CXX_11_HPP_INCLUDED

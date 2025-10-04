///////////////////////////////////////////////////////////////////////////////
//  Copyright 2011 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_CHECK_CPP11_CONFIG_HPP
#define BOOST_MP_CHECK_CPP11_CONFIG_HPP

//
// We now require C++11, if something we use is not supported, then error and say why:
//
#ifdef BOOST_NO_CXX11_RVALUE_REFERENCES
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_RVALUE_REFERENCES being set"
#endif
#ifdef BOOST_NO_CXX11_TEMPLATE_ALIASES
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_TEMPLATE_ALIASES being set"
#endif
#ifdef BOOST_NO_CXX11_HDR_ARRAY
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_HDR_ARRAY being set"
#endif
#ifdef BOOST_NO_CXX11_HDR_TYPE_TRAITS
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_HDR_TYPE_TRAITS being set"
#endif
#ifdef BOOST_NO_CXX11_ALLOCATOR
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_ALLOCATOR being set"
#endif
#ifdef BOOST_NO_CXX11_CONSTEXPR
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_CONSTEXPR being set"
#endif
#ifdef BOOST_MP_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_MP_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS being set"
#endif
#ifdef BOOST_NO_CXX11_REF_QUALIFIERS
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_REF_QUALIFIERS being set"
#endif
#ifdef BOOST_NO_CXX11_HDR_FUNCTIONAL
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_HDR_FUNCTIONAL being set"
#endif
#ifdef BOOST_NO_CXX11_VARIADIC_TEMPLATES
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_VARIADIC_TEMPLATES being set"
#endif
#ifdef BOOST_NO_CXX11_USER_DEFINED_LITERALS
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_USER_DEFINED_LITERALS being set"
#endif
#ifdef BOOST_NO_CXX11_DECLTYPE
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_DECLTYPE being set"
#endif
#ifdef BOOST_NO_CXX11_STATIC_ASSERT
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_STATIC_ASSERT being set"
#endif
#ifdef BOOST_NO_CXX11_DEFAULTED_FUNCTIONS
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_DEFAULTED_FUNCTIONS being set"
#endif
#ifdef BOOST_NO_CXX11_NOEXCEPT
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_NOEXCEPT being set"
#endif
#ifdef BOOST_NO_CXX11_REF_QUALIFIERS
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_REF_QUALIFIERS being set"
#endif
#ifdef BOOST_NO_CXX11_USER_DEFINED_LITERALS
#error "This library now requires a C++11 or later compiler - this message was generated as a result of BOOST_NO_CXX11_USER_DEFINED_LITERALS being set"
#endif

#endif // BOOST_MP_CHECK_CPP11_CONFIG_HPP

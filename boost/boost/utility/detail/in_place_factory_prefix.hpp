// Copyright (C) 2003, Fernando Luis Cacciola Carballal.
//
// Use, modification, and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/lib/optional for documentation.
//
// You are welcome to contact the author at:
//  fernando_cacciola@hotmail.com
//
#ifndef BOOST_UTILITY_DETAIL_INPLACE_FACTORY_PREFIX_25AGO2003_HPP
#define BOOST_UTILITY_DETAIL_INPLACE_FACTORY_PREFIX_25AGO2003_HPP

#include <boost/config.hpp>
#include <boost/preprocessor/repetition/enum.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/arithmetic/inc.hpp>
#include <boost/preprocessor/punctuation/paren.hpp>
#include <boost/preprocessor/facilities/empty.hpp>

#define BOOST_DEFINE_INPLACE_FACTORY_CLASS_MEMBER_INIT(z,n,_) BOOST_PP_CAT(m_a,n) BOOST_PP_LPAREN() BOOST_PP_CAT(a,n) BOOST_PP_RPAREN()
#define BOOST_DEFINE_INPLACE_FACTORY_CLASS_MEMBER_DECL(z,n,_) BOOST_PP_CAT(A,n) const& BOOST_PP_CAT(m_a,n);
#define BOOST_DEFINE_INPLACE_FACTORY_CLASS_MEMBER_ARG(z,n,_)  BOOST_PP_CAT(m_a,n)

#define BOOST_MAX_INPLACE_FACTORY_ARITY 10

#undef BOOST_UTILITY_DETAIL_INPLACE_FACTORY_SUFFIX_25AGO2003_HPP

#endif


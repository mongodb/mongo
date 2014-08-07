/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying 
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#ifndef BOOST_PP_IS_ITERATING
#if !defined(FUSION_VECTOR_FORWARD_CTOR_07122005_1123)
#define FUSION_VECTOR_FORWARD_CTOR_07122005_1123

#include <boost/preprocessor/iterate.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>

#define FUSION_FORWARD_CTOR_FORWARD(z, n, _)    std::forward<U##n>(_##n)

#define BOOST_PP_FILENAME_1 \
    <boost/fusion/container/vector/detail/vector_forward_ctor.hpp>
#define BOOST_PP_ITERATION_LIMITS (1, FUSION_MAX_VECTOR_SIZE)
#include BOOST_PP_ITERATE()

#undef FUSION_FORWARD_CTOR_FORWARD
#endif
#else // defined(BOOST_PP_IS_ITERATING)
///////////////////////////////////////////////////////////////////////////////
//
//  Preprocessor vertical repetition code
//
///////////////////////////////////////////////////////////////////////////////

#define N BOOST_PP_ITERATION()

#if N == 1
    explicit
#endif
    vector(BOOST_PP_ENUM_BINARY_PARAMS(
        N, typename detail::call_param<T, >::type _))
        : vec(BOOST_PP_ENUM_PARAMS(N, _)) {}

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
    template <BOOST_PP_ENUM_PARAMS(N, typename U)>
#if N == 1
    explicit
#endif
    vector(BOOST_PP_ENUM_BINARY_PARAMS(N, U, && _))
        : vec(BOOST_PP_ENUM(N, FUSION_FORWARD_CTOR_FORWARD, _)) {}
#endif

#undef N
#endif // defined(BOOST_PP_IS_ITERATING)


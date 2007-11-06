/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_TUPLE_HPP)
#define FUSION_SEQUENCE_TUPLE_HPP

#include <utility> // for std::pair
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_builder.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_params_with_a_default.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/spirit/fusion/sequence/tuple_forward.hpp>

#define FUSION_TUPLE_CONSTRUCTOR(z, n, _)                                       \
    tuple(BOOST_PP_ENUM_BINARY_PARAMS(                                          \
        n, typename detail::call_param<T, >::type _))                           \
        : base_type(BOOST_PP_ENUM_PARAMS(n, _))                                 \
    {}

namespace boost { namespace fusion
{
    struct void_t;

    template <BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, typename T)>
    struct tuple :
        detail::tuple_builder<
            BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, T)
        >::type
    {
        typedef
            typename detail::tuple_builder<
                BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, T)
            >::type
        base_type;

        tuple()
            : base_type() {}

        template <typename X>
        /*explicit*/ tuple(X const& x)
            : base_type(x) {}

        explicit tuple(typename detail::call_param<T0>::type _0)
            : base_type(_0) {}

        BOOST_PP_REPEAT_FROM_TO(
            2, FUSION_MAX_TUPLE_SIZE, FUSION_TUPLE_CONSTRUCTOR, _)

        template <typename X>
        tuple& operator=(X const& other)
        {
            base() = other;
            return *this;
        }

        typedef detail::tuple_builder<BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, T)> builder;
        
        typedef typename builder::begin begin;
        typedef typename builder::end end;
        typedef typename builder::size size;

        base_type& base() { return *this; }
        base_type const& base() const { return *this; }
    };
}}

#endif

/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE_BUILDER_HPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE_BUILDER_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/sequence/limits.hpp>

//  include tuple0..N where N is FUSION_MAX_TUPLE_SIZE
#include <boost/spirit/fusion/sequence/tuple10.hpp>
#if (FUSION_MAX_TUPLE_SIZE > 10)
#include <boost/spirit/fusion/sequence/tuple20.hpp>
#endif
#if (FUSION_MAX_TUPLE_SIZE > 20)
#include <boost/spirit/fusion/sequence/tuple30.hpp>
#endif
#if (FUSION_MAX_TUPLE_SIZE > 30)
#include <boost/spirit/fusion/sequence/tuple40.hpp>
#endif
#if (FUSION_MAX_TUPLE_SIZE > 40)
#include <boost/spirit/fusion/sequence/tuple50.hpp>
#endif

#include <boost/mpl/not.hpp>
#include <boost/mpl/distance.hpp>
#include <boost/mpl/find.hpp>
#include <boost/mpl/begin_end.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>

namespace boost { namespace fusion
{
    struct void_t;
}}

namespace boost { namespace fusion { namespace detail
{
    template <int N>
    struct get_tuple_n;

    template <>
    struct get_tuple_n<0>
    {
        template <BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, typename T)>
        struct call
        {
            typedef tuple0 type;
        };
    };

#define FUSION_GET_TUPLE_N(z, n, _)                                             \
    template <>                                                                 \
    struct get_tuple_n<n>                                                       \
    {                                                                           \
        template <BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, typename T)>      \
        struct call                                                             \
        {                                                                       \
            typedef BOOST_PP_CAT(tuple, n)<BOOST_PP_ENUM_PARAMS(n, T)> type;    \
        };                                                                      \
    };

    BOOST_PP_REPEAT_FROM_TO(1, FUSION_MAX_TUPLE_SIZE, FUSION_GET_TUPLE_N, _)
#undef FUSION_GET_TUPLE_N

    template <BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, typename T)>
    struct tuple_builder
    {
        typedef
            mpl::BOOST_PP_CAT(vector, FUSION_MAX_TUPLE_SIZE)
                <BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, T)>
        input;

        typedef typename mpl::begin<input>::type begin;
        typedef typename mpl::find<input, void_t>::type end;
        typedef typename mpl::distance<begin, end>::type size;

        typedef typename get_tuple_n<FUSION_GET_VALUE(size)>::template
            call<BOOST_PP_ENUM_PARAMS(FUSION_MAX_TUPLE_SIZE, T)>::type
        type;
    };
}}}

#endif

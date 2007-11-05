/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_MAKE_TUPLE_HPP)
#define FUSION_SEQUENCE_MAKE_TUPLE_HPP

#include <boost/preprocessor/repetition/enum.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/spirit/fusion/sequence/tuple.hpp>
#include <boost/spirit/fusion/sequence/detail/as_tuple_element.hpp>

namespace boost { namespace fusion
{
    inline tuple<>
    make_tuple()
    {
        return tuple<>();
    }

#define BOOST_FUSION_AS_TUPLE_ELEMENT(z, n, data)                               \
    BOOST_DEDUCED_TYPENAME detail::as_tuple_element<BOOST_PP_CAT(T, n)>::type

#define FUSION_MAKE_TUPLE(z, n, _)                                              \
                                                                                \
    template <BOOST_PP_ENUM_PARAMS(n, typename T)>                              \
    inline tuple<BOOST_PP_ENUM(n, BOOST_FUSION_AS_TUPLE_ELEMENT, _)>            \
    make_tuple(BOOST_PP_ENUM_BINARY_PARAMS(n, T, const& _))                     \
    {                                                                           \
        return tuple<BOOST_PP_ENUM(n, BOOST_FUSION_AS_TUPLE_ELEMENT, _)>(       \
            BOOST_PP_ENUM_PARAMS(n, _));                                        \
    }

    BOOST_PP_REPEAT_FROM_TO(1, FUSION_MAX_TUPLE_SIZE, FUSION_MAKE_TUPLE, _)

#undef BOOST_FUSION_AS_TUPLE_ELEMENT
#undef FUSION_MAKE_TUPLE

}}

#endif

/*=============================================================================
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_TIE_HPP)
#define FUSION_SEQUENCE_TIE_HPP

#include <boost/ref.hpp>
#include <boost/preprocessor/repetition/enum.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/spirit/fusion/sequence/tuple.hpp>

namespace boost { namespace fusion
{
    //  Swallows any assignment (by Doug Gregor)
    namespace detail
    {
        struct swallow_assign
        {
            template<typename T>
            swallow_assign const&
            operator=(const T&) const
            {
                return *this;
            }
        };
    }

    //  "ignore" allows tuple positions to be ignored when using "tie".
    detail::swallow_assign const ignore = detail::swallow_assign();

#define FUSION_REFERENCE_TYPE(z, n, data)                                       \
    BOOST_PP_CAT(T, n)&

#define FUSION_TIE(z, n, _)                                                     \
                                                                                \
    template <BOOST_PP_ENUM_PARAMS(n, typename T)>                              \
    inline tuple<BOOST_PP_ENUM(n, FUSION_REFERENCE_TYPE, _)>                    \
    tie(BOOST_PP_ENUM_BINARY_PARAMS(n, T, & _))                                 \
    {                                                                           \
        return tuple<BOOST_PP_ENUM(n, FUSION_REFERENCE_TYPE, _)>(               \
            BOOST_PP_ENUM_PARAMS(n, _));                                        \
    }

    BOOST_PP_REPEAT_FROM_TO(1, FUSION_MAX_TUPLE_SIZE, FUSION_TIE, _)

#undef FUSION_REFERENCE_TYPE
#undef FUSION_TIE

}}

#endif

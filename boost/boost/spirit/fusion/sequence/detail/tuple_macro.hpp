/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE_MACRO_HPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE_MACRO_HPP

///////////////////////////////////////////////////////////////////////////////
//
//  Pre-processor gunk. See tuple10.hpp and detail/tuple10.hpp. The
//  following code is the preprocessor version of the code found in
//  those files, plus/minus a few specific details (specifically,
//  the tuple_access<N> and tupleN+1 classes).
//
///////////////////////////////////////////////////////////////////////////////
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/repetition/repeat.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/arithmetic/inc.hpp>
#include <boost/preprocessor/arithmetic/dec.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/preprocessor/repetition/enum.hpp>

#define FUSION_TUPLE_N_ACCESS(z, n, _)                                          \
                                                                                \
    template <>                                                                 \
    struct tuple_access<n>                                                      \
    {                                                                           \
        template <typename Tuple>                                               \
        static typename tuple_access_result<Tuple, n>::type                     \
        get(Tuple& t)                                                           \
        {                                                                       \
            FUSION_RETURN_TUPLE_MEMBER(n);                                      \
        }                                                                       \
    };

#define FUSION_TUPLE_MEMBER_DEFAULT_INIT(z, n, _)                               \
    BOOST_PP_CAT(m, n)(BOOST_PP_CAT(T, n)())

#define FUSION_TUPLE_MEMBER_INIT(z, n, _)                                       \
    BOOST_PP_CAT(m, n)(BOOST_PP_CAT(_, n))

#define FUSION_TUPLE_MEMBER_ITERATOR_INIT(z, n, _)                              \
    BOOST_PP_CAT(m, n)(*BOOST_PP_CAT(_, n))

#define FUSION_TUPLE_MEMBER(z, n, _)                                            \
    BOOST_PP_CAT(T, n) BOOST_PP_CAT(m, n);

#define FUSION_TUPLE_MEMBER_ASSIGN(z, n, _)                                     \
    this->BOOST_PP_CAT(m, n) = t.BOOST_PP_CAT(m, n);

#define FUSION_TUPLE_RESULT_OF_NEXT_TYPE(z, n, _)                               \
    typedef typename meta::next<                                            \
        BOOST_PP_CAT(BOOST_PP_CAT(i, n), _type)>::type                          \
        BOOST_PP_CAT(BOOST_PP_CAT(i, BOOST_PP_INC(n)), _type);

#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
# define FUSION_TUPLE_RESULT_OF_NEXT(z, n, _)                                   \
    next_iter::BOOST_PP_CAT(BOOST_PP_CAT(i, BOOST_PP_INC(n)), _type)            \
        BOOST_PP_CAT(i, BOOST_PP_INC(n))(fusion::next(BOOST_PP_CAT(i, n)));
#else
# define FUSION_TUPLE_RESULT_OF_NEXT(z, n, _)                                   \
    FUSION_TUPLE_RESULT_OF_NEXT_TYPE(z, n, _)                                   \
        BOOST_PP_CAT(BOOST_PP_CAT(i, BOOST_PP_INC(n)), _type)                   \
            BOOST_PP_CAT(i, BOOST_PP_INC(n))(fusion::next(BOOST_PP_CAT(i, n)));
#endif

#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
# define FUSION_TUPLE_NEXT_ITER_N(z, n, _)                                      \
    namespace detail                                                            \
    {                                                                           \
        template <typename i0_type>                                             \
        struct BOOST_PP_CAT(next_iter, n)                                       \
        {                                                                       \
            BOOST_PP_REPEAT(                                                    \
                BOOST_PP_DEC(n), FUSION_TUPLE_RESULT_OF_NEXT_TYPE, _)           \
        };                                                                      \
    }

#else
# define FUSION_TUPLE_NEXT_ITER_N(z, n, _)
#endif

#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
# define FUSION_TUPLE_CONSTRUCT_FROM_ITER(n)                                    \
    typedef detail::BOOST_PP_CAT(next_iter, n)<i0_type> next_iter;              \
    BOOST_PP_REPEAT(BOOST_PP_DEC(n), FUSION_TUPLE_RESULT_OF_NEXT, _)
#else
# define FUSION_TUPLE_CONSTRUCT_FROM_ITER(n)                                    \
    BOOST_PP_REPEAT(BOOST_PP_DEC(n), FUSION_TUPLE_RESULT_OF_NEXT, _)
#endif

#endif

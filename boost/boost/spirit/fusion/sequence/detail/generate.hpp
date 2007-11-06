/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_GENERATE_HPP)
#define FUSION_SEQUENCE_DETAIL_GENERATE_HPP

#include <boost/spirit/fusion/sequence/tuple.hpp>
#include <boost/spirit/fusion/iterator/value_of.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/preprocessor/repetition/repeat.hpp>
#include <boost/preprocessor/repetition/enum.hpp>
#include <boost/preprocessor/arithmetic/inc.hpp>
#include <boost/preprocessor/arithmetic/dec.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>

#define FUSION_DEREF_ITERATOR(z, n, data)                                       \
    typename checked_deref_iterator<                                            \
        BOOST_PP_CAT(T, n), Last>::type

#define FUSION_NEXT_ITERATOR(z, n, data)                                        \
    typedef typename checked_next_iterator<                                     \
        BOOST_PP_CAT(T, n), Last>::type                                         \
    BOOST_PP_CAT(T, BOOST_PP_INC(n));

namespace boost { namespace fusion { namespace detail
{
    template <typename First, typename Last>
    struct checked_deref_iterator
    {
        typedef typename
            mpl::eval_if<
                meta::equal_to<First, Last>
              , mpl::identity<void_t>
              , meta::value_of<First>
            >::type
        type;
    };

    template <typename First, typename Last>
    struct checked_next_iterator
    {
        typedef typename
            mpl::eval_if<
                meta::equal_to<First, Last>
              , mpl::identity<Last>
              , meta::next<First>
            >::type
        type;
    };

    template <typename First, typename Last>
    struct result_of_generate
    {
        typedef First T0;
        BOOST_PP_REPEAT(
            BOOST_PP_DEC(FUSION_MAX_TUPLE_SIZE), FUSION_NEXT_ITERATOR, _)
        typedef tuple<BOOST_PP_ENUM(FUSION_MAX_TUPLE_SIZE
            , FUSION_DEREF_ITERATOR, _)> type;
    };

    template <typename First, typename Last>
    inline typename result_of_generate<First, Last>::type
    generate(First const& first, Last const&)
    {
        typedef typename result_of_generate<First, Last>::type result;
        return result(first);
    }
}}}

#undef FUSION_DEREF_ITERATOR
#undef FUSION_NEXT_ITERATOR
#endif



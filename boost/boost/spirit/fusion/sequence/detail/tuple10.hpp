/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE10_HPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE10_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <boost/spirit/fusion/iterator/tuple_iterator.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_access_result.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_value_at_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_at_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_size_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple_begin_end_traits.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/mpl/at.hpp>
#include <boost/mpl/int.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/vector/vector10.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/preprocessor/iteration/iterate.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
# include <boost/spirit/fusion/iterator/next.hpp>
#endif

namespace boost { namespace fusion
{
    namespace detail
    {
        struct disambiguate_as_tuple {};
        struct disambiguate_as_data {};
        struct disambiguate_as_iterator {};

        template <typename X, typename T0>
        struct disambiguate
        {
            typedef typename
                mpl::if_<
                    is_convertible<X, T0>
                  , disambiguate_as_data
                  , typename mpl::if_<
                        is_base_and_derived<sequence_root, X>
                      , disambiguate_as_tuple
                      , disambiguate_as_iterator
                    >::type
                >::type
            type;

            static type
            call()
            {
                FUSION_RETURN_DEFAULT_CONSTRUCTED;
            }
        };

        template <int N>
        struct tuple_access;

        template <>
        struct tuple_access<0>
        {
            template <typename Tuple>
            static typename tuple_access_result<Tuple, 0>::type
            get(Tuple& t)
            {
                FUSION_RETURN_TUPLE_MEMBER(0);
            }
        };

        template <>
        struct tuple_access<1>
        {
            template <typename Tuple>
            static typename tuple_access_result<Tuple, 1>::type
            get(Tuple& t)
            {
                FUSION_RETURN_TUPLE_MEMBER(1);
            }
        };

        template <>
        struct tuple_access<2>
        {
            template <typename Tuple>
            static typename tuple_access_result<Tuple, 2>::type
            get(Tuple& t)
            {
                FUSION_RETURN_TUPLE_MEMBER(2);
            }
        };

}}} // namespace boost::fusion::detail

///////////////////////////////////////////////////////////////////////////////
//
//  Bring in the rest of the fixed-tuples using the pre-processor. Generate
//  expansions for the tuple_access<N> and tupleN+1 classes for N = 3..10.
//
///////////////////////////////////////////////////////////////////////////////
#include <boost/spirit/fusion/sequence/detail/tuple_macro.hpp>

namespace boost { namespace fusion
{
    namespace detail
    {
        BOOST_PP_REPEAT_FROM_TO(3, 10, FUSION_TUPLE_N_ACCESS, _)
    }

    struct tuple_tag;

#  define BOOST_PP_ITERATION_PARAMS_1 (3, (4, 10, <boost/spirit/fusion/sequence/detail/tuple_body.hpp>))
#  include BOOST_PP_ITERATE()
}}

#endif

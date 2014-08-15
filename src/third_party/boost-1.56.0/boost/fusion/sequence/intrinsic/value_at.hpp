/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying 
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_VALUE_AT_05052005_0229)
#define FUSION_VALUE_AT_05052005_0229

#include <boost/fusion/support/config.hpp>
#include <boost/mpl/int.hpp>
#include <boost/fusion/sequence/intrinsic_fwd.hpp>
#include <boost/fusion/support/tag_of.hpp>

namespace boost { namespace fusion
{
    // Special tags:
    struct sequence_facade_tag;
    struct boost_tuple_tag; // boost::tuples::tuple tag
    struct boost_array_tag; // boost::array tag
    struct mpl_sequence_tag; // mpl sequence tag
    struct std_pair_tag; // std::pair tag

    namespace extension
    {
        template <typename Tag>
        struct value_at_impl
        {
            template <typename Sequence, typename N>
            struct apply;
        };

        template <>
        struct value_at_impl<sequence_facade_tag>
        {
            template <typename Sequence, typename N>
            struct apply : Sequence::template value_at<Sequence, N> {};
        };

        template <>
        struct value_at_impl<boost_tuple_tag>;

        template <>
        struct value_at_impl<boost_array_tag>;

        template <>
        struct value_at_impl<mpl_sequence_tag>;

        template <>
        struct value_at_impl<std_pair_tag>;
    }

    namespace result_of
    {
        template <typename Sequence, typename N>
        struct value_at
            : extension::value_at_impl<typename detail::tag_of<Sequence>::type>::
                template apply<Sequence, N>
        {};

        template <typename Sequence, int N>
        struct value_at_c
            : fusion::result_of::value_at<Sequence, mpl::int_<N> >
        {};
    }
}}

#endif


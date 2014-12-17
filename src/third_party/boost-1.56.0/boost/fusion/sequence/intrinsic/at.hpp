/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_AT_05042005_0722)
#define FUSION_AT_05042005_0722

#include <boost/fusion/support/config.hpp>
#include <boost/mpl/int.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/fusion/sequence/intrinsic_fwd.hpp>
#include <boost/fusion/support/tag_of.hpp>
#include <boost/fusion/support/detail/access.hpp>

namespace boost { namespace fusion
{
    // Special tags:
    struct sequence_facade_tag;
    struct boost_tuple_tag; // boost::tuples::tuple tag
    struct boost_array_tag; // boost::array tag
    struct mpl_sequence_tag; // mpl sequence tag
    struct std_pair_tag; // std::pair tag
    struct std_tuple_tag; // std::tuple tag

    namespace extension
    {
        template <typename Tag>
        struct at_impl
        {
            template <typename Sequence, typename N>
            struct apply;
        };

        template <>
        struct at_impl<sequence_facade_tag>
        {
            template <typename Sequence, typename N>
            struct apply : Sequence::template at<Sequence, N> {};
        };

        template <>
        struct at_impl<boost_tuple_tag>;

        template <>
        struct at_impl<boost_array_tag>;

        template <>
        struct at_impl<mpl_sequence_tag>;

        template <>
        struct at_impl<std_pair_tag>;

        template <>
        struct at_impl<std_tuple_tag>;
    }

    namespace result_of
    {
        template <typename Sequence, typename N>
        struct at
            : extension::at_impl<typename detail::tag_of<Sequence>::type>::
                template apply<Sequence, N>
        {};

        template <typename Sequence, int N>
        struct at_c
            : at<Sequence, mpl::int_<N> >
        {};
    }


    template <typename N, typename Sequence>
    BOOST_FUSION_GPU_ENABLED
    inline typename
        lazy_disable_if<
            is_const<Sequence>
          , result_of::at<Sequence, N>
        >::type
    at(Sequence& seq)
    {
        return result_of::at<Sequence, N>::call(seq);
    }

    template <typename N, typename Sequence>
    BOOST_FUSION_GPU_ENABLED
    inline typename result_of::at<Sequence const, N>::type
    at(Sequence const& seq)
    {
        return result_of::at<Sequence const, N>::call(seq);
    }

    template <int N, typename Sequence>
    BOOST_FUSION_GPU_ENABLED
    inline typename
        lazy_disable_if<
            is_const<Sequence>
          , result_of::at_c<Sequence, N>
        >::type
    at_c(Sequence& seq)
    {
        return fusion::at<mpl::int_<N> >(seq);
    }

    template <int N, typename Sequence>
    BOOST_FUSION_GPU_ENABLED
    inline typename result_of::at_c<Sequence const, N>::type
    at_c(Sequence const& seq)
    {
        return fusion::at<mpl::int_<N> >(seq);
    }
}}

#endif


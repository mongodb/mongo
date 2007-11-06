/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_JOINT_VIEW_BEGIN_END_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_JOINT_VIEW_BEGIN_END_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/mpl/if.hpp>

namespace boost { namespace fusion
{
    struct joint_view_tag;

    template <typename First, typename Last, typename Concat>
    struct joint_view_iterator;

    namespace joint_view_detail {
        template <typename Sequence>
        struct begin_traits_impl
        {
            typedef typename Sequence::first_type first_type;
            typedef typename Sequence::last_type last_type;
            typedef typename Sequence::concat_type concat_type;
            typedef boost::fusion::meta::equal_to<first_type, last_type> equal_to;

            typedef typename
                boost::mpl::if_<
                    equal_to
                  , concat_type
                  , boost::fusion::joint_view_iterator<first_type, last_type, concat_type>
                >::type
            type;

            static type
            call(Sequence& s);
        };

        template<typename Sequence>
        typename begin_traits_impl<Sequence>::type
        call(Sequence& s, boost::mpl::true_) {
            return s.concat();
        }

        template<typename Sequence>
        typename begin_traits_impl<Sequence>::type
        call(Sequence& s, boost::mpl::false_) {
            typedef BOOST_DEDUCED_TYPENAME begin_traits_impl<Sequence>::type type;
            return type(s.first(), s.concat());
        }

        template<typename Sequence>
        typename begin_traits_impl<Sequence>::type 
        begin_traits_impl<Sequence>::call(Sequence& s)
        {
            return joint_view_detail::call(s, equal_to());
        }

        template <typename Sequence>
        struct end_traits_impl
        {
            typedef typename Sequence::concat_last_type type;

            static type
            call(Sequence& s);
        };

        template<typename Sequence>
        typename end_traits_impl<Sequence>::type 
        end_traits_impl<Sequence>::call(Sequence& s)
        {
            return s.concat_last();
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct begin_impl;

        template <>
        struct begin_impl<joint_view_tag>
        {
            template <typename Sequence>
            struct apply : joint_view_detail::begin_traits_impl<Sequence>
            {};
        };

        template <typename Tag>
        struct end_impl;

        template <>
        struct end_impl<joint_view_tag>
        {
            template <typename Sequence>
            struct apply : joint_view_detail::end_traits_impl<Sequence>
            {};
        };
    }
}}

namespace boost { namespace mpl
{
    template <typename Tag>
    struct begin_impl;

    template <typename Tag>
    struct end_impl;

    template <>
    struct begin_impl<fusion::joint_view_tag>
        : fusion::meta::begin_impl<fusion::joint_view_tag> {};

    template <>
    struct end_impl<fusion::joint_view_tag>
        : fusion::meta::end_impl<fusion::joint_view_tag> {};
}}

#endif



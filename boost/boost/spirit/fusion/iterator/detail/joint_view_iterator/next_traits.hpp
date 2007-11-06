/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_DETAIL_JOINT_VIEW_ITERATOR_NEXT_TRAITS_HPP)
#define FUSION_ITERATOR_DETAIL_JOINT_VIEW_ITERATOR_NEXT_TRAITS_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/mpl/if.hpp>

namespace boost { namespace fusion
{
    struct joint_view_iterator_tag;

    template <typename First, typename Last, typename Concat>
    struct joint_view_iterator;

    namespace join_view_detail {
        template<typename Iterator>
        struct next_traits_impl {
            typedef typename Iterator::first_type first_type;
            typedef typename Iterator::last_type last_type;
            typedef typename Iterator::concat_type concat_type;
            typedef typename meta::next<first_type>::type next_type;
            typedef meta::equal_to<next_type, last_type> equal_to;

            typedef typename
                mpl::if_<
                    equal_to
                  , concat_type
                  , joint_view_iterator<next_type, last_type, concat_type>
                >::type
            type;

            static type
            call(Iterator const& i);
        };

        template<typename Iterator>
        typename next_traits_impl<Iterator>::type 
        call(Iterator const& i, mpl::true_)
        {
            return i.concat;
        }
        
        template<typename Iterator>
        typename next_traits_impl<Iterator>::type 
        call(Iterator const& i, mpl::false_)
        {
            typedef typename next_traits_impl<Iterator>::type type;
            return type(fusion::next(i.first), i.concat);
        }

        template<typename Iterator>
        typename next_traits_impl<Iterator>::type
        next_traits_impl<Iterator>::call(Iterator const& i)
        {
            return join_view_detail::call(i, equal_to());
        }
    }

    namespace meta
    {
        template <typename Tag>
        struct next_impl;

        template <>
        struct next_impl<joint_view_iterator_tag>
        {
            template <typename Iterator>
            struct apply : join_view_detail::next_traits_impl<Iterator>
            {};
        };
    }
}}

#endif



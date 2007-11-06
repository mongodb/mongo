/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_NEXT_HPP)
#define FUSION_ITERATOR_NEXT_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/as_fusion_iterator.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct next_impl
        {
            template <typename Iterator>
            struct apply
            {
                // VC6 needs this
                typedef int type;
            };
        };

        template <typename Iterator>
        struct next
        {
            typedef as_fusion_iterator<Iterator> converter;
            typedef typename converter::type iter;

            typedef typename
                next_impl<FUSION_GET_TAG(iter)>::
                    template apply<iter>::type
            type;
        };
    }

    template <typename Iterator>
    inline typename meta::next<Iterator>::type
    next(Iterator const& i)
    {
        typedef as_fusion_iterator<Iterator> converter;
        typedef typename converter::type iter;

        return meta::next_impl<FUSION_GET_TAG(iter)>::
            template apply<iter>::call(converter::convert(i));
    }
}}

#endif

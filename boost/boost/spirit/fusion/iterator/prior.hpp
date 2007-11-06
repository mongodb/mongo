/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ITERATOR_PRIOR_HPP)
#define FUSION_ITERATOR_PRIOR_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/iterator/as_fusion_iterator.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct prior_impl
        {
            template <typename Iterator>
            struct apply {};
        };

        template <typename Iterator>
        struct prior
        {
            typedef as_fusion_iterator<Iterator> converter;
            typedef typename converter::type iter;

            typedef typename
                prior_impl<FUSION_GET_TAG(iter)>::
                    template apply<iter>::type
            type;
        };
    }

    template <typename Iterator>
    inline typename meta::prior<Iterator>::type
    prior(Iterator const& i)
    {
        typedef as_fusion_iterator<Iterator> converter;
        typedef typename converter::type iter;

        return meta::prior_impl<FUSION_GET_TAG(iter)>::
            template apply<iter>::call(converter::convert(i));
    }
}}

#endif

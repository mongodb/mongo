/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_VALUE_OF_HPP)
#define FUSION_VALUE_OF_HPP

#include <boost/spirit/fusion/detail/config.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Tag>
        struct value_impl
        {
            template <typename Iterator>
            struct apply
            {
                // VC6 needs this
                typedef int type;
            };
        };

        template <typename Iterator>
        struct value_of
        {
            typedef typename
                value_impl<FUSION_GET_TAG(Iterator)>::
                    template apply<Iterator>::type
            type;
        };
    }
}}

#endif

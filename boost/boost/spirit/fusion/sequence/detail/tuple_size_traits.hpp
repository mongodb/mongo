/*=============================================================================
    Copyright (c) 2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_DETAIL_TUPLE_SIZE_TRAITS_HPP)
#define FUSION_SEQUENCE_DETAIL_TUPLE_SIZE_TRAITS_HPP

namespace boost { namespace fusion
{
    struct tuple_tag;

    namespace meta
    {
        template <typename Tag>
        struct size_impl;

        template <>
        struct size_impl<tuple_tag>
        {
            template <typename Sequence>
            struct apply : Sequence::size {};
        };
    }
}}

#endif

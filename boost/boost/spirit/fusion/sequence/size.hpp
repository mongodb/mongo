/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_SIZE_HPP)
#define FUSION_SEQUENCE_SIZE_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/sequence/as_fusion_sequence.hpp>

namespace boost { namespace fusion
{
    ///////////////////////////////////////////////////////////////////////////
    //
    //  size metafunction
    //
    //      Get the size of a Sequence. Usage:
    //
    //          size<Sequence>::value
    //
    ///////////////////////////////////////////////////////////////////////////
    namespace meta
    {
        template <typename Tag>
        struct size_impl
        {
            template <typename Sequence>
            struct apply {};
        };

        template <typename Sequence>
        struct size
            : size_impl<typename as_fusion_sequence<Sequence>::type::tag>::
                template apply<typename as_fusion_sequence<Sequence>::type>
        {};
    }
}}

#endif

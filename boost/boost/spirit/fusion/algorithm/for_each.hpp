/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_FOR_EACH_HPP)
#define FUSION_ALGORITHM_FOR_EACH_HPP

#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/spirit/fusion/algorithm/detail/for_each.ipp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename F>
        struct for_each
        {
            typedef void type;
        };
    }

    namespace function
    {
        struct for_each
        {
            template <typename Sequence, typename F>
            struct apply
            {
                typedef void type;
            };

            template <typename Sequence, typename F>
            inline void
            operator()(Sequence const& seq, F const& f) const
            {
                detail::for_each(
                        fusion::begin(seq)
                      , fusion::end(seq)
                      , f
                      , meta::equal_to<
                            BOOST_DEDUCED_TYPENAME meta::begin<Sequence>::type
                          , BOOST_DEDUCED_TYPENAME meta::end<Sequence>::type>());
            }

            template <typename Sequence, typename F>
            inline void
            operator()(Sequence& seq, F const& f) const
            {
                detail::for_each(
                        fusion::begin(seq)
                      , fusion::end(seq)
                      , f
                      , meta::equal_to<
                            BOOST_DEDUCED_TYPENAME meta::begin<Sequence>::type
                          , BOOST_DEDUCED_TYPENAME meta::end<Sequence>::type>());
            }
        };
    }

    function::for_each const for_each = function::for_each();
}}

#endif


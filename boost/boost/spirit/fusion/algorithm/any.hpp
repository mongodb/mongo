/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt
    Copyright (c) 2005 Eric Niebler

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_ANY_HPP)
#define FUSION_ALGORITHM_ANY_HPP

#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>
#include <boost/spirit/fusion/algorithm/detail/any.ipp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename F>
        struct any
        {
            typedef bool type;
        };
    }

    namespace function
    {
        struct any
        {
            template <typename Sequence, typename F>
            struct apply
            {
                typedef bool type;
            };

            template <typename Sequence, typename F>
            inline bool
            operator()(Sequence const& seq, F const& f) const
            {
                return detail::any(
                        fusion::begin(seq)
                      , fusion::end(seq)
                      , f
                      , meta::equal_to<
                            BOOST_DEDUCED_TYPENAME meta::begin<Sequence>::type
                          , BOOST_DEDUCED_TYPENAME meta::end<Sequence>::type>());
            }

            template <typename Sequence, typename F>
            inline bool
            operator()(Sequence& seq, F const& f) const
            {
                return detail::any(
                        fusion::begin(seq)
                      , fusion::end(seq)
                      , f
                      , meta::equal_to<
                            BOOST_DEDUCED_TYPENAME meta::begin<Sequence>::type
                          , BOOST_DEDUCED_TYPENAME meta::end<Sequence>::type>());
            }
        };
    }

    function::any const any = function::any();
}}

#endif


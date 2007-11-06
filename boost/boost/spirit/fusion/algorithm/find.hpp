/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_FIND_HPP)
#define FUSION_ALGORITHM_FIND_HPP

#include <boost/spirit/fusion/algorithm/detail/find_if.ipp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/type_traits/is_same.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename T>
        struct find
        {
            typedef typename
                detail::static_find_if<
                    typename meta::begin<Sequence>::type
                  , typename meta::end<Sequence>::type
                  , is_same<mpl::_, T>
                >::type
            type;
        };
    }

    namespace function
    {
        struct find
        {
            template <typename Sequence, typename T>
            struct apply : meta::find<Sequence, T> {};

            template <typename Sequence, typename T>
            typename apply<Sequence const, typename T::type>::type
            operator()(Sequence const& seq, T) const
            {
                typedef
                    detail::static_find_if<
                        BOOST_DEDUCED_TYPENAME meta::begin<Sequence const>::type
                      , BOOST_DEDUCED_TYPENAME meta::end<Sequence const>::type
                      , is_same<mpl::_, BOOST_DEDUCED_TYPENAME T::type>
                    >
                filter;

                return filter::call(fusion::begin(seq));
            }

            template <typename Sequence, typename T>
            typename apply<Sequence, typename T::type>::type
            operator()(Sequence& seq, T) const
            {
                typedef
                    detail::static_find_if<
                        BOOST_DEDUCED_TYPENAME meta::begin<Sequence>::type
                      , BOOST_DEDUCED_TYPENAME meta::end<Sequence>::type
                      , is_same<mpl::_, BOOST_DEDUCED_TYPENAME T::type>
                    >
                filter;

                return filter::call(fusion::begin(seq));
            }
        };
    }

    function::find const find = function::find();
}}

#endif


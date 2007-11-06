/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_REMOVE_HPP)
#define FUSION_ALGORITHM_REMOVE_HPP

#include <boost/spirit/fusion/sequence/filter_view.hpp>
#include <boost/mpl/not.hpp>
#include <boost/type_traits/is_same.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename T>
        struct remove
        {
            typedef filter_view<Sequence, mpl::not_<is_same<mpl::_, T> > > type;
        };
    }

    namespace function
    {
        struct remove
        {
            template <typename Sequence, typename T>
            struct apply : meta::remove<Sequence, T> {};

            template <typename Sequence, typename T>
            inline filter_view<
                Sequence const
              , mpl::not_<is_same<mpl::_, typename T::type> > >
            operator()(Sequence const& seq, T) const
            {
                return filter_view<
                    Sequence const
                  , mpl::not_<is_same<mpl::_, BOOST_DEDUCED_TYPENAME T::type>
                > >(seq);
            }

            template <typename Sequence, typename T>
            inline filter_view<
                Sequence
              , mpl::not_<is_same<mpl::_, typename T::type> > >
            operator()(Sequence& seq, T) const
            {
                return filter_view<
                    Sequence
                  , mpl::not_<is_same<mpl::_, BOOST_DEDUCED_TYPENAME T::type>
                > >(seq);
            }
        };
    }

    function::remove const remove = function::remove();
}}

#endif


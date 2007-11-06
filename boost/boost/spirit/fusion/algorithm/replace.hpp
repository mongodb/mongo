/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_REPLACE_HPP)
#define FUSION_ALGORITHM_REPLACE_HPP

#include <boost/spirit/fusion/sequence/single_view.hpp>
#include <boost/spirit/fusion/sequence/joint_view.hpp>
#include <boost/spirit/fusion/sequence/range.hpp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename Position, typename T>
        struct replace
        {
            typedef typename meta::begin<Sequence>::type first_type;
            typedef typename meta::end<Sequence>::type last_type;
            typedef typename meta::next<Position>::type next_type;
#if! BOOST_WORKAROUND(BOOST_MSVC,<=1300)
            BOOST_STATIC_ASSERT((!meta::equal_to<Position, last_type>::value));
#endif
            typedef const single_view<T> insert_type;
            typedef range<first_type, Position> left_type;
            typedef range<next_type, last_type> right_type;
            typedef joint_view<left_type, insert_type, true, true> left_replace_type;
            typedef joint_view<left_replace_type, right_type, true, true> type;
        };
    }

    namespace function
    {
        struct replace
        {
            template <typename Sequence, typename Position, typename T>
            struct apply : meta::replace<Sequence, Position, T> {};

            template <typename Sequence, typename Position, typename T>
            typename apply<Sequence const, Position, T>::type
            operator()(Sequence const& seq, Position const& pos, T const& x) const
            {
                typedef apply<Sequence const, Position, T> replacer;

                typedef typename replacer::left_type left_type;
                typedef typename replacer::right_type right_type;
                typedef typename replacer::left_replace_type left_replace_type;
                typedef typename replacer::insert_type insert_type;
                typedef typename replacer::type result;

                left_type left(fusion::begin(seq), pos);
                right_type right(fusion::next(pos), fusion::end(seq));
                insert_type ins(x);
                left_replace_type left_replace(left, ins);
                return result(left_replace, right);
            }

            template <typename Sequence, typename Position, typename T>
            typename apply<Sequence, Position, T>::type
            operator()(Sequence& seq, Position const& pos, T const& x) const
            {
                typedef apply<Sequence, Position, T> replacer;

                typedef typename replacer::left_type left_type;
                typedef typename replacer::right_type right_type;
                typedef typename replacer::left_replace_type left_replace_type;
                typedef typename replacer::insert_type insert_type;
                typedef typename replacer::type result;

                left_type left(fusion::begin(seq), pos);
                right_type right(fusion::next(pos), fusion::end(seq));
                insert_type ins(x);
                left_replace_type left_replace(left, ins);
                return result(left_replace, right);
            }
        };
    }

    function::replace const replace = function::replace();
}}

#endif


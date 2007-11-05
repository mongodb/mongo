/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_INSERT_HPP)
#define FUSION_ALGORITHM_INSERT_HPP

#include <boost/spirit/fusion/sequence/single_view.hpp>
#include <boost/spirit/fusion/sequence/joint_view.hpp>
#include <boost/spirit/fusion/sequence/range.hpp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename Position, typename T>
        struct insert
        {
            typedef typename meta::begin<Sequence>::type first_type;
            typedef typename meta::end<Sequence>::type last_type;

            typedef const single_view<T> insert_type;
            typedef range<first_type, Position> left_type;
            typedef range<Position, last_type> right_type;
            typedef joint_view<left_type, insert_type, true, true> left_insert_type;
            typedef joint_view<left_insert_type, right_type, true, true> type;
        };
    }

    namespace function
    {
        struct insert
        {
            template <typename Sequence, typename Position, typename T>
            struct apply : meta::insert<Sequence, Position, T> {};

            template <typename Sequence, typename Position, typename T>
            inline typename apply<Sequence const, Position, T>::type
            operator()(Sequence const& seq, Position const& pos, T const& x) const
            {
                typedef apply<Sequence const, Position, T> meta;
                typedef typename meta::left_type left_type;
                typedef typename meta::right_type right_type;
                typedef typename meta::left_insert_type left_insert_type;
                typedef typename meta::insert_type insert_type;
                typedef typename meta::type result;

                left_type left(fusion::begin(seq), pos);
                right_type right(pos, fusion::end(seq));
                insert_type ins(x);
                left_insert_type left_insert(left, ins);
                return result(left_insert, right);
            }

            template <typename Sequence, typename Position, typename T>
            inline typename apply<Sequence, Position, T>::type
            operator()(Sequence& seq, Position const& pos, T const& x) const
            {
                typedef apply<Sequence, Position, T> meta_type;
                typedef typename meta_type::left_type left_type;
                typedef typename meta_type::right_type right_type;
                typedef typename meta_type::left_insert_type left_insert_type;
                typedef typename meta_type::insert_type insert_type;
                typedef typename meta_type::type result;

                left_type left(fusion::begin(seq), pos);
                right_type right(pos, fusion::end(seq));
                insert_type ins(x);
                left_insert_type left_insert(left, ins);
                return result(left_insert, right);
            }
        };
    }

    function::insert const insert = function::insert();
}}

#endif


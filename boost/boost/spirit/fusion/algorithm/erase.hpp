/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_ALGORITHM_ERASE_HPP)
#define FUSION_ALGORITHM_ERASE_HPP

#include <boost/spirit/fusion/sequence/single_view.hpp>
#include <boost/spirit/fusion/sequence/joint_view.hpp>
#include <boost/spirit/fusion/sequence/range.hpp>
#include <boost/spirit/fusion/sequence/begin.hpp>
#include <boost/spirit/fusion/sequence/end.hpp>
#include <boost/spirit/fusion/iterator/next.hpp>
#include <boost/spirit/fusion/iterator/equal_to.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Sequence, typename Position>
        struct erase
        {
            typedef typename meta::begin<Sequence>::type first_type;
            typedef typename meta::end<Sequence>::type last_type;
#if! BOOST_WORKAROUND(BOOST_MSVC,<=1300)
            BOOST_STATIC_ASSERT((!meta::equal_to<Position, last_type>::value));
#endif

            typedef typename meta::next<Position>::type next_type;
            typedef range<first_type, Position> left_type;
            typedef range<next_type, last_type> right_type;
            typedef joint_view<left_type, right_type> type;
        };
    }

    namespace function
    {
        struct erase
        {
            template <typename Sequence, typename Position>
            struct apply : meta::erase<Sequence, Position> {};

            template <typename Sequence, typename Position>
            typename apply<Sequence const, Position>::type
            operator()(Sequence const& seq, Position const& pos) const
            {
                typedef apply<Sequence const, Position> meta_type;
                typedef typename meta_type::left_type left_type;
                typedef typename meta_type::right_type right_type;
                typedef typename meta_type::type result_type;

                left_type left(fusion::begin(seq), pos);
                right_type right(fusion::next(pos), fusion::end(seq));
                return result_type(left, right);
            }

//            template <typename Sequence, typename Position>
//            typename apply<Sequence, Position>::type
//            operator()(Sequence& seq, Position const& pos) const
//            {
//                typedef apply<Sequence, Position> meta;
//                typedef typename meta::left_type left_type;
//                typedef typename meta::right_type right_type;
//                typedef typename meta::type result_type;
//
//                left_type left(fusion::begin(seq), pos);
//                right_type right(fusion::next(pos), fusion::end(seq));
//                return result_type(left, right);
//            }
        };
    }

    function::erase const erase = function::erase();
}}

#endif


///////////////////////////////////////////////////////////////////////////////
// width_of.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_WIDTH_OF_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_WIDTH_OF_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <vector>
#include <boost/ref.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/fold.hpp>
#include <boost/mpl/plus.hpp>
#include <boost/mpl/front.hpp>
#include <boost/mpl/times.hpp>
#include <boost/mpl/assert.hpp>
#include <boost/mpl/lambda.hpp>
#include <boost/mpl/size_t.hpp>
#include <boost/mpl/logical.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/equal_to.hpp>
#include <boost/mpl/transform_view.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/static/as_xpr.hpp>

namespace boost { namespace xpressive { namespace detail
{
    ///////////////////////////////////////////////////////////////////////////////
    // add_width
    template<typename X, typename Y>
    struct add_width
      : mpl::eval_if
        <
            mpl::or_
            <
                mpl::equal_to<X, unknown_width>
              , mpl::equal_to<Y, unknown_width>
            >
          , mpl::identity<unknown_width>
          , mpl::plus<X, Y>
        >::type
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // mult_width
    template<typename X, typename Y>
    struct mult_width
      : mpl::eval_if
        <
            mpl::or_
            <
                mpl::equal_to<X, unknown_width>
              , mpl::equal_to<Y, unknown_width>
            >
          , mpl::identity<unknown_width>
          , mpl::times<X, Y>
        >::type
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // equal_width
    template<typename X, typename Y>
    struct equal_width
      : mpl::if_
        <
            mpl::equal_to<X, Y>
          , X
          , unknown_width
        >::type
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // width_of
    //
    template<typename Xpr>
    struct width_of;

    template<>
    struct width_of<no_next>
      : mpl::size_t<0>
    {
    };

    template<typename Matcher>
    struct width_of<proto::unary_op<Matcher, proto::noop_tag> >
      : as_matcher_type<Matcher>::type::width
    {
    };

    template<typename Left, typename Right>
    struct width_of<proto::binary_op<Left, Right, proto::right_shift_tag> >
      : add_width<width_of<Left>, width_of<Right> >
    {
    };

    template<typename Left, typename Right>
    struct width_of<proto::binary_op<Left, Right, proto::bitor_tag> >
      : equal_width<width_of<Left>, width_of<Right> >
    {
    };

    template<typename Right>
    struct width_of<proto::binary_op<mark_tag, Right, proto::assign_tag> >
      : width_of<Right>
    {
    };

    template<typename Right>
    struct width_of<proto::binary_op<set_initializer_type, Right, proto::assign_tag> >
      : mpl::size_t<1>
    {
    };

    template<typename Modifier, typename Xpr>
    struct width_of<proto::binary_op<Modifier, Xpr, modifier_tag> >
      : width_of<Xpr>
    {
    };

    template<typename Xpr, bool Positive>
    struct width_of<proto::unary_op<Xpr, lookahead_tag<Positive> > >
      : mpl::size_t<0>
    {
    };

    template<typename Xpr, bool Positive>
    struct width_of<proto::unary_op<Xpr, lookbehind_tag<Positive> > >
      : mpl::size_t<0>
    {
    };

    template<typename Xpr>
    struct width_of<proto::unary_op<Xpr, keeper_tag> >
      : width_of<Xpr>
    {
    };

    template<typename Matcher, typename Next>
    struct width_of<static_xpression<Matcher, Next> >
      : add_width<typename Matcher::width, width_of<Next> >
    {
    };

    template<typename BidiIter>
    struct width_of<shared_ptr<matchable<BidiIter> const> >
      : unknown_width
    {
    };

    template<typename BidiIter>
    struct width_of<std::vector<shared_ptr<matchable<BidiIter> const> > >
      : unknown_width
    {
    };

    template<typename BidiIter>
    struct width_of<proto::unary_op<basic_regex<BidiIter>, proto::noop_tag> >
      : unknown_width
    {
    };

    template<typename BidiIter>
    struct width_of<proto::unary_op<reference_wrapper<basic_regex<BidiIter> const>, proto::noop_tag> >
      : unknown_width
    {
    };

    template<typename Op>
    struct width_of<proto::unary_op<Op, proto::unary_plus_tag> >
      : unknown_width
    {
    };

    template<typename Op>
    struct width_of<proto::unary_op<Op, proto::unary_star_tag> >
      : unknown_width
    {
    };

    template<typename Op>
    struct width_of<proto::unary_op<Op, proto::logical_not_tag> >
      : unknown_width
    {
    };

    template<typename Op, uint_t Min, uint_t Max>
    struct width_of<proto::unary_op<Op, generic_quant_tag<Min, Max> > >
      : mpl::if_c<Min==Max, mult_width<width_of<Op>, mpl::size_t<Min> >, unknown_width>::type
    {
    };

    template<typename Op>
    struct width_of<proto::unary_op<Op, proto::unary_minus_tag> >
      : width_of<Op>
    {
    };

    // when complementing a set or an assertion, the width is that of the set (1) or the assertion (0)
    template<typename Op>
    struct width_of<proto::unary_op<Op, proto::complement_tag> >
      : width_of<Op>
    {
    };

    // The comma is used in list-initialized sets, and the width of sets are 1
    template<typename Left, typename Right>
    struct width_of<proto::binary_op<Left, Right, proto::comma_tag> >
      : mpl::size_t<1>
    {
    };

    // The subscript operator[] is used for sets, as in set['a' | range('b','h')], 
    // or for actions as in (any >> expr)[ action ]
    template<typename Left, typename Right>
    struct width_of<proto::binary_op<Left, Right, proto::subscript_tag> >
      : mpl::if_<is_same<Left, set_initializer_type>, mpl::size_t<1>, width_of<Left> >::type
    {
        // If Left is "set" then make sure that Right has a width_of 1
        BOOST_MPL_ASSERT
        ((
            mpl::or_
            <
                mpl::not_<is_same<Left, set_initializer_type> >
              , mpl::equal_to<width_of<Right>, mpl::size_t<1> >
            >
        ));
    };

    // The width of a list of alternates is N if all the alternates have width N, otherwise unknown_width
    template<typename Widths>
    struct alt_width_of
      : mpl::fold
        <
            Widths
          , typename mpl::front<Widths>::type
          , equal_width<mpl::_1, mpl::_2>
        >::type
    {
    };

    template<typename Alternates>
    struct width_of<alternates_list<Alternates> >
      : alt_width_of<mpl::transform_view<Alternates, width_of<mpl::_1> > >
    {
    };

    template<typename Op, typename Arg>
    struct width_of<proto::op_proxy<Op, Arg> >
      : width_of<Op>
    {
    };

}}} // namespace boost::xpressive::detail

#endif

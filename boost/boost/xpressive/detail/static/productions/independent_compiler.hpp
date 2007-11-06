///////////////////////////////////////////////////////////////////////////////
// independent_compiler.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_INDEPENDENT_COMPILER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_INDEPENDENT_COMPILER_HPP_EAN_10_04_2005

#include <boost/mpl/bool.hpp>
#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/detail/static/productions/domain_tags.hpp>

namespace boost { namespace xpressive { namespace detail
{
    template<bool Positive>
    struct lookahead_tag : proto::unary_tag {};

    template<bool Positive>
    struct lookbehind_tag : proto::unary_tag {};

    struct keeper_tag : proto::unary_tag {};

    ///////////////////////////////////////////////////////////////////////////////
    // lookahead_branch
    template<bool Positive>
    struct lookahead_branch
    {
        typedef true_xpression state_type;

        template<typename Op, typename State, typename>
        struct apply
        {
            typedef static_xpression<lookahead_matcher<Op>, State> type;
        };

        template<typename Op, typename State, typename Visitor>
        static static_xpression<lookahead_matcher<Op>, State>
        call(Op const &op, State const &state, Visitor &)
        {
            return make_static_xpression(lookahead_matcher<Op>(op, !Positive), state);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // lookbehind_branch
    template<bool Positive>
    struct lookbehind_branch
    {
        typedef true_xpression state_type;

        template<typename Op, typename State, typename>
        struct apply
        {
            typedef static_xpression<lookbehind_matcher<Op>, State> type;
        };

        template<typename Op, typename State, typename Visitor>
        static static_xpression<lookbehind_matcher<Op>, State>
        call(Op const &op, State const &state, Visitor &)
        {
            return make_static_xpression(lookbehind_matcher<Op>(op, !Positive), state);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // keeper_branch
    struct keeper_branch
    {
        typedef true_xpression state_type;

        template<typename Op, typename State, typename>
        struct apply
        {
            typedef static_xpression<keeper_matcher<Op>, State> type;
        };

        template<typename Op, typename State, typename Visitor>
        static static_xpression<keeper_matcher<Op>, State>
        call(Op const &op, State const &state, Visitor &)
        {
            return make_static_xpression(keeper_matcher<Op>(op), state);
        }
    };

}}}


namespace boost { namespace proto
{
    // ericne, 28/nov/05: CW9_4 doesn't like partial specializations of the form:
    //   template<bool F> struct foo<bar<F> >
    template<>
    struct compiler<xpressive::detail::lookahead_tag<true>, xpressive::detail::seq_tag, void>
      : branch_compiler<xpressive::detail::lookahead_branch<true>, xpressive::detail::ind_tag>
    {
    };

    template<>
    struct compiler<xpressive::detail::lookahead_tag<false>, xpressive::detail::seq_tag, void>
      : branch_compiler<xpressive::detail::lookahead_branch<false>, xpressive::detail::ind_tag>
    {
    };

    template<>
    struct compiler<xpressive::detail::lookbehind_tag<true>, xpressive::detail::seq_tag, void>
      : branch_compiler<xpressive::detail::lookbehind_branch<true>, xpressive::detail::ind_tag>
    {
    };

    template<>
    struct compiler<xpressive::detail::lookbehind_tag<false>, xpressive::detail::seq_tag, void>
      : branch_compiler<xpressive::detail::lookbehind_branch<false>, xpressive::detail::ind_tag>
    {
    };

    template<>
    struct compiler<xpressive::detail::keeper_tag, xpressive::detail::seq_tag, void>
      : branch_compiler<xpressive::detail::keeper_branch, xpressive::detail::ind_tag>
    {
    };
    
    template<typename OpTag>
    struct compiler<OpTag, xpressive::detail::ind_tag, void>
      : transform_compiler<arg_transform, xpressive::detail::seq_tag>
    {
    };

}}


#endif

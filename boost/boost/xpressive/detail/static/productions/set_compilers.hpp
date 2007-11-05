////////////////////////////////////////////////////////////////////////////
// set_compilers.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_SET_COMPILERS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_SET_COMPILERS_HPP_EAN_10_04_2005

#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/proto/compiler/fold.hpp>
#include <boost/xpressive/proto/compiler/branch.hpp>
#include <boost/xpressive/detail/utility/dont_care.hpp>
#include <boost/xpressive/detail/utility/traits_utils.hpp>
#include <boost/xpressive/detail/static/productions/domain_tags.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // set_branch
    //
    struct set_branch
    {
        typedef no_next state_type;

        template<typename, typename State, typename>
        struct apply
        {
            typedef State type;
        };

        template<typename Op, typename State, typename Visitor>
        static State const &call(Op const &op, State const &state, Visitor &)
        {
            return state.set(set_branch::get_matcher(op)), state;
        }

    private:

        template<typename Matcher>
        static Matcher const &get_matcher(static_xpression<Matcher, no_next> const &xpr)
        {
            return xpr;
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // list_branch
    struct list_branch
    {
        typedef int state_type; // not used

        template<typename Op, typename State, typename>
        struct apply
        {
            typedef static_xpression<Op, State> type;
        };

        template<typename Op, typename State>
        static static_xpression<Op, State>
        call(Op const &op, State const &state, dont_care)
        {
            return make_static_xpression(op, state);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // list_noop_compiler
    struct list_noop_compiler
    {
        template<typename, typename State, typename>
        struct apply
        {
            typedef typename State::next_type type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename State::next_type
        call(Op const &op, State const &state, Visitor &visitor)
        {
            typedef typename Visitor::char_type char_type;
            char_type ch = char_cast<char_type>(proto::arg(op), visitor.traits());
            return state.push_back(ch, visitor.traits());
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // list_assign_compiler
    struct list_assign_compiler
    {
        template<typename Op, typename, typename Visitor>
        struct apply
        {
            typedef typename Visitor::traits_type traits_type;
            typedef set_matcher<traits_type, 1> type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &, Visitor &visitor)
        {
            typedef typename Visitor::char_type char_type;
            char_type ch = char_cast<char_type>(proto::arg(proto::right(op)), visitor.traits());
            return typename apply<Op, State, Visitor>::type(ch, visitor.traits());
        }
    };

}}}

namespace boost { namespace proto
{

    ///////////////////////////////////////////////////////////////////////////////
    // compilers for sets such as set['a' | range('a','z')]
    template<>
    struct compiler<bitor_tag, xpressive::detail::set_tag, void>
      : fold_compiler<bitor_tag, xpressive::detail::set_tag>
    {
    };

    template<>
    struct compiler<noop_tag, xpressive::detail::set_tag, void>
      : branch_compiler<xpressive::detail::set_branch, xpressive::detail::seq_tag>
    {
    };

    template<>
    struct compiler<complement_tag, xpressive::detail::set_tag, void>
      : branch_compiler<xpressive::detail::set_branch, xpressive::detail::seq_tag>
    {
    };

    template<>
    struct compiler<comma_tag, xpressive::detail::set_tag, void>
      : branch_compiler<xpressive::detail::set_branch, xpressive::detail::seq_tag>
    {
    };


    ///////////////////////////////////////////////////////////////////////////////
    // compilers for list-initialized sets such as (set='a','b','c')
    template<>
    struct compiler<comma_tag, xpressive::detail::seq_tag, void>
      : branch_compiler<xpressive::detail::list_branch, xpressive::detail::lst_tag>
    {
    };

    template<>
    struct compiler<comma_tag, xpressive::detail::lst_tag, void>
      : fold_compiler<comma_tag, xpressive::detail::lst_tag, false>
    {
    };

    template<>
    struct compiler<noop_tag, xpressive::detail::lst_tag, void>
      : xpressive::detail::list_noop_compiler
    {
    };

    template<>
    struct compiler<assign_tag, xpressive::detail::lst_tag, void>
      : xpressive::detail::list_assign_compiler
    {
    };

}}

#endif

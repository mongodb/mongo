///////////////////////////////////////////////////////////////////////////////
// alt_compilers.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_ALT_COMPILERS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_ALT_COMPILERS_HPP_EAN_10_04_2005

#include <boost/version.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/proto/compiler/fold.hpp>
#include <boost/xpressive/proto/compiler/branch.hpp>
#include <boost/xpressive/detail/utility/cons.hpp>
#include <boost/xpressive/detail/utility/dont_care.hpp>
#include <boost/xpressive/detail/static/productions/domain_tags.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // alt_branch
    //   Describes how to construct an alternate xpression
    struct alt_branch
    {
        typedef boost::fusion::nil state_type;

        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef static_xpression
            <
                alternate_matcher<alternates_list<Op>, typename Visitor::traits_type>
              , State
            > type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &)
        {
            typedef alternate_matcher<alternates_list<Op>, typename Visitor::traits_type> alt_matcher;
            return make_static_xpression(alt_matcher(op), state);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // alt_list_branch
    struct alt_list_branch
    {
        typedef alternate_end_xpression state_type;

        template<typename Op, typename State, typename>
        struct apply
        {
            typedef boost::fusion::cons<Op, State> type;
        };

        template<typename Op, typename State>
        static boost::fusion::cons<Op, State>
        call(Op const &op, State const &state, dont_care)
        {
            return boost::fusion::make_cons(op, state);
        }
    };

}}}

namespace boost { namespace proto
{
    // production for alternates in sequence
    template<>
    struct compiler<bitor_tag, xpressive::detail::seq_tag, void>
      : branch_compiler<xpressive::detail::alt_branch, xpressive::detail::alt_tag>
    {
    };

    // handle alternates with the alt branch compiler
    template<typename OpTag>
    struct compiler<OpTag, xpressive::detail::alt_tag, void>
      : branch_compiler<xpressive::detail::alt_list_branch, xpressive::detail::seq_tag>
    {
    };

    // production for alternates in alternate
    template<>
    struct compiler<bitor_tag, xpressive::detail::alt_tag, void>
      : fold_compiler<bitor_tag, xpressive::detail::alt_tag>
    {
    };

}}

#endif

///////////////////////////////////////////////////////////////////////////////
// quant_compilers.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_QUANT_COMPILERS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_QUANT_COMPILERS_HPP_EAN_10_04_2005

#include <limits.h>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/proto/compiler/branch.hpp>
#include <boost/xpressive/proto/compiler/transform.hpp>
#include <boost/xpressive/proto/compiler/conditional.hpp>
#include <boost/xpressive/detail/static/productions/quant_traits.hpp>
#include <boost/xpressive/detail/static/productions/quant_transforms.hpp>
#include <boost/xpressive/detail/static/productions/marker_transform.hpp>
#include <boost/xpressive/detail/static/productions/domain_tags.hpp>

namespace boost { namespace xpressive { namespace detail
{

    typedef proto::unary_op<epsilon_matcher, proto::noop_tag> nil_op;

    ///////////////////////////////////////////////////////////////////////////////
    // generic_quant_compiler
    template<bool Greedy, uint_t Min, uint_t Max>
    struct generic_quant_compiler
      : proto::conditional_compiler
        <
            use_simple_repeat_predicate
          , proto::branch_compiler<simple_repeat_branch<Greedy, Min, Max>, ind_tag>
          , proto::transform_compiler
            <
                proto::compose_transforms
                <
                    proto::arg_transform
                  , repeater_if_transform<Greedy, Min, Max>
                >
              , seq_tag
            >
        >
    {
    };

    // degenerate case, foo{0,0} becomes nil
    template<bool Greedy>
    struct generic_quant_compiler<Greedy, 0, 0>
      : proto::transform_compiler<proto::always_transform<nil_op>, seq_tag>
    {
    };

    // degenerate case, foo{1,1} becomes foo
    template<bool Greedy>
    struct generic_quant_compiler<Greedy, 1, 1>
      : proto::transform_compiler<proto::identity_transform, seq_tag>
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // non_greedy_compiler
    struct non_greedy_compiler
    {
        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef typename proto::arg_type<Op>::type arg_type;

            // Did you apply operator- to something that wasn't a quantifier?
            BOOST_MPL_ASSERT((is_greedy_quant<arg_type>));

            typedef typename proto::tag_type<arg_type>::type tag_type;
            typedef generic_quant_compiler
            <
                false
              , min_type<tag_type>::type::value
              , max_type<tag_type>::type::value
            > compiler_type;

            typedef typename compiler_type::BOOST_NESTED_TEMPLATE apply
            <
                arg_type
              , State
              , Visitor
            >::type type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &visitor)
        {
            typedef typename apply<Op, State, Visitor>::compiler_type compiler_type;
            return compiler_type::call(proto::arg(op), state, visitor);
        }
    };

}}}


namespace boost { namespace proto
{

    // production for one or more quant
    template<>
    struct compiler<unary_plus_tag, xpressive::detail::seq_tag, void>
      : xpressive::detail::generic_quant_compiler<true, 1, UINT_MAX-1>
    {
    };

    // production for zero or more quant
    template<>
    struct compiler<unary_star_tag, xpressive::detail::seq_tag, void>
      : xpressive::detail::generic_quant_compiler<true, 0, UINT_MAX-1>
    {
    };

    // production for optional
    template<>
    struct compiler<logical_not_tag, xpressive::detail::seq_tag, void>
      : xpressive::detail::generic_quant_compiler<true, 0, 1>
    {
    };

    // production for generic quantifiers
    template<unsigned int Min, unsigned int Max>
    struct compiler<xpressive::detail::generic_quant_tag<Min, Max>, xpressive::detail::seq_tag, void>
      : xpressive::detail::generic_quant_compiler<true, Min, Max>
    {
    };

    // production for non-greedy quantifiers
    template<>
    struct compiler<unary_minus_tag, xpressive::detail::seq_tag, void>
      : xpressive::detail::non_greedy_compiler
    {
    };

}}

#endif

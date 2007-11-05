///////////////////////////////////////////////////////////////////////////////
// productions.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_PRODUCTIONS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_PRODUCTIONS_HPP_EAN_10_04_2005

#include <boost/ref.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>

#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/proto/compiler/fold.hpp>
#include <boost/xpressive/proto/compiler/transform.hpp>
#include <boost/xpressive/proto/compiler/conditional.hpp>

#include <boost/xpressive/detail/static/productions/domain_tags.hpp>
#include <boost/xpressive/detail/static/productions/visitor.hpp>
#include <boost/xpressive/detail/static/productions/quant_compilers.hpp>
#include <boost/xpressive/detail/static/productions/alt_compilers.hpp>
#include <boost/xpressive/detail/static/productions/independent_compiler.hpp>
#include <boost/xpressive/detail/static/productions/noop_compiler.hpp>
#include <boost/xpressive/detail/static/productions/modify_compiler.hpp>
#include <boost/xpressive/detail/static/productions/complement_compiler.hpp>
#include <boost/xpressive/detail/static/productions/set_compilers.hpp>
#include <boost/xpressive/detail/static/productions/marker_compiler.hpp>
#include <boost/xpressive/detail/static/productions/charset_transforms.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    //
    struct is_set_initializer_predicate
    {
        template<typename Op, typename, typename>
        struct apply
        {
            typedef typename is_same<typename proto::left_type<Op>::type, set_initializer_type>::type type;
        };
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    struct action_transform
    {
        template<typename Op, typename, typename>
        struct apply
        {
            typedef proto::binary_op
            <
                typename proto::left_type<Op>::type
              , typename proto::right_type<Op>::type
              , proto::right_shift_tag
            > type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &, Visitor &)
        {
            return proto::left(op) >> proto::right(op);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // subscript_compiler
    struct subscript_compiler
      : proto::conditional_compiler
        <
            is_set_initializer_predicate
          , proto::transform_compiler<charset_transform, seq_tag>
          , proto::transform_compiler<action_transform, seq_tag>
        >
    {
    };

}}}


///////////////////////////////////////////////////////////////////////////////
// misc regex compiler productions
namespace boost { namespace proto
{
    template<typename BidiIter>
    struct value_type<xpressive::basic_regex<BidiIter> >
    {
        // store regex objects in the parse tree by reference
        typedef reference_wrapper<xpressive::basic_regex<BidiIter> const> type;
    };

    // production for sequences in sequence
    template<>
    struct compiler<right_shift_tag, xpressive::detail::seq_tag, void>
      : fold_compiler<right_shift_tag, xpressive::detail::seq_tag>
    {
    };

    template<>
    struct compiler<subscript_tag, xpressive::detail::seq_tag, void>
      : xpressive::detail::subscript_compiler
    {
    };
}}

#endif

////////////////////////////////////////////////////////////////////////////
// complement_compiler.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_COMPLEMENT_COMPILER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_COMPLEMENT_COMPILER_HPP_EAN_10_04_2005

#include <boost/mpl/assert.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/proto/compiler/transform.hpp>
#include <boost/xpressive/detail/utility/dont_care.hpp>
#include <boost/xpressive/detail/utility/never_true.hpp>
#include <boost/xpressive/detail/static/productions/set_compilers.hpp>
#include <boost/xpressive/detail/static/productions/domain_tags.hpp>
#include <boost/xpressive/detail/static/productions/charset_transforms.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // complement
    //   the result of applying operator~ to various expressions
    template<typename Op, typename Visitor>
    struct complement
    {
        // If your compile breaks here, then you are applying the complement operator ~
        // to something that does not support it. For instance, ~(_ >> 'a') will trigger this
        // assertion because the sub-expression (_ >> 'a') has no complement.
        BOOST_MPL_ASSERT((never_true<Op>));
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Char, bool Not, typename Visitor>
    struct complement<proto::unary_op<literal_placeholder<Char, Not>, proto::noop_tag>, Visitor>
    {
        typedef proto::unary_op<literal_placeholder<Char, !Not>, proto::noop_tag> type;

        static type const call(proto::unary_op<literal_placeholder<Char, Not>, proto::noop_tag> const &op, Visitor &)
        {
            literal_placeholder<Char, !Not> literal = proto::arg(op).ch_;
            return proto::noop(literal);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Char, typename Visitor>
    struct complement<proto::unary_op<Char, proto::noop_tag>, Visitor>
    {
        typedef proto::unary_op<literal_placeholder<Char, true>, proto::noop_tag> type;

        static type const call(proto::unary_op<Char, proto::noop_tag> const &op, Visitor &)
        {
            literal_placeholder<Char, true> literal = proto::arg(op);
            return proto::noop(literal);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Traits, int Size, typename Visitor>
    struct complement<proto::unary_op<set_matcher<Traits, Size>, proto::noop_tag>, Visitor>
    {
        typedef proto::unary_op<set_matcher<Traits, Size>, proto::noop_tag> type;

        static type const call(proto::unary_op<set_matcher<Traits, Size>, proto::noop_tag> const &op, Visitor &)
        {
            set_matcher<Traits, Size> set = proto::arg(op);
            set.complement();
            return proto::noop(set);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Visitor>
    struct complement<proto::unary_op<posix_charset_placeholder, proto::noop_tag>, Visitor>
    {
        typedef proto::unary_op<posix_charset_placeholder, proto::noop_tag> type;

        static type const call(proto::unary_op<posix_charset_placeholder, proto::noop_tag> const &op, Visitor &)
        {
            posix_charset_placeholder posix = proto::arg(op);
            posix.not_ = !posix.not_;
            return proto::noop(posix);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Op, typename Visitor>
    struct complement<proto::binary_op<set_initializer_type, Op, proto::subscript_tag>, Visitor>
    {
        typedef typename charset_transform::BOOST_NESTED_TEMPLATE apply
        <
            proto::binary_op<set_initializer_type, Op, proto::subscript_tag>
          , dont_care
          , Visitor
        >::type type;

        static type call(proto::binary_op<set_initializer_type, Op, proto::subscript_tag> const &op, Visitor &visitor)
        {
            return charset_transform::call(op, dont_care(), visitor, true);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // for complementing a list-initialized set, as in ~(set= 'a','b','c')
    template<typename Left, typename Right, typename Visitor>
    struct complement<proto::binary_op<Left, Right, proto::comma_tag>, Visitor>
    {
        // First, convert the parse tree into a set_matcher
        typedef typename proto::compiler<proto::comma_tag, lst_tag>::BOOST_NESTED_TEMPLATE apply
        <
            proto::binary_op<Left, Right, proto::comma_tag>
          , dont_care
          , Visitor
        >::type set_matcher;

        typedef proto::unary_op<set_matcher, proto::noop_tag> type;

        static type const call(proto::binary_op<Left, Right, proto::comma_tag> const &op, Visitor &visitor)
        {
            set_matcher set(proto::compile(op, dont_care(), visitor, lst_tag()));
            set.complement();
            return proto::noop(set);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Op, typename Visitor>
    struct complement<proto::unary_op<Op, lookahead_tag<true> >, Visitor>
    {
        typedef proto::unary_op<Op, lookahead_tag<false> > type;

        static type call(proto::unary_op<Op, lookahead_tag<true> > const &op, Visitor &)
        {
            return proto::make_op<lookahead_tag<false> >(proto::arg(op));
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Op, typename Visitor>
    struct complement<proto::unary_op<Op, lookbehind_tag<true> >, Visitor>
    {
        typedef proto::unary_op<Op, lookbehind_tag<false> > type;

        static type call(proto::unary_op<Op, lookbehind_tag<true> > const &op, Visitor &)
        {
            return proto::make_op<lookbehind_tag<false> >(proto::arg(op));
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Visitor>
    struct complement<proto::unary_op<assert_word_placeholder<word_boundary<true> >, proto::noop_tag>, Visitor>
    {
        typedef proto::unary_op<assert_word_placeholder<word_boundary<false> >, proto::noop_tag> type;

        static type call(proto::unary_op<assert_word_placeholder<word_boundary<true> >, proto::noop_tag> const &, Visitor &)
        {
            return proto::noop(assert_word_placeholder<word_boundary<false> >());
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Visitor>
    struct complement<logical_newline_xpression, Visitor>
    {
        typedef proto::binary_op
        <
            proto::unary_op<logical_newline_xpression, lookahead_tag<false> >
          , proto::unary_op<any_matcher, proto::noop_tag>
          , proto::right_shift_tag
        > type;

        static type call(logical_newline_xpression const &op, Visitor &)
        {
            return proto::make_op<lookahead_tag<false> >(op) >> proto::noop(any_matcher());
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // complementing a complement is a no-op
    template<typename Arg, typename Visitor>
    struct complement<proto::unary_op<Arg, proto::complement_tag>, Visitor>
    {
        typedef Arg type;

        static Arg const &call(proto::unary_op<Arg, proto::complement_tag> const &op, Visitor &)
        {
            return proto::arg(op);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    //
    template<typename Char, typename Visitor>
    struct complement<proto::unary_op<range_placeholder<Char>, proto::noop_tag>, Visitor>
    {
        typedef proto::unary_op<range_placeholder<Char>, proto::noop_tag> type;

        static type const call(proto::unary_op<range_placeholder<Char>, proto::noop_tag> const &op, Visitor &)
        {
            range_placeholder<Char> rng = proto::arg(op);
            rng.not_ = !rng.not_;
            return proto::noop(rng);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // complement_transform
    struct complement_transform
    {
        template<typename Op, typename, typename Visitor>
        struct apply
        {
            typedef typename complement<typename proto::arg_type<Op>::type, Visitor>::type type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename complement<typename proto::arg_type<Op>::type, Visitor>::type
        call(Op const &op, State const &, Visitor &visitor)
        {
            return complement<typename proto::arg_type<Op>::type, Visitor>::call(proto::arg(op), visitor);
        }
    };

}}}

namespace boost { namespace proto
{

    template<>
    struct compiler<complement_tag, xpressive::detail::seq_tag, void>
      : transform_compiler<xpressive::detail::complement_transform, xpressive::detail::seq_tag>
    {
    };

}}

#endif

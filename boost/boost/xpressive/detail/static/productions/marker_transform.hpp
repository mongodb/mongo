///////////////////////////////////////////////////////////////////////////////
// marker_transform.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_MARKER_TRANSFORM_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_MARKER_TRANSFORM_HPP_EAN_10_04_2005

#include <boost/mpl/bool.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/proto/compiler/transform.hpp>

namespace boost { namespace xpressive { namespace detail
{
    ///////////////////////////////////////////////////////////////////////////////
    // is_marker
    template<typename Op>
    struct is_marker
      : mpl::false_
    {};

    // (s1= ...) is a marker
    template<typename Op>
    struct is_marker<proto::binary_op<mark_tag, Op, proto::assign_tag> >
      : mpl::true_
    {};

    ///////////////////////////////////////////////////////////////////////////////
    // is_marker_predicate
    struct is_marker_predicate
    {
        template<typename Op, typename, typename>
        struct apply
          : is_marker<Op>
        {
        };
    };

    ///////////////////////////////////////////////////////////////////////////////
    // marker_transform
    //   Insert mark tags before and after the expression
    struct marker_transform
    {
        template<typename Op, typename, typename>
        struct apply
        {
            typedef proto::binary_op
            <
                proto::unary_op<mark_begin_matcher, proto::noop_tag>
              , proto::binary_op
                <
                    Op
                  , proto::unary_op<mark_end_matcher, proto::noop_tag>
                  , proto::right_shift_tag
                >
              , proto::right_shift_tag
            > type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &, Visitor &visitor, int mark_nbr = 0)
        {
            // if we're inserting a mark, and we're not being told the mark number,
            // we're inserting a hidden mark ... so grab the next hidden mark number.
            if(0 == mark_nbr)
            {
                mark_nbr = visitor.get_hidden_mark();
            }

            return proto::noop(mark_begin_matcher(mark_nbr))
                >> (op >> proto::noop(mark_end_matcher(mark_nbr)));
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // marker_assign_transform
    struct marker_assign_transform
      : proto::compose_transforms<proto::right_transform, marker_transform>
    {
        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &visitor)
        {
            return marker_transform::call(proto::right(op), state, visitor, proto::arg(proto::left(op)).mark_number_);
        }
    };

}}}

#endif

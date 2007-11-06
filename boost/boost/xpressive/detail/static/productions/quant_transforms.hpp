///////////////////////////////////////////////////////////////////////////////
// quant_transforms.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_QUANT_TRANSFORMS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_QUANT_TRANSFORMS_HPP_EAN_10_04_2005

#include <boost/mpl/size_t.hpp>
#include <boost/mpl/assert.hpp>
#include <boost/mpl/not_equal_to.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/static/width_of.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/detail/static/productions/quant_traits.hpp>
#include <boost/xpressive/detail/static/productions/marker_transform.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // simple_repeat_branch
    template<bool Greedy, uint_t Min, uint_t Max>
    struct simple_repeat_branch
    {
        typedef true_xpression state_type;

        template<typename Op, typename State, typename>
        struct apply
        {
            typedef static_xpression<simple_repeat_matcher<Op, Greedy>, State> type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &)
        {
            return make_static_xpression(simple_repeat_matcher<Op, Greedy>(op, Min, Max), state);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // repeater_transform
    template<bool Greedy, uint_t Min, uint_t Max>
    struct repeater_transform
    {
        template<typename Op, typename, typename>
        struct apply
        {
            typedef proto::binary_op
            <
                proto::unary_op<repeat_begin_matcher, proto::noop_tag>
              , proto::binary_op
                <
                    Op
                  , proto::unary_op<repeat_end_matcher<Greedy>, proto::noop_tag>
                  , proto::right_shift_tag
                >
              , proto::right_shift_tag
            > type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &, Visitor &)
        {
            // Get the mark_number from the begin_mark_matcher
            int mark_number = proto::arg(proto::left(op)).mark_number_;
            BOOST_ASSERT(0 != mark_number);

            return proto::noop(repeat_begin_matcher(mark_number))
                >> (op >> proto::noop(repeat_end_matcher<Greedy>(mark_number, Min, Max)));
        }
    };

    template<typename Op>
    epsilon_mark_matcher make_eps(Op const &op, epsilon_mark_matcher *)
    {
        return epsilon_mark_matcher(proto::arg(proto::left(op)).mark_number_);
    }

    template<typename Op>
    epsilon_matcher make_eps(Op const &op, epsilon_matcher *)
    {
        return epsilon_matcher();
    }

    ///////////////////////////////////////////////////////////////////////////////
    // optional_transform
    //   make alternate with epsilon_mark_matcher
    template<bool Greedy, typename Epsilon>
    struct optional_transform
    {
        template<typename Op, typename, typename>
        struct apply
        {
            typedef proto::binary_op
            <
                Op
              , proto::unary_op<Epsilon, proto::noop_tag>
              , proto::bitor_tag
            > type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &, Visitor &)
        {
            return op | proto::noop(make_eps(op, (Epsilon *)0));
        }
    };

    template<typename Epsilon>
    struct optional_transform<false, Epsilon>
    {
        template<typename Op, typename, typename>
        struct apply
        {
            typedef proto::binary_op
            <
                proto::unary_op<Epsilon, proto::noop_tag>
              , Op
              , proto::bitor_tag
            > type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &, Visitor &)
        {
            return proto::noop(make_eps(op, (Epsilon *)0)) | op;
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // marker_if_transform
    //   Insert marker matchers before and after the expression
    typedef proto::conditional_transform<
        is_marker_predicate
      , marker_assign_transform
      , marker_transform
    > marker_if_transform;

    ///////////////////////////////////////////////////////////////////////////////
    // repeater_if_transform
    //   Insert repeat and marker matcher before and after the expression
    template<bool Greedy, uint_t Min, uint_t Max>
    struct repeater_if_transform
      : proto::compose_transforms
        <
            marker_if_transform
          , repeater_transform<Greedy, Min, Max>
        >
    {
    };

    // transform *foo to (+foo | nil)
    template<bool Greedy, uint_t Max>
    struct repeater_if_transform<Greedy, 0, Max>
      : proto::compose_transforms
        <
            repeater_if_transform<Greedy, 1, Max>
          , optional_transform<Greedy, epsilon_mark_matcher>
        >
    {
    };

    // transform !(foo) to (foo | nil), with care to make sure
    // that !(s1= foo) sets s1 to null if foo doesn't match.
    template<bool Greedy>
    struct repeater_if_transform<Greedy, 0, 1>
      : proto::conditional_transform
        <
            is_marker_predicate
          , optional_transform<Greedy, epsilon_mark_matcher>
          , optional_transform<Greedy, epsilon_matcher>
        >
    {
    };

}}}

#endif

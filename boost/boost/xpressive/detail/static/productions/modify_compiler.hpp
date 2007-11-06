///////////////////////////////////////////////////////////////////////////////
// modify_compiler.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_MODIFY_COMPILER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_MODIFY_COMPILER_HPP_EAN_10_04_2005

#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/detail/utility/ignore_unused.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // regex operator tags
    struct modifier_tag
      : proto::binary_tag
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // scoped_swap
    //  for swapping state back after proto::compile returns
    template<typename Old, typename New>
    struct scoped_swap
    {
        ~scoped_swap() { this->old_->swap(*this->new_); }
        Old *old_;
        New *new_;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // modify_compiler
    struct modify_compiler
    {
        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef typename proto::left_type<Op>::type modifier_type;
            typedef typename modifier_type::BOOST_NESTED_TEMPLATE apply<Visitor>::type visitor_type;
            typedef typename proto::right_type<Op>::type op_type;

            typedef typename proto::compiler<typename proto::tag_type<op_type>::type, seq_tag>::
                BOOST_NESTED_TEMPLATE apply
            <
                op_type
              , State
              , visitor_type
            >::type type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &visitor)
        {
            typedef typename apply<Op, State, Visitor>::visitor_type new_visitor_type;
            new_visitor_type new_visitor(proto::left(op).call(visitor));
            new_visitor.swap(visitor);
            scoped_swap<Visitor, new_visitor_type> const undo = {&visitor, &new_visitor};
            detail::ignore_unused(&undo);
            return proto::compile(proto::right(op), state, new_visitor, seq_tag());
        }
    };

}}}

namespace boost { namespace proto
{

    // production for modifiers
    template<>
    struct compiler<xpressive::detail::modifier_tag, xpressive::detail::seq_tag, void>
      : xpressive::detail::modify_compiler
    {
    };

}}

#endif

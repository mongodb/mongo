///////////////////////////////////////////////////////////////////////////////
/// \file fold.hpp
/// A special-purpose proto compiler for merging sequences of binary operations.
/// It compiles the right operand and passes the result as state while compiling
/// the left. Or, it might do the left first, if you choose.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROTO_COMPILER_FOLD_HPP_EAN_04_01_2005
#define BOOST_PROTO_COMPILER_FOLD_HPP_EAN_04_01_2005

#include <boost/xpressive/proto/proto_fwd.hpp>

namespace boost { namespace proto
{

    ///////////////////////////////////////////////////////////////////////////////
    // fold_compiler
    //  Compiles the right side and passes the result as state while compiling the left.
    //  This is useful for serializing a tree.
    template<typename OpTag, typename DomainTag, bool RightFirst>
    struct fold_compiler
    {
        // sample compiler implementation for sequencing
        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef typename right_type<Op>::type right_type;
            typedef typename left_type<Op>::type left_type;

            // compile the right branch
            typedef typename compiler<typename tag_type<right_type>::type, DomainTag>::
                BOOST_NESTED_TEMPLATE apply
            <
                right_type
              , State
              , Visitor
            >::type right_compiled_type;

            // forward the result of the right branch to the left
            typedef typename compiler<typename tag_type<left_type>::type, DomainTag>::
                BOOST_NESTED_TEMPLATE apply
            <
                left_type
              , right_compiled_type
              , Visitor
            >::type type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &visitor)
        {
            return proto::compile(
                proto::left(op)
              , proto::compile(proto::right(op), state, visitor, DomainTag())
              , visitor
              , DomainTag()
            );
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // fold_compiler
    //  Compiles the left side and passes the result as state while compiling the right.
    //  This is useful for serializing a tree.
    template<typename OpTag, typename DomainTag>
    struct fold_compiler<OpTag, DomainTag, false>
    {
        // sample compiler implementation for sequencing
        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef typename right_type<Op>::type right_type;
            typedef typename left_type<Op>::type left_type;

            // compile the right branch
            typedef typename compiler<typename tag_type<left_type>::type, DomainTag>::
                BOOST_NESTED_TEMPLATE apply
            <
                left_type
              , State
              , Visitor
            >::type left_compiled_type;

            // forward the result of the right branch to the left
            typedef typename compiler<typename tag_type<right_type>::type, DomainTag>::
                BOOST_NESTED_TEMPLATE apply
            <
                right_type
              , left_compiled_type
              , Visitor
            >::type type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &visitor)
        {
            return proto::compile(
                proto::right(op)
              , proto::compile(proto::left(op), state, visitor, DomainTag())
              , visitor
              , DomainTag()
            );
        }
    };

}}

#endif

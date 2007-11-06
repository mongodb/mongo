///////////////////////////////////////////////////////////////////////////////
/// \file conditional.hpp
/// A special-purpose proto compiler for compiling an expression either one
/// way or another depending on the properties of the expression.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROTO_COMPILER_CONDITIONAL_HPP_EAN_04_01_2005
#define BOOST_PROTO_COMPILER_CONDITIONAL_HPP_EAN_04_01_2005

#include <boost/mpl/if.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/xpressive/proto/proto_fwd.hpp>

namespace boost { namespace proto
{

    ///////////////////////////////////////////////////////////////////////////////
    // conditional_compiler
    template<typename Predicate, typename IfCompiler, typename ElseCompiler>
    struct conditional_compiler
    {
        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef typename boost::mpl::if_
            <
                typename Predicate::BOOST_NESTED_TEMPLATE apply<Op, State, Visitor>::type
              , IfCompiler
              , ElseCompiler
            >::type compiler_type;

            typedef typename compiler_type::BOOST_NESTED_TEMPLATE apply
            <
                Op
              , State
              , Visitor
            >::type type;
        };

        template<typename Op, typename State, typename Visitor>
        static typename apply<Op, State, Visitor>::type
        call(Op const &op, State const &state, Visitor &visitor)
        {
            typedef typename apply<Op, State, Visitor>::compiler_type compiler_type;
            return compiler_type::call(op, state, visitor);
        }
    };

}}

#endif

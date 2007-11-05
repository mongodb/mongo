///////////////////////////////////////////////////////////////////////////////
/// \file switch.hpp
/// A generalization of the conditional_compiler. Given N different compilers
/// in a MPL-style map and a lambda that generates a key from an expression,
/// find the compiler in the map corresponding to the key and use that compiler
/// to compile the expression.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROTO_COMPILER_SWITCH_HPP_EAN_04_01_2005
#define BOOST_PROTO_COMPILER_SWITCH_HPP_EAN_04_01_2005

#include <boost/mpl/at.hpp>
#include <boost/xpressive/proto/proto_fwd.hpp>

namespace boost { namespace proto
{

    ///////////////////////////////////////////////////////////////////////////////
    // switch_compiler
    //  applies a transform, then looks up the appropriate compiler in a map
    template<typename Lambda, typename Map>
    struct switch_compiler
    {
        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef typename boost::mpl::at
            <
                Map
              , typename Lambda::BOOST_NESTED_TEMPLATE apply<Op, State, Visitor>::type
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

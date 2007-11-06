///////////////////////////////////////////////////////////////////////////////
/// \file error.hpp
/// A special-purpose proto compiler that simply generates an error. Useful for
/// flagging certain constructs as illegal.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROTO_COMPILER_ERROR_HPP_EAN_04_01_2005
#define BOOST_PROTO_COMPILER_ERROR_HPP_EAN_04_01_2005

#include <boost/xpressive/proto/proto_fwd.hpp>

namespace boost { namespace proto
{

    ///////////////////////////////////////////////////////////////////////////////
    // error_compiler
    struct error_compiler
    {
        template<typename Op, typename State, typename Visitor>
        struct apply
        {
            typedef void type;
        };
    };

}}

#endif

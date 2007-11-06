///////////////////////////////////////////////////////////////////////////////
/// \file arg_traits.hpp
/// Contains definitions for value_type\<\>, arg_type\<\>, left_type\<\>,
/// right_type\<\>, tag_type\<\>, and the helper functions arg(), left(),
/// and right().
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROTO_ARG_TRAITS_HPP_EAN_04_01_2005
#define BOOST_PROTO_ARG_TRAITS_HPP_EAN_04_01_2005

#include <boost/call_traits.hpp>
#include <boost/xpressive/proto/proto_fwd.hpp>

namespace boost { namespace proto
{

    ///////////////////////////////////////////////////////////////////////////////
    // value_type
    //  specialize this to control how user-defined types are stored in the parse tree
    template<typename T>
    struct value_type
    {
        typedef typename boost::call_traits<T>::value_type type;
    };

    template<>
    struct value_type<fusion::void_t>
    {
        typedef fusion::void_t type;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // argument type extractors
    template<typename Op>
    struct arg_type
    {
        typedef typename Op::arg_type type;
        typedef type const &const_reference;
    };

    template<typename Op, typename Param>
    struct arg_type<op_proxy<Op, Param> >
    {
        typedef typename Op::arg_type type;
        typedef type const const_reference;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // argument type extractors
    template<typename Op>
    struct left_type
    {
        typedef typename Op::left_type type;
        typedef type const &const_reference;
    };

    template<typename Op, typename Param>
    struct left_type<op_proxy<Op, Param> >
    {
        typedef typename Op::left_type type;
        typedef type const const_reference;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // argument type extractors
    template<typename Op>
    struct right_type
    {
        typedef typename Op::right_type type;
        typedef type const &const_reference;
    };

    template<typename Op, typename Param>
    struct right_type<op_proxy<Op, Param> >
    {
        typedef typename Op::right_type type;
        typedef type const const_reference;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // tag extractor
    template<typename Op>
    struct tag_type
    {
        typedef typename Op::tag_type type;
    };

    template<typename Op, typename Param>
    struct tag_type<op_proxy<Op, Param> >
    {
        typedef typename Op::tag_type type;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // arg
    template<typename Op>
    inline typename arg_type<Op>::const_reference arg(Op const &op)
    {
        return op.cast().arg;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // left
    template<typename Op>
    inline typename left_type<Op>::const_reference left(Op const &op)
    {
        return op.cast().left;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // right
    template<typename Op>
    inline typename right_type<Op>::const_reference right(Op const &op)
    {
        return op.cast().right;
    }

}}

#endif

///////////////////////////////////////////////////////////////////////////////
/// \file operators.hpp
/// Contains all the overloaded operators that make it possible to build
/// expression templates using proto components
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROTO_OPERATORS_HPP_EAN_04_01_2005
#define BOOST_PROTO_OPERATORS_HPP_EAN_04_01_2005

#include <boost/mpl/or.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/preprocessor/punctuation/comma.hpp>
#include <boost/xpressive/proto/proto_fwd.hpp>
#include <boost/xpressive/proto/op_tags.hpp>
#include <boost/xpressive/proto/op_base.hpp>

namespace boost { namespace proto
{
    ///////////////////////////////////////////////////////////////////////////////
    // unary_op_generator
    template<typename Arg, typename Tag>
    struct unary_op_generator
    {
        typedef unary_op<
            typename as_op<Arg>::type
          , Tag
        > type;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // binary_op_generator
    template<typename Left, typename Right, typename Tag>
    struct binary_op_generator
    {
        typedef binary_op<
            typename as_op<Left>::type
          , typename as_op<Right>::type
          , Tag
        > type;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // unary operators
    template<typename Arg>
    unary_op<Arg, noop_tag> const
    noop(Arg const &arg)
    {
        return make_op<noop_tag>(arg);
    }

#define BOOST_PROTO_UNARY_OP(op, tag)                                                           \
    template<typename Arg>                                                                      \
    inline typename lazy_enable_if<is_op<Arg>, unary_op_generator<Arg, tag> >::type const       \
    operator op(Arg const &arg)                                                                 \
    {                                                                                           \
        return make_op<tag>(as_op<Arg>::make(arg));                                             \
    }

#define BOOST_PROTO_BINARY_OP(op, tag)                                                          \
    template<typename Left, typename Right>                                                     \
    inline typename lazy_enable_if<                                                             \
        mpl::or_<is_op<Left>, is_op<Right> >                                                    \
      , binary_op_generator<Left, Right, tag>                                                   \
    >::type const                                                                               \
    operator op(Left const &left, Right const &right)                                           \
    {                                                                                           \
        return make_op<tag>(as_op<Left>::make(left), as_op<Right>::make(right));                \
    }

    BOOST_PROTO_UNARY_OP(+, unary_plus_tag)
    BOOST_PROTO_UNARY_OP(-, unary_minus_tag)
    BOOST_PROTO_UNARY_OP(*, unary_star_tag)
    BOOST_PROTO_UNARY_OP(~, complement_tag)
    BOOST_PROTO_UNARY_OP(&, address_of_tag)
    BOOST_PROTO_UNARY_OP(!, logical_not_tag)
    BOOST_PROTO_UNARY_OP(++, pre_inc_tag)
    BOOST_PROTO_UNARY_OP(--, pre_dec_tag)

    BOOST_PROTO_BINARY_OP(<<, left_shift_tag)
    BOOST_PROTO_BINARY_OP(>>, right_shift_tag)
    BOOST_PROTO_BINARY_OP(*, multiply_tag)
    BOOST_PROTO_BINARY_OP(/, divide_tag)
    BOOST_PROTO_BINARY_OP(%, modulus_tag)
    BOOST_PROTO_BINARY_OP(+, add_tag)
    BOOST_PROTO_BINARY_OP(-, subtract_tag)
    BOOST_PROTO_BINARY_OP(<, less_tag)
    BOOST_PROTO_BINARY_OP(>, greater_tag)
    BOOST_PROTO_BINARY_OP(<=, less_equal_tag)
    BOOST_PROTO_BINARY_OP(>=, greater_equal_tag)
    BOOST_PROTO_BINARY_OP(==, equal_tag)
    BOOST_PROTO_BINARY_OP(!=, not_equal_tag)
    BOOST_PROTO_BINARY_OP(||, logical_or_tag)
    BOOST_PROTO_BINARY_OP(&&, logical_and_tag)
    BOOST_PROTO_BINARY_OP(&, bitand_tag)
    BOOST_PROTO_BINARY_OP(|, bitor_tag)
    BOOST_PROTO_BINARY_OP(^, bitxor_tag)
    BOOST_PROTO_BINARY_OP(BOOST_PP_COMMA(), comma_tag)
    BOOST_PROTO_BINARY_OP(->*, mem_ptr_tag)

    BOOST_PROTO_BINARY_OP(<<=, left_shift_assign_tag)
    BOOST_PROTO_BINARY_OP(>>=, right_shift_assign_tag)
    BOOST_PROTO_BINARY_OP(*=, multiply_assign_tag)
    BOOST_PROTO_BINARY_OP(/=, divide_assign_tag)
    BOOST_PROTO_BINARY_OP(%=, modulus_assign_tag)
    BOOST_PROTO_BINARY_OP(+=, add_assign_tag)
    BOOST_PROTO_BINARY_OP(-=, subtract_assign_tag)
    BOOST_PROTO_BINARY_OP(&=, bitand_assign_tag)
    BOOST_PROTO_BINARY_OP(|=, bitor_assign_tag)
    BOOST_PROTO_BINARY_OP(^=, bitxor_assign_tag)

#undef BOOST_PROTO_BINARY_OP
#undef BOOST_PROTO_UNARY_OP

    ///////////////////////////////////////////////////////////////////////////////
    // post-fix operators
    template<typename Arg>
    inline typename lazy_enable_if<is_op<Arg>, unary_op_generator<Arg, post_inc_tag> >::type const
    operator ++(Arg const &arg, int)
    {
        return make_op<post_inc_tag>(arg.cast());
    }

    template<typename Arg>
    inline typename lazy_enable_if<is_op<Arg>, unary_op_generator<Arg, post_dec_tag> >::type const
    operator --(Arg const &arg, int)
    {
        return make_op<post_dec_tag>(arg.cast());
    }

}}

#endif

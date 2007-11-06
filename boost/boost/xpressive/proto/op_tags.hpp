///////////////////////////////////////////////////////////////////////////////
/// \file op_tags.hpp
/// Contains the tags for all the overloadable operators in C++, as well as
/// the base tags unary_tag, binary_tag and nary_tag, as well as the is_unary\<\>,
/// is_binary\<\> and is_nary\<\> predicates.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROTO_OP_TAGS_HPP_EAN_04_01_2005
#define BOOST_PROTO_OP_TAGS_HPP_EAN_04_01_2005

#include <boost/type_traits/is_base_and_derived.hpp>
#include <boost/xpressive/proto/proto_fwd.hpp>

namespace boost { namespace proto
{

    ///////////////////////////////////////////////////////////////////////////////
    // Operator tags
    struct unary_tag {};
    struct binary_tag {};
    struct nary_tag {}; // for operator()

    struct noop_tag : unary_tag {};
    struct unary_plus_tag : unary_tag {};
    struct unary_minus_tag : unary_tag {};
    struct unary_star_tag : unary_tag {};
    struct complement_tag : unary_tag {};
    struct address_of_tag : unary_tag {};
    struct logical_not_tag : unary_tag {};
    struct pre_inc_tag : unary_tag {};
    struct pre_dec_tag : unary_tag {};
    struct post_inc_tag : unary_tag {};
    struct post_dec_tag : unary_tag {};
    
    struct left_shift_tag : binary_tag {};
    struct right_shift_tag : binary_tag {};
    struct multiply_tag : binary_tag {};
    struct divide_tag : binary_tag {};
    struct modulus_tag : binary_tag {};
    struct add_tag : binary_tag {};
    struct subtract_tag : binary_tag {};
    struct less_tag : binary_tag {};
    struct greater_tag : binary_tag {};
    struct less_equal_tag : binary_tag {};
    struct greater_equal_tag : binary_tag {};
    struct equal_tag : binary_tag {};
    struct not_equal_tag : binary_tag {};
    struct logical_or_tag : binary_tag {};
    struct logical_and_tag : binary_tag {};
    struct bitand_tag : binary_tag {};
    struct bitor_tag : binary_tag {};
    struct bitxor_tag : binary_tag {};
    struct comma_tag : binary_tag {};
    struct mem_ptr_tag : binary_tag {};

    struct assign_tag : binary_tag {};
    struct left_shift_assign_tag : binary_tag {};
    struct right_shift_assign_tag : binary_tag {};
    struct multiply_assign_tag : binary_tag {};
    struct divide_assign_tag : binary_tag {};
    struct modulus_assign_tag : binary_tag {};
    struct add_assign_tag : binary_tag {};
    struct subtract_assign_tag : binary_tag {};
    struct bitand_assign_tag : binary_tag {};
    struct bitor_assign_tag : binary_tag {};
    struct bitxor_assign_tag : binary_tag {};
    struct subscript_tag : binary_tag {};

    struct function_tag : nary_tag {};

    ///////////////////////////////////////////////////////////////////////////////
    // is_unary
    template<typename Tag>
    struct is_unary
      : boost::is_base_and_derived<unary_tag, Tag>
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // is_binary
    template<typename Tag>
    struct is_binary
      : boost::is_base_and_derived<binary_tag, Tag>
    {
    };

    ///////////////////////////////////////////////////////////////////////////////
    // is_nary
    template<typename Tag>
    struct is_nary
      : boost::is_base_and_derived<nary_tag, Tag>
    {
    };

}}

#endif

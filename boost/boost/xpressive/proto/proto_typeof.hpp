///////////////////////////////////////////////////////////////////////////////
/// \file proto_typeof.hpp
/// Type registrations so that proto expression templates can be used together
/// with the Boost.Typeof library.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_PROTO_PROTO_TYPEOF_H
#define BOOST_XPRESSIVE_PROTO_PROTO_TYPEOF_H

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp>
#include <boost/typeof/typeof.hpp>
#include <boost/xpressive/proto/proto_fwd.hpp>

#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()

BOOST_TYPEOF_REGISTER_TYPE(boost::proto::unary_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::binary_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::nary_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::noop_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::unary_plus_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::unary_minus_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::unary_star_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::complement_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::address_of_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::logical_not_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::pre_inc_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::pre_dec_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::post_inc_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::post_dec_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::left_shift_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::right_shift_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::multiply_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::divide_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::modulus_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::add_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::subtract_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::less_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::greater_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::less_equal_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::greater_equal_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::equal_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::not_equal_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::logical_or_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::logical_and_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::bitand_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::bitor_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::bitxor_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::comma_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::mem_ptr_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::left_shift_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::right_shift_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::multiply_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::divide_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::modulus_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::add_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::subtract_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::bitand_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::bitor_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::bitxor_assign_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::subscript_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::proto::function_tag)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::proto::unary_op, (typename)(typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::proto::binary_op, (typename)(typename)(typename))

#endif

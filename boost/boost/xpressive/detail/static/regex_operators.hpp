///////////////////////////////////////////////////////////////////////////////
// regex_operators.hpp
//
//  Copyright 2005 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_REGEX_OPERATORS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_REGEX_OPERATORS_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/utility/enable_if.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/basic_regex.hpp>

namespace boost { namespace xpressive
{

///////////////////////////////////////////////////////////////////////////////
// operator +
template<typename BidiIter>
inline proto::unary_op
<
    proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
  , proto::unary_plus_tag
>
operator +(basic_regex<BidiIter> const &regex)
{
    return +proto::noop(regex);
}

///////////////////////////////////////////////////////////////////////////////
// operator *
template<typename BidiIter>
inline proto::unary_op
<
    proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
  , proto::unary_star_tag
>
operator *(basic_regex<BidiIter> const &regex)
{
    return *proto::noop(regex);
}

///////////////////////////////////////////////////////////////////////////////
// operator !
template<typename BidiIter>
inline proto::unary_op
<
    proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
  , proto::logical_not_tag
>
operator !(basic_regex<BidiIter> const &regex)
{
    return !proto::noop(regex);
}

///////////////////////////////////////////////////////////////////////////////
// operator >>
template<typename Right, typename BidiIter>
inline typename disable_if
<
    proto::is_op<Right>
  , proto::binary_op
    <
        proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
      , proto::unary_op<Right, proto::noop_tag>
      , proto::right_shift_tag
    >
>::type
operator >>(basic_regex<BidiIter> const &regex, Right const &right)
{
    return proto::noop(regex) >> proto::noop(right);
}

///////////////////////////////////////////////////////////////////////////////
// operator >>
template<typename BidiIter, typename Left>
inline typename disable_if
<
    proto::is_op<Left>
  , proto::binary_op
    <
        proto::unary_op<Left, proto::noop_tag>
      , proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
      , proto::right_shift_tag
    >
>::type
operator >>(Left const &left, basic_regex<BidiIter> const &regex)
{
    return proto::noop(left) >> proto::noop(regex);
}

///////////////////////////////////////////////////////////////////////////////
// operator >>
template<typename BidiIter>
inline proto::binary_op
<
    proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
  , proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
  , proto::right_shift_tag
>
operator >>(basic_regex<BidiIter> const &left, basic_regex<BidiIter> const &right)
{
    return proto::noop(left) >> proto::noop(right);
}

///////////////////////////////////////////////////////////////////////////////
// operator |
template<typename Right, typename BidiIter>
inline typename disable_if
<
    proto::is_op<Right>
  , proto::binary_op
    <
        proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
      , proto::unary_op<Right, proto::noop_tag>
      , proto::bitor_tag
    >
>::type
operator |(basic_regex<BidiIter> const &regex, Right const &right)
{
    return proto::noop(regex) | proto::noop(right);
}

///////////////////////////////////////////////////////////////////////////////
// operator |
template<typename BidiIter, typename Left>
inline typename disable_if
<
    proto::is_op<Left>
  , proto::binary_op
    <
        proto::unary_op<Left, proto::noop_tag>
      , proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
      , proto::bitor_tag
    >
>::type
operator |(Left const &left, basic_regex<BidiIter> const &regex)
{
    return proto::noop(left) | proto::noop(regex);
}

///////////////////////////////////////////////////////////////////////////////
// operator |
template<typename BidiIter>
inline proto::binary_op
<
    proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
  , proto::unary_op<basic_regex<BidiIter>, proto::noop_tag>
  , proto::bitor_tag
>
operator |(basic_regex<BidiIter> const &left, basic_regex<BidiIter> const &right)
{
    return proto::noop(left) | proto::noop(right);
}

}}

#endif

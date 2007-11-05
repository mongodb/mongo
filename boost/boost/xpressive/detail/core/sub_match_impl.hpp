///////////////////////////////////////////////////////////////////////////////
// sub_match_impl.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_SUB_MATCH_IMPL_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_SUB_MATCH_IMPL_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/xpressive/sub_match.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// sub_match_impl
//
template<typename BidiIter>
struct sub_match_impl
  : sub_match<BidiIter>
{
    unsigned int repeat_count_;
    BidiIter begin_;
    bool zero_width_;

    sub_match_impl()
      : sub_match<BidiIter>()
      , repeat_count_(0)
      , begin_()
      , zero_width_(false)
    {
    }
};

}}} // namespace boost::xpressive::detail

#endif

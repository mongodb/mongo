///////////////////////////////////////////////////////////////////////////////
// flow_control.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_FLOW_CONTROL_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_FLOW_CONTROL_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>
#include <boost/xpressive/detail/core/state.hpp>
#include <boost/xpressive/detail/utility/ignore_unused.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// push_context_match
//
template<typename BidiIter>
inline bool push_context_match
(
    regex_impl<BidiIter> const &impl
  , state_type<BidiIter> &state
  , matchable<BidiIter> const &next
)
{
    // save state
    match_context<BidiIter> context = state.push_context(impl, next, context);
    detail::ignore_unused(&context);

    // match the nested regex
    bool success = impl.xpr_->match(state);

    // uninitialize the match context (reclaims the sub_match objects if necessary)
    state.pop_context(impl, success);
    return success;
}

///////////////////////////////////////////////////////////////////////////////
// pop_context_match
//
template<typename BidiIter>
inline bool pop_context_match(state_type<BidiIter> &state)
{
    // save state
    // BUGBUG nested regex could have changed state.traits_
    match_context<BidiIter> &context(*state.context_.prev_context_);
    state.swap_context(context);

    // Finished matching the nested regex; now match the rest of the enclosing regex
    bool success = context.next_ptr_->match(state);

    // restore state
    state.swap_context(context);
    return success;
}

}}} // namespace boost::xpressive::detail

#endif


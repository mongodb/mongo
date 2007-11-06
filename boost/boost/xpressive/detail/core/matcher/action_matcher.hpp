///////////////////////////////////////////////////////////////////////////////
// action_matcher.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_ACTION_MATCHER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_ACTION_MATCHER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/access.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/state.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // action_matcher
    //
    template<typename Action>
    struct action_matcher
      : quant_style<quant_none, mpl::size_t<0>, mpl::false_>
    {
        Action *action_ptr_;

        action_matcher()
          : action_ptr_(&action_())
        {
        }

        action_matcher(action_matcher const &)
          : action_ptr_(&action_())
        {
        }

        action_matcher &operator =(action_matcher const &)
        {
            return *this; // no-op
        }

        template<typename BidiIter, typename Next>
        bool match(state_type<BidiIter> &state, Next const &next) const
        {
            Action &action = *this->action_ptr_;
            typename Action::saved_type saved(action.save());

            // set the action state pointer, so that action_state_cast works correctly
            core_access<BidiIter>::get_action_state(*state.context_.results_ptr_) = state.action_state_;

            match_results<BidiIter> const &what = *state.context_.results_ptr_;
            if(!action(what, state.cur_) || !next.match(state))
            {
                action.restore(saved);
                return false;
            }

            return true;
        }

    protected:

        Action &action_()
        {
            return *static_cast<Action *>(this);
        }
    };

}}}

#endif

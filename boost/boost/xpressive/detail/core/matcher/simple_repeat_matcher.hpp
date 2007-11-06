///////////////////////////////////////////////////////////////////////////////
// simple_repeat_matcher.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_SIMPLE_REPEAT_MATCHER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_SIMPLE_REPEAT_MATCHER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/assert.hpp>
#include <boost/mpl/if.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/state.hpp>
#include <boost/xpressive/detail/static/type_traits.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // simple_repeat_traits
    //
    struct greedy_slow_tag {};
    struct greedy_fast_tag {};
    struct non_greedy_tag {};

    typedef static_xpression<any_matcher, true_xpression> any_sxpr;
    typedef matcher_wrapper<any_matcher> any_dxpr;

    template<typename Xpr, typename Greedy, typename Random>
    struct simple_repeat_traits
    {
        typedef typename mpl::if_<Greedy, greedy_slow_tag, non_greedy_tag>::type tag_type;
    };

    template<>
    struct simple_repeat_traits<any_sxpr, mpl::true_, mpl::true_>
    {
        typedef greedy_fast_tag tag_type;
    };

    template<>
    struct simple_repeat_traits<any_dxpr, mpl::true_, mpl::true_>
    {
        typedef greedy_fast_tag tag_type;
    };

    ///////////////////////////////////////////////////////////////////////////////
    // simple_repeat_matcher
    //
    template<typename Xpr, bool Greedy>
    struct simple_repeat_matcher
      : quant_style_variable_width
    {
        typedef Xpr xpr_type;
        typedef mpl::bool_<Greedy> greedy_type;

        Xpr xpr_;
        unsigned int min_, max_;

        simple_repeat_matcher(Xpr const &xpr, unsigned int min, unsigned int max)
          : xpr_(xpr)
          , min_(min)
          , max_(max)
        {
            // it is the job of the parser to make sure this never happens
            BOOST_ASSERT(min <= max);
            BOOST_ASSERT(0 != max);
        }

        template<typename BidiIter, typename Next>
        bool match(state_type<BidiIter> &state, Next const &next) const
        {
            typedef mpl::bool_<is_random<BidiIter>::value> is_rand;
            typedef typename simple_repeat_traits<Xpr, greedy_type, is_rand>::tag_type tag_type;
            return this->match_(state, next, tag_type());
        }

        // greedy, fixed-width quantifier
        template<typename BidiIter, typename Next>
        bool match_(state_type<BidiIter> &state, Next const &next, greedy_slow_tag) const
        {
            int const diff = -static_cast<int>(this->xpr_.get_width(&state));
            BOOST_ASSERT(diff != -static_cast<int>(unknown_width()));
            unsigned int matches = 0;
            BidiIter const tmp = state.cur_;

            if(0 == diff)
            {
                return this->xpr_.match(state) && next.match(state);
            }

            // greedily match as much as we can
            while(matches < this->max_ && this->xpr_.match(state))
            {
                ++matches;
            }

            if(this->min_ > matches)
            {
                state.cur_ = tmp;
                return false;
            }

            // try matching the rest of the pattern, and back off if necessary
            for(; ; --matches, std::advance(state.cur_, diff))
            {
                if(next.match(state))
                {
                    return true;
                }
                else if(this->min_ == matches)
                {
                    state.cur_ = tmp;
                    return false;
                }
            }
        }

        // non-greedy fixed-width quantification
        template<typename BidiIter, typename Next>
        bool match_(state_type<BidiIter> &state, Next const &next, non_greedy_tag) const
        {
            BidiIter const tmp = state.cur_;
            unsigned int matches = 0;

            if(0 == this->xpr_.get_width(&state))
            {
                return this->xpr_.match(state) && next.match(state);
            }

            for(; matches < this->min_; ++matches)
            {
                if(!this->xpr_.match(state))
                {
                    state.cur_ = tmp;
                    return false;
                }
            }

            do
            {
                if(next.match(state))
                {
                    return true;
                }
            }
            while(matches++ < this->max_ && this->xpr_.match(state));

            state.cur_ = tmp;
            return false;
        }

        // when greedily matching any character, skip to the end instead of iterating there.
        template<typename BidiIter, typename Next>
        bool match_(state_type<BidiIter> &state, Next const &next, greedy_fast_tag) const
        {
            BidiIter const tmp = state.cur_;
            std::size_t const diff_to_end = static_cast<std::size_t>(state.end_ - tmp);

            // is there enough room?
            if(this->min_ > diff_to_end)
            {
                return false;
            }

            BidiIter const min_iter = tmp + this->min_;
            state.cur_ += (std::min)((std::size_t)this->max_, diff_to_end);

            for(;; --state.cur_)
            {
                if(next.match(state))
                {
                    return true;
                }
                else if(min_iter == state.cur_)
                {
                    state.cur_ = tmp;
                    return false;
                }
            }
        }

        template<typename BidiIter>
        std::size_t get_width(state_type<BidiIter> *state) const
        {
            if(this->min_ != this->max_)
            {
                return unknown_width();
            }
            return this->min_ * this->xpr_.get_width(state);
        }

    private:
        simple_repeat_matcher &operator =(simple_repeat_matcher const &);
    };

    // BUGBUG can all non-greedy quantification be done with the fixed width quantifier?

    // BUGBUG matchers are chained together using static_xpression so that matchers to
    // the left can invoke matchers to the right. This is so that if the left matcher
    // succeeds but the right matcher fails, the left matcher is given the opportunity
    // to try something else. This is how backtracking works. However, if the left matcher
    // can succeed only one way (as with any_matcher, for example), it does not need
    // backtracking. In this case, leaving its stack frame active is a waste of stack
    // space. Can something be done?

}}}

#endif

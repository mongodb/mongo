///////////////////////////////////////////////////////////////////////////////
// lookbehind_matcher.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_LOOKBEHIND_MATCHER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_LOOKBEHIND_MATCHER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/assert.hpp>
#include <boost/xpressive/regex_error.hpp>
#include <boost/xpressive/regex_constants.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/state.hpp>
#include <boost/xpressive/detail/utility/algorithm.hpp>
#include <boost/xpressive/detail/utility/save_restore.hpp>
#include <boost/xpressive/detail/utility/ignore_unused.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // get_width
    //
    template<typename Xpr>
    inline std::size_t get_width(Xpr const &xpr)
    {
        return xpr.get_width(static_cast<state_type<char const *>*>(0));
    }

    template<typename BidiIter>
    inline std::size_t get_width(shared_ptr<matchable<BidiIter> const> const &xpr)
    {
        return xpr->get_width(0);
    }

    ///////////////////////////////////////////////////////////////////////////////
    // lookbehind_matcher
    //   Xpr can be either a static_xpression, or a shared_ptr<matchable>
    template<typename Xpr>
    struct lookbehind_matcher
      : quant_style<quant_none, mpl::size_t<0>, is_pure<Xpr> >
    {
        lookbehind_matcher(Xpr const &xpr, bool no = false, bool do_save = !is_pure<Xpr>::value)
          : xpr_(xpr)
          , not_(no)
          , do_save_(do_save)
          , width_(detail::get_width(xpr))
        {
            detail::ensure(this->width_ != unknown_width(), regex_constants::error_badlookbehind,
                "Variable-width look-behind assertions are not supported");
        }

        template<typename BidiIter, typename Next>
        bool match(state_type<BidiIter> &state, Next const &next) const
        {
            // Note that if is_pure<Xpr>::value is true, the compiler can optimize this.
            return is_pure<Xpr>::value || !this->do_save_
                ? this->match_(state, next, mpl::true_())
                : this->match_(state, next, mpl::false_());
        }

        template<typename BidiIter, typename Next>
        bool match_(state_type<BidiIter> &state, Next const &next, mpl::true_) const
        {
            typedef typename iterator_difference<BidiIter>::type difference_type;
            BidiIter const tmp = state.cur_;
            if(!detail::advance_to(state.cur_, -static_cast<difference_type>(this->width_), state.begin_))
            {
                state.cur_ = tmp;
                return this->not_ ? next.match(state) : false;
            }

            if(this->not_)
            {
                if(get_pointer(this->xpr_)->match(state))
                {
                    BOOST_ASSERT(state.cur_ == tmp);
                    return false;
                }
                state.cur_ = tmp;
                if(next.match(state))
                {
                    return true;
                }
            }
            else
            {
                if(!get_pointer(this->xpr_)->match(state))
                {
                    state.cur_ = tmp;
                    return false;
                }
                BOOST_ASSERT(state.cur_ == tmp);
                if(next.match(state))
                {
                    return true;
                }
            }

            BOOST_ASSERT(state.cur_ == tmp);
            return false;
        }

        template<typename BidiIter, typename Next>
        bool match_(state_type<BidiIter> &state, Next const &next, mpl::false_) const
        {
            typedef typename iterator_difference<BidiIter>::type difference_type;
            BidiIter const tmp = state.cur_;
            if(!detail::advance_to(state.cur_, -static_cast<difference_type>(this->width_), state.begin_))
            {
                state.cur_ = tmp;
                return this->not_ ? next.match(state) : false;
            }

            // matching xpr could produce side-effects, save state
            memento<BidiIter> mem = save_sub_matches(state);

            if(this->not_)
            {
                // negative look-ahead assertions do not trigger partial matches.
                save_restore<bool> partial_match(state.found_partial_match_);
                detail::ignore_unused(&partial_match);

                if(get_pointer(this->xpr_)->match(state))
                {
                    restore_sub_matches(mem, state);
                    BOOST_ASSERT(state.cur_ == tmp);
                    return false;
                }
                state.cur_ = tmp;
                if(next.match(state))
                {
                    reclaim_sub_matches(mem, state);
                    return true;
                }
                reclaim_sub_matches(mem, state);
            }
            else
            {
                if(!get_pointer(this->xpr_)->match(state))
                {
                    state.cur_ = tmp;
                    reclaim_sub_matches(mem, state);
                    return false;
                }
                BOOST_ASSERT(state.cur_ == tmp);
                if(next.match(state))
                {
                    reclaim_sub_matches(mem, state);
                    return true;
                }
                restore_sub_matches(mem, state);
            }

            BOOST_ASSERT(state.cur_ == tmp);
            return false;
        }

        Xpr xpr_;
        bool not_;
        bool do_save_; // true if matching xpr_ could modify the sub-matches
        std::size_t width_;
    };

}}}

#endif

///////////////////////////////////////////////////////////////////////////////
// assert_word_matcher.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_ASSERT_WORD_MATCHER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_ASSERT_WORD_MATCHER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/assert.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/state.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // word_boundary
    //
    template<bool IsBoundary>
    struct word_boundary
    {
        template<typename BidiIter>
        static bool eval(bool prevword, bool thisword, state_type<BidiIter> &state)
        {
            if((state.flags_.match_not_bow_ && state.bos()) || (state.flags_.match_not_eow_ && state.eos()))
            {
                return !IsBoundary;
            }

            return IsBoundary == (prevword != thisword);
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // word_begin
    //
    struct word_begin
    {
        template<typename BidiIter>
        static bool eval(bool prevword, bool thisword, state_type<BidiIter> &state)
        {
            if(state.flags_.match_not_bow_ && state.bos())
            {
                return false;
            }

            return !prevword && thisword;
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // word_end
    //
    struct word_end
    {
        template<typename BidiIter>
        static bool eval(bool prevword, bool thisword, state_type<BidiIter> &state)
        {
            if(state.flags_.match_not_eow_ && state.eos())
            {
                return false;
            }

            return prevword && !thisword;
        }
    };

    ///////////////////////////////////////////////////////////////////////////////
    // assert_word_matcher
    //
    template<typename Cond, typename Traits>
    struct assert_word_matcher
      : quant_style_assertion
    {
        typedef typename Traits::char_type char_type;

        assert_word_matcher(Traits const &traits)
          : word_(lookup_classname(traits, "w"))
        {
            BOOST_ASSERT(0 != this->word_);
        }

        bool is_word(Traits const &traits, char_type ch) const
        {
            return traits.isctype(traits.translate(ch), this->word_);
        }

        template<typename BidiIter, typename Next>
        bool match(state_type<BidiIter> &state, Next const &next) const
        {
            BidiIter cur = state.cur_;
            bool const thisword = !state.eos() && this->is_word(traits_cast<Traits>(state), *cur);
            bool const prevword = (!state.bos() || state.flags_.match_prev_avail_)
                && this->is_word(traits_cast<Traits>(state), *--cur);

            return Cond::eval(prevword, thisword, state) && next.match(state);
        }

        typename Traits::char_class_type word_;
    };

}}}

#endif

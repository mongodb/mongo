///////////////////////////////////////////////////////////////////////////////
// string_matcher.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_STRING_MATCHER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_STRING_MATCHER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <string>
#include <boost/mpl/bool.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/state.hpp>
#include <boost/xpressive/detail/utility/traits_utils.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // string_matcher
    //
    template<typename Traits, bool ICase>
    struct string_matcher
      : quant_style_fixed_unknown_width
    {
        typedef typename Traits::char_type char_type;
        typedef mpl::bool_<ICase> icase_type;
        std::basic_string<char_type> str_;
        char_type const *end_;

        string_matcher(std::basic_string<char_type> const &str, Traits const &traits)
          : str_(str)
          , end_(str_.data() + str_.size())
        {
            for(typename std::basic_string<char_type>::size_type i = 0; i < this->str_.size(); ++i)
            {
                this->str_[i] = detail::translate(this->str_[i], traits, icase_type());
            }
        }

        string_matcher(string_matcher<Traits, ICase> const &that)
          : str_(that.str_)
          , end_(str_.data() + str_.size())
        {
        }

        template<typename BidiIter, typename Next>
        bool match(state_type<BidiIter> &state, Next const &next) const
        {
            BidiIter const tmp = state.cur_;
            char_type const *begin = this->str_.data();
            for(; begin != this->end_; ++begin, ++state.cur_)
            {
                if(state.eos() ||
                    (detail::translate(*state.cur_, traits_cast<Traits>(state), icase_type()) != *begin))
                {
                    state.cur_ = tmp;
                    return false;
                }
            }

            if(next.match(state))
            {
                return true;
            }

            state.cur_ = tmp;
            return false;
        }

        template<typename BidiIter>
        std::size_t get_width(state_type<BidiIter> *) const
        {
            return this->str_.size();
        }
    };

}}}

#endif

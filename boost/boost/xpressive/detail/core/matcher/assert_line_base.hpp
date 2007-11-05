///////////////////////////////////////////////////////////////////////////////
// assert_line_base.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_DETAIL_ASSERT_LINE_BASE_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_MATCHER_DETAIL_ASSERT_LINE_BASE_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/state.hpp>

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // assert_line_base
    //
    template<typename Traits>
    struct assert_line_base
        : quant_style_assertion
    {
        typedef typename Traits::char_type char_type;
        typedef typename Traits::char_class_type char_class_type;

    protected:
        assert_line_base(Traits const &traits)
            : newline_(lookup_classname(traits, "newline"))
            , nl_(traits.widen('\n'))
            , cr_(traits.widen('\r'))
        {
        }

        template<typename BidiIter>
            bool is_line_break(state_type<BidiIter> &state) const
        {
            BOOST_ASSERT(!state.bos() || state.flags_.match_prev_avail_);
            BidiIter tmp = state.cur_;
            char_type ch = *--tmp;

            if(traits_cast<Traits>(state).isctype(ch, this->newline_))
            {
                // there is no line-break between \r and \n
                if(this->cr_ != ch || state.eos() || this->nl_ != *state.cur_)
                {
                    return true;
                }
            }

            return false;
        }

    private:
        char_class_type newline_;
        char_type nl_, cr_;
    };

}}}

#endif

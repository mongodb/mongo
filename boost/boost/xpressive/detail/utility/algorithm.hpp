///////////////////////////////////////////////////////////////////////////////
// algorithm.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_UTILITY_ALGORITHM_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_UTILITY_ALGORITHM_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <climits>
#include <algorithm>
#include <boost/iterator/iterator_traits.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// any
//
template<typename InIter, typename Pred>
inline bool any(InIter begin, InIter end, Pred pred)
{
    return end != std::find_if(begin, end, pred);
}

///////////////////////////////////////////////////////////////////////////////
// find_nth_if
//
template<typename FwdIter, typename Diff, typename Pred>
FwdIter find_nth_if(FwdIter begin, FwdIter end, Diff count, Pred pred)
{
    for(; begin != end; ++begin)
    {
        if(pred(*begin) && 0 == count--)
        {
            return begin;
        }
    }

    return end;
}

///////////////////////////////////////////////////////////////////////////////
// toi
//
template<typename InIter, typename Traits>
int toi(InIter &begin, InIter end, Traits const &traits, int radix = 10, int max = INT_MAX)
{
    int i = 0, c = 0;
    for(; begin != end && -1 != (c = traits.value(*begin, radix)); ++begin)
    {
        if(max < ((i *= radix) += c))
            return i / radix;
    }
    return i;
}

///////////////////////////////////////////////////////////////////////////////
// advance_to
//
template<typename BidiIter, typename Diff>
inline bool advance_to_impl(BidiIter & iter, Diff diff, BidiIter end, std::bidirectional_iterator_tag)
{
    for(; 0 < diff && iter != end; --diff)
        ++iter;
    for(; 0 > diff && iter != end; ++diff)
        --iter;
    return 0 == diff;
}

template<typename RandIter, typename Diff>
inline bool advance_to_impl(RandIter & iter, Diff diff, RandIter end, std::random_access_iterator_tag)
{
    if(0 < diff)
    {
        if((end - iter) < diff)
            return false;
    }
    else if(0 > diff)
    {
        if((iter - end) < -diff)
            return false;
    }
    iter += diff;
    return true;
}

template<typename Iter, typename Diff>
inline bool advance_to(Iter & iter, Diff diff, Iter end)
{
    return detail::advance_to_impl(iter, diff, end, typename iterator_category<Iter>::type());
}

}}}

#endif

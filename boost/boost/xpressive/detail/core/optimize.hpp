///////////////////////////////////////////////////////////////////////////////
// optimize.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_OPTIMIZE_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_OPTIMIZE_HPP_EAN_10_04_2005

#include <string>
#include <utility>
#include <boost/mpl/bool.hpp>
#include <boost/iterator/iterator_traits.hpp>
#include <boost/xpressive/detail/core/finder.hpp>
#include <boost/xpressive/detail/core/peeker.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>
#include <boost/xpressive/detail/utility/hash_peek_bitset.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// optimize_regex
//
template<typename BidiIter, typename Traits>
inline void optimize_regex(regex_impl<BidiIter> &impl, Traits const &traits, mpl::true_)
{
    typedef typename iterator_value<BidiIter>::type char_type;

    // optimization: get the peek chars OR the boyer-moore search string
    hash_peek_bitset<char_type> bset;
    xpression_peeker<char_type> peeker(&bset, traits);
    impl.xpr_->peek(peeker);

    // if we have a leading string literal, initialize a boyer-moore struct with it
    std::pair<std::basic_string<char_type> const *, bool> str = peeker.get_string();
    if(0 != str.first)
    {
        impl.finder_.reset
        (
            new boyer_moore_finder<BidiIter, Traits>
            (
                str.first->data()
              , str.first->data() + str.first->size()
              , traits
              , str.second
            )
        );
    }
    else if(peeker.line_start())
    {
        impl.finder_.reset
        (
            new line_start_finder<BidiIter, Traits>(traits)
        );
    }
    else if(256 != bset.count())
    {
        impl.finder_.reset
        (
            new hash_peek_finder<BidiIter, Traits>(bset)
        );
    }
}

///////////////////////////////////////////////////////////////////////////////
// optimize_regex
//
template<typename BidiIter, typename Traits>
inline void optimize_regex(regex_impl<BidiIter> &impl, Traits const &traits, mpl::false_)
{
    typedef typename iterator_value<BidiIter>::type char_type;

    // optimization: get the peek chars OR the line start finder
    hash_peek_bitset<char_type> bset;
    xpression_peeker<char_type> peeker(&bset, traits);
    impl.xpr_->peek(peeker);

    if(peeker.line_start())
    {
        impl.finder_.reset
        (
            new line_start_finder<BidiIter, Traits>(traits)
        );
    }
    else if(256 != bset.count())
    {
        impl.finder_.reset
        (
            new hash_peek_finder<BidiIter, Traits>(bset)
        );
    }
}

}}} // namespace boost::xpressive

#endif

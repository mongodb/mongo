/// Contains the definition of the basic_regex\<\> class template and its associated helper functions.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_FINDER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_FINDER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
# pragma warning(push)
# pragma warning(disable : 4189) // local variable is initialized but not referenced
#endif

#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/regex_impl.hpp>
#include <boost/xpressive/detail/utility/boyer_moore.hpp>
#include <boost/xpressive/detail/utility/hash_peek_bitset.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// boyer_moore_finder
//
template<typename BidiIter, typename Traits>
struct boyer_moore_finder
  : finder<BidiIter>
{
    typedef typename iterator_value<BidiIter>::type char_type;

    boyer_moore_finder(char_type const *begin, char_type const *end, Traits const &tr, bool icase)
      : bm_(begin, end, tr, icase)
    {
    }

    bool operator ()(state_type<BidiIter> &state) const
    {
        Traits const &traits = traits_cast<Traits>(state);
        state.cur_ = this->bm_.find(state.cur_, state.end_, traits);
        return state.cur_ != state.end_;
    }

private:
    boyer_moore_finder(boyer_moore_finder const &);
    boyer_moore_finder &operator =(boyer_moore_finder const &);

    boyer_moore<BidiIter, Traits> bm_;
};

///////////////////////////////////////////////////////////////////////////////
// hash_peek_finder
//
template<typename BidiIter, typename Traits>
struct hash_peek_finder
  : finder<BidiIter>
{
    typedef typename iterator_value<BidiIter>::type char_type;

    hash_peek_finder(hash_peek_bitset<char_type> const &bset)
      : bset_(bset)
    {
    }

    bool operator ()(state_type<BidiIter> &state) const
    {
        Traits const &traits = traits_cast<Traits>(state);
        state.cur_ = (this->bset_.icase() 
            ? this->find_(state.cur_, state.end_, traits, mpl::true_())
            : this->find_(state.cur_, state.end_, traits, mpl::false_()));
        return state.cur_ != state.end_;
    }

private:
    hash_peek_finder(hash_peek_finder const &);
    hash_peek_finder &operator =(hash_peek_finder const &);

    template<typename ICase>
    BidiIter find_(BidiIter begin, BidiIter end, Traits const &traits, ICase) const
    {
        for(; begin != end && !this->bset_.test(*begin, traits, ICase()); ++begin)
            ;
        return begin;
    }

    hash_peek_bitset<char_type> bset_;
};

///////////////////////////////////////////////////////////////////////////////
// line_start_finder
//
template<typename BidiIter, typename Traits, std::size_t Size = sizeof(typename iterator_value<BidiIter>::type)>
struct line_start_finder
  : finder<BidiIter>
{
    typedef typename iterator_value<BidiIter>::type char_type;
    typedef typename iterator_difference<BidiIter>::type diff_type;
    typedef typename Traits::char_class_type char_class_type;

    line_start_finder(Traits const &traits)
      : newline_(lookup_classname(traits, "newline"))
    {
    }

    bool operator ()(state_type<BidiIter> &state) const
    {
        if(state.bos() && state.flags_.match_bol_)
        {
            return true;
        }

        Traits const &traits = traits_cast<Traits>(state);
        BidiIter cur = state.cur_;
        BidiIter const end = state.end_;
        std::advance(cur, static_cast<diff_type>(-!state.bos()));

        for(; cur != end; ++cur)
        {
            if(traits.isctype(*cur, this->newline_))
            {
                state.cur_ = ++cur;
                return true;
            }
        }

        return false;
    }

private:
    line_start_finder(line_start_finder const &);
    line_start_finder &operator =(line_start_finder const &);

    char_class_type newline_;
};

///////////////////////////////////////////////////////////////////////////////
// line_start_finder
//
template<typename BidiIter, typename Traits>
struct line_start_finder<BidiIter, Traits, 1u>
  : finder<BidiIter>
{
    typedef typename iterator_value<BidiIter>::type char_type;
    typedef typename iterator_difference<BidiIter>::type diff_type;
    typedef typename Traits::char_class_type char_class_type;

    line_start_finder(Traits const &traits)
    {
        char_class_type newline = lookup_classname(traits, "newline");
        for(int j = 0; j < 256; ++j)
        {
            this->bits_[j] = traits.isctype(static_cast<char_type>(static_cast<unsigned char>(j)), newline);
        }
    }

    bool operator ()(state_type<BidiIter> &state) const
    {
        if(state.bos() && state.flags_.match_bol_)
        {
            return true;
        }

        BidiIter cur = state.cur_;
        BidiIter const end = state.end_;
        std::advance(cur, static_cast<diff_type>(-!state.bos()));

        for(; cur != end; ++cur)
        {
            if(this->bits_[static_cast<unsigned char>(*cur)])
            {
                state.cur_ = ++cur;
                return true;
            }
        }

        return false;
    }

private:
    line_start_finder(line_start_finder const &);
    line_start_finder &operator =(line_start_finder const &);

    bool bits_[256];
};

}}}

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma warning(pop)
#endif

#endif

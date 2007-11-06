///////////////////////////////////////////////////////////////////////////////
// set.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_SET_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_SET_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
# pragma warning(push)
# pragma warning(disable : 4127) // conditional expression constant
# pragma warning(disable : 4100) // unreferenced formal parameter
# pragma warning(disable : 4351) // vc8 new behavior: elements of array 'foo' will be default initialized
#endif

#include <algorithm>
#include <boost/mpl/assert.hpp>
#include <boost/type_traits/same_traits.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/quant_style.hpp>
#include <boost/xpressive/detail/core/state.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// set_matcher
//
template<typename Traits, int Size>
struct set_matcher
  : quant_style_fixed_width<1>
{
    typedef typename Traits::char_type char_type;
    char_type set_[ Size ? Size : 1 ];
    bool not_;
    bool icase_;

    typedef set_matcher<Traits, Size + 1> next_type;
    friend struct set_matcher<Traits, Size - 1>;

    set_matcher(Traits const &)
      : set_()
      , not_(false)
      , icase_(false)
    {
    }

    set_matcher(char_type ch, Traits const &traits)
      : set_()
      , not_(false)
      , icase_(false)
    {
        BOOST_MPL_ASSERT_RELATION(1, ==, Size);
        this->set_[0] = traits.translate(ch);
    }

    void complement()
    {
        this->not_ = !this->not_;
    }

    void nocase(Traits const &traits)
    {
        this->icase_ = true;

        for(int i = 0; i < Size; ++i)
        {
            this->set_[i] = traits.translate_nocase(this->set_[i]);
        }
    }

    next_type push_back(char_type ch, Traits const &traits) const
    {
        return next_type(*this, ch, traits);
    }

    char_type const *begin() const
    {
        return this->set_;
    }

    char_type const *end() const
    {
        return this->set_ + Size;
    }

    bool in_set(Traits const &traits, char_type ch) const
    {
        ch = this->icase_ ? traits.translate_nocase(ch) : traits.translate(ch);

        if(1 == Size)
        {
            return this->set_[0] == ch;
        }
        else
        {
            return this->end() != std::find(this->begin(), this->end(), ch);
        }
    }

    template<typename BidiIter, typename Next>
    bool match(state_type<BidiIter> &state, Next const &next) const
    {
        if(state.eos() || this->not_ == this->in_set(traits_cast<Traits>(state), *state.cur_))
        {
            return false;
        }

        if(++state.cur_, next.match(state))
        {
            return true;
        }

        return --state.cur_, false;
    }

private:

    set_matcher(set_matcher<Traits, Size - 1> const &that, char_type ch, Traits const &traits)
      : set_()
      , not_(false)
      , icase_(that.icase_)
    {
        std::copy(that.begin(), that.end(), this->set_);
        this->set_[ Size - 1 ] = traits.translate(ch);
    }
};

///////////////////////////////////////////////////////////////////////////////
// set_initializer
struct set_initializer
{
};

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma warning(pop)
#endif

}}} // namespace boost::xpressive::detail

#endif

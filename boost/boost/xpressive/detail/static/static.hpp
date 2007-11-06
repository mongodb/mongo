///////////////////////////////////////////////////////////////////////////////
// static.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_STATIC_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_STATIC_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/mpl/assert.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/state.hpp>
#include <boost/xpressive/detail/core/linker.hpp>
#include <boost/xpressive/detail/core/peeker.hpp>

// Random thoughts:
// - must support indirect repeat counts {$n,$m}
// - add ws to eat whitespace (make *ws illegal)
// - a{n,m}    -> repeat<n,m>(a)
// - a{$n,$m}  -> repeat(n,m)(a)
// - add nil to match nothing
// - instead of s1, s2, etc., how about s[1], s[2], etc.? Needlessly verbose?

namespace boost { namespace xpressive { namespace detail
{

#ifdef BOOST_XPR_DEBUG_STACK
///////////////////////////////////////////////////////////////////////////////
// top_type
//
template<typename Top>
struct top_type
{
    typedef Top type;
};

///////////////////////////////////////////////////////////////////////////////
// top_type
//
template<typename Top, typename Next>
struct top_type<stacked_xpression<Top, Next> >
{
    typedef Next type;
};
#endif

///////////////////////////////////////////////////////////////////////////////
// stacked_xpression
//
template<typename Top, typename Next>
struct stacked_xpression
  : Next
{
    // match
    //  delegates to Next
    template<typename BidiIter>
    bool match(state_type<BidiIter> &state) const
    {
        return static_cast<Next const *>(this)->
            BOOST_NESTED_TEMPLATE push_match<Top>(state);
    }

    // top_match
    //   jump back to the xpression on top of the xpression stack,
    //   and keep the xpression on the stack.
    template<typename BidiIter>
    static bool top_match(state_type<BidiIter> &state, xpression_base const *top)
    {
        BOOST_XPR_DEBUG_STACK_ASSERT(typeid(*top) == typeid(typename top_type<Top>::type));
        return static_cast<Top const *>(top)->
            BOOST_NESTED_TEMPLATE push_match<Top>(state);
    }

    // pop_match
    //   jump back to the xpression on top of the xpression stack,
    //   pop the xpression off the stack.
    template<typename BidiIter>
    static bool pop_match(state_type<BidiIter> &state, xpression_base const *top)
    {
        BOOST_XPR_DEBUG_STACK_ASSERT(typeid(*top) == typeid(typename top_type<Top>::type));
        return static_cast<Top const *>(top)->match(state);
    }

    // skip_match
    //   pop the xpression off the top of the stack and ignore it; call
    //   match on next.
    template<typename BidiIter>
    bool skip_match(state_type<BidiIter> &state) const
    {
        // could be static_xpression::skip_impl or stacked_xpression::skip_impl
        // depending on if there is 1 or more than 1 xpression on the
        // xpression stack
        return Top::skip_impl(*static_cast<Next const *>(this), state);
    }

//protected:

    // skip_impl
    //   implementation of skip_match.
    template<typename That, typename BidiIter>
    static bool skip_impl(That const &that, state_type<BidiIter> &state)
    {
        return that.BOOST_NESTED_TEMPLATE push_match<Top>(state);
    }
};

///////////////////////////////////////////////////////////////////////////////
// stacked_xpression_cast
//
template<typename Top, typename Next>
inline stacked_xpression<Top, Next> const &stacked_xpression_cast(Next const &next)
{
    // NOTE: this is a little white lie. The "next" object doesn't really have
    // the type to which we're casting it. It is harmless, though. We are only using
    // the cast to decorate the next object with type information. It is done
    // this way to save stack space.
    BOOST_MPL_ASSERT_RELATION(sizeof(stacked_xpression<Top, Next>), ==, sizeof(Next));
    return *static_cast<stacked_xpression<Top, Next> const *>(&next);
}

///////////////////////////////////////////////////////////////////////////////
// static_xpression
//
template<typename Matcher, typename Next>
struct static_xpression
  : Matcher
{
    Next next_;

    static_xpression(Matcher const &matcher = Matcher(), Next const &next = Next())
      : Matcher(matcher)
      , next_(next)
    {
    }

    // match
    //  delegates to the Matcher
    template<typename BidiIter>
    bool match(state_type<BidiIter> &state) const
    {
        return this->Matcher::match(state, this->next_);
    }

    // push_match
    //   call match on this, but also push "Top" onto the xpression
    //   stack so we know what we are jumping back to later.
    template<typename Top, typename BidiIter>
    bool push_match(state_type<BidiIter> &state) const
    {
        return this->Matcher::match(state, stacked_xpression_cast<Top>(this->next_));
    }

    // skip_impl
    //   implementation of skip_match, called from stacked_xpression::skip_match
    template<typename That, typename BidiIter>
    static bool skip_impl(That const &that, state_type<BidiIter> &state)
    {
        return that.match(state);
    }

    // for linking a compiled regular xpression
    template<typename Char>
    void link(xpression_linker<Char> &linker) const
    {
        linker.link(*static_cast<Matcher const *>(this), &this->next_);
        this->next_.link(linker);
    }

    // for building a lead-follow
    template<typename Char>
    void peek(xpression_peeker<Char> &peeker) const
    {
        this->peek_next_(peeker.peek(*static_cast<Matcher const *>(this)), peeker);
    }

    // for getting xpression width
    template<typename BidiIter>
    std::size_t get_width(state_type<BidiIter> *state) const
    {
        // BUGBUG this gets called from the simple_repeat_matcher::match(), so this is slow.
        // or will the compiler be able to optimize this all away?
        std::size_t this_width = this->Matcher::get_width(state);
        if(this_width == unknown_width())
            return unknown_width();
        std::size_t that_width = this->next_.get_width(state);
        if(that_width == unknown_width())
            return unknown_width();
        return this_width + that_width;
    }

private: // hide this

    static_xpression &operator =(static_xpression const &);

    template<typename Char>
    void peek_next_(mpl::true_, xpression_peeker<Char> &peeker) const
    {
        this->next_.peek(peeker);
    }

    template<typename Char>
    static void peek_next_(mpl::false_, xpression_peeker<Char> &)
    {
        // no-op
    }

    using Matcher::width;
    using Matcher::pure;
};

// syntactic sugar so this xpression can be treated the same as
// (a smart pointer to) a dynamic xpression from templates
template<typename Matcher, typename Next>
inline static_xpression<Matcher, Next> const *
get_pointer(static_xpression<Matcher, Next> const &xpr)
{
    return &xpr;
}

///////////////////////////////////////////////////////////////////////////////
// make_static_xpression
//
template<typename Matcher>
inline static_xpression<Matcher> const
make_static_xpression(Matcher const &matcher)
{
    return static_xpression<Matcher>(matcher);
}

template<typename Matcher, typename Next>
inline static_xpression<Matcher, Next> const
make_static_xpression(Matcher const &matcher, Next const &next)
{
    return static_xpression<Matcher, Next>(matcher, next);
}

///////////////////////////////////////////////////////////////////////////////
// no_next
//
struct no_next
  : xpression_base
{
    template<typename Char>
    void link(xpression_linker<Char> &) const
    {
    }

    template<typename Char>
    void peek(xpression_peeker<Char> &peeker) const
    {
        peeker.fail();
    }

    template<typename BidiIter>
    static std::size_t get_width(state_type<BidiIter> *)
    {
        return 0;
    }
};

///////////////////////////////////////////////////////////////////////////////
// alternates_list
//
template<typename Alternates>
struct alternates_list
  : Alternates
{
    alternates_list(Alternates const &alternates)
      : Alternates(alternates)
    {
    }
private:
    alternates_list &operator =(alternates_list const &);
};

///////////////////////////////////////////////////////////////////////////////
// get_mark_number
//
inline int get_mark_number(mark_tag const &mark)
{
    return proto::arg(mark).mark_number_;
}

}}} // namespace boost::xpressive::detail

#endif

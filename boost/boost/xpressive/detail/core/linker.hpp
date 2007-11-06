///////////////////////////////////////////////////////////////////////////////
// linker.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_CORE_LINKER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_CORE_LINKER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/config.hpp>
#ifndef BOOST_NO_STD_LOCALE
# include <locale>
#endif
#include <stack>
#include <limits>
#include <typeinfo>
#include <boost/shared_ptr.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/spirit/fusion/algorithm/for_each.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/dynamic/matchable.hpp>
#include <boost/xpressive/detail/core/matchers.hpp>
#include <boost/xpressive/detail/core/peeker.hpp>
#include <boost/xpressive/detail/utility/never_true.hpp>

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// icase_modifier
//
//   wrapped by the modifier<> template and inserted into the xpression
//   template with the icase() helper function. icase_modifier morphs
//   a case-sensitive visitor into a case-insensitive visitor, which
//   causes all matchers visited to become case-insensitive.
//
struct icase_modifier
{
    template<typename Visitor>
    struct apply;

    template<typename BidiIter, typename ICase, typename Traits>
    struct apply<xpression_visitor<BidiIter, ICase, Traits> >
    {
        typedef xpression_visitor<BidiIter, mpl::true_, Traits> type;
    };

    template<typename Visitor>
    static typename apply<Visitor>::type
    call(Visitor &visitor)
    {
        return typename apply<Visitor>::type(visitor.traits(), visitor.self());
    }
};

///////////////////////////////////////////////////////////////////////////////
// regex_traits_type : wrap a locale in the appropriate regex_traits
//
template<typename Locale, typename BidiIter>
struct regex_traits_type
{
#ifndef BOOST_NO_STD_LOCALE

    typedef typename iterator_value<BidiIter>::type char_type;

    // if Locale is std::locale, wrap it in a cpp_regex_traits<Char>
    typedef typename mpl::if_
    <
        is_same<Locale, std::locale>
      , cpp_regex_traits<char_type>
      , Locale
    >::type type;

#else

    typedef Locale type;

#endif
};

///////////////////////////////////////////////////////////////////////////////
// locale_modifier
//
//   wrapped by the modifier<> template and inserted into the xpression
//   template with the imbue() helper function. Causes a sub-xpression to
//   use the specified Locale
//
template<typename Locale>
struct locale_modifier
{
    locale_modifier(Locale const &loc)
      : loc_(loc)
    {
    }

    template<typename Visitor>
    struct apply;

    template<typename BidiIter, typename ICase, typename OtherTraits>
    struct apply<xpression_visitor<BidiIter, ICase, OtherTraits> >
    {
        typedef typename regex_traits_type<Locale, BidiIter>::type traits_type;
        typedef xpression_visitor<BidiIter, ICase, traits_type> type;
    };

    template<typename Visitor>
    typename apply<Visitor>::type
    call(Visitor &) const
    {
        return typename apply<Visitor>::type(this->loc_);
    }

    Locale getloc() const
    {
        return this->loc_;
    }

private:
    Locale loc_;
};

///////////////////////////////////////////////////////////////////////////////
// xpression_linker
//
template<typename Char>
struct xpression_linker
{
    template<typename Traits>
    explicit xpression_linker(Traits const &traits)
      : back_stack_()
      , traits_(&traits)
      , traits_type_(&typeid(Traits))
    {
    }

    template<typename Matcher>
    void link(Matcher const &, xpression_base const *)
    {
        // no-op
    }

    void link(repeat_begin_matcher const &, xpression_base const *next)
    {
        this->back_stack_.push(next);
    }

    template<bool Greedy>
    void link(repeat_end_matcher<Greedy> const &matcher, xpression_base const *)
    {
        matcher.back_ = this->back_stack_.top();
        this->back_stack_.pop();
    }

    template<typename Alternates, typename Traits>
    void link(alternate_matcher<Alternates, Traits> const &matcher, xpression_base const *next)
    {
        xpression_peeker<Char> peeker(&matcher.bset_, this->get_traits<Traits>());
        this->alt_link(matcher.alternates_, next, peeker);
    }

    void link(alternate_end_matcher const &matcher, xpression_base const *)
    {
        matcher.back_ = this->back_stack_.top();
        this->back_stack_.pop();
    }

    template<typename Xpr>
    void link(keeper_matcher<Xpr> const &matcher, xpression_base const *)
    {
        get_pointer(matcher.xpr_)->link(*this);
    }

    template<typename Xpr>
    void link(lookahead_matcher<Xpr> const &matcher, xpression_base const *)
    {
        get_pointer(matcher.xpr_)->link(*this);
    }

    template<typename Xpr>
    void link(lookbehind_matcher<Xpr> const &matcher, xpression_base const *)
    {
        get_pointer(matcher.xpr_)->link(*this);
    }

    template<typename Xpr, bool Greedy>
    void link(simple_repeat_matcher<Xpr, Greedy> const &matcher, xpression_base const *)
    {
        matcher.xpr_.link(*this);
    }

    // for use by alt_link_pred below
    template<typename Xpr>
    void alt_branch_link(Xpr const &xpr, xpression_base const *next, xpression_peeker<Char> &peeker)
    {
        this->back_stack_.push(next);
        get_pointer(xpr)->link(*this);
        get_pointer(xpr)->peek(peeker);
    }

private:

    ///////////////////////////////////////////////////////////////////////////////
    // alt_link_pred
    //
    struct alt_link_pred
    {
        xpression_linker<Char> &linker_;
        xpression_peeker<Char> &peeker_;
        xpression_base const *next_;

        alt_link_pred
        (
            xpression_linker<Char> &linker
          , xpression_peeker<Char> &peeker
          , xpression_base const *next
        )
          : linker_(linker)
          , peeker_(peeker)
          , next_(next)
        {
        }

        template<typename Xpr>
        void operator ()(Xpr const &xpr) const
        {
            this->linker_.alt_branch_link(xpr, this->next_, this->peeker_);
        }

    private:
        alt_link_pred &operator =(alt_link_pred const &);
    };

    template<typename BidiIter>
    void alt_link
    (
        std::vector<shared_ptr<matchable<BidiIter> const> > const &alternates
      , xpression_base const *next
      , xpression_peeker<Char> &peeker
    )
    {
        std::for_each(alternates.begin(), alternates.end(), alt_link_pred(*this, peeker, next));
    }

    template<typename Alternates>
    void alt_link
    (
        fusion::sequence_base<Alternates> const &alternates
      , xpression_base const *next
      , xpression_peeker<Char> &peeker
    )
    {
        fusion::for_each(alternates.cast(), alt_link_pred(*this, peeker, next));
    }

    template<typename Traits>
    Traits const &get_traits() const
    {
        BOOST_ASSERT(*this->traits_type_ == typeid(Traits));
        return *static_cast<Traits const *>(this->traits_);
    }

    std::stack<xpression_base const *> back_stack_;
    void const *traits_;
    std::type_info const *traits_type_;
};

}}} // namespace boost::xpressive::detail

#endif

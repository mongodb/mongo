///////////////////////////////////////////////////////////////////////////////
// transmogrify.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_TRANSMOGRIFY_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_TRANSMOGRIFY_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <cstring> // for std::strlen
#include <boost/mpl/if.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/mpl/assert.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/matchers.hpp>
#include <boost/xpressive/detail/static/placeholders.hpp>
#include <boost/xpressive/detail/utility/dont_care.hpp>
#include <boost/xpressive/detail/utility/traits_utils.hpp>

namespace boost { namespace xpressive { namespace detail
{
    ///////////////////////////////////////////////////////////////////////////////
    // transmogrify
    //
    template<typename BidiIter, typename ICase, typename Traits, typename Matcher>
    struct transmogrify
    {
        typedef Matcher type;

        static type const &call(Matcher const &m, dont_care)
        {
            return m;
        }
    };

    template<typename BidiIter, typename ICase, typename Traits>
    struct transmogrify<BidiIter, ICase, Traits, assert_bol_placeholder>
    {
        typedef assert_bol_matcher<Traits> type;

        template<typename Visitor>
        static type call(assert_bol_placeholder, Visitor &visitor)
        {
            return type(visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits>
    struct transmogrify<BidiIter, ICase, Traits, assert_eol_placeholder>
    {
        typedef assert_eol_matcher<Traits> type;

        template<typename Visitor>
        static type call(assert_eol_placeholder, Visitor &visitor)
        {
            return type(visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits>
    struct transmogrify<BidiIter, ICase, Traits, logical_newline_placeholder>
    {
        typedef logical_newline_matcher<Traits> type;

        template<typename Visitor>
        static type call(logical_newline_placeholder, Visitor &visitor)
        {
            return type(visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits, typename Char, bool Not>
    struct transmogrify<BidiIter, ICase, Traits, literal_placeholder<Char, Not> >
    {
        typedef typename iterator_value<BidiIter>::type char_type;
        typedef literal_matcher<Traits, ICase::value, Not> type;

        template<typename Visitor>
        static type call(literal_placeholder<Char, Not> const &m, Visitor &visitor)
        {
            char_type ch = char_cast<char_type>(m.ch_, visitor.traits());
            return type(ch, visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits, typename Char>
    struct transmogrify<BidiIter, ICase, Traits, range_placeholder<Char> >
    {
        // By design, we don't widen character ranges.
        typedef typename iterator_value<BidiIter>::type char_type;
        BOOST_MPL_ASSERT((is_same<Char, char_type>));
        typedef range_matcher<Traits, ICase::value> type;

        template<typename Visitor>
        static type call(range_placeholder<Char> const &m, Visitor &visitor)
        {
            return type(m.ch_min_, m.ch_max_, m.not_, visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits, typename Char>
    struct transmogrify<BidiIter, ICase, Traits, string_placeholder<Char> >
    {
        typedef typename iterator_value<BidiIter>::type char_type;
        typedef string_matcher<Traits, ICase::value> type;

        template<typename Visitor>
        static type call(string_placeholder<Char> const &m, Visitor &visitor)
        {
            return type(string_cast<char_type>(m.str_, visitor.traits()), visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits>
    struct transmogrify<BidiIter, ICase, Traits, mark_placeholder>
    {
        typedef mark_matcher<Traits, ICase::value> type;

        template<typename Visitor>
        static type call(mark_placeholder const &m, Visitor &visitor)
        {
            return type(m.mark_number_, visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits>
    struct transmogrify<BidiIter, ICase, Traits, posix_charset_placeholder>
    {
        typedef posix_charset_matcher<Traits> type;

        template<typename Visitor>
        static type call(posix_charset_placeholder const &m, Visitor &visitor)
        {
            char const *name_end = m.name_ + std::strlen(m.name_);
            return type(visitor.traits().lookup_classname(m.name_, name_end, ICase::value), m.not_);
        }
    };

    template<typename BidiIter, typename Traits, int Size>
    struct transmogrify<BidiIter, mpl::true_, Traits, set_matcher<Traits, Size> >
    {
        typedef set_matcher<Traits, Size> type;

        template<typename Visitor>
        static type call(set_matcher<Traits, Size> m, Visitor &visitor)
        {
            m.nocase(visitor.traits());
            return m;
        }
    };

    template<typename BidiIter, typename ICase, typename Traits, typename Cond>
    struct transmogrify<BidiIter, ICase, Traits, assert_word_placeholder<Cond> >
    {
        typedef assert_word_matcher<Cond, Traits> type;

        template<typename Visitor>
        static type call(dont_care, Visitor &visitor)
        {
            return type(visitor.traits());
        }
    };

    template<typename BidiIter, typename ICase, typename Traits, bool ByRef>
    struct transmogrify<BidiIter, ICase, Traits, regex_placeholder<BidiIter, ByRef> >
    {
        typedef typename mpl::if_c
        <
            ByRef
          , regex_byref_matcher<BidiIter>
          , regex_matcher<BidiIter>
        >::type type;

        static type call(regex_placeholder<BidiIter, ByRef> const &m, dont_care)
        {
            return type(m.impl_);
        }
    };

    template<typename BidiIter, typename ICase, typename Traits>
    struct transmogrify<BidiIter, ICase, Traits, self_placeholder>
    {
        typedef regex_byref_matcher<BidiIter> type;

        template<typename Visitor>
        static type call(self_placeholder, Visitor &visitor)
        {
            return type(visitor.self());
        }
    };

}}}

#endif

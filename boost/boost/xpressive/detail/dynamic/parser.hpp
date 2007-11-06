///////////////////////////////////////////////////////////////////////////////
/// \file parser.hpp
/// Contains the definition of regex_compiler, a factory for building regex objects
/// from strings.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_DYNAMIC_PARSER_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_DYNAMIC_PARSER_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
# pragma warning(push)
# pragma warning(disable : 4127) // conditional expression is constant
#endif

#include <boost/assert.hpp>
#include <boost/xpressive/regex_constants.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/matchers.hpp>
#include <boost/xpressive/detail/dynamic/dynamic.hpp>

// The Regular Expression grammar, in pseudo BNF:
//
// expression   = alternates ;
//
// alternates   = sequence, *('|', sequence) ;
//
// sequence     = quant, *(quant) ;
//
// quant        = atom, [*+?] ;
//
// atom         = literal             |
//                '.'                 |
//                '\' any             |
//                '(' expression ')' ;
//
// literal      = not a meta-character ;
//

namespace boost { namespace xpressive { namespace detail
{

///////////////////////////////////////////////////////////////////////////////
// make_char_xpression
//
template<typename BidiIter, typename Char, typename Traits>
inline sequence<BidiIter> make_char_xpression
(
    Char ch
  , regex_constants::syntax_option_type flags
  , Traits const &traits
)
{
    if(0 != (regex_constants::icase_ & flags))
    {
        literal_matcher<Traits, true, false> matcher(ch, traits);
        return make_dynamic_xpression<BidiIter>(matcher);
    }
    else
    {
        literal_matcher<Traits, false, false> matcher(ch, traits);
        return make_dynamic_xpression<BidiIter>(matcher);
    }
}

///////////////////////////////////////////////////////////////////////////////
// make_any_xpression
//
template<typename BidiIter, typename Traits>
inline sequence<BidiIter> make_any_xpression
(
    regex_constants::syntax_option_type flags
  , Traits const &traits
)
{
    using namespace regex_constants;
    typedef typename iterator_value<BidiIter>::type char_type;
    typedef set_matcher<Traits, 2> set_matcher;
    typedef literal_matcher<Traits, false, true> literal_matcher;

    char_type const newline = traits.widen('\n');
    set_matcher s(traits);
    s.set_[0] = newline;
    s.set_[1] = 0;
    s.complement();

    switch(((int)not_dot_newline | not_dot_null) & flags)
    {
    case not_dot_null:
        return make_dynamic_xpression<BidiIter>(literal_matcher(char_type(0), traits));

    case not_dot_newline:
        return make_dynamic_xpression<BidiIter>(literal_matcher(newline, traits));

    case (int)not_dot_newline | not_dot_null:
        return make_dynamic_xpression<BidiIter>(s);

    default:
        return make_dynamic_xpression<BidiIter>(any_matcher());
    }
}

///////////////////////////////////////////////////////////////////////////////
// make_literal_xpression
//
template<typename BidiIter, typename Char, typename Traits>
inline sequence<BidiIter> make_literal_xpression
(
    std::basic_string<Char> const &literal
  , regex_constants::syntax_option_type flags
  , Traits const &traits
)
{
    BOOST_ASSERT(0 != literal.size());
    if(1 == literal.size())
    {
        return make_char_xpression<BidiIter>(literal[0], flags, traits);
    }

    typedef typename iterator_value<BidiIter>::type char_type;
    BOOST_MPL_ASSERT((is_same<char_type, Char>));

    if(0 != (regex_constants::icase_ & flags))
    {
        string_matcher<Traits, true> matcher(literal, traits);
        return make_dynamic_xpression<BidiIter>(matcher);
    }
    else
    {
        string_matcher<Traits, false> matcher(literal, traits);
        return make_dynamic_xpression<BidiIter>(matcher);
    }
}

///////////////////////////////////////////////////////////////////////////////
// make_backref_xpression
//
template<typename BidiIter, typename Traits>
inline sequence<BidiIter> make_backref_xpression
(
    int mark_nbr
  , regex_constants::syntax_option_type flags
  , Traits const &traits
)
{
    typedef typename iterator_value<BidiIter>::type char_type;
    if(0 != (regex_constants::icase_ & flags))
    {
        return make_dynamic_xpression<BidiIter>
        (
            mark_matcher<Traits, true>(mark_nbr, traits)
        );
    }
    else
    {
        return make_dynamic_xpression<BidiIter>
        (
            mark_matcher<Traits, false>(mark_nbr, traits)
        );
    }
}

///////////////////////////////////////////////////////////////////////////////
// merge_charset
//
template<typename Char, typename Traits>
inline void merge_charset
(
    basic_chset<Char> &basic
  , compound_charset<Traits> const &compound
  , Traits const &traits
)
{
    if(0 != compound.posix_yes())
    {
        typename Traits::char_class_type mask = compound.posix_yes();
        for(int i = 0; i <= UCHAR_MAX; ++i)
        {
            if(traits.isctype((Char)i, mask))
            {
                basic.set((Char)i);
            }
        }
    }

    if(!compound.posix_no().empty())
    {
        for(std::size_t j = 0; j < compound.posix_no().size(); ++j)
        {
            typename Traits::char_class_type mask = compound.posix_no()[j];
            for(int i = 0; i <= UCHAR_MAX; ++i)
            {
                if(!traits.isctype((Char)i, mask))
                {
                    basic.set((Char)i);
                }
            }
        }
    }

    if(compound.is_inverted())
    {
        basic.inverse();
    }
}

///////////////////////////////////////////////////////////////////////////////
// make_charset_xpression
//
template<typename BidiIter, typename Traits>
inline sequence<BidiIter> make_charset_xpression
(
    compound_charset<Traits> &chset
  , Traits const &traits
  , regex_constants::syntax_option_type flags
)
{
    typedef typename Traits::char_type char_type;
    bool const icase = (0 != (regex_constants::icase_ & flags));
    bool const optimize = 1 == sizeof(char_type) && 0 != (regex_constants::optimize & flags);

    // don't care about compile speed -- fold eveything into a bitset<256>
    if(optimize)
    {
        typedef basic_chset<char_type> charset_type;
        charset_type charset(chset.basic_chset());
        if(icase)
        {
            charset_matcher<Traits, true, charset_type> matcher(charset);
            merge_charset(matcher.charset_, chset, traits);
            return make_dynamic_xpression<BidiIter>(matcher);
        }
        else
        {
            charset_matcher<Traits, false, charset_type> matcher(charset);
            merge_charset(matcher.charset_, chset, traits);
            return make_dynamic_xpression<BidiIter>(matcher);
        }
    }

    // special case to make [[:digit:]] fast
    else if(chset.basic_chset().empty() && chset.posix_no().empty())
    {
        BOOST_ASSERT(0 != chset.posix_yes());
        posix_charset_matcher<Traits> matcher(chset.posix_yes(), chset.is_inverted());
        return make_dynamic_xpression<BidiIter>(matcher);
    }

    // default, slow
    else
    {
        if(icase)
        {
            charset_matcher<Traits, true> matcher(chset);
            return make_dynamic_xpression<BidiIter>(matcher);
        }
        else
        {
            charset_matcher<Traits, false> matcher(chset);
            return make_dynamic_xpression<BidiIter>(matcher);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// make_posix_charset_xpression
//
template<typename BidiIter, typename Traits>
inline sequence<BidiIter> make_posix_charset_xpression
(
    typename Traits::char_class_type m
  , bool no
  , regex_constants::syntax_option_type //flags
  , Traits const & //traits
)
{
    posix_charset_matcher<Traits> charset(m, no);
    return make_dynamic_xpression<BidiIter>(charset);
}

///////////////////////////////////////////////////////////////////////////////
// make_assert_begin_line
//
template<typename BidiIter, typename Traits>
inline sequence<BidiIter> make_assert_begin_line
(
    regex_constants::syntax_option_type flags
  , Traits const &traits
)
{
    if(0 != (regex_constants::single_line & flags))
    {
        return detail::make_dynamic_xpression<BidiIter>(detail::assert_bos_matcher());
    }
    else
    {
        detail::assert_bol_matcher<Traits> matcher(traits);
        return detail::make_dynamic_xpression<BidiIter>(matcher);
    }
}

///////////////////////////////////////////////////////////////////////////////
// make_assert_end_line
//
template<typename BidiIter, typename Traits>
inline sequence<BidiIter> make_assert_end_line
(
    regex_constants::syntax_option_type flags
  , Traits const &traits
)
{
    if(0 != (regex_constants::single_line & flags))
    {
        return detail::make_dynamic_xpression<BidiIter>(detail::assert_eos_matcher());
    }
    else
    {
        detail::assert_eol_matcher<Traits> matcher(traits);
        return detail::make_dynamic_xpression<BidiIter>(matcher);
    }
}

///////////////////////////////////////////////////////////////////////////////
// make_assert_word
//
template<typename BidiIter, typename Cond, typename Traits>
inline sequence<BidiIter> make_assert_word(Cond, Traits const &traits)
{
    typedef typename iterator_value<BidiIter>::type char_type;
    return detail::make_dynamic_xpression<BidiIter>
    (
        detail::assert_word_matcher<Cond, Traits>(traits)
    );
}

}}} // namespace boost::xpressive::detail

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma warning(pop)
#endif

#endif

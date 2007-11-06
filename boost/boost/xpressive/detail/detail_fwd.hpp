///////////////////////////////////////////////////////////////////////////////
// detail_fwd.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_DETAIL_FWD_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_DETAIL_FWD_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <climits> // for INT_MAX
#include <boost/mpl/bool.hpp>
#include <boost/mpl/size_t.hpp>
#include <boost/xpressive/xpressive_fwd.hpp>

namespace boost { namespace xpressive { namespace detail
{
    typedef unsigned int uint_t;

    template<uint_t Min, uint_t Max>
    struct generic_quant_tag;

    struct modifier_tag;

    typedef mpl::size_t<INT_MAX / 2 - 1> unknown_width;

    ///////////////////////////////////////////////////////////////////////////////
    // placeholders
    //
    template<typename Char, bool Not = false>
    struct literal_placeholder;

    template<typename Char>
    struct string_placeholder;

    struct mark_placeholder;

    template<typename BidiIter, bool ByRef>
    struct regex_placeholder;

    struct posix_charset_placeholder;

    template<typename Cond>
    struct assert_word_placeholder;

    template<typename Char>
    struct range_placeholder;

    struct assert_bol_placeholder;

    struct assert_eol_placeholder;

    struct logical_newline_placeholder;

    struct self_placeholder;

    ///////////////////////////////////////////////////////////////////////////////
    // matchers
    //
    struct end_matcher;

    struct assert_bos_matcher;

    struct assert_eos_matcher;

    template<typename Traits>
    struct assert_bol_matcher;

    template<typename Traits>
    struct assert_eol_matcher;

    template<typename Cond, typename Traits>
    struct assert_word_matcher;

    struct true_matcher;

    template<typename Alternates, typename Traits>
    struct alternate_matcher;

    struct alternate_end_matcher;

    template<typename Traits>
    struct posix_charset_matcher;

    template<typename BidiIter>
    struct alternates_factory;

    template<typename BidiIter>
    struct sequence;

    template<typename Traits, bool ICase>
    struct mark_matcher;

    struct mark_begin_matcher;

    struct mark_end_matcher;

    template<typename BidiIter>
    struct regex_matcher;

    template<typename BidiIter>
    struct regex_byref_matcher;

    template<typename Traits>
    struct compound_charset;

    template<typename Traits, bool ICase, typename CharSet = compound_charset<Traits> >
    struct charset_matcher;

    template<typename Traits, bool ICase>
    struct range_matcher;

    template<typename Traits, int Size>
    struct set_matcher;

    template<typename Xpr, bool Greedy>
    struct simple_repeat_matcher;

    struct repeat_begin_matcher;

    template<bool Greedy>
    struct repeat_end_matcher;

    template<typename Traits, bool ICase, bool Not>
    struct literal_matcher;

    template<typename Traits, bool ICase>
    struct string_matcher;

    template<typename Action>
    struct action_matcher;

    template<typename Xpr>
    struct is_modifiable;

    template<typename Alternates>
    struct alternates_list;

    template<typename Modifier>
    struct modifier_op;

    template<typename Left, typename Right>
    struct modifier_sequencer;

    struct icase_modifier;

    template<typename BidiIter, typename ICase, typename Traits>
    struct xpression_visitor;

    template<typename BidiIter>
    struct regex_impl;

    template<typename BidiIter>
    struct regex_matcher;

    struct epsilon_matcher;

    struct epsilon_mark_matcher;

    template<typename BidiIter>
    struct nested_results;

    template<typename BidiIter>
    struct regex_id_filter_predicate;

    template<typename Xpr>
    struct keeper_matcher;

    template<typename Xpr>
    struct lookahead_matcher;

    template<typename Xpr>
    struct lookbehind_matcher;

    template<typename Cond>
    struct assert_word_placeholder;

    template<bool IsBoundary>
    struct word_boundary;

    template<typename BidiIter, typename Matcher>
    sequence<BidiIter> make_dynamic_xpression(Matcher const &matcher);

    template<typename Char>
    struct xpression_linker;

    template<typename Char>
    struct xpression_peeker;

    typedef proto::unary_op<mark_placeholder, proto::noop_tag> mark_tag;

    struct any_matcher;

    template<typename Traits>
    struct logical_newline_matcher;

    typedef proto::unary_op<logical_newline_placeholder, proto::noop_tag> logical_newline_xpression;

    struct set_initializer;

    typedef proto::unary_op<set_initializer, proto::noop_tag> set_initializer_type;

    struct seq_tag;

    template<bool Positive>
    struct lookahead_tag;

    template<bool Positive>
    struct lookbehind_tag;

    struct keeper_tag;

    template<typename Locale>
    struct locale_modifier;

    template<typename Matcher>
    struct matcher_wrapper;

    template<typename Locale, typename BidiIter>
    struct regex_traits_type;

    ///////////////////////////////////////////////////////////////////////////////
    // Misc.
    struct no_next;

    template<typename BidiIter>
    struct core_access;

    template<typename BidiIter>
    struct state_type;

    template<typename BidiIter>
    struct matchable;

    template<typename Matcher, typename BidiIter>
    struct dynamic_xpression;

    template<typename Matcher, typename Next>
    struct static_xpression;

    typedef static_xpression<end_matcher, no_next> end_xpression;

    typedef static_xpression<alternate_end_matcher, no_next> alternate_end_xpression;

    typedef static_xpression<true_matcher, no_next> true_xpression;

    template<typename Matcher, typename Next = end_xpression>
    struct static_xpression;

    template<typename Top, typename Next>
    struct stacked_xpression;

    template<typename Xpr>
    struct is_static_xpression;

    template<typename BidiIter>
    struct sub_match_impl;

    template<typename BidiIter>
    struct results_cache;

    template<typename T>
    struct sequence_stack;

    template<typename BidiIter>
    struct results_extras;

    template<typename BidiIter>
    struct match_context;

    template<typename BidiIter>
    struct sub_match_vector;

    struct action_state;

    template<typename Xpr, bool IsOp = proto::is_op<Xpr>::value>
    struct as_xpr_type;

    template<typename Traits, typename BidiIter>
    Traits const &traits_cast(state_type<BidiIter> const &state);

    template<typename Char>
    struct basic_chset;

    template<typename BidiIter>
    struct memento;

    template<typename Char, typename Traits>
    void set_char(compound_charset<Traits> &chset, Char ch, Traits const &traits, bool icase);

    template<typename Char, typename Traits>
    void set_range(compound_charset<Traits> &chset, Char from, Char to, Traits const &traits, bool icase);

    template<typename Traits>
    void set_class(compound_charset<Traits> &chset, typename Traits::char_class_type char_class, bool no, Traits const &traits);

    template<typename Char, typename Traits>
    void set_char(basic_chset<Char> &chset, Char ch, Traits const &traits, bool icase);

    template<typename Char, typename Traits>
    void set_range(basic_chset<Char> &chset, Char from, Char to, Traits const &traits, bool icase);

    template<typename Char, typename Traits>
    void set_class(basic_chset<Char> &chset, typename Traits::char_class_type char_class, bool no, Traits const &traits);

    template<typename Matcher>
    static_xpression<Matcher> const
    make_static_xpression(Matcher const &matcher);

    template<typename Matcher, typename Next>
    static_xpression<Matcher, Next> const
    make_static_xpression(Matcher const &matcher, Next const &next);

    int get_mark_number(mark_tag const &);

    template<typename Xpr, typename BidiIter>
    void static_compile(Xpr const &xpr, regex_impl<BidiIter> &impl);

}}} // namespace boost::xpressive::detail

/// INTERNAL ONLY
namespace boost { namespace xpressive
{

    /// INTERNAL ONLY
    template<typename Traits, std::size_t N>
    typename Traits::char_class_type
    lookup_classname(Traits const &traits, char const (&cname)[N], bool icase = false);

}} // namespace boost::xpressive

#endif

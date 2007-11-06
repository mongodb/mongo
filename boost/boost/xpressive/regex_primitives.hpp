///////////////////////////////////////////////////////////////////////////////
/// \file regex_primitives.hpp
/// Contains the syntax elements for writing static regular expressions.
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_REGEX_PRIMITIVES_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_REGEX_PRIMITIVES_HPP_EAN_10_04_2005

#include <climits>
#include <boost/mpl/assert.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/xpressive/proto/proto.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/core/icase.hpp>
#include <boost/xpressive/detail/core/action.hpp>
#include <boost/xpressive/detail/core/matchers.hpp>
#include <boost/xpressive/detail/static/as_xpr.hpp>
#include <boost/xpressive/detail/static/compile.hpp>
#include <boost/xpressive/detail/static/modifier.hpp>
#include <boost/xpressive/detail/static/regex_operators.hpp>
#include <boost/xpressive/detail/static/productions/productions.hpp>

namespace boost { namespace xpressive { namespace detail
{

typedef assert_word_placeholder<word_boundary<true> > assert_word_boundary;
typedef assert_word_placeholder<word_begin> assert_word_begin;
typedef assert_word_placeholder<word_end> assert_word_end;

/*
///////////////////////////////////////////////////////////////////////////////
/// INTERNAL ONLY
// BOOST_XPRESSIVE_GLOBAL
//  for defining globals that neither violate the One Definition Rule nor
//  lead to undefined behavior due to global object initialization order.
//#define BOOST_XPRESSIVE_GLOBAL(type, name, init)                                        \
//    namespace detail                                                                    \
//    {                                                                                   \
//        template<int Dummy>                                                             \
//        struct BOOST_PP_CAT(global_pod_, name)                                          \
//        {                                                                               \
//            static type const value;                                                    \
//        private:                                                                        \
//            union type_must_be_pod                                                      \
//            {                                                                           \
//                type t;                                                                 \
//                char ch;                                                                \
//            } u;                                                                        \
//        };                                                                              \
//        template<int Dummy>                                                             \
//        type const BOOST_PP_CAT(global_pod_, name)<Dummy>::value = init;                \
//    }                                                                                   \
//    type const &name = detail::BOOST_PP_CAT(global_pod_, name)<0>::value
*/

} // namespace detail

/// INTERNAL ONLY (for backwards compatibility)
unsigned int const repeat_max = UINT_MAX-1;

///////////////////////////////////////////////////////////////////////////////
/// \brief For infinite repetition of a sub-expression.
///
/// Magic value used with the repeat\<\>() function template
/// to specify an unbounded repeat. Use as: repeat<17, inf>('a').
/// The equivalent in perl is /a{17,}/.
unsigned int const inf = UINT_MAX-1;

/// INTERNAL ONLY (for backwards compatibility)
proto::op_proxy<
    proto::unary_op<detail::epsilon_matcher, proto::noop_tag>
> const epsilon = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Successfully matches nothing.
///
/// Successfully matches a zero-width sequence. nil always succeeds and
/// never consumes any characters.
proto::op_proxy<
    proto::unary_op<detail::epsilon_matcher, proto::noop_tag>
> const nil = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches an alpha-numeric character.
///
/// The regex traits are used to determine which characters are alpha-numeric.
/// To match any character that is not alpha-numeric, use ~alnum.
///
/// \attention alnum is equivalent to /[[:alnum:]]/ in perl. ~alnum is equivalent
/// to /[[:^alnum:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const alnum = {"alnum"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches an alphabetic character.
///
/// The regex traits are used to determine which characters are alphabetic.
/// To match any character that is not alphabetic, use ~alpha.
///
/// \attention alpha is equivalent to /[[:alpha:]]/ in perl. ~alpha is equivalent
/// to /[[:^alpha:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const alpha = {"alpha"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a blank (horizonal white-space) character.
///
/// The regex traits are used to determine which characters are blank characters.
/// To match any character that is not blank, use ~blank.
///
/// \attention blank is equivalent to /[[:blank:]]/ in perl. ~blank is equivalent
/// to /[[:^blank:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const blank = {"blank"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a control character.
///
/// The regex traits are used to determine which characters are control characters.
/// To match any character that is not a control character, use ~cntrl.
///
/// \attention cntrl is equivalent to /[[:cntrl:]]/ in perl. ~cntrl is equivalent
/// to /[[:^cntrl:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const cntrl = {"cntrl"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a digit character.
///
/// The regex traits are used to determine which characters are digits.
/// To match any character that is not a digit, use ~digit.
///
/// \attention digit is equivalent to /[[:digit:]]/ in perl. ~digit is equivalent
/// to /[[:^digit:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const digit = {"digit"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a graph character.
///
/// The regex traits are used to determine which characters are graphable.
/// To match any character that is not graphable, use ~graph.
///
/// \attention graph is equivalent to /[[:graph:]]/ in perl. ~graph is equivalent
/// to /[[:^graph:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const graph = {"graph"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a lower-case character.
///
/// The regex traits are used to determine which characters are lower-case.
/// To match any character that is not a lower-case character, use ~lower.
///
/// \attention lower is equivalent to /[[:lower:]]/ in perl. ~lower is equivalent
/// to /[[:^lower:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const lower = {"lower"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a printable character.
///
/// The regex traits are used to determine which characters are printable.
/// To match any character that is not printable, use ~print.
///
/// \attention print is equivalent to /[[:print:]]/ in perl. ~print is equivalent
/// to /[[:^print:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const print = {"print"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a punctuation character.
///
/// The regex traits are used to determine which characters are punctuation.
/// To match any character that is not punctuation, use ~punct.
///
/// \attention punct is equivalent to /[[:punct:]]/ in perl. ~punct is equivalent
/// to /[[:^punct:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const punct = {"punct"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a space character.
///
/// The regex traits are used to determine which characters are space characters.
/// To match any character that is not white-space, use ~space.
///
/// \attention space is equivalent to /[[:space:]]/ in perl. ~space is equivalent
/// to /[[:^space:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const space = {"space"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches an upper-case character.
///
/// The regex traits are used to determine which characters are upper-case.
/// To match any character that is not upper-case, use ~upper.
///
/// \attention upper is equivalent to /[[:upper:]]/ in perl. ~upper is equivalent
/// to /[[:^upper:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const upper = {"upper"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a hexadecimal digit character.
///
/// The regex traits are used to determine which characters are hex digits.
/// To match any character that is not a hex digit, use ~xdigit.
///
/// \attention xdigit is equivalent to /[[:xdigit:]]/ in perl. ~xdigit is equivalent
/// to /[[:^xdigit:]]/ in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const xdigit = {"xdigit"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Beginning of sequence assertion.
///
/// For the character sequence [begin, end), 'bos' matches the
/// zero-width sub-sequence [begin, begin).
proto::op_proxy<
    proto::unary_op<detail::assert_bos_matcher, proto::noop_tag>
> const bos = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief End of sequence assertion. 
///
/// For the character sequence [begin, end),
/// 'eos' matches the zero-width sub-sequence [end, end).
///
/// \attention Unlike the perl end of sequence assertion \$, 'eos' will
/// not match at the position [end-1, end-1) if *(end-1) is '\\n'. To
/// get that behavior, use (!_n >> eos).
proto::op_proxy<
    proto::unary_op<detail::assert_eos_matcher, proto::noop_tag>
> const eos = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Beginning of line assertion. 
///
/// 'bol' matches the zero-width sub-sequence
/// immediately following a logical newline sequence. The regex traits
/// is used to determine what constitutes a logical newline sequence.
proto::op_proxy<
    proto::unary_op<detail::assert_bol_placeholder, proto::noop_tag>
> const bol = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief End of line assertion.
///
/// 'eol' matches the zero-width sub-sequence
/// immediately preceeding a logical newline sequence. The regex traits
/// is used to determine what constitutes a logical newline sequence.
proto::op_proxy<
    proto::unary_op<detail::assert_eol_placeholder, proto::noop_tag>
> const eol = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Beginning of word assertion. 
///
/// 'bow' matches the zero-width sub-sequence
/// immediately following a non-word character and preceeding a word character.
/// The regex traits are used to determine what constitutes a word character.
proto::op_proxy<
    proto::unary_op<detail::assert_word_begin, proto::noop_tag>
> const bow = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief End of word assertion. 
///
/// 'eow' matches the zero-width sub-sequence
/// immediately following a word character and preceeding a non-word character.
/// The regex traits are used to determine what constitutes a word character.
proto::op_proxy<
    proto::unary_op<detail::assert_word_end, proto::noop_tag>
> const eow = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Word boundary assertion.
///
/// '_b' matches the zero-width sub-sequence at the beginning or the end of a word.
/// It is equivalent to (bow | eow). The regex traits are used to determine what
/// constitutes a word character. To match a non-word boundary, use ~_b.
///
/// \attention _b is like \\b in perl. ~_b is like \\B in perl.
proto::op_proxy<
    proto::unary_op<detail::assert_word_boundary, proto::noop_tag>
> const _b = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a word character.
///
/// '_w' matches a single word character. The regex traits are used to determine which
/// characters are word characters. Use ~_w to match a character that is not a word
/// character.
///
/// \attention _w is like \\w in perl. ~_w is like \\W in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const _w = {"w"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a digit character.
///
/// '_d' matches a single digit character. The regex traits are used to determine which
/// characters are digits. Use ~_d to match a character that is not a digit
/// character.
///
/// \attention _d is like \\d in perl. ~_d is like \\D in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const _d = {"d"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a space character.
///
/// '_s' matches a single space character. The regex traits are used to determine which
/// characters are space characters. Use ~_s to match a character that is not a space
/// character.
///
/// \attention _s is like \\s in perl. ~_s is like \\S in perl.
proto::op_proxy<
    proto::unary_op<detail::posix_charset_placeholder, proto::noop_tag>
  , char const *
> const _s = {"s"};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a literal newline character, '\\n'.
///
/// '_n' matches a single newline character, '\\n'. Use ~_n to match a character
/// that is not a newline.
///
/// \attention ~_n is like '.' in perl without the /s modifier.
proto::op_proxy<
    proto::unary_op<detail::literal_placeholder<char>, proto::noop_tag>
  , char
> const _n = {'\n'};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches a logical newline sequence.
///
/// '_ln' matches a logical newline sequence. This can be any character in the
/// line separator class, as determined by the regex traits, or the '\\r\\n' sequence.
/// For the purpose of back-tracking, '\\r\\n' is treated as a unit.
/// To match any one character that is not a logical newline, use ~_ln.
proto::op_proxy<
    detail::logical_newline_xpression
> const _ln = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Matches any one character.
///
/// Match any character, similar to '.' in perl syntax with the /s modifier.
/// '_' matches any one character, including the newline.
///
/// \attention To match any character except the newline, use ~_n
proto::op_proxy<
    proto::unary_op<detail::any_matcher, proto::noop_tag>
> const _ = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Reference to the current regex object
///
/// Useful when constructing recursive regular expression objects. The 'self'
/// identifier is a short-hand for the current regex object. For instance,
/// sregex rx = '(' >> (self | nil) >> ')'; will create a regex object that
/// matches balanced parens such as "((()))".
proto::op_proxy<
    proto::unary_op<detail::self_placeholder, proto::noop_tag>
> const self = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Used to create character sets.
///
/// There are two ways to create character sets with the 'set' identifier. The
/// easiest is to create a comma-separated list of the characters in the set,
/// as in (set= 'a','b','c'). This set will match 'a', 'b', or 'c'. The other
/// way is to define the set as an argument to the set subscript operator.
/// For instance, set[ 'a' | range('b','c') | digit ] will match an 'a', 'b',
/// 'c' or a digit character.
///
/// To complement a set, apply the '~' operator. For instance, ~(set= 'a','b','c')
/// will match any character that is not an 'a', 'b', or 'c'.
///
/// Sets can be composed of other, possibly complemented, sets. For instance,
/// set[ ~digit | ~(set= 'a','b','c') ].
proto::op_proxy<
    detail::set_initializer_type
> const set = {};

///////////////////////////////////////////////////////////////////////////////
/// \brief Sub-match placeholder, like $& in Perl
proto::op_proxy<detail::mark_tag, int> const s0 = {0};

///////////////////////////////////////////////////////////////////////////////
/// \brief Sub-match placeholder, like $1 in perl.
///
/// To create a sub-match, assign a sub-expression to the sub-match placeholder.
/// For instance, (s1= _) will match any one character and remember which
/// character was matched in the 1st sub-match. Later in the pattern, you can
/// refer back to the sub-match. For instance,  (s1= _) >> s1  will match any
/// character, and then match the same character again.
///
/// After a successful regex_match() or regex_search(), the sub-match placeholders
/// can be used to index into the match_results\<\> object to retrieve the Nth
/// sub-match.
proto::op_proxy<detail::mark_tag, int> const s1 = {1};
proto::op_proxy<detail::mark_tag, int> const s2 = {2};
proto::op_proxy<detail::mark_tag, int> const s3 = {3};
proto::op_proxy<detail::mark_tag, int> const s4 = {4};
proto::op_proxy<detail::mark_tag, int> const s5 = {5};
proto::op_proxy<detail::mark_tag, int> const s6 = {6};
proto::op_proxy<detail::mark_tag, int> const s7 = {7};
proto::op_proxy<detail::mark_tag, int> const s8 = {8};
proto::op_proxy<detail::mark_tag, int> const s9 = {9};

// NOTE: For the purpose of xpressive's documentation, make icase() look like an
// ordinary function. In reality, it is a function object defined in detail/icase.hpp
// so that it can serve double-duty as regex_constants::icase, the syntax_option_type.
// Do the same for as_xpr(), which is actually defined in detail/static/as_xpr.hpp
#ifdef BOOST_XPRESSIVE_DOXYGEN_INVOKED
///////////////////////////////////////////////////////////////////////////////
/// \brief Makes a literal into a regular expression.
///
/// Use as_xpr() to turn a literal into a regular expression. For instance,
/// "foo" >> "bar" will not compile because both operands to the right-shift
/// operator are const char*, and no such operator exists. Use as_xpr("foo") >> "bar"
/// instead.
///
/// You can use as_xpr() with character literals in addition to string literals.
/// For instance, as_xpr('a') will match an 'a'. You can also complement a
/// character literal, as with ~as_xpr('a'). This will match any one character
/// that is not an 'a'.
template<typename Literal>
inline typename detail::as_xpr_type<Literal>::const_reference
as_xpr(Literal const &literal)
{
    return detail::as_xpr_type<Literal>::call(xpr);
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Makes a sub-expression case-insensitive.
///
/// Use icase() to make a sub-expression case-insensitive. For instance,
/// "foo" >> icase(set['b'] >> "ar") will match "foo" exactly followed by
/// "bar" irrespective of case.
template<typename Xpr>
inline proto::binary_op<detail::icase_modifier, typename detail::as_xpr_type<Xpr>::type, modifier_tag> const
icase(Xpr const &xpr)
{
    detail::icase_modifier mod;
    return proto::make_op<modifier_tag>(mod, as_xpr(xpr));
}
#endif

///////////////////////////////////////////////////////////////////////////////
/// \brief Embed a regex object by reference.
///
/// \param rex The basic_regex object to embed by reference.
template<typename BidiIter>
inline proto::unary_op<detail::regex_placeholder<BidiIter, true>, proto::noop_tag> const
by_ref(basic_regex<BidiIter> const &rex)
{
    typedef detail::core_access<BidiIter> access;
    shared_ptr<detail::regex_impl<BidiIter> > impl = access::get_regex_impl(rex);
    return proto::noop(detail::regex_placeholder<BidiIter, true>(impl));
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Match a range of characters.
///
/// Match any character in the range [ch_min, ch_max].
///
/// \param ch_min The lower end of the range to match.
/// \param ch_max The upper end of the range to match.
template<typename Char>
inline proto::unary_op<detail::range_placeholder<Char>, proto::noop_tag> const
range(Char ch_min, Char ch_max)
{
    return proto::noop(detail::range_placeholder<Char>(ch_min, ch_max));
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Make a sub-expression optional. Equivalent to !as_xpr(xpr).
///
/// \param xpr The sub-expression to make optional.
template<typename Xpr>
inline proto::unary_op
<
    typename detail::as_xpr_type<Xpr>::type
  , proto::logical_not_tag
> const
optional(Xpr const &xpr)
{
    return !as_xpr(xpr);
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Repeat a sub-expression multiple times.
///
/// There are two forms of the repeat\<\>() function template. To match a
/// sub-expression N times, use repeat\<N\>(xpr). To match a sub-expression
/// from M to N times, use repeat\<M,N\>(xpr).
///
/// The repeat\<\>() function creates a greedy quantifier. To make the quantifier
/// non-greedy, apply the unary minus operator, as in -repeat\<M,N\>(xpr).
///
/// \param xpr The sub-expression to repeat.
template<unsigned int Min, unsigned int Max, typename Xpr>
inline proto::unary_op
<
    typename detail::as_xpr_type<Xpr>::type
  , detail::generic_quant_tag<Min, Max>
> const
repeat(Xpr const &xpr)
{
    return proto::make_op<detail::generic_quant_tag<Min, Max> >(as_xpr(xpr));
}

/// \overload
template<unsigned int Count, typename Xpr2>
inline proto::unary_op
<
    typename detail::as_xpr_type<Xpr2>::type
  , detail::generic_quant_tag<Count, Count>
> const
repeat(Xpr2 const &xpr)
{
    return proto::make_op<detail::generic_quant_tag<Count, Count> >(as_xpr(xpr));
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Create an independent sub-expression.
///
/// Turn off back-tracking for a sub-expression. Any branches or repeats within
/// the sub-expression will match only one way, and no other alternatives are
/// tried.
///
/// \attention keep(xpr) is equivalent to the perl (?>...) extension.
///
/// \param xpr The sub-expression to modify.
template<typename Xpr>
inline proto::unary_op
<
    typename detail::as_xpr_type<Xpr>::type
  , detail::keeper_tag
> const
keep(Xpr const &xpr)
{
    return proto::make_op<detail::keeper_tag>(as_xpr(xpr));
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Look-ahead assertion.
///
/// before(xpr) succeeds if the xpr sub-expression would match at the current
/// position in the sequence, but xpr is not included in the match. For instance,
/// before("foo") succeeds if we are before a "foo". Look-ahead assertions can be
/// negated with the bit-compliment operator.
///
/// \attention before(xpr) is equivalent to the perl (?=...) extension.
/// ~before(xpr) is a negative look-ahead assertion, equivalent to the
/// perl (?!...) extension.
///
/// \param xpr The sub-expression to put in the look-ahead assertion.
template<typename Xpr>
inline proto::unary_op
<
    typename detail::as_xpr_type<Xpr>::type
  , detail::lookahead_tag<true>
> const
before(Xpr const &xpr)
{
    return proto::make_op<detail::lookahead_tag<true> >(as_xpr(xpr));
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Look-behind assertion.
///
/// after(xpr) succeeds if the xpr sub-expression would match at the current
/// position minus N in the sequence, where N is the width of xpr. xpr is not included in
/// the match. For instance,  after("foo") succeeds if we are after a "foo". Look-behind
/// assertions can be negated with the bit-complement operator.
///
/// \attention after(xpr) is equivalent to the perl (?<=...) extension.
/// ~after(xpr) is a negative look-behind assertion, equivalent to the
/// perl (?<!...) extension.
///
/// \param xpr The sub-expression to put in the look-ahead assertion.
///
/// \pre xpr cannot match a variable number of characters.
template<typename Xpr>
inline proto::unary_op
<
    typename detail::as_xpr_type<Xpr>::type
  , detail::lookbehind_tag<true>
> const
after(Xpr const &xpr)
{
    return proto::make_op<detail::lookbehind_tag<true> >(as_xpr(xpr));
}

///////////////////////////////////////////////////////////////////////////////
/// \brief Specify a regex traits or a std::locale.
///
/// imbue() instructs the regex engine to use the specified traits or locale
/// when matching the regex. The entire expression must use the same traits/locale.
/// For instance, the following specifies a locale for use with a regex:
///   std::locale loc;
///   sregex rx = imbue(loc)(+digit);
///
/// \param loc The std::locale or regex traits object.
template<typename Locale>
inline detail::modifier_op<detail::locale_modifier<Locale> > const
imbue(Locale const &loc)
{
    detail::modifier_op<detail::locale_modifier<Locale> > mod =
    {
        detail::locale_modifier<Locale>(loc)
      , regex_constants::ECMAScript
    };
    return mod;
}

}} // namespace boost::xpressive

#endif

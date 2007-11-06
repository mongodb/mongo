///////////////////////////////////////////////////////////////////////////////
// detail/dynamic/parser_traits.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_DYNAMIC_PARSER_TRAITS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_DYNAMIC_PARSER_TRAITS_HPP_EAN_10_04_2005

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <string>
#include <climits>
#include <boost/assert.hpp>
#include <boost/xpressive/regex_error.hpp>
#include <boost/xpressive/regex_traits.hpp>
#include <boost/xpressive/detail/detail_fwd.hpp>
#include <boost/xpressive/detail/dynamic/matchable.hpp>
#include <boost/xpressive/detail/dynamic/parser_enum.hpp>
#include <boost/xpressive/detail/utility/literals.hpp>
#include <boost/xpressive/detail/utility/algorithm.hpp>

namespace boost { namespace xpressive
{

///////////////////////////////////////////////////////////////////////////////
// compiler_traits
//  this works for char and wchar_t. it must be specialized for anything else.
//
template<typename RegexTraits>
struct compiler_traits
{
    typedef typename RegexTraits::char_type char_type;
    typedef std::basic_string<char_type> string_type;
    typedef typename string_type::const_iterator iterator_type;
    typedef RegexTraits regex_traits;
    typedef typename RegexTraits::locale_type locale_type;

    ///////////////////////////////////////////////////////////////////////////////
    // constructor
    explicit compiler_traits(RegexTraits const &traits = RegexTraits())
      : traits_(traits)
      , flags_(regex_constants::ECMAScript)
      , space_(lookup_classname(traits_, "space"))
    {
        BOOST_ASSERT(0 != this->space_);
    }

    ///////////////////////////////////////////////////////////////////////////////
    // flags
    regex_constants::syntax_option_type flags() const
    {
        return this->flags_;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // flags
    void flags(regex_constants::syntax_option_type flags)
    {
        this->flags_ = flags;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // traits
    regex_traits &traits()
    {
        return this->traits_;
    }

    regex_traits const &traits() const
    {
        return this->traits_;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // imbue
    locale_type imbue(locale_type const &loc)
    {
        locale_type oldloc = this->traits().imbue(loc);
        this->space_ = lookup_classname(this->traits(), "space");
        BOOST_ASSERT(0 != this->space_);
        return oldloc;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // getloc
    locale_type getloc() const
    {
        return this->traits().getloc();
    }

    ///////////////////////////////////////////////////////////////////////////////
    // get_token
    //  get a token and advance the iterator
    regex_constants::compiler_token_type get_token(iterator_type &begin, iterator_type end)
    {
        using namespace regex_constants;
        if(this->eat_ws_(begin, end) == end)
        {
            return regex_constants::token_end_of_pattern;
        }

        switch(*begin)
        {
        case BOOST_XPR_CHAR_(char_type, '\\'): return this->get_escape_token(++begin, end);
        case BOOST_XPR_CHAR_(char_type, '.'): ++begin; return token_any;
        case BOOST_XPR_CHAR_(char_type, '^'): ++begin; return token_assert_begin_line;
        case BOOST_XPR_CHAR_(char_type, '$'): ++begin; return token_assert_end_line;
        case BOOST_XPR_CHAR_(char_type, '('): ++begin; return token_group_begin;
        case BOOST_XPR_CHAR_(char_type, ')'): ++begin; return token_group_end;
        case BOOST_XPR_CHAR_(char_type, '|'): ++begin; return token_alternate;
        case BOOST_XPR_CHAR_(char_type, '['): ++begin; return token_charset_begin;
        case BOOST_XPR_CHAR_(char_type, ']'): ++begin; return token_charset_end;

        case BOOST_XPR_CHAR_(char_type, '*'):
        case BOOST_XPR_CHAR_(char_type, '+'):
        case BOOST_XPR_CHAR_(char_type, '?'):
            return token_invalid_quantifier;

        case BOOST_XPR_CHAR_(char_type, '{'):
        default:
            return token_literal;
        }
    }

    ///////////////////////////////////////////////////////////////////////////////
    // get_quant_spec
    bool get_quant_spec(iterator_type &begin, iterator_type end, detail::quant_spec &spec)
    {
        using namespace regex_constants;
        iterator_type old_begin;

        if(this->eat_ws_(begin, end) == end)
        {
            return false;
        }

        switch(*begin)
        {
        case BOOST_XPR_CHAR_(char_type, '*'):
            spec.min_ = 0;
            spec.max_ = (std::numeric_limits<unsigned int>::max)();
            break;

        case BOOST_XPR_CHAR_(char_type, '+'):
            spec.min_ = 1;
            spec.max_ = (std::numeric_limits<unsigned int>::max)();
            break;

        case BOOST_XPR_CHAR_(char_type, '?'):
            spec.min_ = 0;
            spec.max_ = 1;
            break;

        case BOOST_XPR_CHAR_(char_type, '{'):
            old_begin = this->eat_ws_(++begin, end);
            spec.min_ = spec.max_ = detail::toi(begin, end, this->traits());
            detail::ensure
            (
                begin != old_begin && begin != end, error_brace, "invalid quantifier"
            );

            if(*begin == BOOST_XPR_CHAR_(char_type, ','))
            {
                old_begin = this->eat_ws_(++begin, end);
                spec.max_ = detail::toi(begin, end, this->traits());
                detail::ensure
                (
                    begin != end && BOOST_XPR_CHAR_(char_type, '}') == *begin
                  , error_brace, "invalid quantifier"
                );

                if(begin == old_begin)
                {
                    spec.max_ = (std::numeric_limits<unsigned int>::max)();
                }
                else
                {
                    detail::ensure
                    (
                        spec.min_ <= spec.max_, error_badbrace, "invalid quantification range"
                    );
                }
            }
            else
            {
                detail::ensure
                (
                    BOOST_XPR_CHAR_(char_type, '}') == *begin, error_brace, "invalid quantifier"
                );
            }
            break;

        default:
            return false;
        }

        spec.greedy_ = true;
        if(this->eat_ws_(++begin, end) != end && BOOST_XPR_CHAR_(char_type, '?') == *begin)
        {
            ++begin;
            spec.greedy_ = false;
        }

        return true;
    }

    ///////////////////////////////////////////////////////////////////////////
    // get_group_type
    regex_constants::compiler_token_type get_group_type(iterator_type &begin, iterator_type end)
    {
        using namespace regex_constants;
        if(this->eat_ws_(begin, end) != end && BOOST_XPR_CHAR_(char_type, '?') == *begin)
        {
            this->eat_ws_(++begin, end);
            detail::ensure(begin != end, error_paren, "incomplete extension");

            switch(*begin)
            {
            case BOOST_XPR_CHAR_(char_type, ':'): ++begin; return token_no_mark;
            case BOOST_XPR_CHAR_(char_type, '>'): ++begin; return token_independent_sub_expression;
            case BOOST_XPR_CHAR_(char_type, '#'): ++begin; return token_comment;
            case BOOST_XPR_CHAR_(char_type, '='): ++begin; return token_positive_lookahead;
            case BOOST_XPR_CHAR_(char_type, '!'): ++begin; return token_negative_lookahead;
            case BOOST_XPR_CHAR_(char_type, '<'):
                this->eat_ws_(++begin, end);
                detail::ensure(begin != end, error_paren, "incomplete extension");
                switch(*begin)
                {
                case BOOST_XPR_CHAR_(char_type, '='): ++begin; return token_positive_lookbehind;
                case BOOST_XPR_CHAR_(char_type, '!'): ++begin; return token_negative_lookbehind;
                default:
                    throw regex_error(error_badbrace, "unrecognized extension");
                }

            case BOOST_XPR_CHAR_(char_type, 'i'):
            case BOOST_XPR_CHAR_(char_type, 'm'):
            case BOOST_XPR_CHAR_(char_type, 's'):
            case BOOST_XPR_CHAR_(char_type, 'x'):
            case BOOST_XPR_CHAR_(char_type, '-'):
                return this->parse_mods_(begin, end);

            default:
                throw regex_error(error_badbrace, "unrecognized extension");
            }
        }

        return token_literal;
    }

    //////////////////////////////////////////////////////////////////////////
    // get_charset_token
    //  NOTE: white-space is *never* ignored in a charset.
    regex_constants::compiler_token_type get_charset_token(iterator_type &begin, iterator_type end)
    {
        using namespace regex_constants;
        BOOST_ASSERT(begin != end);
        switch(*begin)
        {
        case BOOST_XPR_CHAR_(char_type, '^'): ++begin; return token_charset_invert;
        case BOOST_XPR_CHAR_(char_type, '-'): ++begin; return token_charset_hyphen;
        case BOOST_XPR_CHAR_(char_type, ']'): ++begin; return token_charset_end;
        case BOOST_XPR_CHAR_(char_type, '['):
            {
                iterator_type next = begin; ++next;
                if(next != end && *next == BOOST_XPR_CHAR_(char_type, ':'))
                {
                    begin = ++next;
                    return token_posix_charset_begin;
                }
            }
            break;
        case BOOST_XPR_CHAR_(char_type, ':'):
            {
                iterator_type next = begin; ++next;
                if(next != end && *next == BOOST_XPR_CHAR_(char_type, ']'))
                {
                    begin = ++next;
                    return token_posix_charset_end;
                }
            }
            break;
        case BOOST_XPR_CHAR_(char_type, '\\'):
            if(++begin != end)
            {
                switch(*begin)
                {
                case BOOST_XPR_CHAR_(char_type, 'b'): ++begin; return token_charset_backspace;
                default:;
                }
            }
            return token_escape;
        default:;
        }
        return token_literal;
    }

    //////////////////////////////////////////////////////////////////////////
    // get_escape_token
    regex_constants::compiler_token_type get_escape_token(iterator_type &begin, iterator_type end)
    {
        using namespace regex_constants;
        if(begin != end)
        {
            switch(*begin)
            {
            //case BOOST_XPR_CHAR_(char_type, 'a'): ++begin; return token_escape_bell;
            //case BOOST_XPR_CHAR_(char_type, 'c'): ++begin; return token_escape_control;
            //case BOOST_XPR_CHAR_(char_type, 'e'): ++begin; return token_escape_escape;
            //case BOOST_XPR_CHAR_(char_type, 'f'): ++begin; return token_escape_formfeed;
            //case BOOST_XPR_CHAR_(char_type, 'n'): ++begin; return token_escape_newline;
            //case BOOST_XPR_CHAR_(char_type, 't'): ++begin; return token_escape_horizontal_tab;
            //case BOOST_XPR_CHAR_(char_type, 'v'): ++begin; return token_escape_vertical_tab;
            case BOOST_XPR_CHAR_(char_type, 'A'): ++begin; return token_assert_begin_sequence;
            case BOOST_XPR_CHAR_(char_type, 'b'): ++begin; return token_assert_word_boundary;
            case BOOST_XPR_CHAR_(char_type, 'B'): ++begin; return token_assert_not_word_boundary;
            case BOOST_XPR_CHAR_(char_type, 'E'): ++begin; return token_quote_meta_end;
            case BOOST_XPR_CHAR_(char_type, 'Q'): ++begin; return token_quote_meta_begin;
            case BOOST_XPR_CHAR_(char_type, 'Z'): ++begin; return token_assert_end_sequence;
            // Non-standard extension to ECMAScript syntax
            case BOOST_XPR_CHAR_(char_type, '<'): ++begin; return token_assert_word_begin;
            case BOOST_XPR_CHAR_(char_type, '>'): ++begin; return token_assert_word_end;
            default:; // fall-through
            }
        }

        return token_escape;
    }

private:

    //////////////////////////////////////////////////////////////////////////
    // parse_mods_
    regex_constants::compiler_token_type parse_mods_(iterator_type &begin, iterator_type end)
    {
        using namespace regex_constants;
        bool set = true;
        do switch(*begin)
        {
        case BOOST_XPR_CHAR_(char_type, 'i'): this->flag_(set, icase_); break;
        case BOOST_XPR_CHAR_(char_type, 'm'): this->flag_(!set, single_line); break;
        case BOOST_XPR_CHAR_(char_type, 's'): this->flag_(!set, not_dot_newline); break;
        case BOOST_XPR_CHAR_(char_type, 'x'): this->flag_(set, ignore_white_space); break;
        case BOOST_XPR_CHAR_(char_type, ':'): ++begin; // fall-through
        case BOOST_XPR_CHAR_(char_type, ')'): return token_no_mark;
        case BOOST_XPR_CHAR_(char_type, '-'): if(false == (set = !set)) break; // else fall-through
        default: throw regex_error(error_paren, "unknown pattern modifier");
        }
        while(detail::ensure(++begin != end, error_paren, "incomplete extension"));
        return token_no_mark;
    }

    ///////////////////////////////////////////////////////////////////////////////
    // flag_
    void flag_(bool set, regex_constants::syntax_option_type flag)
    {
        this->flags_ = set ? (this->flags_ | flag) : (this->flags_ & ~flag);
    }

    ///////////////////////////////////////////////////////////////////////////
    // is_space_
    bool is_space_(char_type ch) const
    {
        return this->traits().isctype(ch, this->space_);
    }

    ///////////////////////////////////////////////////////////////////////////////
    // eat_ws_
    iterator_type &eat_ws_(iterator_type &begin, iterator_type end)
    {
        if(0 != (regex_constants::ignore_white_space & this->flags()))
        {
            while(end != begin && (BOOST_XPR_CHAR_(char_type, '#') == *begin || this->is_space_(*begin)))
            {
                if(BOOST_XPR_CHAR_(char_type, '#') == *begin++)
                {
                    while(end != begin && BOOST_XPR_CHAR_(char_type, '\n') != *begin++) {}
                }
                else
                {
                    for(; end != begin && this->is_space_(*begin); ++begin) {}
                }
            }
        }

        return begin;
    }

    regex_traits traits_;
    regex_constants::syntax_option_type flags_;
    typename regex_traits::char_class_type space_;
};

}} // namespace boost::xpressive

#endif

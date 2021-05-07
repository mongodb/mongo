/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   named_scope_format_parser.cpp
 * \author Andrey Semashev
 * \date   14.11.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <limits>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/core.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/spirit/include/karma_uint.hpp>
#include <boost/spirit/include/karma_generate.hpp>
#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/expressions/formatters/named_scope.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/detail/header.hpp>

namespace karma = boost::spirit::karma;

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace expressions {

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! The function skips any spaces from the current position
BOOST_FORCEINLINE const char* skip_spaces(const char* p, const char* end)
{
    while (p < end && *p == ' ')
        ++p;
    return p;
}

//! The function checks if the given character can be part of a function/type/namespace name
BOOST_FORCEINLINE bool is_name_character(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z');
}

//! The function checks if there is 'operator' keyword at the specified position
BOOST_FORCEINLINE bool is_operator_keyword(const char* p)
{
    return std::memcmp(p, "operator", 8) == 0;
}

//! The function tries to parse operator signature
bool detect_operator(const char* begin, const char* end, const char* operator_keyword, const char*& operator_end)
{
    if (end - operator_keyword < 9 || !is_operator_keyword(operator_keyword))
        return false;
    // Check that it's not a function name ending with 'operator', like detect_operator
    if (operator_keyword > begin && is_name_character(*(operator_keyword - 1)))
        return false;

    const char* p = skip_spaces(operator_keyword + 8, end);
    if (p == end)
        return false;

    // Check to see where the operator token ends
    switch (*p)
    {
    case '(':
        // Handle operator()
        p = skip_spaces(++p, end);
        if (p < end && *p == ')')
        {
            operator_end = p + 1;
            return true;
        }

        return false;

    case '[':
        // Handle operator[]
        p = skip_spaces(++p, end);
        if (p < end && *p == ']')
        {
            operator_end = p + 1;
            return true;
        }

        return false;

    case '>':
    case '<':
        // Handle operator<=, operator>=, operator<<, operator>>, operator<<=, operator>>=
        if (end - p >= 3 && (p[0] == p[1] && p[2] == '='))
            operator_end = p + 3;
        else if (end - p >= 2 && (p[0] == p[1] || p[1] == '='))
            operator_end = p + 2;
        else
            operator_end = p + 1;

        return true;

    case '-':
        // Handle operator->, operator->*
        if (end - p >= 2 && p[1] == '>')
        {
            if (end - p >= 3 && p[2] == '*')
                operator_end = p + 3;
            else
                operator_end = p + 2;

            return true;
        }
        // Fall through to other cases involving '-'

    case '=':
    case '|':
    case '&':
    case '+':
        // Handle operator=, operator==, operator+=, operator++, operator||, opeartor&&, etc.
        if (end - p >= 2 && (p[0] == p[1] || p[1] == '='))
            operator_end = p + 2;
        else
            operator_end = p + 1;

        return true;

    case '*':
    case '/':
    case '%':
    case '^':
        // Handle operator*, operator*=, etc.
        if (end - p >= 2 && p[1] == '=')
            operator_end = p + 2;
        else
            operator_end = p + 1;

        return true;

    case ',':
    case '~':
    case '!':
        // Handle operator,, operator~, etc.
        operator_end = p + 1;
        return true;

    case '"':
        // Handle operator""
        if (end - p >= 2 && p[0] == p[1])
        {
            p = skip_spaces(p + 2, end);
            // Skip through the literal suffix
            while (p < end && is_name_character(*p))
                ++p;
            operator_end = p;
            return true;
        }

        return false;

    default:
        // Handle type conversion operators. We can't find the end of the type reliably here.
        operator_end = p;
        return true;
    }
}

//! The function skips all template parameters
inline const char* skip_template_parameters(const char* begin, const char* end)
{
    unsigned int depth = 1;
    const char* p = begin;
    while (depth > 0 && p != end)
    {
        switch (*p)
        {
        case '>':
            --depth;
            break;

        case '<':
            ++depth;
            break;

        case 'o':
            {
                // Skip operators (e.g. when an operator is a non-type template parameter)
                const char* operator_end;
                if (detect_operator(begin, end, p, operator_end))
                {
                    p = operator_end;
                    continue;
                }
            }
            break;

        default:
            break;
        }

        ++p;
    }

    return p;
}

//! The function seeks for the opening parenthesis and also tries to find the function name beginning
inline const char* find_opening_parenthesis(const char* begin, const char* end, const char*& first_name_begin, const char*& last_name_begin)
{
    enum sequence_state
    {
        not_started,      // no significant (non-space) characters have been encountered so far
        started,          // some name has started; the name is a contiguous sequence of characters that may constitute a function or scope name
        continued,        // the previous characters were the scope operator ("::"), so the name is not finished yet
        ended,            // the name has ended; in particular, this means that there were significant characters previously in the string
        operator_detected // operator has been found in the string, don't parse for scopes anymore; this is needed for conversion operators
    };
    sequence_state state = not_started;

    const char* p = begin;
    while (p != end)
    {
        char c = *p;
        switch (c)
        {
        case '(':
            if (state == not_started)
            {
                // If the opening brace is the first meaningful character in the string then this can't be a function signature.
                // Pretend we didn't find the paranthesis to fail the parsing process.
                return end;
            }
            return p;

        case '<':
            if (state == not_started)
            {
                // Template parameters cannot start as the first meaningful character in the signature.
                // Pretend we didn't find the paranthesis to fail the parsing process.
                return end;
            }
            p = skip_template_parameters(p + 1, end);
            if (state != operator_detected)
                state = ended;
            continue;

        case ' ':
            if (state == started)
                state = ended;
            break;

        case ':':
            ++p;
            if (p != end && *p == ':')
            {
                if (state == not_started)
                {
                    // Include the starting "::" in the full name
                    first_name_begin = p - 1;
                }
                if (state != operator_detected)
                    state = continued;
                ++p;
            }
            else if (state != operator_detected)
            {
                // Weird case, a single colon. Maybe, some compilers would put things like "public:" in front of the signature.
                state = ended;
            }
            continue;

        case 'o':
            {
                const char* operator_end;
                if (detect_operator(begin, end, p, operator_end))
                {
                    if (state == not_started || state == ended)
                        first_name_begin = p;
                    last_name_begin = p;
                    p = operator_end;
                    state = operator_detected;
                    continue;
                }
            }
            // Fall through to process this character as other characters

        default:
            if (state != operator_detected)
            {
                if (is_name_character(c) || c == '~') // check for '~' in case of a destructor
                {
                    if (state != started)
                    {
                        if (state == not_started || state == ended)
                            first_name_begin = p;
                        last_name_begin = p;
                        state = started;
                    }
                }
                else
                {
                    state = ended;
                }
            }
            break;
        }

        ++p;
    }

    return p;
}

//! The function seeks for the closing parenthesis
inline const char* find_closing_parenthesis(const char* begin, const char* end, char& first_char)
{
    bool found_first_meaningful_char = false;
    unsigned int depth = 1;
    const char* p = begin;
    while (p != end)
    {
        char c = *p;
        switch (c)
        {
        case ')':
            --depth;
            if (depth == 0)
                return p;
            break;

        case '(':
            ++depth;
            break;

        case '<':
            p = skip_template_parameters(p + 1, end);
            continue;

        case 'o':
            {
                const char* operator_end;
                if (detect_operator(begin, end, p, operator_end))
                {
                    p = operator_end;
                    continue;
                }
            }
            // Fall through to process this character as other characters

        default:
            if (!found_first_meaningful_char && c != ' ')
            {
                found_first_meaningful_char = true;
                first_char = c;
            }
            break;
        }

        ++p;
    }

    return p;
}

bool parse_function_name(const char*& begin, const char*& end, bool include_scope)
{
    // The algorithm tries to match several patterns to recognize function signatures. The most obvious is:
    //
    // A B(C)
    //
    // or just:
    //
    // B(C)
    //
    // in case of constructors, destructors and type conversion operators. The algorithm looks for the opening parenthesis and while doing that
    // it detects the beginning of B. As a result B is the function name.
    //
    // The first significant complication is function and array return types, in which case the syntax becomes nested:
    //
    // A (*B(C))(D)
    // A (&B(C))[D]
    //
    // In addition to that MSVC adds calling convention, such as __cdecl, to function types. In order to detect these cases the algorithm
    // seeks for the closing parenthesis after the opening one. If there is an opening parenthesis or square bracket after the closing parenthesis
    // then this is a function or array return type. The case of arrays is additionally complicated by GCC output:
    //
    // A B(C) [D]
    //
    // where D is template parameters description and is not part of the signature. To discern this special case from the array return type, the algorithm
    // checks for the first significant character within the parenthesis. This character is '&' in case of arrays and something else otherwise.
    //
    // Speaking of template parameters, the parsing algorithm ignores them completely, assuming they are part of the name being parsed. This includes
    // any possible parenthesis, nested template parameters and even operators, which may be present there as non-type template parameters.
    //
    // Operators pose another problem. This is especially the case for type conversion operators, and even more so for conversion operators to
    // function types. In this latter case at least MSVC is known to produce incomprehensible strings which we cannot parse. In other cases it is
    // too difficult to parse the type correctly. So we cheat a little. Whenever we find "operator", we know that we've found the function name
    // already, and the name ends at the opening parenthesis. For other operators we are able to parse them correctly but that doesn't really matter.
    //
    // Note that the algorithm should be tolerant to different flavors of the input strings from different compilers, so we can't rely on spaces
    // delimiting function names and other elements. Also, the algorithm should behave well in case of the fallback string generated by
    // BOOST_CURRENT_FUNCTION (which is "(unknown)" currently). In case of any parsing failure the algorithm should return false, in which case the
    // full original string will be used as the output.

    const char* b = begin;
    const char* e = end;
    while (b != e)
    {
        // Find the opening parenthesis. While looking for it, also find the function name.
        // first_name_begin is the beginning of the function scope, last_name_begin is the actual function name.
        const char* first_name_begin = NULL, *last_name_begin = NULL;
        const char* paren_open = find_opening_parenthesis(b, e, first_name_begin, last_name_begin);
        if (paren_open == e)
            return false;
        // Find the closing parenthesis. Also peek at the first character in the parenthesis, which we'll use to detect array return types.
        char first_char_in_parenthesis = 0;
        const char* paren_close = find_closing_parenthesis(paren_open + 1, e, first_char_in_parenthesis);
        if (paren_close == e)
            return false;

        const char* p = skip_spaces(paren_close + 1, e);

        // Detect function and array return types
        if (p < e && (*p == '(' || (*p == '[' && first_char_in_parenthesis == '&')))
        {
            // This is a function or array return type, the actual function name is within the parenthesis.
            // Re-parse the string within the parenthesis as a function signature.
            b = paren_open + 1;
            e = paren_close;
            continue;
        }

        // We found something that looks like a function signature
        if (include_scope)
        {
            if (!first_name_begin)
                return false;

            begin = first_name_begin;
        }
        else
        {
            if (!last_name_begin)
                return false;

            begin = last_name_begin;
        }

        end = paren_open;

        return true;
    }

    return false;
}

template< typename CharT >
class named_scope_formatter
{
    BOOST_COPYABLE_AND_MOVABLE_ALT(named_scope_formatter)

public:
    typedef void result_type;

    typedef CharT char_type;
    typedef std::basic_string< char_type > string_type;
    typedef basic_formatting_ostream< char_type > stream_type;
    typedef attributes::named_scope::value_type::value_type value_type;

    struct literal
    {
        typedef void result_type;

        explicit literal(string_type& lit) { m_literal.swap(lit); }

        result_type operator() (stream_type& strm, value_type const&) const
        {
            strm << m_literal;
        }

    private:
        string_type m_literal;
    };

    struct scope_name
    {
        typedef void result_type;

        result_type operator() (stream_type& strm, value_type const& value) const
        {
            strm << value.scope_name;
        }
    };

    struct function_name
    {
        typedef void result_type;

        explicit function_name(bool include_scope) : m_include_scope(include_scope)
        {
        }

        result_type operator() (stream_type& strm, value_type const& value) const
        {
            if (value.type == attributes::named_scope_entry::function)
            {
                const char* begin = value.scope_name.c_str();
                const char* end = begin + value.scope_name.size();
                if (parse_function_name(begin, end, m_include_scope))
                {
                    strm.write(begin, end - begin);
                    return;
                }
            }

            strm << value.scope_name;
        }

    private:
        const bool m_include_scope;
    };

    struct full_file_name
    {
        typedef void result_type;

        result_type operator() (stream_type& strm, value_type const& value) const
        {
            strm << value.file_name;
        }
    };

    struct file_name
    {
        typedef void result_type;

        result_type operator() (stream_type& strm, value_type const& value) const
        {
            std::size_t n = value.file_name.size(), i = n;
            for (; i > 0; --i)
            {
                const char c = value.file_name[i - 1];
#if defined(BOOST_WINDOWS)
                if (c == '\\')
                    break;
#endif
                if (c == '/')
                    break;
            }
            strm.write(value.file_name.c_str() + i, n - i);
        }
    };

    struct line_number
    {
        typedef void result_type;

        result_type operator() (stream_type& strm, value_type const& value) const
        {
            strm.flush();

            char_type buf[std::numeric_limits< unsigned int >::digits10 + 2];
            char_type* p = buf;

            typedef karma::uint_generator< unsigned int, 10 > uint_gen;
            karma::generate(p, uint_gen(), value.line);

            typedef typename stream_type::streambuf_type streambuf_type;
            static_cast< streambuf_type* >(strm.rdbuf())->append(buf, static_cast< std::size_t >(p - buf));
        }
    };

private:
    typedef boost::log::aux::light_function< void (stream_type&, value_type const&) > formatter_type;
    typedef std::vector< formatter_type > formatters;

private:
    formatters m_formatters;

public:
    BOOST_DEFAULTED_FUNCTION(named_scope_formatter(), {})
    named_scope_formatter(named_scope_formatter const& that) : m_formatters(that.m_formatters) {}
    named_scope_formatter(BOOST_RV_REF(named_scope_formatter) that) { m_formatters.swap(that.m_formatters); }

    named_scope_formatter& operator= (named_scope_formatter that)
    {
        this->swap(that);
        return *this;
    }

    result_type operator() (stream_type& strm, value_type const& value) const
    {
        for (typename formatters::const_iterator it = m_formatters.begin(), end = m_formatters.end(); strm.good() && it != end; ++it)
        {
            (*it)(strm, value);
        }
    }

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
    template< typename FunT >
    void add_formatter(FunT&& fun)
    {
        m_formatters.emplace_back(boost::forward< FunT >(fun));
    }
#else
    template< typename FunT >
    void add_formatter(FunT const& fun)
    {
        m_formatters.push_back(formatter_type(fun));
    }
#endif

    void swap(named_scope_formatter& that)
    {
        m_formatters.swap(that.m_formatters);
    }
};

//! Parses the named scope format string and constructs the formatter function
template< typename CharT >
BOOST_FORCEINLINE boost::log::aux::light_function< void (basic_formatting_ostream< CharT >&, attributes::named_scope::value_type::value_type const&) >
do_parse_named_scope_format(const CharT* begin, const CharT* end)
{
    typedef CharT char_type;
    typedef boost::log::aux::light_function< void (basic_formatting_ostream< char_type >&, attributes::named_scope::value_type::value_type const&) > result_type;
    typedef named_scope_formatter< char_type > formatter_type;
    formatter_type fmt;

    std::basic_string< char_type > literal;

    while (begin != end)
    {
        const char_type* p = std::find(begin, end, static_cast< char_type >('%'));
        literal.append(begin, p);

        if ((end - p) >= 2)
        {
            switch (p[1])
            {
            case '%':
                literal.push_back(static_cast< char_type >('%'));
                break;

            case 'n':
                if (!literal.empty())
                    fmt.add_formatter(typename formatter_type::literal(literal));
                fmt.add_formatter(typename formatter_type::scope_name());
                break;

            case 'c':
                if (!literal.empty())
                    fmt.add_formatter(typename formatter_type::literal(literal));
                fmt.add_formatter(typename formatter_type::function_name(true));
                break;

            case 'C':
                if (!literal.empty())
                    fmt.add_formatter(typename formatter_type::literal(literal));
                fmt.add_formatter(typename formatter_type::function_name(false));
                break;

            case 'f':
                if (!literal.empty())
                    fmt.add_formatter(typename formatter_type::literal(literal));
                fmt.add_formatter(typename formatter_type::full_file_name());
                break;

            case 'F':
                if (!literal.empty())
                    fmt.add_formatter(typename formatter_type::literal(literal));
                fmt.add_formatter(typename formatter_type::file_name());
                break;

            case 'l':
                if (!literal.empty())
                    fmt.add_formatter(typename formatter_type::literal(literal));
                fmt.add_formatter(typename formatter_type::line_number());
                break;

            default:
                literal.append(p, p + 2);
                break;
            }

            begin = p + 2;
        }
        else
        {
            if (p != end)
                literal.push_back(static_cast< char_type >('%')); // a single '%' character at the end of the string
            begin = end;
        }
    }

    if (!literal.empty())
        fmt.add_formatter(typename formatter_type::literal(literal));

    return result_type(boost::move(fmt));
}

} // namespace


#ifdef BOOST_LOG_USE_CHAR

//! Parses the named scope format string and constructs the formatter function
BOOST_LOG_API boost::log::aux::light_function< void (basic_formatting_ostream< char >&, attributes::named_scope::value_type::value_type const&) >
parse_named_scope_format(const char* begin, const char* end)
{
    return do_parse_named_scope_format(begin, end);
}

#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T

//! Parses the named scope format string and constructs the formatter function
BOOST_LOG_API boost::log::aux::light_function< void (basic_formatting_ostream< wchar_t >&, attributes::named_scope::value_type::value_type const&) >
parse_named_scope_format(const wchar_t* begin, const wchar_t* end)
{
    return do_parse_named_scope_format(begin, end);
}

#endif // BOOST_LOG_USE_WCHAR_T

} // namespace aux

} // namespace expressions

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

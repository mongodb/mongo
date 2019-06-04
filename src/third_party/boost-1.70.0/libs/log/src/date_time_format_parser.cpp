/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   date_time_format_parser.cpp
 * \author Andrey Semashev
 * \date   30.09.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <cstring>
#include <string>
#include <algorithm>
#include <boost/spirit/include/karma_uint.hpp>
#include <boost/spirit/include/karma_generate.hpp>
#include <boost/range/iterator_range_core.hpp>
#include <boost/log/detail/attachable_sstream_buf.hpp>
#include <boost/log/detail/date_time_format_parser.hpp>
#include <boost/log/detail/header.hpp>

namespace karma = boost::spirit::karma;

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

template< typename CharT >
struct string_constants;

#ifdef BOOST_LOG_USE_CHAR

template< >
struct string_constants< char >
{
    typedef char char_type;

    static const char_type* iso_date_format() { return "%Y%m%d"; }
    static const char_type* extended_iso_date_format() { return "%Y-%m-%d"; }

    static const char_type* iso_time_format() { return "%H%M%S"; }
    static const char_type* extended_iso_time_format() { return "%H:%M:%S"; }
    static const char_type* default_time_format() { return "%H:%M:%S.%f"; }
};

#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T

template< >
struct string_constants< wchar_t >
{
    typedef wchar_t char_type;

    static const char_type* iso_date_format() { return L"%Y%m%d"; }
    static const char_type* extended_iso_date_format() { return L"%Y-%m-%d"; }

    static const char_type* iso_time_format() { return L"%H%M%S"; }
    static const char_type* extended_iso_time_format() { return L"%H:%M:%S"; }
    static const char_type* default_time_format() { return L"%H:%M:%S.%f"; }
};

#endif // BOOST_LOG_USE_WCHAR_T

template< typename CallbackT >
struct common_flags
{
    typedef CallbackT callback_type;
    typedef typename callback_type::char_type char_type;
    typedef std::basic_string< char_type > string_type;

    const char_type* parse(const char_type* begin, const char_type* end, callback_type& callback)
    {
        switch (begin[1])
        {
        case '%':
            m_literal.push_back('%');
            break;

        default:
            flush(callback);
            callback.on_placeholder(iterator_range< const char_type* >(begin, begin + 2));
            break;
        }

        return begin + 2;
    }

    void add_literal(const char_type* begin, const char_type* end)
    {
        m_literal.append(begin, end);
    }

    void flush(callback_type& callback)
    {
        if (!m_literal.empty())
        {
            const char_type* p = m_literal.c_str();
            callback.on_literal(iterator_range< const char_type* >(p, p + m_literal.size()));
            m_literal.clear();
        }
    }

private:
    string_type m_literal;
};

template< typename BaseT >
struct date_flags :
    public BaseT
{
    typedef typename BaseT::callback_type callback_type;
    typedef typename BaseT::char_type char_type;

    const char_type* parse(const char_type* begin, const char_type* end, callback_type& callback)
    {
        typedef string_constants< char_type > constants;

        switch (begin[1])
        {
        case 'Y':
            {
                this->flush(callback);

                std::size_t len = end - begin;
                if (len >= 8 && std::memcmp(begin, constants::extended_iso_date_format(), 8 * sizeof(char_type)) == 0)
                {
                    callback.on_extended_iso_date();
                    return begin + 8;
                }
                else if (len >= 6 && std::memcmp(begin, constants::iso_date_format(), 6 * sizeof(char_type)) == 0)
                {
                    callback.on_iso_date();
                    return begin + 6;
                }
                else
                {
                    callback.on_full_year();
                }
            }
            break;

        case 'y':
            this->flush(callback);
            callback.on_short_year();
            break;

        case 'm':
            this->flush(callback);
            callback.on_numeric_month();
            break;

        case 'B':
            this->flush(callback);
            callback.on_full_month();
            break;

        case 'b':
            this->flush(callback);
            callback.on_short_month();
            break;

        case 'd':
            this->flush(callback);
            callback.on_month_day(true);
            break;

        case 'e':
            this->flush(callback);
            callback.on_month_day(false);
            break;

        case 'w':
            this->flush(callback);
            callback.on_numeric_week_day();
            break;

        case 'A':
            this->flush(callback);
            callback.on_full_week_day();
            break;

        case 'a':
            this->flush(callback);
            callback.on_short_week_day();
            break;

        default:
            return BaseT::parse(begin, end, callback);
        }

        return begin + 2;
    }
};

template< typename BaseT >
struct time_flags :
    public BaseT
{
    typedef typename BaseT::callback_type callback_type;
    typedef typename BaseT::char_type char_type;

    const char_type* parse(const char_type* begin, const char_type* end, callback_type& callback)
    {
        typedef string_constants< char_type > constants;

        switch (begin[1])
        {
        case 'O':
        case 'H':
            {
                this->flush(callback);

                std::size_t len = end - begin;
                if (len >= 11 && std::memcmp(begin, constants::default_time_format(), 11 * sizeof(char_type)) == 0)
                {
                    callback.on_default_time();
                    return begin + 11;
                }
                else if (len >= 8 && std::memcmp(begin, constants::extended_iso_time_format(), 8 * sizeof(char_type)) == 0)
                {
                    callback.on_extended_iso_time();
                    return begin + 8;
                }
                else if (len >= 6 && std::memcmp(begin, constants::iso_time_format(), 6 * sizeof(char_type)) == 0)
                {
                    callback.on_iso_time();
                    return begin + 6;
                }
                else
                {
                    callback.on_hours(true);
                }
            }
            break;

        case 'T':
            this->flush(callback);
            callback.on_extended_iso_time();
            break;

        case 'k':
            this->flush(callback);
            callback.on_hours(false);
            break;

        case 'I':
            this->flush(callback);
            callback.on_hours_12(true);
            break;

        case 'l':
            this->flush(callback);
            callback.on_hours_12(false);
            break;

        case 'M':
            this->flush(callback);
            callback.on_minutes();
            break;

        case 'S':
            this->flush(callback);
            callback.on_seconds();
            break;

        case 'f':
            this->flush(callback);
            callback.on_fractional_seconds();
            break;

        case 'P':
            this->flush(callback);
            callback.on_am_pm(false);
            break;

        case 'p':
            this->flush(callback);
            callback.on_am_pm(true);
            break;

        case 'Q':
            this->flush(callback);
            callback.on_extended_iso_time_zone();
            break;

        case 'q':
            this->flush(callback);
            callback.on_iso_time_zone();
            break;

        case '-':
            this->flush(callback);
            callback.on_duration_sign(false);
            break;

        case '+':
            this->flush(callback);
            callback.on_duration_sign(true);
            break;

        default:
            return BaseT::parse(begin, end, callback);
        }

        return begin + 2;
    }
};

template< typename CharT, typename ParserT, typename CallbackT >
inline void parse_format(const CharT* begin, const CharT* end, ParserT& parser, CallbackT& callback)
{
    typedef CharT char_type;

    while (begin != end)
    {
        const char_type* p = std::find(begin, end, static_cast< char_type >('%'));
        parser.add_literal(begin, p);

        if ((end - p) >= 2)
        {
            begin = parser.parse(p, end, callback);
        }
        else
        {
            if (p != end)
                parser.add_literal(p, end); // a single '%' character at the end of the string
            begin = end;
        }
    }

    parser.flush(callback);
}

} // namespace

//! Parses the date format string and invokes the callback object
template< typename CharT >
BOOST_LOG_API void parse_date_format(const CharT* begin, const CharT* end, date_format_parser_callback< CharT >& callback)
{
    typedef CharT char_type;
    typedef date_format_parser_callback< char_type > callback_type;
    date_flags< common_flags< callback_type > > parser;
    parse_format(begin, end, parser, callback);
}

//! Parses the time format string and invokes the callback object
template< typename CharT >
BOOST_LOG_API void parse_time_format(const CharT* begin, const CharT* end, time_format_parser_callback< CharT >& callback)
{
    typedef CharT char_type;
    typedef time_format_parser_callback< char_type > callback_type;
    time_flags< common_flags< callback_type > > parser;
    parse_format(begin, end, parser, callback);
}

//! Parses the date and time format string and invokes the callback object
template< typename CharT >
BOOST_LOG_API void parse_date_time_format(const CharT* begin, const CharT* end, date_time_format_parser_callback< CharT >& callback)
{
    typedef CharT char_type;
    typedef date_time_format_parser_callback< char_type > callback_type;
    date_flags< time_flags< common_flags< callback_type > > > parser;
    parse_format(begin, end, parser, callback);
}

template< typename CharT >
BOOST_LOG_API void put_integer(boost::log::aux::basic_ostringstreambuf< CharT >& strbuf, uint32_t value, unsigned int width, CharT fill_char)
{
    typedef CharT char_type;
    char_type buf[std::numeric_limits< uint32_t >::digits10 + 2];
    char_type* p = buf;

    typedef karma::uint_generator< uint32_t, 10 > uint_gen;
    karma::generate(p, uint_gen(), value);
    const std::size_t len = p - buf;
    if (len < width)
        strbuf.append(width - len, fill_char);
    strbuf.append(buf, len);
}

#ifdef BOOST_LOG_USE_CHAR

template BOOST_LOG_API
void parse_date_format(const char* begin, const char* end, date_format_parser_callback< char >& callback);
template BOOST_LOG_API
void parse_time_format(const char* begin, const char* end, time_format_parser_callback< char >& callback);
template BOOST_LOG_API
void parse_date_time_format(const char* begin, const char* end, date_time_format_parser_callback< char >& callback);
template BOOST_LOG_API
void put_integer(boost::log::aux::basic_ostringstreambuf< char >& strbuf, uint32_t value, unsigned int width, char fill_char);

#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T

template BOOST_LOG_API
void parse_date_format(const wchar_t* begin, const wchar_t* end, date_format_parser_callback< wchar_t >& callback);
template BOOST_LOG_API
void parse_time_format(const wchar_t* begin, const wchar_t* end, time_format_parser_callback< wchar_t >& callback);
template BOOST_LOG_API
void parse_date_time_format(const wchar_t* begin, const wchar_t* end, date_time_format_parser_callback< wchar_t >& callback);
template BOOST_LOG_API
void put_integer(boost::log::aux::basic_ostringstreambuf< wchar_t >& strbuf, uint32_t value, unsigned int width, wchar_t fill_char);

#endif // BOOST_LOG_USE_WCHAR_T

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

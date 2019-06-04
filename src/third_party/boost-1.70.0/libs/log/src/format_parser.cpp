/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   format_parser.cpp
 * \author Andrey Semashev
 * \date   16.11.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <string>
#include <algorithm>
#include <boost/throw_exception.hpp>
#include <boost/exception/exception.hpp>
#include <boost/move/core.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/spirit/include/qi_uint.hpp>
#include <boost/spirit/include/qi_parse.hpp>
#include <boost/log/detail/format.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/support/exception.hpp>
#include "spirit_encoding.hpp"
#include <boost/log/detail/header.hpp>

namespace qi = boost::spirit::qi;

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

template< typename CharT >
BOOST_LOG_API format_description< CharT > parse_format(const CharT* begin, const CharT* end)
{
    typedef CharT char_type;
    typedef format_description< char_type > description;
    typedef typename encoding< char_type >::type traits;

    const char_type* original_begin = begin;
    description descr;
    unsigned int literal_start_pos = 0;

    while (begin != end)
    {
        const char_type* p = std::find(begin, end, static_cast< char_type >('%'));
        descr.literal_chars.append(begin, p);

        if ((end - p) >= 2)
        {
            // Check for a percent placeholder
            char_type c = p[1];
            if (c == static_cast< char_type >('%'))
            {
                descr.literal_chars.push_back(static_cast< char_type >('%'));
                begin = p + 2;
                continue;
            }

            // From here on, no more literals are possible. Append the literal element.
            {
                const unsigned int literal_chars_size = static_cast< unsigned int >(descr.literal_chars.size());
                if (literal_start_pos < literal_chars_size)
                {
                    descr.format_elements.push_back(format_element::literal(literal_start_pos, literal_chars_size - literal_start_pos));
                    literal_start_pos = literal_chars_size;
                }
            }

            // Check if this is a positional argument
            if (traits::isdigit(c))
            {
                if (c != static_cast< char_type >('0'))
                {
                    // Positional argument in the form "%N%"
                    unsigned int n = 0;
                    const char_type* pp = p + 1;
                    qi::parse(pp, end, qi::uint_, n);
                    if (n == 0 || pp == end || *pp != static_cast< char_type >('%'))
                    {
                        boost::throw_exception(boost::enable_error_info(parse_error("Invalid positional format placeholder")) << boost::throw_file(__FILE__) << boost::throw_line(__LINE__)
                            << boost::log::position_info(static_cast< unsigned int >(p - original_begin))
                        );
                    }

                    // Safety check against ridiculously large argument numbers which would lead to excessive memory consumption.
                    // This could be useful if the format string is gathered from an external source (e.g. a config file).
                    if (n > 1000)
                    {
                        boost::throw_exception(boost::enable_error_info(limitation_error("Positional format placeholder too big")) << boost::throw_file(__FILE__) << boost::throw_line(__LINE__)
                            << boost::log::position_info(static_cast< unsigned int >(p - original_begin))
                        );
                    }

                    // We count positional arguments from 0, not from 1 as in format strings
                    descr.format_elements.push_back(format_element::positional_argument(n - 1));
                    begin = pp + 1; // skip the closing '%'

                    continue;
                }
                else
                {
                    // This must be the filler character, not supported yet
                }
            }

            // This must be something else, not supported yet
            boost::throw_exception(boost::enable_error_info(parse_error("Unsupported format placeholder")) << boost::throw_file(__FILE__) << boost::throw_line(__LINE__)
                << boost::log::position_info(static_cast< unsigned int >(p - original_begin))
            );
        }
        else
        {
            if (p != end)
                descr.literal_chars.push_back(static_cast< char_type >('%')); // a single '%' character at the end of the string
            begin = end;
        }
    }

    const unsigned int literal_chars_size = static_cast< unsigned int >(descr.literal_chars.size());
    if (literal_start_pos < literal_chars_size)
        descr.format_elements.push_back(format_element::literal(literal_start_pos, literal_chars_size - literal_start_pos));

    return BOOST_LOG_NRVO_RESULT(descr);
}


#ifdef BOOST_LOG_USE_CHAR

template BOOST_LOG_API
format_description< char > parse_format(const char* begin, const char* end);

#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T

template BOOST_LOG_API
format_description< wchar_t > parse_format(const wchar_t* begin, const wchar_t* end);

#endif // BOOST_LOG_USE_WCHAR_T

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

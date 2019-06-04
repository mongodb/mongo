/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   formatter_parser.cpp
 * \author Andrey Semashev
 * \date   07.04.2008
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_WITHOUT_SETTINGS_PARSERS

#include <boost/log/detail/setup_config.hpp>
#include <map>
#include <string>
#include <sstream>
#include <stdexcept>
#include <boost/assert.hpp>
#include <boost/bind.hpp>
#include <boost/move/core.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <boost/log/expressions/formatter.hpp>
#include <boost/log/attributes/attribute_name.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/detail/code_conversion.hpp>
#include <boost/log/detail/default_attribute_names.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/functional/nop.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/log/detail/locks.hpp>
#include <boost/log/detail/light_rw_mutex.hpp>
#endif
#include "parser_utils.hpp"
#include "spirit_encoding.hpp"
#if !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)
#include "default_formatter_factory.hpp"
#endif
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! The structure contains formatter factories repository
template< typename CharT >
struct formatters_repository :
    public log::aux::lazy_singleton< formatters_repository< CharT > >
{
    //! Base class type
    typedef log::aux::lazy_singleton< formatters_repository< CharT > > base_type;

#if !defined(BOOST_LOG_BROKEN_FRIEND_TEMPLATE_SPECIALIZATIONS)
    friend class log::aux::lazy_singleton< formatters_repository< CharT > >;
#else
    friend class base_type;
#endif

    typedef CharT char_type;
    typedef formatter_factory< char_type > formatter_factory_type;

    //! Attribute name ordering predicate
    struct attribute_name_order
    {
        typedef bool result_type;
        result_type operator() (attribute_name const& left, attribute_name const& right) const
        {
            return left.id() < right.id();
        }
    };

    //! Map of formatter factories
    typedef std::map< attribute_name, shared_ptr< formatter_factory_type >, attribute_name_order > factories_map;


#if !defined(BOOST_LOG_NO_THREADS)
    //! Synchronization mutex
    mutable log::aux::light_rw_mutex m_Mutex;
#endif
    //! The map of formatter factories
    factories_map m_Map;
#if !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)
    //! Default factory
    mutable aux::default_formatter_factory< char_type > m_DefaultFactory;
#endif

    //! The method returns the filter factory for the specified attribute name
    formatter_factory_type& get_factory(attribute_name const& name) const
    {
        typename factories_map::const_iterator it = m_Map.find(name);
        if (it != m_Map.end())
        {
            return *it->second;
        }
        else
        {
#if !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)
            return m_DefaultFactory;
#else
            BOOST_LOG_THROW_DESCR(setup_error, "No formatter factory registered for attribute " + name.string());
#endif
        }
    }

private:
    formatters_repository()
    {
    }
};

//! Function object for formatter chaining
template< typename CharT, typename SecondT >
struct chained_formatter
{
    typedef void result_type;
    typedef basic_formatter< CharT > formatter_type;
    typedef typename formatter_type::stream_type stream_type;

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
    explicit chained_formatter(formatter_type&& first, SecondT&& second) :
#else
    template< typename T >
    explicit chained_formatter(BOOST_RV_REF(formatter_type) first, T const& second) :
#endif
        m_first(boost::move(first)), m_second(boost::move(second))
    {
    }

    result_type operator() (record_view const& rec, stream_type& strm) const
    {
        m_first(rec, strm);
        m_second(rec, strm);
    }

private:
    formatter_type m_first;
    SecondT m_second;
};

//! String literal formatter
template< typename CharT >
struct literal_formatter
{
    typedef void result_type;
    typedef CharT char_type;
    typedef std::basic_string< char_type > string_type;
    typedef basic_formatting_ostream< char_type > stream_type;

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
    explicit literal_formatter(string_type&& str) : m_str(boost::move(str))
#else
    explicit literal_formatter(string_type const& str) : m_str(str)
#endif
    {
    }

    result_type operator() (record_view const& rec, stream_type& strm) const
    {
        strm << m_str;
    }

private:
    const string_type m_str;
};

//! Formatter parsing grammar
template< typename CharT >
class formatter_parser
{
private:
    typedef CharT char_type;
    typedef const char_type* iterator_type;
    typedef std::basic_string< char_type > string_type;
    typedef basic_formatter< char_type > formatter_type;
    typedef boost::log::aux::char_constants< char_type > constants;
    typedef typename log::aux::encoding< char_type >::type encoding;
    typedef log::aux::encoding_specific< encoding > encoding_specific;
    typedef formatter_factory< char_type > formatter_factory_type;
    typedef typename formatter_factory_type::args_map args_map;

private:
    //! The formatter being constructed
    optional< formatter_type > m_Formatter;

    //! Attribute name
    attribute_name m_AttrName;
    //! Formatter factory arguments
    args_map m_FactoryArgs;

    //! Formatter argument name
    mutable string_type m_ArgName;
    //! Argument value
    mutable string_type m_ArgValue;

public:
    //! Constructor
    formatter_parser()
    {
    }

    //! Parses formatter
    void parse(iterator_type& begin, iterator_type end)
    {
        iterator_type p = begin;

        while (p != end)
        {
            // Find the end of a string literal
            iterator_type start = p;
            for (; p != end; ++p)
            {
                char_type c = *p;
                if (c == constants::char_backslash)
                {
                    // We found an escaped character
                    ++p;
                    if (p == end)
                        BOOST_LOG_THROW_DESCR(parse_error, "Invalid escape sequence in the formatter string");
                }
                else if (c == constants::char_percent)
                {
                    // We found an attribute
                    break;
                }
            }

            if (start != p)
                push_string(start, p);

            if (p != end)
            {
                // We found an attribute placeholder
                iterator_type start = constants::trim_spaces_left(++p, end);
                p = constants::scan_attr_placeholder(start, end);
                if (p == end)
                    BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder in the formatter string");

                on_attribute_name(start, p);

                p = constants::trim_spaces_left(p, end);
                if (p == end)
                    BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder in the formatter string");

                if (*p == constants::char_paren_bracket_left)
                {
                    // We found formatter arguments
                    p = parse_args(constants::trim_spaces_left(++p, end), end);
                    p = constants::trim_spaces_left(p, end);
                    if (p == end)
                        BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder in the formatter string");
                }

                if (*p != constants::char_percent)
                    BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder in the formatter string");

                ++p;

                push_attr();
            }
        }

        begin = p;
    }

    //! Returns the parsed formatter
    formatter_type get_formatter()
    {
        if (!m_Formatter)
        {
            // This may happen if parser input is an empty string
            return formatter_type(nop());
        }

        return boost::move(m_Formatter.get());
    }

private:
    //! The method parses formatter arguments
    iterator_type parse_args(iterator_type begin, iterator_type end)
    {
        iterator_type p = begin;
        if (p == end)
            BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder arguments description in the formatter string");
        if (*p == constants::char_paren_bracket_right)
            return ++p;

        while (true)
        {
            char_type c = *p;

            // Read argument name
            iterator_type start = p;
            if (!encoding::isalpha(*p))
                BOOST_LOG_THROW_DESCR(parse_error, "Placeholder argument name is invalid");
            for (++p; p != end; ++p)
            {
                c = *p;
                if (encoding::isspace(c) || c == constants::char_equal)
                    break;
                if (!encoding::isalnum(c) && c != constants::char_underline)
                    BOOST_LOG_THROW_DESCR(parse_error, "Placeholder argument name is invalid");
            }

            if (start == p)
                BOOST_LOG_THROW_DESCR(parse_error, "Placeholder argument name is empty");

            on_arg_name(start, p);

            p = constants::trim_spaces_left(p, end);
            if (p == end || *p != constants::char_equal)
                BOOST_LOG_THROW_DESCR(parse_error, "Placeholder argument description is not valid");

            // Read argument value
            start = p = constants::trim_spaces_left(++p, end);
            p = constants::parse_operand(p, end, m_ArgValue);
            if (p == start)
                BOOST_LOG_THROW_DESCR(parse_error, "Placeholder argument value is not specified");

            push_arg();

            p = constants::trim_spaces_left(p, end);
            if (p == end)
                BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder arguments description in the formatter string");

            c = *p;
            if (c == constants::char_paren_bracket_right)
            {
                break;
            }
            else if (c == constants::char_comma)
            {
                p = constants::trim_spaces_left(++p, end);
                if (p == end)
                    BOOST_LOG_THROW_DESCR(parse_error, "Placeholder argument name is invalid");
            }
            else
            {
                BOOST_LOG_THROW_DESCR(parse_error, "Invalid attribute placeholder arguments description in the formatter string");
            }
        }

        return ++p;
    }

    //! The method is called when an argument name is discovered
    void on_arg_name(iterator_type begin, iterator_type end)
    {
        m_ArgName.assign(begin, end);
    }

    //! The method is called when an argument is filled
    void push_arg()
    {
        m_FactoryArgs[m_ArgName] = m_ArgValue;
        m_ArgName.clear();
        m_ArgValue.clear();
    }

    //! The method is called when an attribute name is discovered
    void on_attribute_name(iterator_type begin, iterator_type end)
    {
        if (begin == end)
            BOOST_LOG_THROW_DESCR(parse_error, "Empty attribute name encountered");

        // For compatibility with Boost.Log v1 we recognize %_% as the message attribute name
        const std::size_t len = end - begin;
        if (std::char_traits< char_type >::length(constants::message_text_keyword()) == len &&
            std::char_traits< char_type >::compare(constants::message_text_keyword(), begin, len) == 0)
        {
            m_AttrName = log::aux::default_attribute_names::message();
        }
        else
        {
            m_AttrName = attribute_name(log::aux::to_narrow(string_type(begin, end)));
        }
    }
    //! The method is called when an attribute is filled
    void push_attr()
    {
        BOOST_ASSERT_MSG(!!m_AttrName, "Attribute name is not set");

        if (m_AttrName == log::aux::default_attribute_names::message())
        {
            // We make a special treatment for the message text formatter
            append_formatter(expressions::aux::message_formatter());
        }
        else
        {
            // Use the factory to create the formatter
            formatters_repository< char_type > const& repo = formatters_repository< char_type >::get();
            formatter_factory_type& factory = repo.get_factory(m_AttrName);
            append_formatter(factory.create_formatter(m_AttrName, m_FactoryArgs));
        }

        // Eventually, clear all the auxiliary data
        m_AttrName = attribute_name();
        m_FactoryArgs.clear();
    }

    //! The method is called when a string literal is discovered
    void push_string(iterator_type begin, iterator_type end)
    {
        string_type s(begin, end);
        constants::translate_escape_sequences(s);
        append_formatter(literal_formatter< char_type >(boost::move(s)));
    }

    //! The method appends a formatter part to the final formatter
    template< typename FormatterT >
    void append_formatter(FormatterT fmt)
    {
        if (!!m_Formatter)
            m_Formatter = boost::in_place(chained_formatter< char_type, FormatterT >(boost::move(m_Formatter.get()), boost::move(fmt)));
        else
            m_Formatter = boost::in_place(boost::move(fmt));
    }

    //  Assignment and copying are prohibited
    BOOST_DELETED_FUNCTION(formatter_parser(formatter_parser const&))
    BOOST_DELETED_FUNCTION(formatter_parser& operator= (formatter_parser const&))
};

} // namespace

//! The function registers a user-defined formatter factory
template< typename CharT >
BOOST_LOG_SETUP_API void register_formatter_factory(attribute_name const& name, shared_ptr< formatter_factory< CharT > > const& factory)
{
    BOOST_ASSERT(!!name);
    BOOST_ASSERT(!!factory);

    formatters_repository< CharT >& repo = formatters_repository< CharT >::get();
    BOOST_LOG_EXPR_IF_MT(log::aux::exclusive_lock_guard< log::aux::light_rw_mutex > lock(repo.m_Mutex);)
    repo.m_Map[name] = factory;
}

//! The function parses a formatter from the string
template< typename CharT >
BOOST_LOG_SETUP_API basic_formatter< CharT > parse_formatter(const CharT* begin, const CharT* end)
{
    typedef CharT char_type;

    formatter_parser< char_type > parser;
    const char_type* p = begin;

    BOOST_LOG_EXPR_IF_MT(formatters_repository< CharT >& repo = formatters_repository< CharT >::get();)
    BOOST_LOG_EXPR_IF_MT(log::aux::shared_lock_guard< log::aux::light_rw_mutex > lock(repo.m_Mutex);)

    parser.parse(p, end);

    return parser.get_formatter();
}

#ifdef BOOST_LOG_USE_CHAR
template BOOST_LOG_SETUP_API
void register_formatter_factory< char >(
    attribute_name const& attr_name, shared_ptr< formatter_factory< char > > const& factory);
template BOOST_LOG_SETUP_API
basic_formatter< char > parse_formatter< char >(const char* begin, const char* end);
#endif // BOOST_LOG_USE_CHAR

#ifdef BOOST_LOG_USE_WCHAR_T
template BOOST_LOG_SETUP_API
void register_formatter_factory< wchar_t >(
    attribute_name const& attr_name, shared_ptr< formatter_factory< wchar_t > > const& factory);
template BOOST_LOG_SETUP_API
basic_formatter< wchar_t > parse_formatter< wchar_t >(const wchar_t* begin, const wchar_t* end);
#endif // BOOST_LOG_USE_WCHAR_T

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_WITHOUT_SETTINGS_PARSERS

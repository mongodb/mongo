/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   matches_relation_factory.hpp
 * \author Andrey Semashev
 * \date   03.08.2013
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#if !defined(BOOST_LOG_WITHOUT_SETTINGS_PARSERS) && !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)

#include <boost/log/detail/setup_config.hpp>

#if defined(BOOST_LOG_USE_STD_REGEX) && defined(BOOST_NO_CXX11_HDR_REGEX)
#error "Boost.Log: Cannot use std::regex because it is not supported by the standard library."
#endif

#if !defined(BOOST_LOG_USE_BOOST_REGEX) && !defined(BOOST_LOG_USE_STD_REGEX) && !defined(BOOST_LOG_USE_BOOST_XPRESSIVE)
// Use Boost.Regex backend by default. It produces smaller executables and also has the best performance for small string matching.
// Note: This default has to be in sync with Boost.Log Jamfile.v2.
#define BOOST_LOG_USE_BOOST_REGEX
#endif

#include <string>
#if defined(BOOST_LOG_USE_STD_REGEX)
#include <regex>
#include <boost/log/support/std_regex.hpp>
#elif defined(BOOST_LOG_USE_BOOST_REGEX)
#include <boost/regex.hpp>
#include <boost/log/support/regex.hpp>
#else
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/log/support/xpressive.hpp>
#endif
#include <boost/log/utility/string_literal.hpp>
#include <boost/log/utility/functional/matches.hpp>
#include <boost/log/utility/type_dispatch/standard_types.hpp>
#include <boost/log/detail/code_conversion.hpp>
#if defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)
#include <boost/fusion/container/set.hpp>
#include <boost/fusion/sequence/intrinsic/at_key.hpp>
#include <boost/fusion/algorithm/iteration/for_each.hpp>
#endif
#include "default_filter_factory.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

#if defined(BOOST_LOG_USE_STD_REGEX) || defined(BOOST_LOG_USE_BOOST_REGEX)

#if defined(BOOST_LOG_USE_STD_REGEX)
namespace regex_namespace = std;
#else
namespace regex_namespace = boost;
#endif

#if defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)

//! A special filtering predicate that adopts the string operand to the attribute value character type
struct matches_predicate :
    public matches_fun
{
    template< typename CharT >
    struct initializer
    {
        typedef void result_type;
        typedef CharT char_type;
        typedef std::basic_string< char_type > string_type;

        explicit initializer(string_type const& val) : m_initializer(val)
        {
        }

        template< typename T >
        result_type operator() (T& val) const
        {
            try
            {
                typedef typename T::value_type target_char_type;
                std::basic_string< target_char_type > str;
                log::aux::code_convert(m_initializer, str);
                val.assign(str, T::ECMAScript | T::optimize);
            }
            catch (...)
            {
            }
        }

    private:
        string_type const& m_initializer;
    };

    typedef matches_fun::result_type result_type;

    template< typename CharT >
    explicit matches_predicate(std::basic_string< CharT > const& operand)
    {
        fusion::for_each(m_operands, initializer< CharT >(operand));
    }

    template< typename T >
    result_type operator() (T const& val) const
    {
        typedef typename T::value_type char_type;
        typedef regex_namespace::basic_regex< char_type > regex_type;
        return matches_fun::operator() (val, fusion::at_key< regex_type >(m_operands));
    }

private:
    fusion::set< regex_namespace::regex, regex_namespace::wregex > m_operands;
};

#else

//! A special filtering predicate that adopts the string operand to the attribute value character type
template< typename CharT >
struct matches_predicate :
    public matches_fun
{
    typedef typename matches_fun::result_type result_type;
    typedef CharT char_type;
    typedef std::basic_string< char_type > string_type;
    typedef regex_namespace::basic_regex< char_type > regex_type;

    explicit matches_predicate(string_type const& operand) :
        m_operand(operand, regex_type::ECMAScript | regex_type::optimize)
    {
    }

    template< typename T >
    result_type operator() (T const& val) const
    {
        return matches_fun::operator() (val, m_operand);
    }

private:
    regex_type m_operand;
};

#endif

#else // defined(BOOST_LOG_USE_STD_REGEX) || defined(BOOST_LOG_USE_BOOST_REGEX)

#if defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)

//! A special filtering predicate that adopts the string operand to the attribute value character type
struct matches_predicate :
    public matches_fun
{
    template< typename CharT >
    struct initializer
    {
        typedef void result_type;
        typedef CharT char_type;
        typedef std::basic_string< char_type > string_type;

        explicit initializer(string_type const& val) : m_initializer(val)
        {
        }

        template< typename T >
        result_type operator() (T& val) const
        {
            try
            {
                typedef typename T::char_type target_char_type;
                std::basic_string< target_char_type > str;
                log::aux::code_convert(m_initializer, str);
                val = T::compile(str.c_str(), str.size(), T::ECMAScript | T::optimize);
            }
            catch (...)
            {
            }
        }

    private:
        string_type const& m_initializer;
    };

    typedef matches_fun::result_type result_type;

    template< typename CharT >
    explicit matches_predicate(std::basic_string< CharT > const& operand)
    {
        fusion::for_each(m_operands, initializer< CharT >(operand));
    }

    template< typename T >
    result_type operator() (T const& val) const
    {
        typedef typename T::value_type char_type;
        typedef xpressive::basic_regex< const char_type* > regex_type;
        return matches_fun::operator() (val, fusion::at_key< regex_type >(m_operands));
    }

private:
    fusion::set< xpressive::cregex, xpressive::wcregex > m_operands;
};

#else

//! A special filtering predicate that adopts the string operand to the attribute value character type
template< typename CharT >
struct matches_predicate :
    public matches_fun
{
    typedef typename matches_fun::result_type result_type;
    typedef CharT char_type;
    typedef std::basic_string< char_type > string_type;
    typedef xpressive::basic_regex< const char_type* > regex_type;

    explicit matches_predicate(string_type const& operand) :
        m_operand(regex_type::compile(operand.c_str(), operand.size(), regex_type::ECMAScript | regex_type::optimize))
    {
    }

    template< typename T >
    result_type operator() (T const& val) const
    {
        return matches_fun::operator() (val, m_operand);
    }

private:
    regex_type m_operand;
};

#endif

#endif // defined(BOOST_LOG_USE_STD_REGEX) || defined(BOOST_LOG_USE_BOOST_REGEX)

} // namespace

//! The function parses the "matches" relation
template< typename CharT >
filter parse_matches_relation(attribute_name const& name, std::basic_string< CharT > const& operand)
{
#if defined(BOOST_LOG_USE_CHAR) && defined(BOOST_LOG_USE_WCHAR_T)
    return predicate_wrapper< log::string_types::type, matches_predicate >(name, matches_predicate(operand));
#else
    return predicate_wrapper< std::basic_string< CharT >, matches_predicate< CharT > >(name, matches_predicate< CharT >(operand));
#endif
}

//  Explicitly instantiate factory implementation
#ifdef BOOST_LOG_USE_CHAR
template
filter parse_matches_relation< char >(attribute_name const& name, std::basic_string< char > const& operand);
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template
filter parse_matches_relation< wchar_t >(attribute_name const& name, std::basic_string< wchar_t > const& operand);
#endif

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // !defined(BOOST_LOG_WITHOUT_SETTINGS_PARSERS) && !defined(BOOST_LOG_WITHOUT_DEFAULT_FACTORIES)

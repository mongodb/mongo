/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   spirit_encoding.hpp
 * \author Andrey Semashev
 * \date   20.07.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#ifndef BOOST_LOG_SPIRIT_ENCODING_HPP_INCLUDED_
#define BOOST_LOG_SPIRIT_ENCODING_HPP_INCLUDED_

#include <boost/log/detail/config.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/spirit/include/support_standard.hpp>
#include <boost/spirit/include/support_standard_wide.hpp>
#include <boost/spirit/home/support/common_terminals.hpp>
#include <boost/log/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

template< typename >
struct encoding;

template< >
struct encoding< char >
{
    typedef spirit::char_encoding::standard type;
};
template< >
struct encoding< wchar_t >
{
    typedef spirit::char_encoding::standard_wide type;
};

//! A simple trait that allows to use charset-specific Qi parsers in a generic way
template< typename EncodingT >
struct encoding_specific;

#define BOOST_LOG_CHARSET_PARSERS\
    ((char_type, char_))\
    ((string_type, string))\
    ((alnum_type, alnum))\
    ((alpha_type, alpha))\
    ((blank_type, blank))\
    ((cntrl_type, cntrl))\
    ((digit_type, digit))\
    ((graph_type, graph))\
    ((print_type, print))\
    ((punct_type, punct))\
    ((space_type, space))\
    ((xdigit_type, xdigit))\
    ((no_case_type, no_case))\
    ((lower_type, lower))\
    ((upper_type, upper))\
    ((lowernum_type, lowernum))\
    ((uppernum_type, uppernum))

#define BOOST_LOG_DECLARE_CHARSET_PARSER(r, charset, parser)\
    typedef spirit::charset::BOOST_PP_TUPLE_ELEM(2, 0, parser) BOOST_PP_TUPLE_ELEM(2, 0, parser);\
    BOOST_LOG_API static BOOST_PP_TUPLE_ELEM(2, 0, parser) const& BOOST_PP_TUPLE_ELEM(2, 1, parser);

#define BOOST_LOG_DECLARE_CHARSET_PARSERS(charset)\
    BOOST_PP_SEQ_FOR_EACH(BOOST_LOG_DECLARE_CHARSET_PARSER, charset, BOOST_LOG_CHARSET_PARSERS)

template< >
struct encoding_specific< spirit::char_encoding::standard >
{
    BOOST_LOG_DECLARE_CHARSET_PARSERS(standard)
};

template< >
struct encoding_specific< spirit::char_encoding::standard_wide >
{
    BOOST_LOG_DECLARE_CHARSET_PARSERS(standard_wide)
};

#undef BOOST_LOG_DECLARE_CHARSET_PARSERS
#undef BOOST_LOG_DECLARE_CHARSET_PARSER

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

#endif // BOOST_LOG_SPIRIT_ENCODING_HPP_INCLUDED_

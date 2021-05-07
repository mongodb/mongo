/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   spirit_encoding.cpp
 * \author Andrey Semashev
 * \date   20.07.2012
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include "spirit_encoding.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

#define BOOST_LOG_DEFINE_CHARSET_PARSER(r, charset, parser)\
    BOOST_LOG_API encoding_specific< spirit::char_encoding::charset >::BOOST_PP_TUPLE_ELEM(2, 0, parser) const&\
        encoding_specific< spirit::char_encoding::charset >::BOOST_PP_TUPLE_ELEM(2, 1, parser) =\
            spirit::charset::BOOST_PP_TUPLE_ELEM(2, 1, parser);

#define BOOST_LOG_DEFINE_CHARSET_PARSERS(charset)\
    BOOST_PP_SEQ_FOR_EACH(BOOST_LOG_DEFINE_CHARSET_PARSER, charset, BOOST_LOG_CHARSET_PARSERS)

BOOST_LOG_DEFINE_CHARSET_PARSERS(standard)
BOOST_LOG_DEFINE_CHARSET_PARSERS(standard_wide)

#undef BOOST_LOG_DEFINE_CHARSET_PARSERS
#undef BOOST_LOG_DEFINE_CHARSET_PARSER

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>

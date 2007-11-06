/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_LISTS_FWD_HPP)
#define BOOST_SPIRIT_LISTS_FWD_HPP

#include <boost/spirit/core/parser.hpp>

namespace boost { namespace spirit {

    struct no_list_endtoken;

    template <
        typename ItemT, typename DelimT, typename EndT = no_list_endtoken,
        typename CategoryT = plain_parser_category
    >
    struct list_parser;

}} // namespace boost::spirit

#endif


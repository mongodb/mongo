/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_ESCAPE_CHAR_FWD_HPP)
#define BOOST_SPIRIT_ESCAPE_CHAR_FWD_HPP

namespace boost { namespace spirit {

    template <unsigned long Flags, typename CharT = char>
    struct escape_char_parser;

    template <
        class ParserT, typename ActionT,
        unsigned long Flags, typename CharT = char>
    struct escape_char_action;

}} // namespace boost::spirit

#endif


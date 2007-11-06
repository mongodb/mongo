/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_DISTINCT_FWD_HPP)
#define BOOST_SPIRIT_DISTINCT_FWD_HPP

namespace boost { namespace spirit {

    template<typename CharT> class chset;

    template <typename CharT = char, typename TailT = chset<CharT> >
    class distinct_parser;

    template <typename CharT = char, typename TailT = chset<CharT> >
    class distinct_directive;

    template <typename ScannerT = scanner<> >
    class dynamic_distinct_parser;

    template <typename ScannerT = scanner<> >
    class dynamic_distinct_directive;

}} // namespace boost::spirit

#endif



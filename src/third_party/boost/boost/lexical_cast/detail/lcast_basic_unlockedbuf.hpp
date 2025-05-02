// Copyright Kevlin Henney, 2000-2005.
// Copyright Alexander Nasonov, 2006-2010.
// Copyright Antony Polukhin, 2011-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_BASIC_UNLOCKEDBUF_HPP
#define BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_BASIC_UNLOCKEDBUF_HPP

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
#   pragma once
#endif


#ifdef BOOST_NO_STRINGSTREAM
#include <strstream>
#else
#include <sstream>
#endif

#include <boost/detail/basic_pointerbuf.hpp>
#ifndef BOOST_NO_CWCHAR
#   include <cwchar>
#endif

namespace boost { namespace detail { namespace lcast {

    // acts as a stream buffer which wraps around a pair of pointers
    // and gives acces to internals
    template <class BufferType, class CharT>
    class basic_unlockedbuf : public basic_pointerbuf<CharT, BufferType> {
    public:
        typedef basic_pointerbuf<CharT, BufferType> base_type;
        typedef typename base_type::streamsize streamsize;

        using base_type::pptr;
        using base_type::pbase;
        using base_type::setbuf;
    };

#if defined(BOOST_NO_STRINGSTREAM)
    template <class CharT, class Traits>
    using out_stream_t = std::ostream;

    template <class CharT, class Traits>
    using stringbuffer_t = basic_unlockedbuf<std::strstreambuf, char>;
#elif defined(BOOST_NO_STD_LOCALE)
    template <class CharT, class Traits>
    using out_stream_t = std::ostream;

    template <class CharT, class Traits>
    using stringbuffer_t = basic_unlockedbuf<std::stringbuf, char>;

    template <class CharT, class Traits>
    using buffer_t = basic_unlockedbuf<std::streambuf, char>;
#else
    template <class CharT, class Traits>
    using out_stream_t = std::basic_ostream<CharT, Traits>;

    template <class CharT, class Traits>
    using stringbuffer_t = basic_unlockedbuf<std::basic_stringbuf<CharT, Traits>, CharT>;

    template <class CharT, class Traits>
    using buffer_t = basic_unlockedbuf<std::basic_streambuf<CharT, Traits>, CharT>;
#endif

}}} // namespace boost::detail::lcast

#endif // BOOST_LEXICAL_CAST_DETAIL_CONVERTER_LEXICAL_BASIC_UNLOCKEDBUF_HPP


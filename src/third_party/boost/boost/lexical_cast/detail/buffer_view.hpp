// Copyright Antony Polukhin, 2011-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_LEXICAL_CAST_DETAIL_BUFFER_VIEW_HPP
#define BOOST_LEXICAL_CAST_DETAIL_BUFFER_VIEW_HPP

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
#   pragma once
#endif

#include <iosfwd>

namespace boost { namespace conversion { namespace detail {

    template < typename CharT >
    struct buffer_view {
        const CharT* begin;
        const CharT* end;
    };

    template < typename CharT >
    buffer_view<CharT> make_buffer_view(const CharT* begin, const CharT* end) {
        return buffer_view<CharT>{begin, end};
    }

    inline buffer_view<char> make_buffer_view(const signed char* begin, const signed char* end) {
        return buffer_view<char>{
            reinterpret_cast<const char*>(begin),
            reinterpret_cast<const char*>(end)
        };
    }

    inline buffer_view<char> make_buffer_view(const unsigned char* begin, const unsigned char* end) {
        return buffer_view<char>{
            reinterpret_cast<const char*>(begin),
            reinterpret_cast<const char*>(end)
        };
    }

    template< typename CharT, typename Elem, typename Traits >
    std::basic_ostream<Elem,Traits>& operator<<( 
                std::basic_ostream<Elem, Traits>& os,
                buffer_view<CharT> r)
    {
        while (r.begin != r.end) {
          os << r.begin[0];
          ++r.begin;
        }
        return os;
    }

}}}  // namespace boost::conversion::detail

#endif // BOOST_LEXICAL_CAST_DETAIL_BUFFER_VIEW_HPP


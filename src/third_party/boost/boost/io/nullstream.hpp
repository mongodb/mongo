/*
Copyright 2021-2022 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_IO_NULLSTREAM_HPP
#define BOOST_IO_NULLSTREAM_HPP

#include <boost/config.hpp>
#include <ostream>
#include <streambuf>

namespace boost {
namespace io {

template<class CharT, class Traits = std::char_traits<CharT> >
class basic_nullbuf
    : public std::basic_streambuf<CharT, Traits> {
protected:
    typename Traits::int_type overflow(typename Traits::int_type c)
        BOOST_OVERRIDE {
        return Traits::not_eof(c);
    }

    std::streamsize xsputn(const CharT*, std::streamsize n) BOOST_OVERRIDE {
        return n;
    }
};

namespace detail {

template<class CharT, class Traits>
class nullbuf {
public:
    boost::io::basic_nullbuf<CharT, Traits>* buf() {
        return &buf_;
    }

private:
    boost::io::basic_nullbuf<CharT, Traits> buf_;
};

} /* detail */

template<class CharT, class Traits = std::char_traits<CharT> >
class basic_onullstream
    : detail::nullbuf<CharT, Traits>
    , public std::basic_ostream<CharT, Traits> {
public:
    basic_onullstream()
        : std::basic_ostream<CharT, Traits>(detail::nullbuf<CharT,
             Traits>::buf()) { }
};

typedef basic_onullstream<char> onullstream;
typedef basic_onullstream<wchar_t> wonullstream;

} /* io */
} /* boost */

#endif

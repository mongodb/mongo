//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_SRC_UTIL_GREGORIAN_HPP
#define BOOST_LOCALE_SRC_UTIL_GREGORIAN_HPP

#include <locale>

namespace boost { namespace locale { namespace util {

    std::locale install_gregorian_calendar(const std::locale& in, const std::string& terr);

}}} // namespace boost::locale::util

#endif

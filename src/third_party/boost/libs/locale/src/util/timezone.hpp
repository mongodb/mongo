//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_UTIL_TIMEZONE_HPP
#define BOOST_LOCALE_IMPL_UTIL_TIMEZONE_HPP
#include <boost/locale/util/string.hpp>
#include <cstdlib>
#include <cstring>
#include <string>

namespace boost { namespace locale { namespace util {
    inline int parse_tz(const std::string& tz)
    {
        std::string ltz;
        for(const char c : tz) {
            if(is_lower_ascii(c))
                ltz += c - 'a' + 'A';
            else if(c != ' ')
                ltz += c;
        }
        if(ltz.compare(0, 3, "GMT") != 0 && ltz.compare(0, 3, "UTC") != 0)
            return 0;
        if(ltz.size() <= 3)
            return 0;
        int gmtoff = 0;
        const char* begin = ltz.c_str() + 3;
        char* end = const_cast<char*>(begin);
        int hours = strtol(begin, &end, 10);
        if(end != begin)
            gmtoff += hours * 3600;
        if(*end == ':') {
            begin = end + 1;
            int minutes = strtol(begin, &end, 10);
            if(end != begin)
                gmtoff += minutes * 60;
        }
        return gmtoff;
    }

}}} // namespace boost::locale::util

#endif

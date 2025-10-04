//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/util.hpp>
#include <boost/locale/util/string.hpp>
#include <algorithm>

#if BOOST_LOCALE_USE_WIN32_API
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#if BOOST_LOCALE_USE_WIN32_API
// Get information about the user default locale and put it into the buffer.
// Return true on success
template<size_t N>
static bool get_user_default_locale_info(LCTYPE lcType, char (&buf)[N])
{
    return GetLocaleInfoA(LOCALE_USER_DEFAULT, lcType, buf, N) != 0;
}
#endif

namespace boost { namespace locale { namespace util {
    std::string get_system_locale(bool use_utf8_on_windows)
    {
        const char* lang = nullptr;
        if(!lang || !*lang)
            lang = getenv("LC_ALL");
        if(!lang || !*lang)
            lang = getenv("LC_CTYPE");
        if(!lang || !*lang)
            lang = getenv("LANG");
#if !BOOST_LOCALE_USE_WIN32_API
        (void)use_utf8_on_windows; // not relevant for non-windows
        if(!lang || !*lang)
            lang = "C";
        return lang;
#else
        if(lang && *lang)
            return lang;

        char buf[10]{};
        if(!get_user_default_locale_info(LOCALE_SISO639LANGNAME, buf))
            return "C";
        std::string lc_name = buf;
        if(get_user_default_locale_info(LOCALE_SISO3166CTRYNAME, buf)) {
            lc_name += "_";
            lc_name += buf;
        }
        if(use_utf8_on_windows || !get_user_default_locale_info(LOCALE_IDEFAULTANSICODEPAGE, buf))
            lc_name += ".UTF-8";
        else {
            if(std::find_if_not(buf, str_end(buf), is_numeric_ascii) != str_end(buf))
                lc_name += ".UTF-8";
            else
                lc_name.append(".windows-").append(buf);
        }
        return lc_name;

#endif
    }
}}} // namespace boost::locale::util

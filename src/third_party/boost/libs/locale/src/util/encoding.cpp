//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "encoding.hpp"
#include <boost/locale/util/string.hpp>
#if BOOST_LOCALE_USE_WIN32_API
#    include "win_codepages.hpp"
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif
#include <algorithm>
#include <cstring>

namespace boost { namespace locale { namespace util {
    std::string normalize_encoding(const core::string_view encoding)
    {
        std::string result;
        result.reserve(encoding.length());
        for(char c : encoding) {
            if(is_lower_ascii(c) || is_numeric_ascii(c))
                result += c;
            else if(is_upper_ascii(c))
                result += char(c - 'A' + 'a');
        }
        return result;
    }

#if BOOST_LOCALE_USE_WIN32_API
    static int normalized_encoding_to_windows_codepage(const std::string& encoding)
    {
        windows_encoding* end = std::end(all_windows_encodings);

        windows_encoding* ptr = std::lower_bound(all_windows_encodings, end, encoding.c_str());
        while(ptr != end && ptr->name == encoding) {
            if(ptr->was_tested)
                return ptr->codepage;
            else if(IsValidCodePage(ptr->codepage)) {
                // the thread safety is not an issue, maximum
                // it would be checked more then once
                ptr->was_tested = 1;
                return ptr->codepage;
            } else
                ++ptr;
        }
        return -1;
    }

    int encoding_to_windows_codepage(const core::string_view encoding)
    {
        return normalized_encoding_to_windows_codepage(normalize_encoding(encoding));
    }

#endif
}}} // namespace boost::locale::util

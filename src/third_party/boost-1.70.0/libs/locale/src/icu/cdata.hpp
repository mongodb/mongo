//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_ICU_CDATA_HPP
#define BOOST_LOCALE_ICU_CDATA_HPP

#include <unicode/locid.h>
#include <string>

namespace boost {
    namespace locale {
        namespace impl_icu {
            struct cdata {
                icu::Locale locale;
                std::string encoding;
                bool utf8;
            };
        }
    }
}


#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

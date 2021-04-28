//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_IMPL_ICU_GET_TIME_ZONE_HPP
#define BOOST_LOCALE_IMPL_ICU_GET_TIME_ZONE_HPP

#include <unicode/calendar.h>
#include <string>

namespace boost {
    namespace locale {
        namespace impl_icu {

            // Provides a workaround for an ICU default timezone bug and also
            // handles time_zone string correctly - if empty returns default
            // otherwise returns the instance created with time_zone
            icu::TimeZone *get_time_zone(std::string const &time_zone);
        }
    }
}
#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

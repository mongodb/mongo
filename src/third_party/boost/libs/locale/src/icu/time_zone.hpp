//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_ICU_GET_TIME_ZONE_HPP
#define BOOST_LOCALE_IMPL_ICU_GET_TIME_ZONE_HPP

#include <boost/locale/config.hpp>
#include <cstdint> // Avoid ICU defining e.g. INT8_MIN causing macro redefinition warnings
#include <string>
#include <unicode/timezone.h>

namespace boost { namespace locale { namespace impl_icu {

    // Return an ICU time zone instance.
    // If the argument is empty returns the default timezone.
    inline icu::TimeZone* get_time_zone(const std::string& time_zone)
    {
        if(time_zone.empty())
            return icu::TimeZone::createDefault();
        else
            return icu::TimeZone::createTimeZone(time_zone.c_str());
    }
}}} // namespace boost::locale::impl_icu
#endif

//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_ICU_CDATA_HPP
#define BOOST_LOCALE_ICU_CDATA_HPP

#include <boost/locale/config.hpp>
#include <boost/locale/util/locale_data.hpp>
#include <string>
#include <unicode/locid.h>

namespace boost { namespace locale { namespace impl_icu {
    class cdata : util::locale_data {
        icu::Locale locale_;

    public:
        cdata() = default;
        void set(const std::string& id)
        {
            parse(id);
            locale_ = icu::Locale::createCanonical(id.c_str());
        }
        const util::locale_data& data() { return *this; }
        const icu::Locale& locale() const { return locale_; }
        using locale_data::encoding;
        using locale_data::is_utf8;
    };
}}} // namespace boost::locale::impl_icu

#endif

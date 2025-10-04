//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/format.hpp>
#include <boost/locale/generator.hpp>
#include <boost/locale/info.hpp>
#include "../util/numeric_conversion.hpp"
#include <algorithm>
#include <iostream>
#include <limits>

namespace boost { namespace locale { namespace detail {

    struct format_parser::data {
        unsigned position;
        std::streamsize precision;
        std::ios_base::fmtflags flags;
        ios_info info;
        std::locale saved_locale;
        bool restore_locale;
        void* cookie;
        void (*imbuer)(void*, const std::locale&);
    };

    format_parser::format_parser(std::ios_base& ios, void* cookie, void (*imbuer)(void*, const std::locale&)) :
        ios_(ios), d(new data)
    {
        d->position = std::numeric_limits<unsigned>::max();
        d->precision = ios.precision();
        d->flags = ios.flags();
        d->info = ios_info::get(ios);
        d->saved_locale = ios.getloc();
        d->restore_locale = false;
        d->cookie = cookie;
        d->imbuer = imbuer;
    }

    void format_parser::imbue(const std::locale& l)
    {
        d->imbuer(d->cookie, l);
    }

    format_parser::~format_parser() = default;

    void format_parser::restore()
    {
        ios_info::get(ios_) = d->info;
        ios_.width(0);
        ios_.flags(d->flags);
        if(d->restore_locale)
            imbue(d->saved_locale);
    }

    unsigned format_parser::get_position()
    {
        return d->position;
    }

    void format_parser::set_one_flag(const std::string& key, const std::string& value)
    {
        if(key.empty())
            return;
        decltype(d->position) position;
        if(util::try_to_int(key, position) && position > 0u) {
            d->position = position - 1u;
        } else if(key == "num" || key == "number") {
            as::number(ios_);

            if(value == "hex")
                ios_.setf(std::ios_base::hex, std::ios_base::basefield);
            else if(value == "oct")
                ios_.setf(std::ios_base::oct, std::ios_base::basefield);
            else if(value == "sci" || value == "scientific")
                ios_.setf(std::ios_base::scientific, std::ios_base::floatfield);
            else if(value == "fix" || value == "fixed")
                ios_.setf(std::ios_base::fixed, std::ios_base::floatfield);
        } else if(key == "cur" || key == "currency") {
            as::currency(ios_);
            if(value == "iso")
                as::currency_iso(ios_);
            else if(value == "nat" || value == "national")
                as::currency_national(ios_);
        } else if(key == "per" || key == "percent")
            as::percent(ios_);
        else if(key == "date") {
            as::date(ios_);
            if(value == "s" || value == "short")
                as::date_short(ios_);
            else if(value == "m" || value == "medium")
                as::date_medium(ios_);
            else if(value == "l" || value == "long")
                as::date_long(ios_);
            else if(value == "f" || value == "full")
                as::date_full(ios_);
        } else if(key == "time") {
            as::time(ios_);
            if(value == "s" || value == "short")
                as::time_short(ios_);
            else if(value == "m" || value == "medium")
                as::time_medium(ios_);
            else if(value == "l" || value == "long")
                as::time_long(ios_);
            else if(value == "f" || value == "full")
                as::time_full(ios_);
        } else if(key == "dt" || key == "datetime") {
            as::datetime(ios_);
            if(value == "s" || value == "short") {
                as::date_short(ios_);
                as::time_short(ios_);
            } else if(value == "m" || value == "medium") {
                as::date_medium(ios_);
                as::time_medium(ios_);
            } else if(value == "l" || value == "long") {
                as::date_long(ios_);
                as::time_long(ios_);
            } else if(value == "f" || value == "full") {
                as::date_full(ios_);
                as::time_full(ios_);
            }
        } else if(key == "spell" || key == "spellout")
            as::spellout(ios_);
        else if(key == "ord" || key == "ordinal")
            as::ordinal(ios_);
        else if(key == "left" || key == "<")
            ios_.setf(std::ios_base::left, std::ios_base::adjustfield);
        else if(key == "right" || key == ">")
            ios_.setf(std::ios_base::right, std::ios_base::adjustfield);
        else if(key == "gmt")
            as::gmt(ios_);
        else if(key == "local")
            as::local_time(ios_);
        else if(key == "timezone" || key == "tz")
            ios_info::get(ios_).time_zone(value);
        else if(key == "w" || key == "width") {
            int v;
            if(util::try_to_int(value, v))
                ios_.width(v);
        } else if(key == "p" || key == "precision") {
            int v;
            if(util::try_to_int(value, v))
                ios_.precision(v);
        } else if(key == "locale") {
            if(!d->restore_locale) {
                d->saved_locale = ios_.getloc();
                d->restore_locale = true;
            }

            generator gen;
            gen.categories(category_t::formatting);

            std::locale new_loc;
            if(value.find('.') == std::string::npos)
                new_loc = gen(value + "." + std::use_facet<info>(d->saved_locale).encoding());
            else
                new_loc = gen(value);

            imbue(new_loc);
        }
    }
}}} // namespace boost::locale::detail
// boostinspect:nominmax

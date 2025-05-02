//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2022 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_PREDEFINED_FORMATTERS_HPP_INCLUDED
#define BOOST_LOCALE_PREDEFINED_FORMATTERS_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include "icu_util.hpp"
#include <boost/thread/tss.hpp>
#include <locale>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4251) // "identifier" : class "type" needs to have dll-interface...
#endif
#include <unicode/locid.h>
#include <unicode/numfmt.h>
#include <unicode/smpdtfmt.h>
#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

namespace boost { namespace locale { namespace impl_icu {

    enum class format_len {
        Short,
        Medium,
        Long,
        Full,
    };

    enum class num_fmt_type { number, sci, curr_nat, curr_iso, percent, spell, ordinal };

    class formatters_cache : public std::locale::facet {
    public:
        static std::locale::id id;

        formatters_cache(const icu::Locale& locale);

        icu::NumberFormat& number_format(num_fmt_type type) const;

        const icu::UnicodeString& date_format(format_len f) const { return date_format_[int(f)]; }

        const icu::UnicodeString& time_format(format_len f) const { return time_format_[int(f)]; }

        const icu::UnicodeString& date_time_format(format_len d, format_len t) const
        {
            return date_time_format_[int(d)][int(t)];
        }

        const icu::UnicodeString& default_date_format() const { return default_date_format_; }
        const icu::UnicodeString& default_time_format() const { return default_time_format_; }
        const icu::UnicodeString& default_date_time_format() const { return default_date_time_format_; }

        icu::SimpleDateFormat* date_formatter() const;

    private:
        icu::NumberFormat* create_number_format(num_fmt_type type, UErrorCode& err) const;

        static constexpr auto num_fmt_type_count = static_cast<unsigned>(num_fmt_type::ordinal) + 1;
        static constexpr auto format_len_count = static_cast<unsigned>(format_len::Full) + 1;

        mutable boost::thread_specific_ptr<icu::NumberFormat> number_format_[num_fmt_type_count];
        icu::UnicodeString date_format_[format_len_count];
        icu::UnicodeString time_format_[format_len_count];
        icu::UnicodeString date_time_format_[format_len_count][format_len_count];
        icu::UnicodeString default_date_format_, default_time_format_, default_date_time_format_;
        mutable boost::thread_specific_ptr<icu::SimpleDateFormat> date_formatter_;
        icu::Locale locale_;
    };

}}} // namespace boost::locale::impl_icu

#endif

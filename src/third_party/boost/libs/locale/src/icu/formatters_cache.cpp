//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "formatters_cache.hpp"
#include <boost/assert.hpp>
#include <boost/core/ignore_unused.hpp>
#include <memory>
#include <type_traits>
#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4251) // "identifier" : class "type" needs to have dll-interface...
#endif
#include <unicode/datefmt.h>
#include <unicode/numfmt.h>
#include <unicode/rbnf.h>
#include <unicode/smpdtfmt.h>
#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

namespace boost { namespace locale { namespace impl_icu {

    std::locale::id formatters_cache::id;

    namespace {

        struct init {
            init() { ignore_unused(std::has_facet<formatters_cache>(std::locale::classic())); }
        } instance;

        void get_icu_pattern(std::unique_ptr<icu::DateFormat> fmt, icu::UnicodeString& out_str)
        {
            icu::SimpleDateFormat* sfmt = icu_cast<icu::SimpleDateFormat>(fmt.get());
            if(sfmt)
                sfmt->toPattern(out_str);
            else
                out_str.remove(); // LCOV_EXCL_LINE
        }
        void get_icu_pattern(icu::DateFormat* fmt, icu::UnicodeString& out_str)
        {
            return get_icu_pattern(std::unique_ptr<icu::DateFormat>(fmt), out_str);
        }
    } // namespace

    formatters_cache::formatters_cache(const icu::Locale& locale) : locale_(locale)
    {
#define BOOST_LOCALE_ARRAY_SIZE(T) std::extent<typename std::remove_reference<decltype(T)>::type>::value
        constexpr icu::DateFormat::EStyle styles[]{icu::DateFormat::kShort,
                                                   icu::DateFormat::kMedium,
                                                   icu::DateFormat::kLong,
                                                   icu::DateFormat::kFull};
        constexpr int num_styles = BOOST_LOCALE_ARRAY_SIZE(styles);

        static_assert(num_styles == BOOST_LOCALE_ARRAY_SIZE(date_format_), "!");
        for(int i = 0; i < num_styles; i++)
            get_icu_pattern(icu::DateFormat::createDateInstance(styles[i], locale), date_format_[i]);

        static_assert(num_styles == BOOST_LOCALE_ARRAY_SIZE(time_format_), "!");
        for(int i = 0; i < num_styles; i++)
            get_icu_pattern(icu::DateFormat::createTimeInstance(styles[i], locale), time_format_[i]);

        static_assert(num_styles == BOOST_LOCALE_ARRAY_SIZE(date_time_format_), "!");
        static_assert(num_styles == BOOST_LOCALE_ARRAY_SIZE(date_time_format_[0]), "!");
        for(int i = 0; i < num_styles; i++) {
            for(int j = 0; j < num_styles; j++) {
                get_icu_pattern(icu::DateFormat::createDateTimeInstance(styles[i], styles[j], locale),
                                date_time_format_[i][j]);
            }
        }
#undef BOOST_LOCALE_ARRAY_SIZE

        const auto get_str_or = [](const icu::UnicodeString& str, const char* default_str) {
            return str.isEmpty() ? default_str : str;
        };
        default_date_format_ = get_str_or(date_format(format_len::Medium), "yyyy-MM-dd");
        default_time_format_ = get_str_or(time_format(format_len::Medium), "HH:mm:ss");
        default_date_time_format_ =
          get_str_or(date_time_format(format_len::Full, format_len::Full), "yyyy-MM-dd HH:mm:ss");
    }

    icu::NumberFormat* formatters_cache::create_number_format(num_fmt_type type, UErrorCode& err) const
    {
        switch(type) {
            case num_fmt_type::number: return icu::NumberFormat::createInstance(locale_, err); break;
            case num_fmt_type::sci: return icu::NumberFormat::createScientificInstance(locale_, err); break;
            case num_fmt_type::curr_nat: return icu::NumberFormat::createInstance(locale_, UNUM_CURRENCY, err); break;
            case num_fmt_type::curr_iso:
                return icu::NumberFormat::createInstance(locale_, UNUM_CURRENCY_ISO, err);
                break;
            case num_fmt_type::percent: return icu::NumberFormat::createPercentInstance(locale_, err); break;
            case num_fmt_type::spell: return new icu::RuleBasedNumberFormat(icu::URBNF_SPELLOUT, locale_, err); break;
            case num_fmt_type::ordinal: return new icu::RuleBasedNumberFormat(icu::URBNF_ORDINAL, locale_, err); break;
        }
        throw std::logic_error("locale::internal error should not get there"); // LCOV_EXCL_LINE
    }

    icu::NumberFormat& formatters_cache::number_format(num_fmt_type type) const
    {
        icu::NumberFormat* result = number_format_[int(type)].get();
        if(!result) {
            UErrorCode err = U_ZERO_ERROR;
            std::unique_ptr<icu::NumberFormat> new_ptr(create_number_format(type, err));
            check_and_throw_icu_error(err, "Failed to create a formatter");
            result = new_ptr.get();
            BOOST_ASSERT(result);
            number_format_[int(type)].reset(new_ptr.release());
        }
        return *result;
    }

    icu::SimpleDateFormat* formatters_cache::date_formatter() const
    {
        icu::SimpleDateFormat* result = date_formatter_.get();
        if(!result) {
            std::unique_ptr<icu::DateFormat> fmt(
              icu::DateFormat::createDateTimeInstance(icu::DateFormat::kMedium, icu::DateFormat::kMedium, locale_));

            result = icu_cast<icu::SimpleDateFormat>(fmt.get());
            if(result) {
                fmt.release();
                date_formatter_.reset(result);
            }
        }
        return result;
    }

}}} // namespace boost::locale::impl_icu

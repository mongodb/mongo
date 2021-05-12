//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_PREDEFINED_FORMATTERS_HPP_INCLUDED
#define BOOST_LOCALE_PREDEFINED_FORMATTERS_HPP_INCLUDED

#include <string>
#include <memory>
#include <boost/cstdint.hpp>
#include <boost/thread.hpp>
#include <boost/locale/config.hpp>
#include <boost/locale/hold_ptr.hpp>

#include <unicode/locid.h>
#include <unicode/numfmt.h>
#include <unicode/rbnf.h>
#include <unicode/datefmt.h>
#include <unicode/smpdtfmt.h>
#include <unicode/decimfmt.h>

namespace boost {
namespace locale {
    namespace impl_icu {        

        class icu_formatters_cache : public std::locale::facet {
        public:

            static std::locale::id id;

            icu_formatters_cache(icu::Locale const &locale) :
                locale_(locale)
            {

                static const icu::DateFormat::EStyle styles[4] = { 
                    icu::DateFormat::kShort,
                    icu::DateFormat::kMedium,
                    icu::DateFormat::kLong,
                    icu::DateFormat::kFull
                };


                for(int i=0;i<4;i++) {
                    hold_ptr<icu::DateFormat> fmt(icu::DateFormat::createDateInstance(styles[i],locale));
                    icu::SimpleDateFormat *sfmt = dynamic_cast<icu::SimpleDateFormat*>(fmt.get());
                    if(sfmt) {
                        sfmt->toPattern(date_format_[i]);
                    }
                }

                for(int i=0;i<4;i++) {
                    hold_ptr<icu::DateFormat> fmt(icu::DateFormat::createTimeInstance(styles[i],locale));
                    icu::SimpleDateFormat *sfmt = dynamic_cast<icu::SimpleDateFormat*>(fmt.get());
                    if(sfmt) {
                        sfmt->toPattern(time_format_[i]);
                    }
                }

                for(int i=0;i<4;i++) {
                    for(int j=0;j<4;j++) {
                        hold_ptr<icu::DateFormat> fmt(
                            icu::DateFormat::createDateTimeInstance(styles[i],styles[j],locale));
                        icu::SimpleDateFormat *sfmt = dynamic_cast<icu::SimpleDateFormat*>(fmt.get());
                        if(sfmt) {
                            sfmt->toPattern(date_time_format_[i][j]);
                        }
                    }
                }


            }

            typedef enum {
                fmt_number,
                fmt_sci,
                fmt_curr_nat,
                fmt_curr_iso,
                fmt_per,
                fmt_spell,
                fmt_ord,
                fmt_count
            } fmt_type;

            icu::NumberFormat *number_format(fmt_type type) const
            {
                icu::NumberFormat *ptr = number_format_[type].get();
                if(ptr)
                    return ptr;
                UErrorCode err=U_ZERO_ERROR;
                hold_ptr<icu::NumberFormat> ap;

                switch(type) {
                case fmt_number:
                    ap.reset(icu::NumberFormat::createInstance(locale_,err));
                    break;
                case fmt_sci:
                    ap.reset(icu::NumberFormat::createScientificInstance(locale_,err));
                    break;
                #if U_ICU_VERSION_MAJOR_NUM*100 + U_ICU_VERSION_MINOR_NUM >= 402
                    #if U_ICU_VERSION_MAJOR_NUM*100 + U_ICU_VERSION_MINOR_NUM >= 408
                    case fmt_curr_nat:
                        ap.reset(icu::NumberFormat::createInstance(locale_,UNUM_CURRENCY,err));
                        break;
                    case fmt_curr_iso:
                        ap.reset(icu::NumberFormat::createInstance(locale_,UNUM_CURRENCY_ISO,err));
                        break;
                    #else
                    case fmt_curr_nat:
                        ap.reset(icu::NumberFormat::createInstance(locale_,icu::NumberFormat::kCurrencyStyle,err));
                        break;
                    case fmt_curr_iso:
                        ap.reset(icu::NumberFormat::createInstance(locale_,icu::NumberFormat::kIsoCurrencyStyle,err));
                        break;
                    #endif
                #else
                case fmt_curr_nat:
                case fmt_curr_iso:
                    ap.reset(icu::NumberFormat::createCurrencyInstance(locale_,err));
                    break;
                #endif
                case fmt_per:
                    ap.reset(icu::NumberFormat::createPercentInstance(locale_,err));
                    break;
                case fmt_spell:
                    ap.reset(new icu::RuleBasedNumberFormat(icu::URBNF_SPELLOUT,locale_,err));
                    break;
                case fmt_ord:
                    ap.reset(new icu::RuleBasedNumberFormat(icu::URBNF_ORDINAL,locale_,err));
                    break;
                default:
                    throw std::runtime_error("locale::internal error should not get there");
                }

                test(err);
                ptr = ap.get();
                number_format_[type].reset(ap.release());
                return ptr;
            }

            void test(UErrorCode err) const
            {
                if(U_FAILURE(err))
                    throw std::runtime_error("Failed to create a formatter");
            }
            
            icu::UnicodeString date_format_[4];
            icu::UnicodeString time_format_[4];
            icu::UnicodeString date_time_format_[4][4];

            icu::SimpleDateFormat *date_formatter() const
            {
                icu::SimpleDateFormat *p=date_formatter_.get();
                if(p)
                    return p;

                hold_ptr<icu::DateFormat> fmt(icu::DateFormat::createDateTimeInstance(
                    icu::DateFormat::kMedium,
                    icu::DateFormat::kMedium,
                    locale_));
                
                if(dynamic_cast<icu::SimpleDateFormat *>(fmt.get())) {
                    p = static_cast<icu::SimpleDateFormat *>(fmt.release());
                    date_formatter_.reset(p);
                }
                return p;
            }
        
        private:

            mutable boost::thread_specific_ptr<icu::NumberFormat>    number_format_[fmt_count];
            mutable boost::thread_specific_ptr<icu::SimpleDateFormat> date_formatter_;
            icu::Locale locale_;
        };



    } // namespace impl_icu
} // namespace locale
} // namespace boost



#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

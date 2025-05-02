//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_UTIL_NUMERIC_HPP
#define BOOST_LOCALE_IMPL_UTIL_NUMERIC_HPP
#include <boost/locale/formatting.hpp>
#include <boost/locale/info.hpp>
#include <boost/predef/os.h>
#include <algorithm>
#include <ctime>
#include <ios>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "timezone.hpp"

namespace boost { namespace locale { namespace util {
    template<typename CharType>
    struct formatting_size_traits {
        static size_t size(const std::basic_string<CharType>& s, const std::locale& /*l*/) { return s.size(); }
    };

    template<>
    struct formatting_size_traits<char> {
        static size_t size(const std::string& s, const std::locale& l)
        {
            if(!std::has_facet<info>(l))
                return s.size();
            if(!std::use_facet<info>(l).utf8())
                return s.size();
            // count code points, poor man's text size
            return std::count_if(s.begin(), s.end(), [](const unsigned char c) {
                return (c <= 127)               // ASCII
                       || ((c & 0xC0) == 0xC0); // first UTF-8 byte
            });
        }
    };

    template<typename CharType>
    class base_num_format : public std::num_put<CharType> {
    public:
        typedef typename std::num_put<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;

        base_num_format(size_t refs = 0) : std::num_put<CharType>(refs) {}

    protected:
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, unsigned long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, double val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, long double val) const override
        {
            return do_real_put(out, ios, fill, val);
        }

        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, long long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }
        iter_type do_put(iter_type out, std::ios_base& ios, CharType fill, unsigned long long val) const override
        {
            return do_real_put(out, ios, fill, val);
        }

    private:
        template<typename ValueType>
        iter_type do_real_put(iter_type out, std::ios_base& ios, CharType fill, ValueType val) const
        {
            typedef std::num_put<CharType> super;

            ios_info& info = ios_info::get(ios);

            switch(info.display_flags()) {
                case flags::posix: {
                    typedef std::basic_ostringstream<CharType> sstream_type;
                    sstream_type ss;
                    ss.imbue(std::locale::classic());
                    ss.flags(ios.flags());
                    ss.precision(ios.precision());
                    ss.width(ios.width());
                    iter_type ret_ptr = super::do_put(out, ss, fill, val);
                    ios.width(0);
                    return ret_ptr;
                }
                case flags::date: return format_time(out, ios, fill, static_cast<std::time_t>(val), 'x');
                case flags::time: return format_time(out, ios, fill, static_cast<std::time_t>(val), 'X');
                case flags::datetime: return format_time(out, ios, fill, static_cast<std::time_t>(val), 'c');
                case flags::strftime:
                    return format_time(out,
                                       ios,
                                       fill,
                                       static_cast<std::time_t>(val),
                                       info.date_time_pattern<CharType>());
                case flags::currency: {
                    bool nat = info.currency_flags() == flags::currency_default
                               || info.currency_flags() == flags::currency_national;
                    bool intl = !nat;
                    return do_format_currency(intl, out, ios, fill, static_cast<long double>(val));
                }

                case flags::number:
                case flags::percent:
                case flags::spellout:
                case flags::ordinal: break;
            }
            return super::do_put(out, ios, fill, val);
        }

        virtual iter_type
        do_format_currency(bool intl, iter_type out, std::ios_base& ios, CharType fill, long double val) const
        {
            if(intl)
                return format_currency<true>(out, ios, fill, val);
            else
                return format_currency<false>(out, ios, fill, val);
        }

        template<bool intl>
        iter_type format_currency(iter_type out, std::ios_base& ios, CharType fill, long double val) const
        {
            std::locale loc = ios.getloc();
            int digits = std::use_facet<std::moneypunct<CharType, intl>>(loc).frac_digits();
            while(digits > 0) {
                val *= 10;
                digits--;
            }
            std::ios_base::fmtflags f = ios.flags();
            ios.flags(f | std::ios_base::showbase);
            out = std::use_facet<std::money_put<CharType>>(loc).put(out, intl, ios, fill, val);
            ios.flags(f);
            return out;
        }

        iter_type format_time(iter_type out, std::ios_base& ios, CharType fill, std::time_t time, char c) const
        {
            string_type fmt;
            fmt += CharType('%');
            fmt += CharType(c);
            return format_time(out, ios, fill, time, fmt);
        }

        iter_type
        format_time(iter_type out, std::ios_base& ios, CharType fill, std::time_t time, const string_type& format) const
        {
            const std::string& tz = ios_info::get(ios).time_zone();
#if BOOST_OS_BSD_FREE || defined(__APPLE__)
            std::vector<char> tz_nonconst;
#endif
            std::tm tm;
            if(tz.empty()) {
#ifdef BOOST_WINDOWS
                // Windows uses TLS
                tm = *localtime(&time);
#else
                localtime_r(&time, &tm);
#endif
            } else {
                int gmtoff = parse_tz(tz);
                time += gmtoff;
#ifdef BOOST_WINDOWS
                // Windows uses TLS
                tm = *gmtime(&time);
#else
                gmtime_r(&time, &tm);
#endif

#if BOOST_OS_LINUX || BOOST_OS_BSD_FREE || defined(__APPLE__)
                // These have extra fields to specify timezone
                if(gmtoff != 0) {
#    if BOOST_OS_BSD_FREE || defined(__APPLE__)
                    // bsd and apple want tm_zone be non-const
                    tz_nonconst.assign(tz.begin(), tz.end());
                    tm.tm_zone = tz_nonconst.data();
#    else
                    tm.tm_zone = tz.data();
#    endif
                    tm.tm_gmtoff = gmtoff;
                }
#endif
            }
            std::basic_ostringstream<CharType> tmp_out;
            std::use_facet<std::time_put<CharType>>(ios.getloc())
              .put(tmp_out, tmp_out, fill, &tm, format.c_str(), format.c_str() + format.size());
            string_type str = tmp_out.str();
            std::streamsize on_left = 0, on_right = 0;
            std::streamsize points = formatting_size_traits<CharType>::size(str, ios.getloc());
            if(points < ios.width()) {
                std::streamsize n = ios.width() - points;

                std::ios_base::fmtflags flags = ios.flags() & std::ios_base::adjustfield;

                // we do not really know internal point, so we assume that it does not
                // exist. so according to the standard field should be right aligned
                if(flags != std::ios_base::left)
                    on_left = n;
                on_right = n - on_left;
            }
            while(on_left > 0) {
                *out++ = fill;
                on_left--;
            }
            std::copy(str.begin(), str.end(), out);
            while(on_right > 0) {
                *out++ = fill;
                on_right--;
            }
            ios.width(0);
            return out;
        }

    }; // num_format

    template<typename CharType>
    class base_num_parse : public std::num_get<CharType> {
    public:
        base_num_parse(size_t refs = 0) : std::num_get<CharType>(refs) {}

    protected:
        typedef typename std::num_get<CharType>::iter_type iter_type;
        typedef std::basic_string<CharType> string_type;

        iter_type
        do_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned short& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned int& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type
        do_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, float& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type
        do_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, double& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         long double& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         long long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

        iter_type do_get(iter_type in,
                         iter_type end,
                         std::ios_base& ios,
                         std::ios_base::iostate& err,
                         unsigned long long& val) const override
        {
            return do_real_get(in, end, ios, err, val);
        }

    private:
        template<typename ValueType>
        iter_type
        do_real_get(iter_type in, iter_type end, std::ios_base& ios, std::ios_base::iostate& err, ValueType& val) const
        {
            typedef std::num_get<CharType> super;

            ios_info& info = ios_info::get(ios);

            switch(info.display_flags()) {
                case flags::posix: {
                    std::stringstream ss;
                    ss.imbue(std::locale::classic());
                    ss.flags(ios.flags());
                    ss.precision(ios.precision());
                    return super::do_get(in, end, ss, err, val);
                }
                case flags::currency: {
                    long double ret_val = 0;
                    if(info.currency_flags() == flags::currency_default
                       || info.currency_flags() == flags::currency_national)
                        in = parse_currency<false>(in, end, ios, err, ret_val);
                    else
                        in = parse_currency<true>(in, end, ios, err, ret_val);
                    if(!(err & std::ios_base::failbit))
                        val = static_cast<ValueType>(ret_val);
                    return in;
                }

                // date-time parsing is not supported
                // due to buggy standard
                case flags::date:
                case flags::time:
                case flags::datetime:
                case flags::strftime:

                case flags::number:
                case flags::percent:
                case flags::spellout:
                case flags::ordinal: break;
            }
            return super::do_get(in, end, ios, err, val);
        }

        template<bool intl>
        iter_type parse_currency(iter_type in,
                                 iter_type end,
                                 std::ios_base& ios,
                                 std::ios_base::iostate& err,
                                 long double& val) const
        {
            std::locale loc = ios.getloc();
            int digits = std::use_facet<std::moneypunct<CharType, intl>>(loc).frac_digits();
            long double rval;
            in = std::use_facet<std::money_get<CharType>>(loc).get(in, end, intl, ios, err, rval);
            if(!(err & std::ios::failbit)) {
                while(digits > 0) {
                    rval /= 10;
                    digits--;
                }
                val = rval;
            }
            return in;
        }
    };

}}} // namespace boost::locale::util

#endif

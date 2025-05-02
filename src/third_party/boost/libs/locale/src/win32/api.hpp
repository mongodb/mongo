//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_WIN32_API_HPP
#define BOOST_LOCALE_IMPL_WIN32_API_HPP

#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lcid.hpp"

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#ifndef UNICODE
#    define UNICODE
#endif
#include <windows.h>

#include <boost/locale/collator.hpp>
#include <boost/locale/conversion.hpp>
#include <boost/assert.hpp>

namespace boost { namespace locale { namespace impl_win {

    struct numeric_info {
        std::wstring thousands_sep;
        std::wstring decimal_point;
        std::string grouping;
    };

    inline DWORD collation_level_to_flag(collate_level level)
    {
        switch(level) {
            case collate_level::primary: return NORM_IGNORESYMBOLS | NORM_IGNORECASE | NORM_IGNORENONSPACE;
            case collate_level::secondary: return NORM_IGNORESYMBOLS | NORM_IGNORECASE;
            case collate_level::tertiary: return NORM_IGNORESYMBOLS;
            case collate_level::quaternary:
            case collate_level::identical: return 0;
        }
        return 0; // LCOV_EXCL_LINE
    }

    struct winlocale {
        explicit winlocale(unsigned locale_id = 0) : lcid(locale_id) {}
        explicit winlocale(const std::string& name) { lcid = locale_to_lcid(name); }

        bool is_c() const { return lcid == 0; }

        unsigned lcid;
    };

    ////////////////////////////////////////////////////////////////////////
    ///
    /// Number Format
    ///
    ////////////////////////////////////////////////////////////////////////

    inline numeric_info wcsnumformat_l(const winlocale& l)
    {
        numeric_info res;
        res.decimal_point = L'.';
        unsigned lcid = l.lcid;

        if(lcid == 0)
            return res;

        // limits according to MSDN
        constexpr int th_size = 4;
        constexpr int de_size = 4;
        constexpr int gr_size = 10;

        wchar_t th[th_size] = {0};
        wchar_t de[de_size] = {0};
        wchar_t gr[gr_size] = {0};

        if(GetLocaleInfoW(lcid, LOCALE_STHOUSAND, th, th_size) == 0
           || GetLocaleInfoW(lcid, LOCALE_SDECIMAL, de, de_size) == 0
           || GetLocaleInfoW(lcid, LOCALE_SGROUPING, gr, gr_size) == 0)
        {
            return res; // LCOV_EXCL_LINE
        }
        res.decimal_point = de;
        res.thousands_sep = th;
        bool inf_group = false;
        for(unsigned i = 0; gr[i]; i++) {
            if(gr[i] == L';')
                continue;
            if(L'1' <= gr[i] && gr[i] <= L'9')
                res.grouping += char(gr[i] - L'0');
            else if(gr[i] == L'0')
                inf_group = true;
        }
        if(!inf_group) {
            BOOST_LOCALE_START_CONST_CONDITION
            if(std::numeric_limits<char>::is_signed)
                res.grouping += std::numeric_limits<char>::min();
            else
                res.grouping += std::numeric_limits<char>::max();
            BOOST_LOCALE_END_CONST_CONDITION
        }
        return res;
    }

    inline std::wstring win_map_string_l(unsigned flags, const wchar_t* begin, const wchar_t* end, const winlocale& l)
    {
        std::wstring res;
        if(end - begin > std::numeric_limits<int>::max())
            throw std::length_error("String to long for int type");
        int len = LCMapStringW(l.lcid, flags, begin, static_cast<int>(end - begin), 0, 0);
        if(len == 0)
            return res; // LCOV_EXCL_LINE
        if(len == std::numeric_limits<int>::max())
            throw std::length_error("String to long for int type");
        std::vector<wchar_t> buf(len + 1);
        int l2 =
          LCMapStringW(l.lcid, flags, begin, static_cast<int>(end - begin), buf.data(), static_cast<int>(buf.size()));
        res.assign(buf.data(), l2);
        return res;
    }

    ////////////////////////////////////////////////////////////////////////
    ///
    /// Collation
    ///
    ////////////////////////////////////////////////////////////////////////

    inline int wcscoll_l(collate_level level,
                         const wchar_t* lb,
                         const wchar_t* le,
                         const wchar_t* rb,
                         const wchar_t* re,
                         const winlocale& l)
    {
        if(le - lb > std::numeric_limits<int>::max() || re - rb > std::numeric_limits<int>::max())
            throw std::length_error("String to long for int type");
        const int result = CompareStringW(l.lcid,
                                          collation_level_to_flag(level),
                                          lb,
                                          static_cast<int>(le - lb),
                                          rb,
                                          static_cast<int>(re - rb));
        BOOST_ASSERT_MSG(result != 0, "CompareStringW failed");
        return result - 2; // Subtract 2 to get the meaning of <0, ==0, and >0
    }

    ////////////////////////////////////////////////////////////////////////
    ///
    /// Money Format
    ///
    ////////////////////////////////////////////////////////////////////////

    inline std::wstring wcsfmon_l(double value, const winlocale& l)
    {
        std::wostringstream ss;
        ss.imbue(std::locale::classic());

        ss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value;
        std::wstring sval = ss.str();
        const auto len = GetCurrencyFormatW(l.lcid, 0, sval.c_str(), nullptr, nullptr, 0);
        std::vector<wchar_t> buf(len + 1);
        GetCurrencyFormatW(l.lcid, 0, sval.c_str(), nullptr, buf.data(), len);
        return buf.data();
    }

    ////////////////////////////////////////////////////////////////////////
    ///
    /// Time Format
    ///
    ////////////////////////////////////////////////////////////////////////

    inline std::wstring wcs_format_date_l(const wchar_t* format, SYSTEMTIME const* tm, const winlocale& l)
    {
        const auto len = GetDateFormatW(l.lcid, 0, tm, format, nullptr, 0);
        std::vector<wchar_t> buf(len + 1);
        GetDateFormatW(l.lcid, 0, tm, format, buf.data(), len);
        return buf.data();
    }

    inline std::wstring wcs_format_time_l(const wchar_t* format, SYSTEMTIME const* tm, const winlocale& l)
    {
        const auto len = GetTimeFormatW(l.lcid, 0, tm, format, nullptr, 0);
        std::vector<wchar_t> buf(len + 1);
        GetTimeFormatW(l.lcid, 0, tm, format, buf.data(), len);
        return buf.data();
    }

    inline std::wstring wcsfold(const wchar_t* begin, const wchar_t* end)
    {
        const winlocale l(0x0409); // en-US
        return win_map_string_l(LCMAP_LOWERCASE, begin, end, l);
    }

    inline std::wstring wcsnormalize(norm_type norm, const wchar_t* begin, const wchar_t* end)
    {
        // We use FoldString, under Vista it actually does normalization;
        // under XP and below it does something similar, half job, better then nothing
        unsigned flags = MAP_PRECOMPOSED;
        switch(norm) {
            case norm_nfd: flags = MAP_COMPOSITE; break;
            case norm_nfc: flags = MAP_PRECOMPOSED; break;
            case norm_nfkd: flags = MAP_FOLDCZONE; break;
            case norm_nfkc: flags = MAP_FOLDCZONE | MAP_COMPOSITE; break;
        }

        if(end - begin > std::numeric_limits<int>::max())
            throw std::length_error("String to long for int type");
        int len = FoldStringW(flags, begin, static_cast<int>(end - begin), nullptr, 0);
        if(len == 0)
            return std::wstring(); // LCOV_EXCL_LINE
        if(len == std::numeric_limits<int>::max())
            throw std::length_error("String to long for int type");
        std::vector<wchar_t> v(len + 1);
        len = FoldStringW(flags, begin, static_cast<int>(end - begin), v.data(), len + 1);
        return std::wstring(v.data(), len);
    }

    inline std::wstring wcsxfrm_l(collate_level level, const wchar_t* begin, const wchar_t* end, const winlocale& l)
    {
        return win_map_string_l(LCMAP_SORTKEY | collation_level_to_flag(level), begin, end, l);
    }

    inline std::wstring towupper_l(const wchar_t* begin, const wchar_t* end, const winlocale& l)
    {
        return win_map_string_l(LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, begin, end, l);
    }

    inline std::wstring towlower_l(const wchar_t* begin, const wchar_t* end, const winlocale& l)
    {
        return win_map_string_l(LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, begin, end, l);
    }

    inline std::wstring wcsftime_l(char c, const std::tm* tm, const winlocale& l)
    {
        SYSTEMTIME wtm = SYSTEMTIME();
        wtm.wYear = static_cast<WORD>(tm->tm_year + 1900);
        wtm.wMonth = static_cast<WORD>(tm->tm_mon + 1);
        wtm.wDayOfWeek = static_cast<WORD>(tm->tm_wday);
        wtm.wDay = static_cast<WORD>(tm->tm_mday);
        wtm.wHour = static_cast<WORD>(tm->tm_hour);
        wtm.wMinute = static_cast<WORD>(tm->tm_min);
        wtm.wSecond = static_cast<WORD>(tm->tm_sec);
        switch(c) {
            case 'a': // Abbr Weekday
                return wcs_format_date_l(L"ddd", &wtm, l);
            case 'A': // Full Weekday
                return wcs_format_date_l(L"dddd", &wtm, l);
            case 'b': // Abbr Month
                return wcs_format_date_l(L"MMM", &wtm, l);
            case 'B': // Full Month
                return wcs_format_date_l(L"MMMM", &wtm, l);
            case 'c': // DateTile Full
                return wcs_format_date_l(0, &wtm, l) + L" " + wcs_format_time_l(0, &wtm, l);
            // not supported by WIN ;(
            //  case 'C': // Century -> 1980 -> 19
            //  retur
            case 'd': // Day of Month [01,31]
                return wcs_format_date_l(L"dd", &wtm, l);
            case 'D': // %m/%d/%y
                return wcs_format_date_l(L"MM/dd/yy", &wtm, l);
            case 'e': // Day of Month [1,31]
                return wcs_format_date_l(L"d", &wtm, l);
            case 'h': // == b
                return wcs_format_date_l(L"MMM", &wtm, l);
            case 'H': // 24 clock hour 00,23
                return wcs_format_time_l(L"HH", &wtm, l);
            case 'I': // 12 clock hour 01,12
                return wcs_format_time_l(L"hh", &wtm, l);
            /*
            case 'j': // day of year 001,366
                return "D";*/
            case 'm': // month as [01,12]
                return wcs_format_date_l(L"MM", &wtm, l);
            case 'M': // minute [00,59]
                return wcs_format_time_l(L"mm", &wtm, l);
            case 'n': // \n
                return L"\n";
            case 'p': // am-pm
                return wcs_format_time_l(L"tt", &wtm, l);
            case 'r': // time with AM/PM %I:%M:%S %p
                return wcs_format_time_l(L"hh:mm:ss tt", &wtm, l);
            case 'R': // %H:%M
                return wcs_format_time_l(L"HH:mm", &wtm, l);
            case 'S': // second [00,61]
                return wcs_format_time_l(L"ss", &wtm, l);
            case 't': // \t
                return L"\t";
            case 'T': // %H:%M:%S
                return wcs_format_time_l(L"HH:mm:ss", &wtm, l);
                /*          case 'u': // weekday 1,7 1=Monday
                        case 'U': // week number of year [00,53] Sunday first
                        case 'V': // week number of year [01,53] Monday first
                        case 'w': // weekday 0,7 0=Sunday
                        case 'W': // week number of year [00,53] Monday first, */
            case 'x': // Date
                return wcs_format_date_l(0, &wtm, l);
            case 'X': // Time
                return wcs_format_time_l(0, &wtm, l);
            case 'y': // Year [00-99]
                return wcs_format_date_l(L"yy", &wtm, l);
            case 'Y': // Year 1998
                return wcs_format_date_l(L"yyyy", &wtm, l);
            case '%': // %
                return L"%";
            default: return L"";
        }
    }

}}} // namespace boost::locale::impl_win

// boostinspect:nominmax
#endif

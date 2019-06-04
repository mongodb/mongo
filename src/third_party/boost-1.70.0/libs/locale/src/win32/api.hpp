//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_IMPL_WIN32_API_HPP
#define BOOST_LOCALE_IMPL_WIN32_API_HPP

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <limits>
#include <ctime>

#include "lcid.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <boost/locale/conversion.hpp>
#include <boost/locale/collator.hpp>

#define BOOST_LOCALE_WINDOWS_2000_API

#if defined(_WIN32_NT) && _WIN32_NT >= 0x600 && !defined(BOOST_LOCALE_WINDOWS_2000_API)
#define BOOST_LOCALE_WINDOWS_VISTA_API
#else
#define BOOST_LOCALE_WINDOWS_2000_API
#endif

namespace boost {
namespace locale {
namespace impl_win {
    
    struct numeric_info {
        std::wstring thousands_sep;
        std::wstring decimal_point;
        std::string grouping;
    };

    inline DWORD collation_level_to_flag(collator_base::level_type level)
    {
        DWORD flags;
        switch(level) {
        case collator_base::primary:
            flags = NORM_IGNORESYMBOLS | NORM_IGNORECASE | NORM_IGNORENONSPACE;
            break;
        case collator_base::secondary:
            flags = NORM_IGNORESYMBOLS | NORM_IGNORECASE;
            break;
        case collator_base::tertiary:
            flags = NORM_IGNORESYMBOLS;
            break;
        default:
            flags = 0;
        }
        return flags;
    }

    
  
    #ifdef BOOST_LOCALE_WINDOWS_2000_API 
    
    class winlocale{
    public:
        winlocale() : 
            lcid(0) 
        {
        }

        winlocale(std::string const &name)
        {
            lcid = locale_to_lcid(name);
        }
        
        unsigned lcid;
        
        bool is_c() const
        {
            return lcid == 0;
        }
    };


    ////////////////////////////////////////////////////////////////////////
    ///
    /// Number Format
    ///
    ////////////////////////////////////////////////////////////////////////
    
    inline numeric_info wcsnumformat_l(winlocale const &l)
    {
        numeric_info res;
        res.decimal_point = L'.';
        unsigned lcid = l.lcid;

        if(lcid == 0)
            return res;

        // limits according to MSDN
        static const int th_size = 4;
        static const int de_size = 4;
        static const int gr_size = 10;

        wchar_t th[th_size]={0}; 
        wchar_t de[de_size]={0};
        wchar_t gr[gr_size]={0}; 

        if( GetLocaleInfoW(lcid,LOCALE_STHOUSAND,th,th_size)==0
            || GetLocaleInfoW(lcid,LOCALE_SDECIMAL ,de,de_size)==0
            || GetLocaleInfoW(lcid,LOCALE_SGROUPING,gr,gr_size)==0)
        {
            return res;
        }
        res.decimal_point = de;
        res.thousands_sep = th;
        bool inf_group = false;
        for(unsigned i=0;gr[i];i++) {
            if(gr[i]==L';')
                continue;
            if(L'1'<= gr[i] && gr[i]<=L'9') {
                res.grouping += char(gr[i]-L'0');
            }
            else if(gr[i]==L'0')
                inf_group = true;
        }
        if(!inf_group) {
            if(std::numeric_limits<char>::is_signed) {
                res.grouping+=std::numeric_limits<char>::min();
            }
            else {
                res.grouping+=std::numeric_limits<char>::max();
            }
        }
        return res;
    }

    inline std::wstring win_map_string_l(unsigned flags,wchar_t const *begin,wchar_t const *end,winlocale const &l)
    {
        std::wstring res;
        int len = LCMapStringW(l.lcid,flags,begin,end-begin,0,0);
        if(len == 0)
            return res;
        std::vector<wchar_t> buf(len+1);
        int l2 = LCMapStringW(l.lcid,flags,begin,end-begin,&buf.front(),buf.size());
        res.assign(&buf.front(),l2);
        return res;
    }

    ////////////////////////////////////////////////////////////////////////
    ///
    /// Collation
    ///
    ////////////////////////////////////////////////////////////////////////


    inline int wcscoll_l(   collator_base::level_type level,
                            wchar_t const *lb,wchar_t const *le,
                            wchar_t const *rb,wchar_t const *re,
                            winlocale const &l)
    {
        return CompareStringW(l.lcid,collation_level_to_flag(level),lb,le-lb,rb,re-rb) - 2;
    }


    ////////////////////////////////////////////////////////////////////////
    ///
    /// Money Format
    ///
    ////////////////////////////////////////////////////////////////////////

    inline std::wstring wcsfmon_l(double value,winlocale const &l)
    {
        std::wostringstream ss;
        ss.imbue(std::locale::classic());

        ss << std::setprecision(std::numeric_limits<double>::digits10+1) << value;
        std::wstring sval = ss.str();
        int len = GetCurrencyFormatW(l.lcid,0,sval.c_str(),0,0,0);
        std::vector<wchar_t> buf(len+1);
        GetCurrencyFormatW(l.lcid,0,sval.c_str(),0,&buf.front(),len);
        return &buf.front();
    }

    ////////////////////////////////////////////////////////////////////////
    ///
    /// Time Format
    ///
    ////////////////////////////////////////////////////////////////////////

    
    inline std::wstring wcs_format_date_l(wchar_t const *format,SYSTEMTIME const *tm,winlocale const &l)
    {
        int len = GetDateFormatW(l.lcid,0,tm,format,0,0);
        std::vector<wchar_t> buf(len+1);
        GetDateFormatW(l.lcid,0,tm,format,&buf.front(),len);
        return &buf.front(); 
    }
    
    inline std::wstring wcs_format_time_l(wchar_t const *format,SYSTEMTIME const *tm,winlocale const &l)
    {
        int len = GetTimeFormatW(l.lcid,0,tm,format,0,0);
        std::vector<wchar_t> buf(len+1);
        GetTimeFormatW(l.lcid,0,tm,format,&buf.front(),len);
        return &buf.front(); 
    }

    inline std::wstring wcsfold(wchar_t const *begin,wchar_t const *end)
    {
        winlocale l;
        l.lcid = 0x0409; // en-US
        return win_map_string_l(LCMAP_LOWERCASE,begin,end,l);
    }

    inline std::wstring wcsnormalize(norm_type norm,wchar_t const *begin,wchar_t const *end)
    {
        // We use FoldString, under Vista it actually does normalization;
        // under XP and below it does something similar, half job, better then nothing
        unsigned flags = 0;
        switch(norm) {
        case norm_nfd:
            flags = MAP_COMPOSITE;
            break;
        case norm_nfc:
            flags = MAP_PRECOMPOSED;
            break;
        case norm_nfkd:
            flags = MAP_FOLDCZONE;
            break;
        case norm_nfkc:
            flags = MAP_FOLDCZONE | MAP_COMPOSITE;
            break;
        default:
            flags = MAP_PRECOMPOSED;
        }

        int len = FoldStringW(flags,begin,end-begin,0,0);
        if(len == 0)
            return std::wstring();
        std::vector<wchar_t> v(len+1);
        len = FoldStringW(flags,begin,end-begin,&v.front(),len+1);
        return std::wstring(&v.front(),len);
    }


    #endif

    inline std::wstring wcsxfrm_l(collator_base::level_type level,wchar_t const *begin,wchar_t const *end,winlocale const &l)
    {
        int flag = LCMAP_SORTKEY | collation_level_to_flag(level);

        return win_map_string_l(flag,begin,end,l);
    }

    inline std::wstring towupper_l(wchar_t const *begin,wchar_t const *end,winlocale const &l)
    {
        return win_map_string_l(LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING,begin,end,l);
    }

    inline std::wstring towlower_l(wchar_t const *begin,wchar_t const *end,winlocale const &l)
    {
        return win_map_string_l(LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,begin,end,l);
    }

    inline std::wstring wcsftime_l(char c,std::tm const *tm,winlocale const &l)
    {
        SYSTEMTIME wtm=SYSTEMTIME();
        wtm.wYear = tm->tm_year + 1900;
        wtm.wMonth = tm->tm_mon+1;
        wtm.wDayOfWeek = tm->tm_wday;
        wtm.wDay = tm->tm_mday;
        wtm.wHour = tm->tm_hour;
        wtm.wMinute = tm->tm_min;
        wtm.wSecond = tm->tm_sec;
        switch(c) {
        case 'a': // Abbr Weekday
            return wcs_format_date_l(L"ddd",&wtm,l);
        case 'A': // Full Weekday
            return wcs_format_date_l(L"dddd",&wtm,l);
        case 'b': // Abbr Month
            return wcs_format_date_l(L"MMM",&wtm,l);
        case 'B': // Full Month
            return wcs_format_date_l(L"MMMM",&wtm,l);
        case 'c': // DateTile Full
            return wcs_format_date_l(0,&wtm,l) + L" " + wcs_format_time_l(0,&wtm,l);
        // not supported by WIN ;(
        //  case 'C': // Century -> 1980 -> 19
        //  retur
        case 'd': // Day of Month [01,31]
            return wcs_format_date_l(L"dd",&wtm,l);
        case 'D': // %m/%d/%y
            return wcs_format_date_l(L"MM/dd/yy",&wtm,l);
        case 'e': // Day of Month [1,31]
            return wcs_format_date_l(L"d",&wtm,l);
        case 'h': // == b
            return wcs_format_date_l(L"MMM",&wtm,l);
        case 'H': // 24 clock hour 00,23
            return wcs_format_time_l(L"HH",&wtm,l);
        case 'I': // 12 clock hour 01,12
            return wcs_format_time_l(L"hh",&wtm,l);
        /*
        case 'j': // day of year 001,366
            return "D";*/
        case 'm': // month as [01,12]
            return wcs_format_date_l(L"MM",&wtm,l);
        case 'M': // minute [00,59]
            return wcs_format_time_l(L"mm",&wtm,l);
        case 'n': // \n
            return L"\n";
        case 'p': // am-pm
            return wcs_format_time_l(L"tt",&wtm,l);
        case 'r': // time with AM/PM %I:%M:%S %p
            return wcs_format_time_l(L"hh:mm:ss tt",&wtm,l);
        case 'R': // %H:%M
            return wcs_format_time_l(L"HH:mm",&wtm,l);
        case 'S': // second [00,61]
            return wcs_format_time_l(L"ss",&wtm,l);
        case 't': // \t
            return L"\t";
        case 'T': // %H:%M:%S
            return wcs_format_time_l(L"HH:mm:ss",&wtm,l);
/*          case 'u': // weekday 1,7 1=Monday
        case 'U': // week number of year [00,53] Sunday first
        case 'V': // week number of year [01,53] Moday first
        case 'w': // weekday 0,7 0=Sunday
        case 'W': // week number of year [00,53] Moday first, */
        case 'x': // Date
            return wcs_format_date_l(0,&wtm,l);
        case 'X': // Time
            return wcs_format_time_l(0,&wtm,l);
        case 'y': // Year [00-99]
            return wcs_format_date_l(L"yy",&wtm,l);
        case 'Y': // Year 1998
            return wcs_format_date_l(L"yyyy",&wtm,l);
        case '%': // %
            return L"%";
        default:
            return L"";
        }
    }



} // win
} // locale
} // boost
#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4


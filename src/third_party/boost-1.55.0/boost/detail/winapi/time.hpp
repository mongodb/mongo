//  time.hpp  --------------------------------------------------------------//

//  Copyright 2010 Vicente J. Botet Escriba

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt


#ifndef BOOST_DETAIL_WINAPI_TIME_HPP
#define BOOST_DETAIL_WINAPI_TIME_HPP

#include <boost/detail/winapi/basic_types.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace detail {
namespace winapi {
#if defined( BOOST_USE_WINDOWS_H )
    typedef FILETIME FILETIME_;
    typedef PFILETIME PFILETIME_;
    typedef LPFILETIME LPFILETIME_;

    typedef SYSTEMTIME SYSTEMTIME_;
    typedef SYSTEMTIME* PSYSTEMTIME_;

    #ifdef BOOST_HAS_GETSYSTEMTIMEASFILETIME  // Windows CE does not define GetSystemTimeAsFileTime
    using ::GetSystemTimeAsFileTime;
    #endif
    using ::FileTimeToLocalFileTime;
    using ::GetSystemTime;
    using ::SystemTimeToFileTime;
    using ::GetTickCount;

#else
extern "C" {
    typedef struct _FILETIME {
        DWORD_ dwLowDateTime;
        DWORD_ dwHighDateTime;
    } FILETIME_, *PFILETIME_, *LPFILETIME_;

    typedef struct _SYSTEMTIME {
      WORD_ wYear;
      WORD_ wMonth;
      WORD_ wDayOfWeek;
      WORD_ wDay;
      WORD_ wHour;
      WORD_ wMinute;
      WORD_ wSecond;
      WORD_ wMilliseconds;
    } SYSTEMTIME_, *PSYSTEMTIME_;

    #ifdef BOOST_HAS_GETSYSTEMTIMEASFILETIME  // Windows CE does not define GetSystemTimeAsFileTime
    __declspec(dllimport) void WINAPI
        GetSystemTimeAsFileTime(FILETIME_* lpFileTime);
    #endif
    __declspec(dllimport) int WINAPI
        FileTimeToLocalFileTime(const FILETIME_* lpFileTime,
                FILETIME_* lpLocalFileTime);
    __declspec(dllimport) void WINAPI
        GetSystemTime(SYSTEMTIME_* lpSystemTime);
    __declspec(dllimport) int WINAPI
        SystemTimeToFileTime(const SYSTEMTIME_* lpSystemTime,
                FILETIME_* lpFileTime);
    __declspec(dllimport) DWORD_ WINAPI
        GetTickCount();
}
#endif

#ifndef BOOST_HAS_GETSYSTEMTIMEASFILETIME
inline void WINAPI GetSystemTimeAsFileTime(FILETIME_* lpFileTime)
{
    SYSTEMTIME_ st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, lpFileTime);
}
#endif

}
}
}

#endif // BOOST_DETAIL_WINAPI_TIME_HPP

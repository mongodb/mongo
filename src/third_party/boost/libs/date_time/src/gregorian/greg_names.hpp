/* Copyright (c) 2002-2004 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the 
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE_1_0.txt or http://www.boost.org/LICENSE_1_0.txt)
 * Author: Jeff Garland, Bart Garst
 * $Date$
 */



#ifndef DATE_TIME_SRC_GREG_NAMES_HPP___
#define DATE_TIME_SRC_GREG_NAMES_HPP___

#include "boost/date_time/gregorian/greg_month.hpp"
#include "boost/date_time/special_defs.hpp"
namespace boost {
namespace gregorian {


  const char* const short_month_names[NumMonths]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec", "NAM"};
  const char* const long_month_names[NumMonths]={"January","February","March","April","May","June","July","August","September","October","November","December","NotAMonth"};
  const char* const special_value_names[date_time::NumSpecialValues]={"not-a-date-time","-infinity","+infinity","min_date_time","max_date_time","not_special"};


  const char* const short_weekday_names[]={"Sun", "Mon", "Tue", 
                                           "Wed", "Thu", "Fri", "Sat"};
  const char* const long_weekday_names[]= {"Sunday","Monday","Tuesday",
                                           "Wednesday", "Thursday",
                                           "Friday", "Saturday"};

#ifndef BOOST_NO_STD_WSTRING
  const wchar_t* const w_short_month_names[NumMonths]={L"Jan",L"Feb",L"Mar",L"Apr",L"May",L"Jun",L"Jul",L"Aug",L"Sep",L"Oct",L"Nov",L"Dec",L"NAM"};
  const wchar_t* const w_long_month_names[NumMonths]={L"January",L"February",L"March",L"April",L"May",L"June",L"July",L"August",L"September",L"October",L"November",L"December",L"NotAMonth"};
  const wchar_t* const w_special_value_names[date_time::NumSpecialValues]={L"not-a-date-time",L"-infinity",L"+infinity",L"min_date_time",L"max_date_time",L"not_special"};

  const wchar_t* const w_short_weekday_names[]={L"Sun", L"Mon", L"Tue", 
                                           L"Wed", L"Thu", L"Fri", L"Sat"};
  const wchar_t* const w_long_weekday_names[]= {L"Sunday",L"Monday",L"Tuesday",
                                           L"Wednesday", L"Thursday",
                                           L"Friday", L"Saturday"};
#endif // BOOST_NO_STD_WSTRING
} } // boost::gregorian
#endif // DATE_TIME_SRC_GREG_NAMES_HPP___

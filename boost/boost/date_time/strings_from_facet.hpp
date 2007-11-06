#ifndef DATE_TIME_STRINGS_FROM_FACET__HPP___
#define DATE_TIME_STRINGS_FROM_FACET__HPP___

/* Copyright (c) 2004 CrystalClear Software, Inc.
 * Use, modification and distribution is subject to the
 * Boost Software License, Version 1.0. (See accompanying
 * file LICENSE-1.0 or http://www.boost.org/LICENSE-1.0)
 * Author: Jeff Garland
 * $Date: 2006/02/18 20:58:01 $
 */

#include <sstream>
#include <string>
#include <vector>
#include <locale>

namespace boost { namespace date_time {

//! This function gathers up all the month strings from a std::locale
/*! Using the time_put facet, this function creates a collection of
 *  all the month strings from a locale.  This is handy when building
 *  custom date parsers or formatters that need to be localized.
 *
 *@param charT The type of char to use when gathering typically char
 *             or wchar_t.
 *@param locale The locale to use when gathering the strings
 *@param short_strings True(default) to gather short strings,
 *                     false for long strings.
 *@return A vector of strings containing the strings in order. eg:
 *        Jan, Feb, Mar, etc.
 */
template<typename charT>
std::vector<std::basic_string<charT> >
gather_month_strings(const std::locale& locale, bool short_strings=true)
{
  typedef std::basic_string<charT> string_type;
  typedef std::vector<string_type> collection_type;
  typedef std::basic_ostringstream<charT> ostream_type;
  typedef std::ostreambuf_iterator<charT> ostream_iter_type;
  typedef std::basic_ostringstream<charT> stringstream_type;
  typedef std::time_put<charT>           time_put_facet_type;
  charT short_fmt[3] = { '%', 'b' };
  charT long_fmt[3]  = { '%', 'B' };
  collection_type months;
  string_type outfmt(short_fmt);
  if (!short_strings) {
    outfmt = long_fmt;
  }
  {
    //grab the needed strings by using the locale to
    //output each month
    for (int m=0; m < 12; m++) {
      tm tm_value;
      tm_value.tm_mon = m;
      stringstream_type ss;
      ostream_iter_type oitr(ss);
      std::use_facet<time_put_facet_type>(locale).put(oitr, ss, ss.fill(),
                                                      &tm_value,
                                                      &*outfmt.begin(),
                                                      &*outfmt.begin()+outfmt.size());
      months.push_back(ss.str());
    }
  }
  return months;
}

//! This function gathers up all the weekday strings from a std::locale
/*! Using the time_put facet, this function creates a collection of
 *  all the weekday strings from a locale starting with the string for
 *  'Sunday'.  This is handy when building custom date parsers or
 *  formatters that need to be localized.
 *
 *@param charT The type of char to use when gathering typically char
 *             or wchar_t.
 *@param locale The locale to use when gathering the strings
 *@param short_strings True(default) to gather short strings,
 *                     false for long strings.
 *@return A vector of strings containing the weekdays in order. eg:
 *        Sun, Mon, Tue, Wed, Thu, Fri, Sat
 */
template<typename charT>
std::vector<std::basic_string<charT> >
gather_weekday_strings(const std::locale& locale, bool short_strings=true)
{
  typedef std::basic_string<charT> string_type;
  typedef std::vector<string_type> collection_type;
  typedef std::basic_ostringstream<charT> ostream_type;
  typedef std::ostreambuf_iterator<charT> ostream_iter_type;
  typedef std::basic_ostringstream<charT> stringstream_type;
  typedef std::time_put<charT>           time_put_facet_type;
  charT short_fmt[3] = { '%', 'a' };
  charT long_fmt[3]  = { '%', 'A' };

  collection_type weekdays;


  string_type outfmt(short_fmt);
  if (!short_strings) {
    outfmt = long_fmt;
  }
  {
    //grab the needed strings by using the locale to
    //output each month / weekday
    for (int i=0; i < 7; i++) {
      tm tm_value;
      tm_value.tm_wday = i;
      stringstream_type ss;
      ostream_iter_type oitr(ss);
      std::use_facet<time_put_facet_type>(locale).put(oitr, ss, ss.fill(),
                                                      &tm_value,
                                                      &*outfmt.begin(),
                                                      &*outfmt.begin()+outfmt.size());

      weekdays.push_back(ss.str());
    }
  }
  return weekdays;
}

} } //namespace


#endif

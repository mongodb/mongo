//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/config.hpp>
#ifdef BOOST_MSVC
#  pragma warning(disable : 4996)
#endif
#include <locale>
#include <string>
#include <ios>
#include <boost/locale/date_time_facet.hpp>
#include <boost/locale/date_time.hpp>
#include <boost/locale/hold_ptr.hpp>
#include <stdlib.h>
#include <ctime>
#include <memory>
#include <algorithm>
#include <limits>

#include "timezone.hpp"
#include "gregorian.hpp"

namespace boost {
namespace locale {
namespace util {
    namespace {

        int is_leap(int year)
        {
            if(year % 400 == 0)
                return 1;
            if(year % 100 == 0)
                return 0;
            if(year % 4 == 0)
                return 1;
            return 0;
        }

        int days_in_month(int year,int month)
        {
            static const int tbl[2][12] = {
                { 31,28,31,30,31,30,31,31,30,31,30,31 },
                { 31,29,31,30,31,30,31,31,30,31,30,31 }
            };
            return tbl[is_leap(year)][month - 1];
        }

        inline int days_from_0(int year)
        {
            year--;
            return 365 * year + (year / 400) - (year/100) + (year / 4);
        }

        int days_from_1970(int year)
        {
            static const int days_from_0_to_1970 = days_from_0(1970);
            return days_from_0(year) - days_from_0_to_1970;
        }

        int days_from_1jan(int year,int month,int day)
        {
            static const int days[2][12] = {
                { 0,31,59,90,120,151,181,212,243,273,304,334 },
                { 0,31,60,91,121,152,182,213,244,274,305,335 }
            };
            return days[is_leap(year)][month-1] + day - 1;
        }
        
        std::time_t internal_timegm(std::tm const *t)
        {
            int year = t->tm_year + 1900;
            int month = t->tm_mon;
            if(month > 11) {
                year += month/12;
                month %= 12;
            }
            else if(month < 0) {
                int years_diff = (-month + 11)/12;
                year -= years_diff;
                month+=12 * years_diff;
            }
            month++;
            int day = t->tm_mday;
            int day_of_year = days_from_1jan(year,month,day);
            int days_since_epoch = days_from_1970(year) + day_of_year;
            
            std::time_t seconds_in_day = 3600 * 24;
            std::time_t result =  seconds_in_day * days_since_epoch + 3600 * t->tm_hour + 60 * t->tm_min + t->tm_sec;
            
            return result;
        }

    } // anon




    namespace {

        // Locale dependent data

        bool comparator(char const *left,char const *right)
        {
            return strcmp(left,right) < 0;
        }
        
        //
        // Ref: CLDR 1.9 common/supplemental/supplementalData.xml
        //
        // monday - default
        // fri - MV
        // sat - AE AF BH DJ DZ EG ER ET IQ IR JO KE KW LY MA OM QA SA SD SO SY TN YE
        // sun - AR AS AZ BW CA CN FO GE GL GU HK IL IN JM JP KG KR LA MH MN MO MP MT NZ PH PK SG TH TT TW UM US UZ VI ZW
        //

        int first_day_of_week(char const *terr) {
            static char const * const sat[] = {
                "AE","AF","BH","DJ","DZ","EG","ER","ET","IQ","IR",
                "JO","KE","KW","LY","MA","OM","QA","SA","SD","SO",
                "SY","TN","YE"
            };
            static char const * const sunday[] = {
                "AR","AS","AZ","BW","CA","CN","FO","GE","GL","GU",
                "HK","IL","IN","JM","JP","KG","KR","LA","MH","MN",
                "MO","MP","MT","NZ","PH","PK","SG","TH","TT","TW",
                "UM","US","UZ","VI","ZW" 
            };
            if(strcmp(terr,"MV") == 0)
                return 5; // fri
            if(std::binary_search<char const * const *>(sat,sat+sizeof(sat)/(sizeof(sat[0])),terr,comparator))
                return 6; // sat
            if(std::binary_search<char const * const *>(sunday,sunday+sizeof(sunday)/(sizeof(sunday[0])),terr,comparator))
                return 0; // sun
            // default
            return 1; // mon
        }
    }

    class gregorian_calendar : public abstract_calendar {
    public:
            
            gregorian_calendar(std::string const &terr)
            {
                first_day_of_week_ = first_day_of_week(terr.c_str());
                time_ = std::time(0);
                is_local_ = true;
                tzoff_ = 0;
                from_time(time_);
            }
                
            ///
            /// Make a polymorphic copy of the calendar
            ///
            virtual gregorian_calendar *clone() const
            {
                return new gregorian_calendar(*this);
            }

            ///
            /// Set specific \a value for period \a p, note not all values are settable.
            ///
            virtual void set_value(period::marks::period_mark p,int value) 
            {
                using namespace period::marks;
                switch(p) {
                case era:                        ///< Era i.e. AC, BC in Gregorian and Julian calendar, range [0,1]
                    return;
                case year:                       ///< Year, it is calendar specific
                case extended_year:              ///< Extended year for Gregorian/Julian calendars, where 1 BC == 0, 2 BC == -1.
                    tm_updated_.tm_year = value - 1900;
                    break;
                case month:
                    tm_updated_.tm_mon = value;
                    break;
                case day:
                    tm_updated_.tm_mday = value;
                    break;
                case hour:                       ///< 24 clock hour [0..23]
                    tm_updated_.tm_hour = value;
                    break;
                case hour_12:                    ///< 12 clock hour [0..11]
                    tm_updated_.tm_hour = tm_updated_.tm_hour / 12 * 12 + value;
                    break;
                case am_pm:                      ///< am or pm marker, [0..1]
                    tm_updated_.tm_hour = 12 * value + tm_updated_.tm_hour % 12;
                    break;
                case minute:                     ///< minute [0..59]
                    tm_updated_.tm_min = value;
                    break;
                case second:
                    tm_updated_.tm_sec = value;
                    break;
                case day_of_year:
                    normalize();
                    tm_updated_.tm_mday += (value - (tm_updated_.tm_yday + 1));
                    break;
                case day_of_week:           ///< Day of week, starting from Sunday, [1..7]
                    if(value < 1) // make sure it is positive 
                        value += (-value / 7) * 7 + 7;
                    // convert to local DOW
                    value = (value - 1 - first_day_of_week_ + 14) % 7 + 1;
                    // fall throght
                case day_of_week_local:     ///< Local day of week, for example in France Monday is 1, in US Sunday is 1, [1..7]
                    normalize();
                    tm_updated_.tm_mday += (value - 1) - (tm_updated_.tm_wday - first_day_of_week_ + 7) % 7;
                    break;
                case day_of_week_in_month:  ///< Original number of the day of the week in month. (1st sunday, 2nd sunday etc)
                case week_of_year:          ///< The week number in the year, 4 is the minimal number of days to be in month
                case week_of_month:         ///< The week number withing current month
                    {
                        normalize();
                        int current_week = get_value(p,current);
                        int diff = 7 * (value - current_week);
                        tm_updated_.tm_mday += diff;
                    }
                    break;
                case period::marks::first_day_of_week:          ///< For example Sunday in US, Monday in France
                default:
                    return;
                }
                normalized_ = false;
            }

            void normalize()
            {
                if(!normalized_) {
                    std::tm val = tm_updated_;
                    val.tm_isdst = -1;
                    val.tm_wday = -1; // indecator of error
                    std::time_t point = -1;
                    if(is_local_) {
                        point = std::mktime(&val);
                        if(point == static_cast<std::time_t>(-1)){
                            #ifndef BOOST_WINDOWS
                            // windows does not handle negative time_t, under other plaforms
                            // it may be actually valid value in  1969-12-31 23:59:59
                            // so we check that a filed was updated - does not happen in case of error
                            if(val.tm_wday == -1)
                            #endif
                            {
                                throw date_time_error("boost::locale::gregorian_calendar: invalid time");
                            }
                        }
                    }
                    else {
                        point = internal_timegm(&val);
                        #ifdef BOOST_WINDOWS
                        // Windows uses TLS, thread safe
                        std::tm *revert_point = 0;
                        if(point < 0  || (revert_point = gmtime(&point)) == 0)
                            throw date_time_error("boost::locale::gregorian_calendar time is out of range");
                        val = *revert_point;
                        #else
                        if(!gmtime_r(&point,&val))
                            throw date_time_error("boost::locale::gregorian_calendar invalid time");
                        #endif
                        
                    }
                    
                    time_ = point - tzoff_;
                    tm_ = val;
                    tm_updated_ = val;
                    normalized_ = true;
                }
            }

            int get_week_number(int day,int wday) const
            {
                ///
                /// This is the number of days that are considered withing
                /// period such that the week belongs there
                ///
                static const int days_in_full_week = 4;


                // Alaways use local week start
                int current_dow = (wday - first_day_of_week_ + 7) % 7;
                // Calculate local week day of Jan 1st.
                int first_week_day = (current_dow + 700 - day) % 7; 
                    // adding something big devidable by 7

                int start_of_period_in_weeks;
                if(first_week_day < days_in_full_week) {
                    start_of_period_in_weeks = - first_week_day;
                }
                else {
                    start_of_period_in_weeks = 7 - first_week_day;
                }
                int week_number_in_days = day - start_of_period_in_weeks;
                if(week_number_in_days < 0)
                    return -1;
                return week_number_in_days / 7 + 1;
            }

            ///
            /// Get specific value for period \a p according to a value_type \a v
            ///
            virtual int get_value(period::marks::period_mark p,value_type v) const 
            {
                using namespace period::marks;
                switch(p) {
                case era:
                    return 1;
                case year:
                case extended_year:
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        #ifdef BOOST_WINDOWS
                        return 1970; // Unix epoch windows can't handle negative time_t
                        #else
                        if(sizeof(std::time_t) == 4)
                            return 1901; // minimal year with 32 bit time_t
                        else
                            return 1; 
                        #endif
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        if(sizeof(std::time_t) == 4)
                            return 2038; // Y2K38 - maximal with 32 bit time_t
                        else
                            return std::numeric_limits<int>::max();
                    case current:
                        return tm_.tm_year + 1900;
                    };
                    break;
                case month:
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 0;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 11;
                    case current:
                        return tm_.tm_mon;
                    };
                    break;
                case day:
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 1;
                    case absolute_maximum:
                        return 31;
                    case least_maximum:
                        return 28;
                    case actual_maximum:
                        return days_in_month(tm_.tm_year + 1900,tm_.tm_mon + 1);
                    case current:
                        return tm_.tm_mday;
                    };
                    break;
                case day_of_year:                ///< The number of day in year, starting from 1
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 1;
                    case absolute_maximum:
                        return 366;
                    case least_maximum:
                        return 365;
                    case actual_maximum:
                        return is_leap(tm_.tm_year + 1900) ? 366 : 365;
                    case current:
                        return tm_.tm_yday + 1;
                    }
                    break;
                case day_of_week:                ///< Day of week, starting from Sunday, [1..7]
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 1;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 7;
                    case current:
                        return tm_.tm_wday + 1;
                    }
                    break;
                case day_of_week_local:     ///< Local day of week, for example in France Monday is 1, in US Sunday is 1, [1..7]
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 1;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 7;
                    case current:
                        return (tm_.tm_wday - first_day_of_week_ + 7) % 7 + 1;
                    }
                    break;
                case hour:                       ///< 24 clock hour [0..23]
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 0;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 23;
                    case current:
                        return tm_.tm_hour;
                    }
                    break;
                case hour_12:                    ///< 12 clock hour [0..11]
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 0;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 11;
                    case current:
                        return tm_.tm_hour % 12;
                    }
                    break;
                case am_pm:                      ///< am or pm marker, [0..1]
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 0;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 1;
                    case current:
                        return tm_.tm_hour >= 12 ? 1 : 0;
                    }
                    break;
                case minute:                     ///< minute [0..59]
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 0;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 59;
                    case current:
                        return tm_.tm_min;
                    }
                    break;
                case second:                     ///< second [0..59]
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 0;
                    case absolute_maximum:
                    case least_maximum:
                    case actual_maximum:
                        return 59;
                    case current:
                        return tm_.tm_sec;
                    }
                    break;
                case period::marks::first_day_of_week:          ///< For example Sunday in US, Monday in France
                    return first_day_of_week_ + 1;
                
                case week_of_year:               ///< The week number in the year
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 1;
                    case absolute_maximum:
                        return 53;
                    case least_maximum:
                        return 52;
                    case actual_maximum:
                        {
                            int year = tm_.tm_year + 1900;
                            int end_of_year_days = (is_leap(year) ? 366 : 365) - 1;
                            int dow_of_end_of_year = (end_of_year_days - tm_.tm_yday + tm_.tm_wday) % 7;
                            return get_week_number(end_of_year_days,dow_of_end_of_year);
                        }
                    case current:
                        {
                            int val = get_week_number(tm_.tm_yday,tm_.tm_wday);
                            if(val < 0)
                                return 53;
                            return val;
                        }
                    }
                case week_of_month:              ///< The week number withing current month
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 1;
                    case absolute_maximum:
                        return 5;
                    case least_maximum:
                        return 4;
                    case actual_maximum:
                        {
                            int end_of_month_days = days_in_month(tm_.tm_year + 1900,tm_.tm_mon + 1);
                            int dow_of_end_of_month = (end_of_month_days - tm_.tm_mday + tm_.tm_wday) % 7;
                            return get_week_number(end_of_month_days,dow_of_end_of_month);
                        }
                    case current:
                        {
                            int val = get_week_number(tm_.tm_mday,tm_.tm_wday);
                            if(val < 0)
                                return 5;
                            return val;
                        }
                    }

                case day_of_week_in_month:       ///< Original number of the day of the week in month.
                    switch(v) {
                    case absolute_minimum:
                    case greatest_minimum:
                    case actual_minimum:
                        return 1;
                    case absolute_maximum:
                        return 5;
                    case least_maximum:
                        return 4;
                    case actual_maximum:
                        if(tm_.tm_mon == 1 && !is_leap(tm_.tm_year + 1900)) {
                            // only in february in non leap year is 28 days, the rest
                            // conver more then 4 weeks
                            return 4;
                        }
                        return 5;
                    case current:
                        return (tm_.tm_mday - 1) / 7 + 1;
                    default:
                        ;
                    }
                default:
                    ;
                }
                return 0;
 
            }

            ///
            /// Set current time point
            ///
            virtual void set_time(posix_time const &p)
            {
                from_time(static_cast<std::time_t>(p.seconds));
            }
            virtual posix_time get_time() const  
            {
                posix_time pt = { time_, 0};
                return pt;
            }

            ///
            /// Set option for calendar, for future use
            ///
            virtual void set_option(calendar_option_type opt,int /*v*/)
            {
                switch(opt) {
                case is_gregorian:
                    throw date_time_error("is_gregorian is not settable options for calendar");
                case is_dst:
                    throw date_time_error("is_dst is not settable options for calendar");
                default:
                    ;
                }
            }
            ///
            /// Get option for calendar, currently only check if it is Gregorian calendar
            ///
            virtual int get_option(calendar_option_type opt) const 
            {
                switch(opt) {
                case is_gregorian:
                    return 1;
                case is_dst:
                    return tm_.tm_isdst == 1;
                default:
                    return 0;
                };
            }

            ///
            /// Adjust period's \a p value by \a difference items using a update_type \a u.
            /// Note: not all values are adjustable
            ///
            virtual void adjust_value(period::marks::period_mark p,update_type u,int difference)
            {
                switch(u) {
                case move:
                    {
                        using namespace period::marks;
                        switch(p) {
                        case year:                       ///< Year, it is calendar specific
                        case extended_year:              ///< Extended year for Gregorian/Julian calendars, where 1 BC == 0, 2 BC == -1.
                            tm_updated_.tm_year +=difference;
                            break;
                        case month:
                            tm_updated_.tm_mon +=difference;
                            break;
                        case day:
                        case day_of_year:
                        case day_of_week:                ///< Day of week, starting from Sunday, [1..7]
                        case day_of_week_local: ///< Local day of week, for example in France Monday is 1, in US Sunday is 1, [1..7]
                            tm_updated_.tm_mday +=difference;
                            break;
                        case hour:                       ///< 24 clock hour [0..23]
                        case hour_12:                    ///< 12 clock hour [0..11]
                            tm_updated_.tm_hour += difference;
                            break;
                        case am_pm:                      ///< am or pm marker, [0..1]
                            tm_updated_.tm_hour += 12 * difference;
                            break;
                        case minute:                     ///< minute [0..59]
                            tm_updated_.tm_min += difference;
                            break;
                        case second:
                            tm_updated_.tm_sec += difference;
                            break;
                        case week_of_year:               ///< The week number in the year
                        case week_of_month:              ///< The week number withing current month
                        case day_of_week_in_month:       ///< Original number of the day of the week in month.
                            tm_updated_.tm_mday +=difference * 7;
                            break;
                        default:
                            ; // Not all values are adjustable
                        }
                        normalized_ = false;
                        normalize();
                    }
                    break;
                case roll:
                    { // roll
                        int cur_min = get_value(p,actual_minimum);
                        int cur_max = get_value(p,actual_maximum);
                        int max_diff = cur_max - cur_min + 1;
                        if(max_diff > 0) {
                            int value = get_value(p,current);
                            int addon = 0;
                            if(difference < 0)
                                addon = ((-difference/max_diff) + 1) * max_diff;
                            value = (value - cur_min + difference + addon) % max_diff + cur_min;
                            set_value(p,value);
                            normalize();
                        }
                    }
                default:
                    ;
                }
            }

            int get_diff(period::marks::period_mark p,int diff,gregorian_calendar const *other) const
            {
                if(diff == 0)
                    return 0;
                hold_ptr<gregorian_calendar> self(clone());
                self->adjust_value(p,move,diff);
                if(diff > 0){
                    if(self->time_ > other->time_)
                        return diff - 1;
                    else
                        return diff;
                }
                else {
                    if(self->time_ < other->time_)
                        return diff + 1;
                    else
                        return diff;
                }
             }

            ///
            /// Calculate the difference between this calendar  and \a other in \a p units
            ///
            virtual int difference(abstract_calendar const *other_cal,period::marks::period_mark p) const 
            {
                hold_ptr<gregorian_calendar> keeper;
                gregorian_calendar const *other = dynamic_cast<gregorian_calendar const *>(other_cal);
                if(!other) {
                    keeper.reset(clone());
                    keeper->set_time(other_cal->get_time());
                    other = keeper.get();
                }

                int factor = 1; // for weeks vs days handling

                using namespace period::marks;
                switch(p) {
                case era:
                    return 0;
                case year:
                case extended_year:
                    {
                        int diff = other->tm_.tm_year - tm_.tm_year;
                        return get_diff(period::marks::year,diff,other);
                    }
                case month:
                    {
                        int diff = 12 * (other->tm_.tm_year - tm_.tm_year) 
                                    + other->tm_.tm_mon - tm_.tm_mon;
                        return get_diff(period::marks::month,diff,other);
                    }
                case day_of_week_in_month:
                case week_of_month:
                case week_of_year:
                    factor = 7;
                    // fall 
                case day:
                case day_of_year:
                case day_of_week:
                case day_of_week_local:
                    {
                        int diff = other->tm_.tm_yday - tm_.tm_yday;
                        if(other->tm_.tm_year != tm_.tm_year) {
                            diff += days_from_0(other->tm_.tm_year + 1900) -
                                    days_from_0(tm_.tm_year + 1900);
                        }
                        return get_diff(period::marks::day,diff,other) / factor;
                    }
                case am_pm:
                    return static_cast<int>( (other->time_ - time_) / (3600*12) );
                case hour:
                case hour_12:
                    return static_cast<int>( (other->time_ - time_) / 3600 );
                case minute:
                    return static_cast<int>( (other->time_ - time_) / 60 );
                case second:
                    return static_cast<int>( other->time_ - time_  );
                default:
                    return 0;
                };
            }

            ///
            /// Set time zone, empty - use system
            ///
            virtual void set_timezone(std::string const &tz)
            {
                if(tz.empty()) {
                    is_local_ = true;
                    tzoff_ = 0;
                }
                else {
                    is_local_ = false;
                    tzoff_ = parse_tz(tz);
                }
                from_time(time_);
                time_zone_name_ = tz;
            }
            virtual std::string get_timezone() const
            {
                return time_zone_name_;
            }

            virtual bool same(abstract_calendar const *other) const 
            {
                gregorian_calendar const *gcal = dynamic_cast<gregorian_calendar const *>(other);
                if(!gcal)
                    return false;
                return 
                    gcal->tzoff_ == tzoff_ 
                    && gcal->is_local_ == is_local_
                    && gcal->first_day_of_week_  == first_day_of_week_;
            }

            virtual ~gregorian_calendar()
            {
            }

    private:

        void from_time(std::time_t point)
        {
            std::time_t real_point = point + tzoff_;
            std::tm *t = 0;
            #ifdef BOOST_WINDOWS
            // Windows uses TLS, thread safe
            t = is_local_ ? localtime(&real_point) : gmtime(&real_point);
            #else
            std::tm tmp_tm;
            t = is_local_ ? localtime_r(&real_point,&tmp_tm) : gmtime_r(&real_point,&tmp_tm);
            #endif
            if(!t) {
                throw date_time_error("boost::locale::gregorian_calendar: invalid time point");
            }
            tm_ = *t;
            tm_updated_ = *t;
            normalized_ = true;
            time_ = point;
        }
        int first_day_of_week_;
        std::time_t time_;
        std::tm tm_;
        std::tm tm_updated_;
        bool normalized_;
        bool is_local_;
        int tzoff_;
        std::string time_zone_name_;
        
    };
    
    abstract_calendar *create_gregorian_calendar(std::string const &terr)
    {
        return new gregorian_calendar(terr);
    }

    class gregorian_facet : public calendar_facet {
    public:
        gregorian_facet(std::string const &terr,size_t refs = 0) : 
            calendar_facet(refs),
            terr_(terr)
        {
        }
        virtual abstract_calendar *create_calendar() const 
        {
            return create_gregorian_calendar(terr_);
        }
    private:
        std::string terr_;
    };
    
    std::locale install_gregorian_calendar(std::locale const &in,std::string const &terr)
    {
        return std::locale(in,new gregorian_facet(terr));
    }


} // util
} // locale 
} //boost


// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

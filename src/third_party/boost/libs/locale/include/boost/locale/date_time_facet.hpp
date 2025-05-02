//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_DATE_TIME_FACET_HPP_INCLUDED
#define BOOST_LOCALE_DATE_TIME_FACET_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <boost/locale/detail/facet_id.hpp>
#include <cstdint>
#include <locale>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \brief Namespace that contains various types for manipulation with dates
    namespace period {

        /// \brief This namespace holds a enum of various period types like era, year, month, etc..
        namespace marks {

            /// \brief the type that defines a flag that holds a period identifier
            enum period_mark {
                invalid,       ///< Special invalid value, should not be used directly
                era,           ///< Era i.e. AC, BC in Gregorian and Julian calendar, range [0,1]
                year,          ///< Year, it is calendar specific, for example 2011 in Gregorian calendar.
                extended_year, ///< Extended year for Gregorian/Julian calendars, where 1 BC == 0, 2 BC == -1.
                month,         ///< The month of year, calendar specific, in Gregorian [0..11]
                day,           ///< The day of month, calendar specific, in Gregorian [1..31]
                day_of_year,   ///< The number of day in year, starting from 1, in Gregorian  [1..366]
                day_of_week,   ///< Day of week, Sunday=1, Monday=2,..., Saturday=7.
                               ///< Note that updating this value respects local day of week, so for example,
                               ///< If first day of week is Monday and the current day is Tuesday then setting
                               ///< the value to Sunday (1) would forward the date by 5 days forward and not backward
                               ///< by two days as it could be expected if the numbers were taken as is.
                day_of_week_in_month, ///< Original number of the day of the week in month. For example 1st Sunday,
                                      ///< 2nd Sunday, etc. in Gregorian [1..5]
                day_of_week_local, ///< Local day of week, for example in France Monday is 1, in US Sunday is 1, [1..7]
                hour,              ///< 24 clock hour [0..23]
                hour_12,           ///< 12 clock hour [0..11]
                am_pm,             ///< am or pm marker [0..1]
                minute,            ///< minute [0..59]
                second,            ///< second [0..59]
                week_of_year,      ///< The week number in the year
                week_of_month,     ///< The week number within current month
                first_day_of_week, ///< First day of week, constant, for example Sunday in US = 1, Monday in France = 2
            };

        } // namespace marks

        /// \brief This class holds a type that represents certain period of time like
        /// year, hour, second and so on.
        ///
        /// It can be created from either marks::period_mark type or by using shortcuts in period
        /// namespace - calling functions like period::year(), period::hour() and so on.
        ///
        /// Basically it represents the same object as enum marks::period_mark but allows to
        /// provide save operator overloading that would not collide with casing of enum to
        /// numeric values.
        class period_type {
        public:
            /// Create a period of specific type, default is invalid.
            period_type(marks::period_mark m = marks::invalid) : mark_(m) {}

            /// Get the value of marks::period_mark it was created with.
            marks::period_mark mark() const { return mark_; }

            /// Check if two periods are the same
            bool operator==(const period_type& other) const { return mark() == other.mark(); }
            /// Check if two periods are different
            bool operator!=(const period_type& other) const { return mark() != other.mark(); }

        private:
            marks::period_mark mark_;
        };

    } // namespace period

    /// Structure that define POSIX time, seconds and milliseconds
    /// since Jan 1, 1970, 00:00 not including leap seconds.
    struct posix_time {
        int64_t seconds;      ///< Seconds since epoch
        uint32_t nanoseconds; ///< Nanoseconds resolution
    };

    /// This class defines generic calendar class, it is used by date_time and calendar
    /// objects internally. It is less useful for end users, but it is build for localization
    /// backend implementation
    class BOOST_SYMBOL_VISIBLE abstract_calendar {
    public:
        /// Type that defines how to fetch the value
        enum value_type {
            absolute_minimum, ///< Absolute possible minimum for the value, for example for day is 1
            actual_minimum,   ///< Actual minimal value for this period.
            greatest_minimum, ///< Maximal minimum value that can be for this period
            current,          ///< Current value of this period
            least_maximum,    ///< The last maximal value for this period, For example for Gregorian calendar
                              ///< day it is 28
            actual_maximum,   ///< Actual maximum, for it can be 28, 29, 30, 31 for day according to current month
            absolute_maximum, ///< Maximal value, for Gregorian day it would be 31.
        };

        /// A way to update the value
        enum update_type {
            move, ///< Change the value up or down effecting others for example 1990-12-31 + 1 day = 1991-01-01
            roll, ///< Change the value up or down not effecting others for example 1990-12-31 + 1 day = 1990-12-01
        };

        /// Information about calendar
        enum calendar_option_type {
            is_gregorian, ///< Check if the calendar is Gregorian
            is_dst        ///< Check if the current time is in daylight time savings
        };

        /// Make a polymorphic copy of the calendar
        virtual abstract_calendar* clone() const = 0;

        /// Set specific \a value for period \a p, note not all values are settable.
        ///
        /// After calling set_value you may want to call normalize() function to make sure
        /// all periods are updated, if you set several fields that are part of a single
        /// date/time representation you should call set_value several times and then
        /// call normalize().
        ///
        /// If normalize() is not called after set_value, the behavior is undefined
        virtual void set_value(period::marks::period_mark m, int value) = 0;

        /// Recalculate all periods after setting them, should be called after use of set_value() function.
        virtual void normalize() = 0;

        /// Get specific value for period \a p according to a value_type \a v
        virtual int get_value(period::marks::period_mark m, value_type v) const = 0;

        /// Set current time point
        virtual void set_time(const posix_time& p) = 0;
        /// Get current time point
        virtual posix_time get_time() const = 0;
        /// Get current time since epoch in milliseconds
        virtual double get_time_ms() const = 0;

        /// Set option for calendar, for future use
        virtual void set_option(calendar_option_type opt, int v) = 0;
        /// Get option for calendar, currently only check if it is Gregorian calendar
        virtual int get_option(calendar_option_type opt) const = 0;

        /// Adjust period's \a p value by \a difference items using a update_type \a u.
        /// Note: not all values are adjustable
        virtual void adjust_value(period::marks::period_mark m, update_type u, int difference) = 0;

        /// Calculate the difference between this calendar  and \a other in \a p units
        virtual int difference(const abstract_calendar& other, period::marks::period_mark m) const = 0;

        /// Set time zone, empty - use system
        virtual void set_timezone(const std::string& tz) = 0;
        /// Get current time zone, empty - system one
        virtual std::string get_timezone() const = 0;

        /// Check of two calendars have same rules
        virtual bool same(const abstract_calendar* other) const = 0;

        virtual ~abstract_calendar() = default;
    };

    /// \brief the facet that generates calendar for specific locale
    class BOOST_SYMBOL_VISIBLE calendar_facet : public std::locale::facet, public detail::facet_id<calendar_facet> {
    public:
        /// Basic constructor
        calendar_facet(size_t refs = 0) : std::locale::facet(refs) {}
        /// Create a new calendar that points to current point of time.
        virtual abstract_calendar* create_calendar() const = 0;
    };

}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif

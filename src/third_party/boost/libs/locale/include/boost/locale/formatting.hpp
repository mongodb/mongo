//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_FORMATTING_HPP_INCLUDED
#define BOOST_LOCALE_FORMATTING_HPP_INCLUDED

#include <boost/locale/detail/any_string.hpp>
#include <boost/locale/time_zone.hpp>
#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <string>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \brief This namespace holds additional formatting
    /// flags that can be set using ios_info.
    namespace flags {

        /// Formatting flags, each one of them has corresponding manipulation
        /// in namespace \a as
        enum display_flags_type {
            posix = 0,
            number = 1,
            currency = 2,
            percent = 3,
            date = 4,
            time = 5,
            datetime = 6,
            strftime = 7,
            spellout = 8,
            ordinal = 9,

            display_flags_mask = 31,

            currency_default = 0 << 5,
            currency_iso = 1 << 5,
            currency_national = 2 << 5,

            currency_flags_mask = 3 << 5,

            time_default = 0 << 7,
            time_short = 1 << 7,
            time_medium = 2 << 7,
            time_long = 3 << 7,
            time_full = 4 << 7,
            time_flags_mask = 7 << 7,

            date_default = 0 << 10,
            date_short = 1 << 10,
            date_medium = 2 << 10,
            date_long = 3 << 10,
            date_full = 4 << 10,
            date_flags_mask = 7 << 10,
        };

        /// Special string patterns that can be used for text formatting
        enum pattern_type {
            datetime_pattern, ///< strftime like formatting
            time_zone_id      ///< time zone name
        };

        /// Special integer values that can be used for formatting
        enum value_type {
            domain_id ///< Domain code - for message formatting
        };

    } // namespace flags

    /// \brief This class holds external data beyond existing fmtflags that std::ios_base holds
    ///
    /// You should almost never create this object directly. Instead, you should access it via
    /// ios_info::get(stream_object) static member function. It automatically creates default formatting data for that
    /// stream
    class BOOST_LOCALE_DECL ios_info {
    public:
        /// \cond INTERNAL
        ios_info();
        ios_info(const ios_info&);
        ios_info& operator=(const ios_info&);
        ~ios_info();
        /// \endcond

        /// Get ios_info instance for specific stream object
        static ios_info& get(std::ios_base& ios);

        /// Set flags that define how to format data, e.g. number, spell, currency etc.
        void display_flags(uint64_t flags);
        /// Get flags that define how to format data, e.g. number, spell, currency etc.
        uint64_t display_flags() const;

        /// Set flags that define how to format currency
        void currency_flags(uint64_t flags);
        /// Get flags that define how to format currency
        uint64_t currency_flags() const;

        /// Set flags that define how to format date
        void date_flags(uint64_t flags);
        /// Get flags that define how to format date
        uint64_t date_flags() const;

        /// Set flags that define how to format time
        void time_flags(uint64_t flags);
        /// Get flags that define how to format time
        uint64_t time_flags() const;

        /// Set special message domain identification
        void domain_id(int);
        /// Get special message domain identification
        int domain_id() const;

        /// Set time zone for formatting dates and time
        void time_zone(const std::string&);
        /// Get time zone for formatting dates and time
        const std::string& time_zone() const;

        /// Set date/time pattern (strftime like)
        template<typename CharType>
        void date_time_pattern(const std::basic_string<CharType>& str)
        {
            datetime_.set<CharType>(str);
        }
        /// Get date/time pattern (strftime like)
        template<typename CharType>
        std::basic_string<CharType> date_time_pattern() const
        {
            return datetime_.get<CharType>();
        }

        /// \cond INTERNAL
        void on_imbue();
        /// \endcond

    private:
        uint64_t flags_;
        int domain_id_;
        std::string time_zone_;
        detail::any_string datetime_;
    };

    /// \brief This namespace includes all manipulators that can be used on IO streams
    namespace as {
        /// \defgroup manipulators I/O Stream manipulators
        ///
        /// @{

        /// Format values with "POSIX" or "C"  locale. Note, if locale was created with additional non-classic locale
        /// then These numbers may be localized
        inline std::ios_base& posix(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::posix);
            return ios;
        }

        /// Format a number. Note, unlike standard number formatting, integers would be treated like real numbers when
        /// std::fixed or std::scientific manipulators were applied
        inline std::ios_base& number(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::number);
            return ios;
        }

        /// Format currency, number is treated like amount of money
        inline std::ios_base& currency(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::currency);
            return ios;
        }

        /// Format percent, value 0.3 is treated as 30%.
        inline std::ios_base& percent(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::percent);
            return ios;
        }

        /// Format a date, number is treated as POSIX time
        inline std::ios_base& date(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::date);
            return ios;
        }

        /// Format a time, number is treated as POSIX time
        inline std::ios_base& time(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::time);
            return ios;
        }

        /// Format a date and time, number is treated as POSIX time
        inline std::ios_base& datetime(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::datetime);
            return ios;
        }

        /// Create formatted date time, Please note, this manipulator only changes formatting mode,
        /// and not format itself, so you are probably looking for ftime manipulator
        inline std::ios_base& strftime(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::strftime);
            return ios;
        }

        /// Spell the number, like "one hundred and ten"
        inline std::ios_base& spellout(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::spellout);
            return ios;
        }

        /// Write an order of the number like 4th.
        inline std::ios_base& ordinal(std::ios_base& ios)
        {
            ios_info::get(ios).display_flags(flags::ordinal);
            return ios;
        }

        /// Set default currency formatting style -- national, like "$"
        inline std::ios_base& currency_default(std::ios_base& ios)
        {
            ios_info::get(ios).currency_flags(flags::currency_default);
            return ios;
        }

        /// Set ISO currency formatting style, like "USD", (requires ICU >= 4.2)
        inline std::ios_base& currency_iso(std::ios_base& ios)
        {
            ios_info::get(ios).currency_flags(flags::currency_iso);
            return ios;
        }

        /// Set national currency formatting style, like "$"
        inline std::ios_base& currency_national(std::ios_base& ios)
        {
            ios_info::get(ios).currency_flags(flags::currency_national);
            return ios;
        }

        /// set default (medium) time formatting style
        inline std::ios_base& time_default(std::ios_base& ios)
        {
            ios_info::get(ios).time_flags(flags::time_default);
            return ios;
        }

        /// set short time formatting style
        inline std::ios_base& time_short(std::ios_base& ios)
        {
            ios_info::get(ios).time_flags(flags::time_short);
            return ios;
        }

        /// set medium time formatting style
        inline std::ios_base& time_medium(std::ios_base& ios)
        {
            ios_info::get(ios).time_flags(flags::time_medium);
            return ios;
        }

        /// set long time formatting style
        inline std::ios_base& time_long(std::ios_base& ios)
        {
            ios_info::get(ios).time_flags(flags::time_long);
            return ios;
        }

        /// set full time formatting style
        inline std::ios_base& time_full(std::ios_base& ios)
        {
            ios_info::get(ios).time_flags(flags::time_full);
            return ios;
        }

        /// set default (medium) date formatting style
        inline std::ios_base& date_default(std::ios_base& ios)
        {
            ios_info::get(ios).date_flags(flags::date_default);
            return ios;
        }

        /// set short date formatting style
        inline std::ios_base& date_short(std::ios_base& ios)
        {
            ios_info::get(ios).date_flags(flags::date_short);
            return ios;
        }

        /// set medium date formatting style
        inline std::ios_base& date_medium(std::ios_base& ios)
        {
            ios_info::get(ios).date_flags(flags::date_medium);
            return ios;
        }

        /// set long date formatting style
        inline std::ios_base& date_long(std::ios_base& ios)
        {
            ios_info::get(ios).date_flags(flags::date_long);
            return ios;
        }

        /// set full date formatting style
        inline std::ios_base& date_full(std::ios_base& ios)
        {
            ios_info::get(ios).date_flags(flags::date_full);
            return ios;
        }

        /// \cond INTERNAL
        namespace detail {
            inline bool is_datetime_display_flags(const uint64_t display_flags)
            {
                return (display_flags == flags::date || display_flags == flags::time || display_flags == flags::datetime
                        || display_flags == flags::strftime);
            }

            template<typename CharType>
            struct add_ftime {
                std::basic_string<CharType> ftime;

                void apply(std::basic_ios<CharType>& ios) const
                {
                    ios_info::get(ios).date_time_pattern(ftime);
                    as::strftime(ios);
                }
            };

            template<typename CharType>
            std::basic_ostream<CharType>& operator<<(std::basic_ostream<CharType>& out, const add_ftime<CharType>& fmt)
            {
                fmt.apply(out);
                return out;
            }

            template<typename CharType>
            std::basic_istream<CharType>& operator>>(std::basic_istream<CharType>& in, const add_ftime<CharType>& fmt)
            {
                fmt.apply(in);
                return in;
            }

        } // namespace detail
        /// \endcond

        /// Set strftime like formatting string
        ///
        /// Please note, formatting flags are very similar but not exactly the same as flags for C function strftime.
        /// Differences: some flags as "%e" do not add blanks to fill text up to two spaces, not all flags supported.
        ///
        /// Flags:
        /// -   "%a" -- Abbreviated  weekday (Sun.)
        /// -   "%A" -- Full weekday (Sunday)
        /// -   "%b" -- Abbreviated month (Jan.)
        /// -   "%B" -- Full month (January)
        /// -   "%c" -- Locale date-time format. **Note:** prefer using "as::datetime"
        /// -   "%d" -- Day of Month [01,31]
        /// -   "%e" -- Day of Month [1,31]
        /// -   "%h" -- Same as "%b"
        /// -   "%H" -- 24 clock hour [00,23]
        /// -   "%I" -- 12 clock hour [01,12]
        /// -   "%j" -- Day of year [1,366]
        /// -   "%m" -- Month [01,12]
        /// -   "%M" -- Minute [00,59]
        /// -   "%n" -- New Line
        /// -   "%p" -- AM/PM in locale representation
        /// -   "%r" -- Time with AM/PM, same as "%I:%M:%S %p"
        /// -   "%R" -- Same as "%H:%M"
        /// -   "%S" -- Second [00,61]
        /// -   "%t" -- Tab character
        /// -   "%T" -- Same as "%H:%M:%S"
        /// -   "%x" -- Local date representation. **Note:** prefer using "as::date"
        /// -   "%X" -- Local time representation. **Note:** prefer using "as::time"
        /// -   "%y" -- Year [00,99]
        /// -   "%Y" -- 4 digits year. (2009)
        /// -   "%Z" -- Time Zone
        /// -   "%%" -- Percent symbol
        ///

        template<typename CharType>
#ifdef BOOST_LOCALE_DOXYGEN
        unspecified_type
#else
        detail::add_ftime<CharType>
#endif
        ftime(const std::basic_string<CharType>& format)
        {
            detail::add_ftime<CharType> fmt;
            fmt.ftime = format;
            return fmt;
        }

        /// See ftime(std::basic_string<CharType> const &format)
        template<typename CharType>
#ifdef BOOST_LOCALE_DOXYGEN
        unspecified_type
#else
        detail::add_ftime<CharType>
#endif
        ftime(const CharType* format)
        {
            detail::add_ftime<CharType> fmt;
            fmt.ftime = format;
            return fmt;
        }

        /// \cond INTERNAL
        namespace detail {
            struct set_timezone {
                std::string id;
            };
            template<typename CharType>
            std::basic_ostream<CharType>& operator<<(std::basic_ostream<CharType>& out, const set_timezone& fmt)
            {
                ios_info::get(out).time_zone(fmt.id);
                return out;
            }

            template<typename CharType>
            std::basic_istream<CharType>& operator>>(std::basic_istream<CharType>& in, const set_timezone& fmt)
            {
                ios_info::get(in).time_zone(fmt.id);
                return in;
            }
        } // namespace detail
        /// \endcond

        /// Set GMT time zone to stream
        inline std::ios_base& gmt(std::ios_base& ios)
        {
            ios_info::get(ios).time_zone("GMT");
            return ios;
        }

        /// Set local time zone to stream
        inline std::ios_base& local_time(std::ios_base& ios)
        {
            ios_info::get(ios).time_zone(time_zone::global());
            return ios;
        }

        /// Set time zone using \a id
        inline
#ifdef BOOST_LOCALE_DOXYGEN
          unspecified_type
#else
          detail::set_timezone
#endif
          time_zone(const char* id)
        {
            detail::set_timezone tz;
            tz.id = id;
            return tz;
        }

        /// Set time zone using \a id
        inline
#ifdef BOOST_LOCALE_DOXYGEN
          unspecified_type
#else
          detail::set_timezone
#endif
          time_zone(const std::string& id)
        {
            detail::set_timezone tz;
            tz.id = id;
            return tz;
        }

        /// @}

    } // namespace as

}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif

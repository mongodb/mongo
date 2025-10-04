//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef NOMINMAX
#    define NOMINMAX
#endif

#include <boost/locale/date_time.hpp>
#include <boost/locale/formatting.hpp>
#include <boost/core/exchange.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <cmath>

namespace boost { namespace locale {

    using namespace period;

    /////////////////////////
    // Calendar
    ////////////////////////

    calendar::calendar(const std::locale& l, const std::string& zone) :
        locale_(l), tz_(zone), impl_(std::use_facet<calendar_facet>(l).create_calendar())
    {
        impl_->set_timezone(tz_);
    }

    calendar::calendar(const std::string& zone) :
        tz_(zone), impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
    {
        impl_->set_timezone(tz_);
    }

    calendar::calendar(const std::locale& l) :
        locale_(l), tz_(time_zone::global()), impl_(std::use_facet<calendar_facet>(l).create_calendar())
    {
        impl_->set_timezone(tz_);
    }

    calendar::calendar(std::ios_base& ios) :
        locale_(ios.getloc()), tz_(ios_info::get(ios).time_zone()),
        impl_(std::use_facet<calendar_facet>(locale_).create_calendar())
    {
        impl_->set_timezone(tz_);
    }

    calendar::calendar() :
        tz_(time_zone::global()), impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
    {
        impl_->set_timezone(tz_);
    }

    calendar::~calendar() = default;

    calendar::calendar(const calendar& other) : locale_(other.locale_), tz_(other.tz_), impl_(other.impl_->clone()) {}

    calendar& calendar::operator=(const calendar& other)
    {
        impl_.reset(other.impl_->clone());
        locale_ = other.locale_;
        tz_ = other.tz_;
        return *this;
    }

    bool calendar::is_gregorian() const
    {
        return impl_->get_option(abstract_calendar::is_gregorian) != 0;
    }

    const std::string& calendar::get_time_zone() const
    {
        return tz_;
    }

    const std::locale& calendar::get_locale() const
    {
        return locale_;
    }

    int calendar::minimum(period_type f) const
    {
        return impl_->get_value(f.mark(), abstract_calendar::absolute_minimum);
    }

    int calendar::greatest_minimum(period_type f) const
    {
        return impl_->get_value(f.mark(), abstract_calendar::greatest_minimum);
    }

    int calendar::maximum(period_type f) const
    {
        return impl_->get_value(f.mark(), abstract_calendar::absolute_maximum);
    }

    int calendar::least_maximum(period_type f) const
    {
        return impl_->get_value(f.mark(), abstract_calendar::least_maximum);
    }

    int calendar::first_day_of_week() const
    {
        return impl_->get_value(period::marks::first_day_of_week, abstract_calendar::current);
    }

    bool calendar::operator==(const calendar& other) const
    {
        return impl_->same(other.impl_.get());
    }

    bool calendar::operator!=(const calendar& other) const
    {
        return !(*this == other);
    }

    //////////////////////////////////
    // date_time
    /////////////////

    date_time::date_time() : impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
    {
        impl_->set_timezone(time_zone::global());
    }

    date_time::date_time(const date_time& other) : impl_(other.impl_->clone()) {}

    date_time::date_time(const date_time& other, const date_time_period_set& s)
    {
        impl_.reset(other.impl_->clone());
        for(unsigned i = 0; i < s.size(); i++)
            impl_->set_value(s[i].type.mark(), s[i].value);
        impl_->normalize();
    }

    date_time& date_time::operator=(const date_time& other)
    {
        impl_.reset(other.impl_->clone());
        return *this;
    }

    date_time::date_time(double t) : impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
    {
        impl_->set_timezone(time_zone::global());
        time(t);
    }

    date_time::date_time(double t, const calendar& cal) : impl_(cal.impl_->clone())
    {
        time(t);
    }

    date_time::date_time(const calendar& cal) : impl_(cal.impl_->clone()) {}

    date_time::date_time(const date_time_period_set& s) :
        impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
    {
        impl_->set_timezone(time_zone::global());
        for(unsigned i = 0; i < s.size(); i++)
            impl_->set_value(s[i].type.mark(), s[i].value);
        impl_->normalize();
    }
    date_time::date_time(const date_time_period_set& s, const calendar& cal) : impl_(cal.impl_->clone())
    {
        for(unsigned i = 0; i < s.size(); i++)
            impl_->set_value(s[i].type.mark(), s[i].value);
        impl_->normalize();
    }

    date_time& date_time::operator=(const date_time_period_set& s)
    {
        for(unsigned i = 0; i < s.size(); i++)
            impl_->set_value(s[i].type.mark(), s[i].value);
        impl_->normalize();
        return *this;
    }

    void date_time::set(period_type f, int v)
    {
        impl_->set_value(f.mark(), v);
        impl_->normalize();
    }

    int date_time::get(period_type f) const
    {
        return impl_->get_value(f.mark(), abstract_calendar::current);
    }

    date_time date_time::operator+(const date_time_period& v) const
    {
        date_time tmp(*this);
        tmp += v;
        return tmp;
    }

    date_time date_time::operator-(const date_time_period& v) const
    {
        date_time tmp(*this);
        tmp -= v;
        return tmp;
    }

    date_time date_time::operator<<(const date_time_period& v) const
    {
        date_time tmp(*this);
        tmp <<= v;
        return tmp;
    }

    date_time date_time::operator>>(const date_time_period& v) const
    {
        date_time tmp(*this);
        tmp >>= v;
        return tmp;
    }

    date_time& date_time::operator+=(const date_time_period& v)
    {
        impl_->adjust_value(v.type.mark(), abstract_calendar::move, v.value);
        return *this;
    }

    date_time& date_time::operator-=(const date_time_period& v)
    {
        impl_->adjust_value(v.type.mark(), abstract_calendar::move, -v.value);
        return *this;
    }

    date_time& date_time::operator<<=(const date_time_period& v)
    {
        impl_->adjust_value(v.type.mark(), abstract_calendar::roll, v.value);
        return *this;
    }

    date_time& date_time::operator>>=(const date_time_period& v)
    {
        impl_->adjust_value(v.type.mark(), abstract_calendar::roll, -v.value);
        return *this;
    }

    date_time date_time::operator+(const date_time_period_set& v) const
    {
        date_time tmp(*this);
        tmp += v;
        return tmp;
    }

    date_time date_time::operator-(const date_time_period_set& v) const
    {
        date_time tmp(*this);
        tmp -= v;
        return tmp;
    }

    date_time date_time::operator<<(const date_time_period_set& v) const
    {
        date_time tmp(*this);
        tmp <<= v;
        return tmp;
    }

    date_time date_time::operator>>(const date_time_period_set& v) const
    {
        date_time tmp(*this);
        tmp >>= v;
        return tmp;
    }

    date_time& date_time::operator+=(const date_time_period_set& v)
    {
        for(unsigned i = 0; i < v.size(); i++)
            *this += v[i];
        return *this;
    }

    date_time& date_time::operator-=(const date_time_period_set& v)
    {
        for(unsigned i = 0; i < v.size(); i++)
            *this -= v[i];
        return *this;
    }

    date_time& date_time::operator<<=(const date_time_period_set& v)
    {
        for(unsigned i = 0; i < v.size(); i++)
            *this <<= v[i];
        return *this;
    }

    date_time& date_time::operator>>=(const date_time_period_set& v)
    {
        for(unsigned i = 0; i < v.size(); i++)
            *this >>= v[i];
        return *this;
    }

    double date_time::time() const
    {
        return impl_->get_time_ms() / 1000.;
    }

    void date_time::time(double v)
    {
        constexpr int64_t ns_in_s = static_cast<int64_t>(1000) * 1000 * 1000;

        double seconds;
        const double fract_seconds = std::modf(v, &seconds); // v = seconds + fract_seconds
        posix_time ptime;
        ptime.seconds = static_cast<int64_t>(seconds);
        int64_t nano = static_cast<int64_t>(fract_seconds * ns_in_s);

        if(nano < 0) {
            // Add 1s from seconds to nano to make nano positive
            ptime.seconds -= 1;
            nano = std::max(int64_t(0), nano + ns_in_s); // std::max to handle rounding issues
        } else if(nano >= ns_in_s)                       // Unlikely rounding issue, when fract_seconds is close to 1.
            nano = ns_in_s - 1;                          // LCOV_EXCL_LINE

        BOOST_ASSERT(nano < ns_in_s);
        static_assert(ns_in_s <= std::numeric_limits<uint32_t>::max(), "Insecure cast");
        ptime.nanoseconds = static_cast<uint32_t>(nano);
        impl_->set_time(ptime);
    }

    std::string date_time::timezone() const
    {
        return impl_->get_timezone();
    }

    namespace {
        int compare(const posix_time& left, const posix_time& right)
        {
            if(left.seconds < right.seconds)
                return -1;
            if(left.seconds > right.seconds)
                return 1;
            if(left.nanoseconds < right.nanoseconds)
                return -1;
            if(left.nanoseconds > right.nanoseconds)
                return 1;
            return 0;
        }
    } // namespace

    bool date_time::operator==(const date_time& other) const
    {
        return compare(impl_->get_time(), other.impl_->get_time()) == 0;
    }

    bool date_time::operator!=(const date_time& other) const
    {
        return !(*this == other);
    }

    bool date_time::operator<(const date_time& other) const
    {
        return compare(impl_->get_time(), other.impl_->get_time()) < 0;
    }

    bool date_time::operator>=(const date_time& other) const
    {
        return !(*this < other);
    }

    bool date_time::operator>(const date_time& other) const
    {
        return compare(impl_->get_time(), other.impl_->get_time()) > 0;
    }

    bool date_time::operator<=(const date_time& other) const
    {
        return !(*this > other);
    }

    void date_time::swap(date_time& other) noexcept
    {
        impl_.swap(other.impl_);
    }

    int date_time::difference(const date_time& other, period_type f) const
    {
        return impl_->difference(*other.impl_.get(), f.mark());
    }

    int date_time::maximum(period_type f) const
    {
        return impl_->get_value(f.mark(), abstract_calendar::actual_maximum);
    }

    int date_time::minimum(period_type f) const
    {
        return impl_->get_value(f.mark(), abstract_calendar::actual_minimum);
    }

    bool date_time::is_in_daylight_saving_time() const
    {
        return impl_->get_option(abstract_calendar::is_dst) != 0;
    }

    namespace time_zone {
        boost::mutex& tz_mutex()
        {
            static boost::mutex m;
            return m;
        }
        std::string& tz_id()
        {
            static std::string id;
            return id;
        }
        std::string global()
        {
            boost::unique_lock<boost::mutex> lock(tz_mutex());
            return tz_id();
        }
        std::string global(const std::string& new_id)
        {
            boost::unique_lock<boost::mutex> lock(tz_mutex());
            return boost::exchange(tz_id(), new_id);
        }
    } // namespace time_zone

}} // namespace boost::locale

// boostinspect:nominmax

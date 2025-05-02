//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/date_time.hpp>
#include <boost/locale/date_time_facet.hpp>
#include <boost/locale/formatting.hpp>
#include <boost/locale/hold_ptr.hpp>
#include "all_generator.hpp"
#include "cdata.hpp"
#include "icu_util.hpp"
#include "time_zone.hpp"
#include "uconv.hpp"
#include <boost/thread.hpp>
#include <cmath>
#include <memory>
#include <unicode/calendar.h>
#include <unicode/gregocal.h>
#include <unicode/utypes.h>

namespace boost { namespace locale { namespace impl_icu {

    static void check_and_throw_dt(UErrorCode& e)
    {
        if(U_FAILURE(e))
            throw date_time_error(u_errorName(e));
    }
    using period::marks::period_mark;

    static UCalendarDateFields to_icu(period::marks::period_mark f)
    {
        using namespace period::marks;

        switch(f) {
            case era: return UCAL_ERA;
            case year: return UCAL_YEAR;
            case extended_year: return UCAL_EXTENDED_YEAR;
            case month: return UCAL_MONTH;
            case day: return UCAL_DATE;
            case day_of_year: return UCAL_DAY_OF_YEAR;
            case day_of_week: return UCAL_DAY_OF_WEEK;
            case day_of_week_in_month: return UCAL_DAY_OF_WEEK_IN_MONTH;
            case day_of_week_local: return UCAL_DOW_LOCAL;
            case hour: return UCAL_HOUR_OF_DAY;
            case hour_12: return UCAL_HOUR;
            case am_pm: return UCAL_AM_PM;
            case minute: return UCAL_MINUTE;
            case second: return UCAL_SECOND;
            case week_of_year: return UCAL_WEEK_OF_YEAR;
            case week_of_month: return UCAL_WEEK_OF_MONTH;
            case first_day_of_week:
            case invalid: break;
        }
        throw std::invalid_argument("Invalid date_time period type"); // LCOV_EXCL_LINE
    }

    class calendar_impl : public abstract_calendar {
    public:
        calendar_impl(const cdata& dat)
        {
            UErrorCode err = U_ZERO_ERROR;
            calendar_.reset(icu::Calendar::createInstance(dat.locale(), err));
            // Use accuracy of seconds, see #221
            const double rounded_time = std::floor(calendar_->getTime(err) / U_MILLIS_PER_SECOND) * U_MILLIS_PER_SECOND;
            calendar_->setTime(rounded_time, err);
            check_and_throw_dt(err);
            encoding_ = dat.encoding();
        }
        calendar_impl(const calendar_impl& other)
        {
            calendar_.reset(other.calendar_->clone());
            encoding_ = other.encoding_;
        }

        calendar_impl* clone() const override { return new calendar_impl(*this); }

        void set_value(period::marks::period_mark p, int value) override { calendar_->set(to_icu(p), int32_t(value)); }

        int get_value(period::marks::period_mark p, value_type type) const override
        {
            UErrorCode err = U_ZERO_ERROR;
            int v = 0;
            if(p == period::marks::first_day_of_week) {
                guard l(lock_);
                v = calendar_->getFirstDayOfWeek(err);
            } else {
                UCalendarDateFields field = to_icu(p);
                guard l(lock_);
                switch(type) {
                    case absolute_minimum: v = calendar_->getMinimum(field); break;
                    case actual_minimum: v = calendar_->getActualMinimum(field, err); break;
                    case greatest_minimum: v = calendar_->getGreatestMinimum(field); break;
                    case current: v = calendar_->get(field, err); break;
                    case least_maximum: v = calendar_->getLeastMaximum(field); break;
                    case actual_maximum: v = calendar_->getActualMaximum(field, err); break;
                    case absolute_maximum: v = calendar_->getMaximum(field); break;
                }
            }
            check_and_throw_dt(err);
            return v;
        }

        void normalize() override
        {
            // Can't call complete() explicitly (protected)
            // calling get which calls complete
            UErrorCode code = U_ZERO_ERROR;
            calendar_->get(UCAL_YEAR, code);
            check_and_throw_dt(code);
        }

        void set_time(const posix_time& p) override
        {
            // Ignore `p.nanoseconds / 1e6` for simplicity of users as there is no
            // easy way to set the sub-seconds via `date_time`.
            // Matches behavior of other backends that only have seconds resolution
            const double utime = p.seconds * 1e3;
            UErrorCode code = U_ZERO_ERROR;
            calendar_->setTime(utime, code);
            check_and_throw_dt(code);
        }
        posix_time get_time() const override
        {
            const double timeMs = get_time_ms();
            posix_time res;
            res.seconds = static_cast<int64_t>(std::floor(timeMs / 1e3));
            const double remainTimeMs = std::fmod(timeMs, 1e3); // = timeMs - seconds * 1000
            constexpr uint32_t ns_in_s = static_cast<uint32_t>(1000) * 1000 * 1000;
            res.nanoseconds = std::min(static_cast<uint32_t>(remainTimeMs * 1e6), ns_in_s - 1u);
            return res;
        }
        double get_time_ms() const override
        {
            UErrorCode code = U_ZERO_ERROR;
            double result;
            {
                guard l(lock_);
                result = calendar_->getTime(code);
            }
            check_and_throw_dt(code);
            return result;
        }

        void set_option(calendar_option_type opt, int /*v*/) override
        {
            switch(opt) {
                case is_gregorian: throw date_time_error("is_gregorian is not settable options for calendar");
                case is_dst: throw date_time_error("is_dst is not settable options for calendar");
            }
            throw std::invalid_argument("Invalid option type"); // LCOV_EXCL_LINE
        }
        int get_option(calendar_option_type opt) const override
        {
            switch(opt) {
                case is_gregorian: return icu_cast<const icu::GregorianCalendar>(calendar_.get()) != nullptr;
                case is_dst: {
                    guard l(lock_);
                    UErrorCode err = U_ZERO_ERROR;
                    bool res = (calendar_->inDaylightTime(err) != 0);
                    check_and_throw_dt(err);
                    return res;
                }
            }
            throw std::invalid_argument("Invalid option type"); // LCOV_EXCL_LINE
        }
        void adjust_value(period::marks::period_mark p, update_type u, int difference) override
        {
            UErrorCode err = U_ZERO_ERROR;
            switch(u) {
                case move: calendar_->add(to_icu(p), difference, err); break;
                case roll: calendar_->roll(to_icu(p), difference, err); break;
            }
            check_and_throw_dt(err);
        }
        int difference(const abstract_calendar& other, period::marks::period_mark m) const override
        {
            // era can't be queried via fieldDifference
            if(BOOST_UNLIKELY(m == period::marks::era))
                return get_value(m, value_type::current) - other.get_value(m, value_type::current);

            const double other_time_ms = other.get_time_ms();

            // fieldDifference has side effect of moving calendar (WTF?)
            // So we clone it for performing this operation
            hold_ptr<icu::Calendar> self(calendar_->clone());

            UErrorCode err = U_ZERO_ERROR;
            const int diff = self->fieldDifference(other_time_ms, to_icu(m), err);
            check_and_throw_dt(err);
            return diff;
        }
        void set_timezone(const std::string& tz) override { calendar_->adoptTimeZone(get_time_zone(tz)); }
        std::string get_timezone() const override
        {
            icu::UnicodeString tz;
            calendar_->getTimeZone().getID(tz);
            icu_std_converter<char> cvt(encoding_);
            return cvt.std(tz);
        }
        bool same(const abstract_calendar* other) const override
        {
            const calendar_impl* oc = dynamic_cast<const calendar_impl*>(other);
            if(!oc)
                return false;
            return calendar_->isEquivalentTo(*oc->calendar_) != 0;
        }

    private:
        typedef boost::unique_lock<boost::mutex> guard;
        mutable boost::mutex lock_;
        std::string encoding_;
        hold_ptr<icu::Calendar> calendar_;
    };

    class icu_calendar_facet : public calendar_facet {
    public:
        icu_calendar_facet(const cdata& d, size_t refs = 0) : calendar_facet(refs), data_(d) {}
        abstract_calendar* create_calendar() const override { return new calendar_impl(data_); }

    private:
        cdata data_;
    };

    std::locale create_calendar(const std::locale& in, const cdata& d)
    {
        return std::locale(in, new icu_calendar_facet(d));
    }

}}} // namespace boost::locale::impl_icu

//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/locale/date_time.hpp>
#include <boost/locale/formatting.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <math.h>

namespace boost {
namespace locale {

using namespace period;

/////////////////////////
// Calendar
////////////////////////

calendar::calendar(std::locale const &l,std::string const &zone) :
    locale_(l),
    tz_(zone),
    impl_(std::use_facet<calendar_facet>(l).create_calendar())
{
    impl_->set_timezone(tz_);
}

calendar::calendar(std::string const &zone) :
    tz_(zone),
    impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
{
    impl_->set_timezone(tz_);
}


calendar::calendar(std::locale const &l) :
    locale_(l),
    tz_(time_zone::global()),
    impl_(std::use_facet<calendar_facet>(l).create_calendar())
{
    impl_->set_timezone(tz_);
}

calendar::calendar(std::ios_base &ios) :
    locale_(ios.getloc()),
    tz_(ios_info::get(ios).time_zone()),
    impl_(std::use_facet<calendar_facet>(locale_).create_calendar())
{
    impl_->set_timezone(tz_);
    
}

calendar::calendar() :
    tz_(time_zone::global()),
    impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
{
    impl_->set_timezone(tz_);
}

calendar::~calendar()
{
}

calendar::calendar(calendar const &other) :
    locale_(other.locale_),
    tz_(other.tz_),
    impl_(other.impl_->clone())
{
}

calendar const &calendar::operator = (calendar const &other) 
{
    if(this !=&other) {
        impl_.reset(other.impl_->clone());
        locale_ = other.locale_;
        tz_ = other.tz_;
    }
    return *this;
}


bool calendar::is_gregorian() const
{
    return impl_->get_option(abstract_calendar::is_gregorian)!=0;
}

std::string calendar::get_time_zone() const
{
    return tz_;
}

std::locale calendar::get_locale() const
{
    return locale_;
}

int calendar::minimum(period_type f) const
{
    return impl_->get_value(f.mark(),abstract_calendar::absolute_minimum);
}

int calendar::greatest_minimum(period_type f) const
{
    return impl_->get_value(f.mark(),abstract_calendar::greatest_minimum);
}

int calendar::maximum(period_type f) const
{
    return impl_->get_value(f.mark(),abstract_calendar::absolute_maximum);
}

int calendar::least_maximum(period_type f) const
{
    return impl_->get_value(f.mark(),abstract_calendar::least_maximum);
}

int calendar::first_day_of_week() const
{
    return impl_->get_value(period::marks::first_day_of_week,abstract_calendar::current);
}

bool calendar::operator==(calendar const &other) const
{
    return impl_->same(other.impl_.get());
}

bool calendar::operator!=(calendar const &other) const
{
    return !(*this==other);
}

//////////////////////////////////
// date_time
/////////////////

date_time::date_time() :
    impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
{
    impl_->set_timezone(time_zone::global());
}

date_time::date_time(date_time const &other)
{
    impl_.reset(other.impl_->clone());
}

date_time::date_time(date_time const &other,date_time_period_set const &s)
{
    impl_.reset(other.impl_->clone());
    for(unsigned i=0;i<s.size();i++) {
        impl_->set_value(s[i].type.mark(),s[i].value);
    }
    impl_->normalize();
}

date_time const &date_time::operator = (date_time const &other)
{
    if(this != &other) {
        date_time tmp(other);
        swap(tmp);
    }
    return *this;
}

date_time::~date_time()
{
}

date_time::date_time(double t) :
    impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
{
    impl_->set_timezone(time_zone::global());
    time(t);
}

date_time::date_time(double t,calendar const &cal) :
    impl_(cal.impl_->clone())
{
    time(t);
}

date_time::date_time(calendar const &cal) :
    impl_(cal.impl_->clone())
{
}



date_time::date_time(date_time_period_set const &s) :
    impl_(std::use_facet<calendar_facet>(std::locale()).create_calendar())
{
    impl_->set_timezone(time_zone::global());
    for(unsigned i=0;i<s.size();i++) {
        impl_->set_value(s[i].type.mark(),s[i].value);
    }
    impl_->normalize();
}
date_time::date_time(date_time_period_set const &s,calendar const &cal) :
    impl_(cal.impl_->clone())
{
    for(unsigned i=0;i<s.size();i++) {
        impl_->set_value(s[i].type.mark(),s[i].value);
    }
    impl_->normalize();
}

date_time const &date_time::operator=(date_time_period_set const &s)
{
    for(unsigned i=0;i<s.size();i++)
        impl_->set_value(s[i].type.mark(),s[i].value);
    impl_->normalize();
    return *this;
}

void date_time::set(period_type f,int v)
{
    impl_->set_value(f.mark(),v);
    impl_->normalize();
}

int date_time::get(period_type f) const
{
    return impl_->get_value(f.mark(),abstract_calendar::current);
}

date_time date_time::operator+(date_time_period const &v) const
{
    date_time tmp(*this);
    tmp+=v;
    return tmp;
}

date_time date_time::operator-(date_time_period const &v) const
{
    date_time tmp(*this);
    tmp-=v;
    return tmp;
}

date_time date_time::operator<<(date_time_period const &v) const
{
    date_time tmp(*this);
    tmp<<=v;
    return tmp;
}

date_time date_time::operator>>(date_time_period const &v) const
{
    date_time tmp(*this);
    tmp>>=v;
    return tmp;
}

date_time const &date_time::operator+=(date_time_period const &v) 
{
    impl_->adjust_value(v.type.mark(),abstract_calendar::move,v.value);
    return *this;
}

date_time const &date_time::operator-=(date_time_period const &v) 
{
    impl_->adjust_value(v.type.mark(),abstract_calendar::move,-v.value);
    return *this;
}

date_time const &date_time::operator<<=(date_time_period const &v) 
{
    impl_->adjust_value(v.type.mark(),abstract_calendar::roll,v.value);
    return *this;
}

date_time const &date_time::operator>>=(date_time_period const &v) 
{
    impl_->adjust_value(v.type.mark(),abstract_calendar::roll,-v.value);
    return *this;
}


date_time date_time::operator+(date_time_period_set const &v) const
{
    date_time tmp(*this);
    tmp+=v;
    return tmp;
}

date_time date_time::operator-(date_time_period_set const &v) const
{
    date_time tmp(*this);
    tmp-=v;
    return tmp;
}

date_time date_time::operator<<(date_time_period_set const &v) const
{
    date_time tmp(*this);
    tmp<<=v;
    return tmp;
}

date_time date_time::operator>>(date_time_period_set const &v) const
{
    date_time tmp(*this);
    tmp>>=v;
    return tmp;
}

date_time const &date_time::operator+=(date_time_period_set const &v) 
{
    for(unsigned i=0;i<v.size();i++) {
        *this+=v[i];
    }
    return *this;
}

date_time const &date_time::operator-=(date_time_period_set const &v) 
{
    for(unsigned i=0;i<v.size();i++) {
        *this-=v[i];
    }
    return *this;
}

date_time const &date_time::operator<<=(date_time_period_set const &v) 
{
    for(unsigned i=0;i<v.size();i++) {
        *this<<=v[i];
    }
    return *this;
}

date_time const &date_time::operator>>=(date_time_period_set const &v) 
{
    for(unsigned i=0;i<v.size();i++) {
        *this>>=v[i];
    }
    return *this;
}

double date_time::time() const
{
    posix_time ptime = impl_->get_time();
    return double(ptime.seconds)+1e-9*ptime.nanoseconds;
}

void date_time::time(double v)
{
    double dseconds = floor(v);
    int64_t seconds = static_cast<int64_t>(dseconds);
    double fract = v - dseconds;
    int nano = static_cast<int>(fract * 1e9);
    if(nano < 0)
        nano = 0;
    else if(nano >999999999)
        nano = 999999999;
    posix_time ptime;
    ptime.seconds = seconds;
    ptime.nanoseconds = nano;
    impl_->set_time(ptime);
}

namespace {
    int compare(posix_time const &left,posix_time const &right)
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
}

bool date_time::operator==(date_time const &other) const
{
    return compare(impl_->get_time(),other.impl_->get_time()) == 0;
}

bool date_time::operator!=(date_time const &other) const
{
    return !(*this==other);
}

bool date_time::operator<(date_time const &other) const
{
    return compare(impl_->get_time(),other.impl_->get_time()) < 0;
}

bool date_time::operator>=(date_time const &other) const
{
    return !(*this<other);
}

bool date_time::operator>(date_time const &other) const
{
    return compare(impl_->get_time(),other.impl_->get_time()) > 0;
}

bool date_time::operator<=(date_time const &other) const
{
    return !(*this>other);
}

void date_time::swap(date_time &other)
{
    impl_.swap(other.impl_);
}

int date_time::difference(date_time const &other,period_type f) const
{
    return impl_->difference(other.impl_.get(),f.mark());
}

int date_time::maximum(period_type f) const
{
    return impl_->get_value(f.mark(),abstract_calendar::actual_maximum);
}

int date_time::minimum(period_type f) const
{
    return impl_->get_value(f.mark(),abstract_calendar::actual_minimum);
}

bool date_time::is_in_daylight_saving_time() const
{
    return impl_->get_option(abstract_calendar::is_dst)!=0;
}

namespace time_zone {
    boost::mutex &tz_mutex()
    {
        static boost::mutex m;
        return m;
    }
    std::string &tz_id()
    {
        static std::string id;
        return id;
    }
    std::string global()
    {
        boost::unique_lock<boost::mutex> lock(tz_mutex());
        std::string id = tz_id();
        return id;
    }
    std::string global(std::string const &new_id)
    {
        boost::unique_lock<boost::mutex> lock(tz_mutex());
        std::string id = tz_id();
        tz_id() = new_id;
        return id;
    }
}



} // locale
} // boost



// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4


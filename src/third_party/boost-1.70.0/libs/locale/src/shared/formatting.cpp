//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/locale/formatting.hpp>
#include <boost/locale/date_time.hpp>
#include <typeinfo>
#include <algorithm>
#include "ios_prop.hpp"

namespace boost {
    namespace locale {

        ios_info::string_set::string_set() : 
            type(0),
            size(0),
            ptr(0)
        {
        }
        ios_info::string_set::~string_set()
        {
            delete [] ptr;
        }
        ios_info::string_set::string_set(string_set const &other)
        {
            if(other.ptr!=0) {
                ptr=new char[other.size];
                size=other.size;
                type=other.type;
                memcpy(ptr,other.ptr,size);
            }
            else {
                ptr=0;
                size=0;
                type=0;
            }
        }
        
        void ios_info::string_set::swap(string_set &other)
        {
            std::swap(type,other.type);
            std::swap(size,other.size);
            std::swap(ptr,other.ptr);
        }
        
        ios_info::string_set const &ios_info::string_set::operator=(string_set const &other)
        {
            if(this!=&other) {
                string_set tmp(other);
                swap(tmp);
            }
            return *this;
        }

        struct ios_info::data {};

        ios_info::ios_info() : 
            flags_(0),
            domain_id_(0),
            d(0)
        {
            time_zone_ = time_zone::global();
        }
        ios_info::~ios_info()
        {
        }
        
        ios_info::ios_info(ios_info const &other)
        {
            flags_ = other.flags_;
            domain_id_ = other.domain_id_;
            time_zone_ = other.time_zone_;
            datetime_ = other.datetime_;
        }


        ios_info const &ios_info::operator=(ios_info const &other)
        {
            if(this!=&other) {
                flags_ = other.flags_;
                domain_id_ = other.domain_id_;
                time_zone_ = other.time_zone_;
                datetime_ = other.datetime_;
            }
            return *this;
        }

        void ios_info::display_flags(uint64_t f) 
        {
            flags_ = (flags_ & ~uint64_t(flags::display_flags_mask)) | f;
        }
        void ios_info::currency_flags(uint64_t f) 
        {
            flags_ = (flags_ & ~uint64_t(flags::currency_flags_mask)) | f;
        }
        void ios_info::date_flags(uint64_t f) 
        {
            flags_ = (flags_ & ~uint64_t(flags::date_flags_mask)) | f;
        }
        void ios_info::time_flags(uint64_t f) 
        {
            flags_ = (flags_ & ~uint64_t(flags::time_flags_mask)) | f;
        }
        
        void ios_info::domain_id(int id)
        {
            domain_id_ = id;
        }

        void ios_info::time_zone(std::string const &tz)
        {
            time_zone_ = tz;
        }

        uint64_t ios_info::display_flags() const
        {
            return flags_ & flags::display_flags_mask;
        }

        uint64_t ios_info::currency_flags() const
        {
            return flags_ & flags::currency_flags_mask;
        }

        uint64_t ios_info::date_flags() const
        {
            return flags_ & flags::date_flags_mask;
        }
        
        uint64_t ios_info::time_flags() const
        {
            return flags_ & flags::time_flags_mask;
        }

        int ios_info::domain_id() const
        {
            return domain_id_;
        }

        std::string ios_info::time_zone() const
        {
            return time_zone_;
        }

        ios_info::string_set const &ios_info::date_time_pattern_set() const
        {
            return datetime_;
        }

        
        ios_info::string_set &ios_info::date_time_pattern_set()
        {
            return datetime_;
        }

        ios_info &ios_info::get(std::ios_base &ios)
        {
            return impl::ios_prop<ios_info>::get(ios);
        }

        void ios_info::on_imbue()
        {
        }

        namespace {
            struct initializer {
                initializer() {
                    impl::ios_prop<ios_info>::global_init();
                }
            } initializer_instance;
        } // namespace

    } // locale
} // boost

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

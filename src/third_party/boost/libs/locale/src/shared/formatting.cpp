//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/date_time.hpp>
#include <boost/locale/formatting.hpp>
#include "ios_prop.hpp"

namespace boost { namespace locale {
    ios_info::ios_info() : flags_(0), domain_id_(0), time_zone_(time_zone::global()) {}

    ios_info::~ios_info() = default;

    ios_info::ios_info(const ios_info&) = default;
    ios_info& ios_info::operator=(const ios_info&) = default;

    void ios_info::display_flags(uint64_t f)
    {
        flags_ = (flags_ & ~uint64_t(flags::display_flags_mask)) | f;
    }
    uint64_t ios_info::display_flags() const
    {
        return flags_ & flags::display_flags_mask;
    }

    void ios_info::currency_flags(uint64_t f)
    {
        flags_ = (flags_ & ~uint64_t(flags::currency_flags_mask)) | f;
    }
    uint64_t ios_info::currency_flags() const
    {
        return flags_ & flags::currency_flags_mask;
    }

    void ios_info::date_flags(uint64_t f)
    {
        flags_ = (flags_ & ~uint64_t(flags::date_flags_mask)) | f;
    }
    uint64_t ios_info::date_flags() const
    {
        return flags_ & flags::date_flags_mask;
    }

    void ios_info::time_flags(uint64_t f)
    {
        flags_ = (flags_ & ~uint64_t(flags::time_flags_mask)) | f;
    }
    uint64_t ios_info::time_flags() const
    {
        return flags_ & flags::time_flags_mask;
    }

    void ios_info::domain_id(int id)
    {
        domain_id_ = id;
    }
    int ios_info::domain_id() const
    {
        return domain_id_;
    }

    void ios_info::time_zone(const std::string& tz)
    {
        time_zone_ = tz;
    }
    const std::string& ios_info::time_zone() const
    {
        return time_zone_;
    }

    ios_info& ios_info::get(std::ios_base& ios)
    {
        return impl::ios_prop<ios_info>::get(ios);
    }

    void ios_info::on_imbue() {}

    namespace {
        struct initializer {
            initializer() { impl::ios_prop<ios_info>::global_init(); }
        } initializer_instance;
    } // namespace

}} // namespace boost::locale

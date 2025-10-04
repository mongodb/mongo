//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "win_backend.hpp"
#include <boost/locale/gnu_gettext.hpp>
#include <boost/locale/info.hpp>
#include <boost/locale/localization_backend.hpp>
#include <boost/locale/util.hpp>
#include <boost/locale/util/locale_data.hpp>
#include "../shared/message.hpp"
#include "../util/gregorian.hpp"
#include "../util/make_std_unique.hpp"
#include "all_generator.hpp"
#include "api.hpp"
#include <algorithm>
#include <iterator>
#include <vector>

namespace boost { namespace locale { namespace impl_win {

    class winapi_localization_backend : public localization_backend {
    public:
        winapi_localization_backend() : invalid_(true) {}
        winapi_localization_backend(const winapi_localization_backend& other) :
            localization_backend(), paths_(other.paths_), domains_(other.domains_), locale_id_(other.locale_id_),
            invalid_(true)
        {}
        winapi_localization_backend* clone() const override { return new winapi_localization_backend(*this); }

        void set_option(const std::string& name, const std::string& value) override
        {
            invalid_ = true;
            if(name == "locale")
                locale_id_ = value;
            else if(name == "message_path")
                paths_.push_back(value);
            else if(name == "message_application")
                domains_.push_back(value);
        }
        void clear_options() override
        {
            invalid_ = true;
            locale_id_.clear();
            paths_.clear();
            domains_.clear();
        }

        void prepare_data()
        {
            if(!invalid_)
                return;
            invalid_ = false;
            if(locale_id_.empty())
                real_id_ = util::get_system_locale(true); // always UTF-8
            else
                real_id_ = locale_id_;
            data_.parse(real_id_);
            if(!data_.is_utf8())
                lc_ = winlocale(); // Make it C as non-UTF8 locales are not supported
            else
                lc_ = winlocale(real_id_);
        }

        std::locale install(const std::locale& base, category_t category, char_facet_t type) override
        {
            prepare_data();

            switch(category) {
                case category_t::convert: return create_convert(base, lc_, type);
                case category_t::collation: return create_collate(base, lc_, type);
                case category_t::formatting: return create_formatting(base, lc_, type);
                case category_t::parsing: return create_parsing(base, lc_, type);
                case category_t::calendar: {
                    util::locale_data inf;
                    inf.parse(real_id_);
                    return util::install_gregorian_calendar(base, inf.country());
                }
                case category_t::message: return detail::install_message_facet(base, type, data_, domains_, paths_);
                case category_t::information: return util::create_info(base, real_id_);
                case category_t::codepage: return util::create_utf8_codecvt(base, type);
                case category_t::boundary: break; // Not implemented
            }
            return base;
        }

    private:
        std::vector<std::string> paths_;
        std::vector<std::string> domains_;
        std::string locale_id_;
        std::string real_id_;
        util::locale_data data_;

        bool invalid_;
        winlocale lc_;
    };

    std::unique_ptr<localization_backend> create_localization_backend()
    {
        return make_std_unique<winapi_localization_backend>();
    }

}}} // namespace boost::locale::impl_win

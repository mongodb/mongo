//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "icu_backend.hpp"
#include <boost/locale/gnu_gettext.hpp>
#include <boost/locale/localization_backend.hpp>
#include <boost/locale/util.hpp>
#include "../shared/message.hpp"
#include "../util/make_std_unique.hpp"
#include "all_generator.hpp"
#include "cdata.hpp"

#include <unicode/ucnv.h>

namespace boost { namespace locale { namespace impl_icu {
    class icu_localization_backend : public localization_backend {
    public:
        icu_localization_backend() : invalid_(true), use_ansi_encoding_(false) {}
        icu_localization_backend(const icu_localization_backend& other) :
            localization_backend(), paths_(other.paths_), domains_(other.domains_), locale_id_(other.locale_id_),
            invalid_(true), use_ansi_encoding_(other.use_ansi_encoding_)
        {}
        icu_localization_backend* clone() const override { return new icu_localization_backend(*this); }

        void set_option(const std::string& name, const std::string& value) override
        {
            invalid_ = true;
            if(name == "locale")
                locale_id_ = value;
            else if(name == "message_path")
                paths_.push_back(value);
            else if(name == "message_application")
                domains_.push_back(value);
            else if(name == "use_ansi_encoding")
                use_ansi_encoding_ = value == "true";
        }
        void clear_options() override
        {
            invalid_ = true;
            use_ansi_encoding_ = false;
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
                real_id_ = util::get_system_locale(/*utf8*/ !use_ansi_encoding_);
            else
                real_id_ = locale_id_;

            data_.set(real_id_);
        }

        std::locale install(const std::locale& base, category_t category, char_facet_t type) override
        {
            prepare_data();

            switch(category) {
                case category_t::convert: return create_convert(base, data_, type);
                case category_t::collation: return create_collate(base, data_, type);
                case category_t::formatting: return create_formatting(base, data_, type);
                case category_t::parsing: return create_parsing(base, data_, type);
                case category_t::codepage: return create_codecvt(base, data_.encoding(), type);
                case category_t::message:
                    return detail::install_message_facet(base, type, data_.data(), domains_, paths_);
                case category_t::boundary: return create_boundary(base, data_, type);
                case category_t::calendar: return create_calendar(base, data_);
                case category_t::information: return util::create_info(base, real_id_);
            }
            return base;
        }

    private:
        std::vector<std::string> paths_;
        std::vector<std::string> domains_;
        std::string locale_id_;
        std::string real_id_;
        cdata data_;
        bool invalid_;
        bool use_ansi_encoding_;
    };

    std::unique_ptr<localization_backend> create_localization_backend()
    {
        return make_std_unique<icu_localization_backend>();
    }

}}} // namespace boost::locale::impl_icu

//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "posix_backend.hpp"
#include <boost/locale/gnu_gettext.hpp>
#include <boost/locale/info.hpp>
#include <boost/locale/localization_backend.hpp>
#include <boost/locale/util.hpp>
#include <boost/locale/util/locale_data.hpp>
#include <algorithm>
#include <iterator>
#include <langinfo.h>
#include <vector>
#if defined(__FreeBSD__)
#    include <xlocale.h>
#endif

#include "../shared/message.hpp"
#include "../util/gregorian.hpp"
#include "../util/make_std_unique.hpp"
#include "all_generator.hpp"

namespace boost { namespace locale { namespace impl_posix {

    class posix_localization_backend : public localization_backend {
    public:
        posix_localization_backend() : invalid_(true) {}
        posix_localization_backend(const posix_localization_backend& other) :
            localization_backend(), paths_(other.paths_), domains_(other.domains_), locale_id_(other.locale_id_),
            invalid_(true)
        {}
        posix_localization_backend* clone() const override { return new posix_localization_backend(*this); }

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

        static void free_locale_by_ptr(locale_t* lc)
        {
            freelocale(*lc);
            delete lc;
        }

        void prepare_data()
        {
            if(!invalid_)
                return;

            real_id_ = locale_id_.empty() ? util::get_system_locale() : locale_id_;
            data_.parse(real_id_);

            lc_.reset();
            locale_t tmp = newlocale(LC_ALL_MASK, real_id_.c_str(), nullptr);
            if(!tmp)
                tmp = newlocale(LC_ALL_MASK, "C", nullptr);
            if(!tmp)
                throw std::runtime_error("newlocale failed");

            locale_t* tmp_p;
            try {
                tmp_p = new locale_t(tmp);
            } catch(...) {
                freelocale(tmp);
                throw;
            }
            lc_ = std::shared_ptr<locale_t>(tmp_p, free_locale_by_ptr);
            invalid_ = false;
        }

        std::locale install(const std::locale& base, category_t category, char_facet_t type) override
        {
            prepare_data();

            switch(category) {
                case category_t::convert: return create_convert(base, lc_, type);
                case category_t::collation: return create_collate(base, lc_, type);
                case category_t::formatting: return create_formatting(base, lc_, type);
                case category_t::parsing: return create_parsing(base, lc_, type);
                case category_t::codepage: return create_codecvt(base, nl_langinfo_l(CODESET, *lc_), type);
                case category_t::calendar: return util::install_gregorian_calendar(base, data_.country());
                case category_t::message: return detail::install_message_facet(base, type, data_, domains_, paths_);
                case category_t::information: return util::create_info(base, real_id_);
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
        std::shared_ptr<locale_t> lc_;
    };

    std::unique_ptr<localization_backend> create_localization_backend()
    {
        return make_std_unique<posix_localization_backend>();
    }

}}} // namespace boost::locale::impl_posix

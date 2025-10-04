//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "std_backend.hpp"
#include <boost/locale/gnu_gettext.hpp>
#include <boost/locale/localization_backend.hpp>
#include <boost/locale/util.hpp>
#include <boost/locale/util/locale_data.hpp>
#include <boost/assert.hpp>
#include <boost/core/ignore_unused.hpp>
#include <algorithm>
#include <iterator>
#include <vector>

#if BOOST_LOCALE_USE_WIN32_API
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include "../win32/lcid.hpp"
#    include <windows.h>
#endif
#include "../shared/message.hpp"
#include "../util/encoding.hpp"
#include "../util/gregorian.hpp"
#include "../util/make_std_unique.hpp"
#include "../util/numeric_conversion.hpp"
#include "all_generator.hpp"

namespace {
struct windows_name {
    std::string name, codepage;
    explicit operator bool() const { return !name.empty() && !codepage.empty(); }
};

windows_name to_windows_name(const std::string& l)
{
#if BOOST_LOCALE_USE_WIN32_API
    windows_name res;
    const unsigned lcid = boost::locale::impl_win::locale_to_lcid(l);
    char win_lang[256]{};
    if(lcid == 0 || GetLocaleInfoA(lcid, LOCALE_SENGLANGUAGE, win_lang, sizeof(win_lang)) == 0)
        return res;
    res.name = win_lang;
    char win_country[256]{};
    if(GetLocaleInfoA(lcid, LOCALE_SENGCOUNTRY, win_country, sizeof(win_country)) != 0) {
        res.name += "_";
        res.name += win_country;
    }

    char win_codepage[10]{};
    if(GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE, win_codepage, sizeof(win_codepage)) != 0)
        res.codepage = win_codepage;
    return res;
#else
    boost::ignore_unused(l);
    return {};
#endif
}

bool loadable(const std::string& name)
{
    try {
        std::locale l(name);
        return true;
    } catch(const std::exception&) {
        return false;
    }
}
} // namespace

namespace boost { namespace locale { namespace impl_std {

    class std_localization_backend : public localization_backend {
    public:
        std_localization_backend() : invalid_(true), use_ansi_encoding_(false) {}
        std_localization_backend(const std_localization_backend& other) :
            localization_backend(), paths_(other.paths_), domains_(other.domains_), locale_id_(other.locale_id_),
            invalid_(true), use_ansi_encoding_(other.use_ansi_encoding_)
        {}
        std_localization_backend* clone() const override { return new std_localization_backend(*this); }

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
            std::string lid = locale_id_;
            if(lid.empty()) {
                bool use_utf8 = !use_ansi_encoding_;
                lid = util::get_system_locale(use_utf8);
            }
            in_use_id_ = lid;
            data_.parse(lid);

            const auto l_win = to_windows_name(lid);

            if(!data_.is_utf8()) {
                utf_mode_ = utf8_support::none;
                if(loadable(lid))
                    name_ = lid;
                else if(l_win && loadable(l_win.name)) {
                    if(util::are_encodings_equal(l_win.codepage, data_.encoding()))
                        name_ = l_win.name;
                    else {
                        int codepage_int;
                        if(util::try_to_int(l_win.codepage, codepage_int)
                           && codepage_int == util::encoding_to_windows_codepage(data_.encoding()))
                        {
                            name_ = l_win.name;
                        } else
                            name_ = "C";
                    }
                } else
                    name_ = "C";
            } else {
                if(loadable(lid)) {
                    name_ = lid;
                    utf_mode_ = utf8_support::native;
                } else {
                    std::vector<std::string> alt_names;
                    if(l_win)
                        alt_names.push_back(l_win.name);
                    // Try different spellings
                    alt_names.push_back(util::locale_data(data_).encoding("UTF-8").to_string());
                    alt_names.push_back(util::locale_data(data_).encoding("utf8", false).to_string());
                    // Without encoding, let from_wide classes handle it
                    alt_names.push_back(util::locale_data(data_).encoding("").to_string());
                    // Final try: Classic locale, but enable Unicode (if supported)
                    alt_names.push_back("C.UTF-8");
                    alt_names.push_back("C.utf8");
                    // If everything fails rely on the classic locale
                    alt_names.push_back("C");
                    for(const std::string& name : alt_names) {
                        if(loadable(name)) {
                            name_ = name;
                            break;
                        }
                    }
                    BOOST_ASSERT(!name_.empty());
                    utf_mode_ = utf8_support::from_wide;
                }
            }
        }

        std::locale install(const std::locale& base, category_t category, char_facet_t type) override
        {
            prepare_data();

            switch(category) {
                case category_t::convert: return create_convert(base, name_, type, utf_mode_);
                case category_t::collation: return create_collate(base, name_, type, utf_mode_);
                case category_t::formatting: return create_formatting(base, name_, type, utf_mode_);
                case category_t::parsing: return create_parsing(base, name_, type, utf_mode_);
                case category_t::codepage: return create_codecvt(base, name_, type, utf_mode_);
                case category_t::calendar: return util::install_gregorian_calendar(base, data_.country());
                case category_t::message: return detail::install_message_facet(base, type, data_, domains_, paths_);
                case category_t::information: return util::create_info(base, in_use_id_);
                case category_t::boundary: break; // Not implemented
            }
            return base;
        }

    private:
        std::vector<std::string> paths_;
        std::vector<std::string> domains_;
        std::string locale_id_;

        util::locale_data data_;
        std::string name_;
        std::string in_use_id_;
        utf8_support utf_mode_;
        bool invalid_;
        bool use_ansi_encoding_;
    };

    std::unique_ptr<localization_backend> create_localization_backend()
    {
        return make_std_unique<std_localization_backend>();
    }

}}} // namespace boost::locale::impl_std

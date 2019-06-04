//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/locale/localization_backend.hpp>
#include <boost/locale/gnu_gettext.hpp>
#include <boost/locale/util.hpp>
#include "all_generator.hpp"
#include "cdata.hpp"
#include "icu_backend.hpp"
#include "../util/locale_data.hpp"
#include <algorithm>
#include <iterator>

#include <unicode/ucnv.h>

namespace boost {
namespace locale {
namespace impl_icu { 
    class icu_localization_backend : public localization_backend {
    public:
        icu_localization_backend() : 
            invalid_(true),
            use_ansi_encoding_(false)
        {
        }
        icu_localization_backend(icu_localization_backend const &other) : 
            localization_backend(),
            paths_(other.paths_),
            domains_(other.domains_),
            locale_id_(other.locale_id_),
            invalid_(true),
            use_ansi_encoding_(other.use_ansi_encoding_)
        {
        }
        virtual icu_localization_backend *clone() const
        {
            return new icu_localization_backend(*this);
        }

        void set_option(std::string const &name,std::string const &value) 
        {
            invalid_ = true;
            if(name=="locale")
                locale_id_ = value;
            else if(name=="message_path")
                paths_.push_back(value);
            else if(name=="message_application")
                domains_.push_back(value);
            else if(name=="use_ansi_encoding")
                use_ansi_encoding_ = value == "true";

        }
        void clear_options()
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
            real_id_ = locale_id_;
            if(real_id_.empty()) {
                bool utf8 = ! use_ansi_encoding_;
                real_id_ = util::get_system_locale(utf8);
            }
            
            util::locale_data d;
            d.parse(real_id_);

            data_.locale = icu::Locale::createCanonical(real_id_.c_str());
            data_.encoding = d.encoding;
            data_.utf8 = d.utf8;
            language_ = d.language;
            country_ = d.country;
            variant_ = d.variant;
        }
        
        virtual std::locale install(std::locale const &base,
                                    locale_category_type category,
                                    character_facet_type type = nochar_facet)
        {
            prepare_data();

            switch(category) {
            case convert_facet:
                return create_convert(base,data_,type);
            case collation_facet:
                return create_collate(base,data_,type);
            case formatting_facet:
                return create_formatting(base,data_,type);
            case parsing_facet:
                return create_parsing(base,data_,type);
            case codepage_facet:
                return create_codecvt(base,data_.encoding,type);
            case message_facet:
                {
                    gnu_gettext::messages_info minf;
                    minf.language = language_;
                    minf.country = country_;
                    minf.variant = variant_;
                    minf.encoding = data_.encoding;
                    std::copy(domains_.begin(),domains_.end(),std::back_inserter<gnu_gettext::messages_info::domains_type>(minf.domains));
                    minf.paths = paths_;
                    switch(type) {
                    case char_facet:
                        return std::locale(base,gnu_gettext::create_messages_facet<char>(minf));
                    case wchar_t_facet:
                        return std::locale(base,gnu_gettext::create_messages_facet<wchar_t>(minf));
                    #ifdef BOOST_LOCALE_ENABLE_CHAR16_T
                    case char16_t_facet:
                        return std::locale(base,gnu_gettext::create_messages_facet<char16_t>(minf));
                    #endif
                    #ifdef BOOST_LOCALE_ENABLE_CHAR32_T
                    case char32_t_facet:
                        return std::locale(base,gnu_gettext::create_messages_facet<char32_t>(minf));
                    #endif
                    default:
                        return base;
                    }
                }
            case boundary_facet:
                return create_boundary(base,data_,type); 
            case calendar_facet:
                return create_calendar(base,data_);
            case information_facet:
                return util::create_info(base,real_id_);
            default:
                return base;
            }
        }

    private:

        std::vector<std::string> paths_;
        std::vector<std::string> domains_;
        std::string locale_id_;

        cdata data_;
        std::string language_;
        std::string country_;
        std::string variant_;
        std::string real_id_;
        bool invalid_;
        bool use_ansi_encoding_;
    };
    
    localization_backend *create_localization_backend()
    {
        return new icu_localization_backend();
    }

}  // impl icu
}  // locale
}  // boost
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 

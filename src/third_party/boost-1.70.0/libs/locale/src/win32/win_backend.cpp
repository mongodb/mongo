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
#include <boost/locale/info.hpp>
#include "all_generator.hpp"
#include "win_backend.hpp"
#include <boost/locale/util.hpp>
#include "../util/gregorian.hpp"
#include "../util/locale_data.hpp"
#include "api.hpp"
#include <algorithm>
#include <iterator>

namespace boost {
namespace locale {
namespace impl_win { 
    
    class winapi_localization_backend : public localization_backend {
    public:
        winapi_localization_backend() : 
            invalid_(true)
        {
        }
        winapi_localization_backend(winapi_localization_backend const &other) :
            localization_backend(), 
            paths_(other.paths_),
            domains_(other.domains_),
            locale_id_(other.locale_id_),
            invalid_(true)
        {
        }
        virtual winapi_localization_backend *clone() const
        {
            return new winapi_localization_backend(*this);
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

        }
        void clear_options()
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
            if(locale_id_.empty()) {
                real_id_ = util::get_system_locale(true); // always UTF-8
                lc_ = winlocale(real_id_);
            }
            else {
                lc_=winlocale(locale_id_);
                real_id_ = locale_id_;
            }
            util::locale_data d;
            d.parse(real_id_);
            if(!d.utf8) {
                lc_ = winlocale(); 
                // Make it C as non-UTF8 locales are not supported
            }
        }
        
        virtual std::locale install(std::locale const &base,
                                    locale_category_type category,
                                    character_facet_type type = nochar_facet)
        {
            prepare_data();

            switch(category) {
            case convert_facet:
                return create_convert(base,lc_,type);
            case collation_facet:
                return create_collate(base,lc_,type);
            case formatting_facet:
                return create_formatting(base,lc_,type);
            case parsing_facet:
                return create_parsing(base,lc_,type);
            case calendar_facet:
                {
                    util::locale_data inf;
                    inf.parse(real_id_);
                    return util::install_gregorian_calendar(base,inf.country);
                }
            case message_facet:
                {
                    gnu_gettext::messages_info minf;
                    std::locale tmp=util::create_info(std::locale::classic(),real_id_);
                    boost::locale::info const &inf=std::use_facet<boost::locale::info>(tmp);
                    minf.language = inf.language();
                    minf.country = inf.country();
                    minf.variant = inf.variant();
                    minf.encoding = inf.encoding();
                    std::copy(domains_.begin(),domains_.end(),std::back_inserter<gnu_gettext::messages_info::domains_type>(minf.domains));
                    minf.paths = paths_;
                    switch(type) {
                    case char_facet:
                        return std::locale(base,gnu_gettext::create_messages_facet<char>(minf));
                    case wchar_t_facet:
                        return std::locale(base,gnu_gettext::create_messages_facet<wchar_t>(minf));
                    default:
                        return base;
                    }
                }
            case information_facet:
                return util::create_info(base,real_id_);
            case codepage_facet:
                return util::create_utf8_codecvt(base,type);
            default:
                return base;
            }
        }

    private:

        std::vector<std::string> paths_;
        std::vector<std::string> domains_;
        std::string locale_id_;
        std::string real_id_;

        bool invalid_;
        winlocale lc_;
    };
    
    localization_backend *create_localization_backend()
    {
        return new winapi_localization_backend();
    }

}  // impl win
}  // locale
}  // boost
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 

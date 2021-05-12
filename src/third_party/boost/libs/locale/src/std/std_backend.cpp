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
#include "all_generator.hpp"
#include "../util/locale_data.hpp"
#include "../util/gregorian.hpp"
#include <boost/locale/util.hpp>
#include <algorithm>
#include <iterator>

#if defined(BOOST_WINDOWS)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include "../encoding/conv.hpp"
#  include "../win32/lcid.hpp"
#endif

#include "std_backend.hpp"

namespace boost {
namespace locale {
namespace impl_std { 
    
    class std_localization_backend : public localization_backend {
    public:
        std_localization_backend() : 
            invalid_(true),
            use_ansi_encoding_(false)
        {
        }
        std_localization_backend(std_localization_backend const &other) : 
            localization_backend(),
            paths_(other.paths_),
            domains_(other.domains_),
            locale_id_(other.locale_id_),
            invalid_(true),
            use_ansi_encoding_(other.use_ansi_encoding_)
        {
        }
        virtual std_localization_backend *clone() const
        {
            return new std_localization_backend(*this);
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
            std::string lid=locale_id_;
            if(lid.empty()) {
                bool use_utf8 = ! use_ansi_encoding_;
                lid = util::get_system_locale(use_utf8);
            }
            in_use_id_ = lid;
            data_.parse(lid);
            name_ = "C";
            utf_mode_ = utf8_none;

            #if defined(BOOST_WINDOWS)
            std::pair<std::string,int> wl_inf = to_windows_name(lid);
            std::string win_name = wl_inf.first;
            int win_codepage = wl_inf.second;
            #endif

            if(!data_.utf8) {
                if(loadable(lid)) {
                    name_ = lid;
                    utf_mode_ = utf8_none;
                }
                #if defined(BOOST_WINDOWS)
                else if(loadable(win_name) 
                        && win_codepage == conv::impl::encoding_to_windows_codepage(data_.encoding.c_str())) 
                {
                    name_ = win_name;
                    utf_mode_ = utf8_none;
                }
                #endif
            }
            else {
                if(loadable(lid)) {
                    name_ = lid;
                    utf_mode_ = utf8_native_with_wide;
                }
                #if defined(BOOST_WINDOWS)
                else if(loadable(win_name)) {
                    name_ = win_name;
                    utf_mode_ = utf8_from_wide;
                }
                #endif
            }
        }
        
        #if defined(BOOST_WINDOWS)
        std::pair<std::string,int> to_windows_name(std::string const &l)
        {
            std::pair<std::string,int> res("C",0);
            unsigned lcid = impl_win::locale_to_lcid(l);
            char win_lang[256]  = {0};
            char win_country[256]  = {0};
            char win_codepage[10] = {0};
            if(GetLocaleInfoA(lcid,LOCALE_SENGLANGUAGE,win_lang,sizeof(win_lang))==0)
                return res;
            std::string lc_name = win_lang;
            if(GetLocaleInfoA(lcid,LOCALE_SENGCOUNTRY,win_country,sizeof(win_country))!=0) {
                lc_name += "_";
                lc_name += win_country;
            }
            
            res.first = lc_name;

            if(GetLocaleInfoA(lcid,LOCALE_IDEFAULTANSICODEPAGE,win_codepage,sizeof(win_codepage))!=0)
                res.second = atoi(win_codepage);
            return res;
        }
        #endif
        
        bool loadable(std::string name)
        {
            try {
                std::locale l(name.c_str());
                return true;
            }
            catch(std::exception const &/*e*/) {
                return false;
            }
        }
        
        virtual std::locale install(std::locale const &base,
                                    locale_category_type category,
                                    character_facet_type type = nochar_facet)
        {
            prepare_data();

            switch(category) {
            case convert_facet:
                return create_convert(base,name_,type,utf_mode_);
            case collation_facet:
                return create_collate(base,name_,type,utf_mode_);
            case formatting_facet:
                return create_formatting(base,name_,type,utf_mode_);
            case parsing_facet:
                return create_parsing(base,name_,type,utf_mode_);
            case codepage_facet:
                return create_codecvt(base,name_,type,utf_mode_);
            case calendar_facet:
                return util::install_gregorian_calendar(base,data_.country);
            case message_facet:
                {
                    gnu_gettext::messages_info minf;
                    minf.language = data_.language;
                    minf.country = data_.country;
                    minf.variant = data_.variant;
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
            case information_facet:
                return util::create_info(base,in_use_id_);
            default:
                return base;
            }
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
    
    localization_backend *create_localization_backend()
    {
        return new std_localization_backend();
    }

}  // impl icu
}  // locale
}  // boost
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 

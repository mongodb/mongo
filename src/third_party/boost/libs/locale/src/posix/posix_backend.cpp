//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#if defined(__FreeBSD__)
#include <xlocale.h>
#endif
#include <boost/locale/localization_backend.hpp>
#include <boost/locale/gnu_gettext.hpp>
#include <boost/locale/info.hpp>
#include "all_generator.hpp"
#include "posix_backend.hpp"

#include "../util/locale_data.hpp"
#include "../util/gregorian.hpp"
#include <boost/locale/util.hpp>
#include <algorithm>
#include <iterator>

#include <langinfo.h>

namespace boost {
namespace locale {
namespace impl_posix { 
    
    class posix_localization_backend : public localization_backend {
    public:
        posix_localization_backend() : 
            invalid_(true)
        {
        }
        posix_localization_backend(posix_localization_backend const &other) : 
            localization_backend(),
            paths_(other.paths_),
            domains_(other.domains_),
            locale_id_(other.locale_id_),
            invalid_(true)
        {
        }
        virtual posix_localization_backend *clone() const
        {
            return new posix_localization_backend(*this);
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

        static void free_locale_by_ptr(locale_t *lc)
        {
            freelocale(*lc);
            delete lc;
        }

        void prepare_data()
        {
            if(!invalid_)
                return;
            invalid_ = false;
            lc_.reset();
            real_id_ = locale_id_;
            if(real_id_.empty())
                real_id_ = util::get_system_locale();
            
            locale_t tmp = newlocale(LC_ALL_MASK,real_id_.c_str(),0);
            
            if(!tmp) {
                tmp=newlocale(LC_ALL_MASK,"C",0);
            }
            if(!tmp) {
                throw std::runtime_error("newlocale failed");
            }
            
            locale_t *tmp_p = 0;
            
            try {
                tmp_p = new locale_t();
            }
            catch(...) {
                freelocale(tmp);
                throw;
            }
            
            *tmp_p = tmp;
            lc_ = boost::shared_ptr<locale_t>(tmp_p,free_locale_by_ptr);
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
            case codepage_facet:
                return create_codecvt(base,nl_langinfo_l(CODESET,*lc_),type);
            case calendar_facet:
                {
                    util::locale_data inf;
                    inf.parse(real_id_);
                    return util::install_gregorian_calendar(base,inf.country);
                }
            case message_facet:
                {
                    gnu_gettext::messages_info minf;
                    util::locale_data inf;
                    inf.parse(real_id_);
                    minf.language = inf.language;
                    minf.country = inf.country;
                    minf.variant = inf.variant;
                    minf.encoding = inf.encoding;
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
                return util::create_info(base,real_id_);
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
        boost::shared_ptr<locale_t> lc_;
    };
    
    localization_backend *create_localization_backend()
    {
        return new posix_localization_backend();
    }

}  // impl posix
}  // locale
}  // boost
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 

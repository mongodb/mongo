//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <locale>
#include <boost/cstdint.hpp>
#include <boost/locale/util.hpp>
#include "all_generator.hpp"
#include <vector>
namespace boost {
namespace locale {
namespace impl_std {
    template<typename CharType>
    std::locale codecvt_bychar( std::locale const &in,
                                std::string const &locale_name)
    {
        return std::locale(in,new std::codecvt_byname<CharType,char,std::mbstate_t>(locale_name.c_str()));
    }
    

    std::locale create_codecvt( std::locale const &in,
                                std::string const &locale_name,
                                character_facet_type type,
                                utf8_support utf) 
    {
        if(utf == utf8_from_wide) {
            return util::create_utf8_codecvt(in,type);
        }
        switch(type) {
        case char_facet:
            return codecvt_bychar<char>(in,locale_name);
        case wchar_t_facet:
            return codecvt_bychar<wchar_t>(in,locale_name);
        #if defined(BOOST_LOCALE_ENABLE_CHAR16_T) && !defined(BOOST_NO_CHAR16_T_CODECVT)
        case char16_t_facet:
            return codecvt_bychar<char16_t>(in,locale_name);
        #endif
        #if defined(BOOST_LOCALE_ENABLE_CHAR32_T) && !defined(BOOST_NO_CHAR32_T_CODECVT)
        case char32_t_facet:
            return codecvt_bychar<char32_t>(in,locale_name);
        #endif
        default:
            return in;
        }
    }

} // impl_std
} // locale 
} // boost

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

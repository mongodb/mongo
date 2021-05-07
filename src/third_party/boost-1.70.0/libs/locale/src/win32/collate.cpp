//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <locale>
#include <string>
#include <ios>
#include <boost/locale/encoding.hpp>
#include <boost/locale/generator.hpp>
#include "api.hpp"
#include "../shared/mo_hash.hpp"

namespace boost {
namespace locale {
namespace impl_win {

class utf8_collator : public collator<char> {
public:
    utf8_collator(winlocale lc,size_t refs = 0) : 
        collator<char>(refs),
        lc_(lc)
    {
    }
    virtual int do_compare(collator_base::level_type level,char const *lb,char const *le,char const *rb,char const *re) const
    {
        std::wstring l=conv::to_utf<wchar_t>(lb,le,"UTF-8");
        std::wstring r=conv::to_utf<wchar_t>(rb,re,"UTF-8");
        return wcscoll_l(level,l.c_str(),l.c_str()+l.size(),r.c_str(),r.c_str()+r.size(),lc_);
    }
    virtual long do_hash(collator_base::level_type level,char const *b,char const *e) const
    {
        std::string key = do_transform(level,b,e);
        return gnu_gettext::pj_winberger_hash_function(key.c_str(),key.c_str() + key.size());
    }
    virtual std::string do_transform(collator_base::level_type level,char const *b,char const *e) const
    {
        std::wstring tmp=conv::to_utf<wchar_t>(b,e,"UTF-8");
        std::wstring wkey = wcsxfrm_l(level,tmp.c_str(),tmp.c_str()+tmp.size(),lc_);
        std::string key;
        if(sizeof(wchar_t)==2)
            key.reserve(wkey.size()*2);
        else
            key.reserve(wkey.size()*3);
        for(unsigned i=0;i<wkey.size();i++) {
            if(sizeof(wchar_t)==2) {
                uint16_t tv = static_cast<uint16_t>(wkey[i]);
                key += char(tv >> 8);
                key += char(tv & 0xFF);
            }
            else { // 4
                uint32_t tv = static_cast<uint32_t>(wkey[i]);
                // 21 bit
                key += char((tv >> 16) & 0xFF);
                key += char((tv >> 8) & 0xFF);
                key += char(tv & 0xFF);
            }
        }
        return key;
    }
private:
    winlocale lc_;
};


class utf16_collator : public collator<wchar_t> {
public:
    typedef std::collate<wchar_t> wfacet;
    utf16_collator(winlocale lc,size_t refs = 0) : 
        collator<wchar_t>(refs),
        lc_(lc)
    {
    }
    virtual int do_compare(collator_base::level_type level,wchar_t const *lb,wchar_t const *le,wchar_t const *rb,wchar_t const *re) const
    {
        return wcscoll_l(level,lb,le,rb,re,lc_);
    }
    virtual long do_hash(collator_base::level_type level,wchar_t const *b,wchar_t const *e) const
    {
        std::wstring key = do_transform(level,b,e);
        char const *begin = reinterpret_cast<char const *>(key.c_str());
        char const *end = begin + key.size()*sizeof(wchar_t);
        return gnu_gettext::pj_winberger_hash_function(begin,end);
    }
    virtual std::wstring do_transform(collator_base::level_type level,wchar_t const *b,wchar_t const *e) const
    {
        return wcsxfrm_l(level,b,e,lc_);
    }
private:
    winlocale lc_;
};


std::locale create_collate( std::locale const &in,
                            winlocale const &lc,
                            character_facet_type type)
{
    if(lc.is_c()) {
        switch(type) {
        case char_facet:
            return std::locale(in,new std::collate_byname<char>("C"));
        case wchar_t_facet:
            return std::locale(in,new std::collate_byname<wchar_t>("C"));
        }
    }
    else {
        switch(type) {
        case char_facet:
            return std::locale(in,new utf8_collator(lc));
        case wchar_t_facet:
            return std::locale(in,new utf16_collator(lc));
        }
    }
    return in;
}


} // impl_std
} // locale 
} //boost



// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

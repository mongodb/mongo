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
#include <locale>
#include <stdexcept>
#include <boost/locale/generator.hpp>
#include <boost/locale/conversion.hpp>
#include <boost/locale/encoding.hpp>
#include <boost/shared_ptr.hpp>
#include <vector>
#include <string.h>
#include <wctype.h>
#include <ctype.h>
#include <langinfo.h>
#include "all_generator.hpp"

namespace boost {
namespace locale {
namespace impl_posix {

template<typename CharType>
struct case_traits;

template<>
struct case_traits<char> {
    static char lower(char c,locale_t lc)
    {
        return tolower_l(c,lc);
    }
    static char upper(char c,locale_t lc)
    {
        return toupper_l(c,lc);
    }
};

template<>
struct case_traits<wchar_t> {
    static wchar_t lower(wchar_t c,locale_t lc)
    {
        return towlower_l(c,lc);
    }
    static wchar_t upper(wchar_t c,locale_t lc)
    {
        return towupper_l(c,lc);
    }
};


template<typename CharType>
class std_converter : public converter<CharType> 
{
public:
    typedef CharType char_type;
    typedef std::basic_string<char_type> string_type;
    typedef std::ctype<char_type> ctype_type;
    std_converter(boost::shared_ptr<locale_t> lc,size_t refs = 0) : 
        converter<CharType>(refs),
        lc_(lc)
    {
    }
    virtual string_type convert(converter_base::conversion_type how,char_type const *begin,char_type const *end,int /*flags*/ = 0) const 
    {
        switch(how) {
        case converter_base::upper_case:
            {
                string_type res;
                res.reserve(end-begin);
                while(begin!=end) {
                    res+=case_traits<char_type>::upper(*begin++,*lc_);
                }
                return res;
            }
        case converter_base::lower_case:
        case converter_base::case_folding:
            {
                string_type res;
                res.reserve(end-begin);
                while(begin!=end) {
                    res+=case_traits<char_type>::lower(*begin++,*lc_);
                }
                return res;
            }
        default:
            return string_type(begin,end-begin);
        }
    }
private:
    boost::shared_ptr<locale_t> lc_;
};

class utf8_converter : public converter<char> {
public:
    utf8_converter(boost::shared_ptr<locale_t> lc,size_t refs = 0) : 
        converter<char>(refs),
        lc_(lc)
    {
    }
    virtual std::string convert(converter_base::conversion_type how,char const *begin,char const *end,int /*flags*/ = 0) const 
    {
        switch(how) {
        case upper_case:
            {
                std::wstring tmp = conv::to_utf<wchar_t>(begin,end,"UTF-8");
                std::wstring wres;
                wres.reserve(tmp.size());
                for(unsigned i=0;i<tmp.size();i++)
                    wres+=towupper_l(tmp[i],*lc_);
                return conv::from_utf<wchar_t>(wres,"UTF-8");
            }
            
        case lower_case:
        case case_folding:
            {
                std::wstring tmp = conv::to_utf<wchar_t>(begin,end,"UTF-8");
                std::wstring wres;
                wres.reserve(tmp.size());
                for(unsigned i=0;i<tmp.size();i++)
                    wres+=towlower_l(tmp[i],*lc_);
                return conv::from_utf<wchar_t>(wres,"UTF-8");
            }
        default:
            return std::string(begin,end-begin);
        }
    }
private:
    boost::shared_ptr<locale_t> lc_;
};

std::locale create_convert( std::locale const &in,
                            boost::shared_ptr<locale_t> lc,
                            character_facet_type type)
{
        switch(type) {
        case char_facet: 
            {
                std::string encoding = nl_langinfo_l(CODESET,*lc);
                for(unsigned i=0;i<encoding.size();i++)
                    if('A'<=encoding[i] && encoding[i]<='Z')
                        encoding[i]=encoding[i]-'A'+'a';
                if(encoding=="utf-8" || encoding=="utf8" || encoding=="utf_8") {
                    return std::locale(in,new utf8_converter(lc));
                }
                return std::locale(in,new std_converter<char>(lc));
            }
        case wchar_t_facet:
            return std::locale(in,new std_converter<wchar_t>(lc));
        default:
            return in;
        }
}


} // namespace impl_std
} // locale 
} // boost
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

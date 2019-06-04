//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE

#include <boost/config.hpp>
#ifdef BOOST_MSVC
#  pragma warning(disable : 4996)
#endif

#include <locale>
#include <stdexcept>
#include <boost/locale/generator.hpp>
#include <boost/locale/conversion.hpp>
#include <boost/locale/encoding.hpp>
#include <vector>



#include "all_generator.hpp"

namespace boost {
namespace locale {
namespace impl_std {


template<typename CharType>
class std_converter : public converter<CharType> 
{
public:
    typedef CharType char_type;
    typedef std::basic_string<char_type> string_type;
    typedef std::ctype<char_type> ctype_type;
    std_converter(std::locale const &base,size_t refs = 0) : 
        converter<CharType>(refs),
        base_(base)
    {
    }
    virtual string_type convert(converter_base::conversion_type how,char_type const *begin,char_type const *end,int /*flags*/ = 0) const 
    {
        switch(how) {
        case converter_base::upper_case:
        case converter_base::lower_case:
        case converter_base::case_folding:
            {
                ctype_type const &ct=std::use_facet<ctype_type>(base_);
                size_t len = end - begin;
                std::vector<char_type> res(len+1,0);
                char_type *lbegin = &res[0];
                std::copy(begin,end,lbegin);
                if(how == converter_base::upper_case)
                    ct.toupper(lbegin,lbegin+len);
                else
                    ct.tolower(lbegin,lbegin+len);
                return string_type(lbegin,len);
            }
        default:
            return string_type(begin,end-begin);
        }
    }
private:
    std::locale base_;
};

class utf8_converter : public converter<char> {
public:
    typedef std::ctype<char> ctype_type;
    typedef std::ctype<wchar_t> wctype_type;
    utf8_converter(std::locale const &base,size_t refs = 0) : 
        converter<char>(refs),
        base_(base)
    {
    }
    virtual std::string convert(converter_base::conversion_type how,char const *begin,char const *end,int /*flags*/ = 0) const 
    {
        switch(how) {
        case upper_case:
        case lower_case:
        case case_folding:
            {
                std::wstring tmp = conv::to_utf<wchar_t>(begin,end,"UTF-8");
                wctype_type const &ct=std::use_facet<wctype_type>(base_);
                size_t len = tmp.size();
                std::vector<wchar_t> res(len+1,0);
                wchar_t *lbegin = &res[0];
                std::copy(tmp.c_str(),tmp.c_str()+len,lbegin);
                if(how == upper_case)
                    ct.toupper(lbegin,lbegin+len);
                else
                    ct.tolower(lbegin,lbegin+len);
                return conv::from_utf<wchar_t>(lbegin,lbegin+len,"UTF-8");
            }
        default:
            return std::string(begin,end-begin);
        }
    }
private:
    std::locale base_;
};

std::locale create_convert( std::locale const &in,
                            std::string const &locale_name,
                            character_facet_type type,
                            utf8_support utf)
{
        switch(type) {
        case char_facet: 
            {
                if(utf == utf8_native_with_wide || utf == utf8_from_wide) {
                    std::locale base(std::locale::classic(),new std::ctype_byname<wchar_t>(locale_name.c_str()));
                    return std::locale(in,new utf8_converter(base));
                }
                std::locale base(std::locale::classic(),new std::ctype_byname<char>(locale_name.c_str()));
                return std::locale(in,new std_converter<char>(base));
            }
        case wchar_t_facet:
            {
                std::locale base(std::locale::classic(),new std::ctype_byname<wchar_t>(locale_name.c_str()));
                return std::locale(in,new std_converter<wchar_t>(base));
            }
        #ifdef BOOST_LOCALE_ENABLE_CHAR16_T
        case char16_t_facet:
            {
                std::locale base(std::locale::classic(),new std::ctype_byname<char16_t>(locale_name.c_str()));
                return std::locale(in,new std_converter<char16_t>(base));
            }
        #endif
        #ifdef BOOST_LOCALE_ENABLE_CHAR32_T
        case char32_t_facet:
            {
                std::locale base(std::locale::classic(),new std::ctype_byname<char32_t>(locale_name.c_str()));
                return std::locale(in,new std_converter<char32_t>(base));
            }
        #endif
        default:
            return in;
        }
}


} // namespace impl_std
} // locale 
} // boost
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

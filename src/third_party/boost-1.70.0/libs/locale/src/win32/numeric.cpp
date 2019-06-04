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
#include <boost/locale/formatting.hpp>
#include <boost/locale/generator.hpp>
#include <boost/locale/encoding.hpp>
#include <boost/shared_ptr.hpp>
#include <sstream>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <wctype.h>
#include <ctype.h>

#include "all_generator.hpp"
#include "api.hpp"
#include "../util/numeric.hpp"

namespace boost {
namespace locale {
namespace impl_win {
    namespace {

        std::ostreambuf_iterator<wchar_t> write_it(std::ostreambuf_iterator<wchar_t> out,std::wstring const &s)
        {
            for(size_t i=0;i<s.size();i++)
                *out++ = s[i];
            return out;
        }
        
        std::ostreambuf_iterator<char> write_it(std::ostreambuf_iterator<char> out,std::wstring const &s)
        {
            std::string tmp = conv::from_utf(s,"UTF-8");
            for(size_t i=0;i<tmp.size();i++)
                *out++ = tmp[i];
            return out;
        }
    }


template<typename CharType>
class num_format : public util::base_num_format<CharType>
{
public:
    typedef typename std::num_put<CharType>::iter_type iter_type;
    typedef std::basic_string<CharType> string_type;
    typedef CharType char_type;

    num_format(winlocale const &lc,size_t refs = 0) : 
        util::base_num_format<CharType>(refs),
        lc_(lc)
    {
    }
private:

    virtual 
    iter_type do_format_currency(bool /*intl*/,iter_type out,std::ios_base &ios,char_type fill,long double val) const
    {
        if(lc_.is_c()) {
            std::locale loc = ios.getloc();
            int digits = std::use_facet<std::moneypunct<char_type> >(loc).frac_digits();
            while(digits > 0) {
                val*=10;
                digits --;
            }
            std::ios_base::fmtflags f=ios.flags();
            ios.flags(f | std::ios_base::showbase);
            out = std::use_facet<std::money_put<char_type> >(loc).put(out,false,ios,fill,val);
            ios.flags(f);
            return out;
        }
        else {
            std::wstring cur = wcsfmon_l(val,lc_);
            return write_it(out,cur);
        }
    }

private:
    winlocale lc_;

};  /// num_format

template<typename CharType>
class time_put_win : public std::time_put<CharType> {
public:
    time_put_win(winlocale const &lc, size_t refs = 0) : 
        std::time_put<CharType>(refs),
        lc_(lc)
    {
    }
    virtual ~time_put_win()
    {
    }
    typedef typename std::time_put<CharType>::iter_type iter_type;
    typedef CharType char_type;
    typedef std::basic_string<char_type> string_type;

    virtual iter_type do_put(   iter_type out,
                                std::ios_base &/*ios*/,
                                CharType /*fill*/,
                                std::tm const *tm,
                                char format,
                                char /*modifier*/) const
    {
        return write_it(out,wcsftime_l(format,tm,lc_));
    }

private:
    winlocale lc_;
};


template<typename CharType>
class num_punct_win : public std::numpunct<CharType> {
public:
    typedef std::basic_string<CharType> string_type;
    num_punct_win(winlocale const &lc,size_t refs = 0) : 
        std::numpunct<CharType>(refs)
    {
        numeric_info np = wcsnumformat_l(lc) ;
        if(sizeof(CharType) == 1 && np.thousands_sep == L"\xA0")
            np.thousands_sep=L" ";

        to_str(np.thousands_sep,thousands_sep_);
        to_str(np.decimal_point,decimal_point_);
        grouping_ = np.grouping;
        if(thousands_sep_.size() > 1)
            grouping_ = std::string();
        if(decimal_point_.size() > 1)
            decimal_point_ = CharType('.');
    }
    
    void to_str(std::wstring &s1,std::wstring &s2)
    {
        s2.swap(s1);
    }

    void to_str(std::wstring &s1,std::string &s2)
    {
        s2=conv::from_utf(s1,"UTF-8");
    }
    virtual CharType do_decimal_point() const
    {
        return *decimal_point_.c_str();
    }
    virtual CharType do_thousands_sep() const
    {
        return *thousands_sep_.c_str();
    }
    virtual std::string do_grouping() const
    {
        return grouping_;
    }
    virtual string_type do_truename() const
    {
        static const char t[]="true";
        return string_type(t,t+sizeof(t)-1);
    }
    virtual string_type do_falsename() const
    {
        static const char t[]="false";
        return string_type(t,t+sizeof(t)-1);
    }
private:
    string_type decimal_point_;
    string_type thousands_sep_;
    std::string grouping_;
};

template<typename CharType>
std::locale create_formatting_impl(std::locale const &in,winlocale const &lc)
{
    if(lc.is_c()) {
        std::locale tmp(in,new std::numpunct_byname<CharType>("C"));
        tmp=std::locale(tmp,new std::time_put_byname<CharType>("C"));
        tmp = std::locale(tmp,new num_format<CharType>(lc));
        return tmp;
    }
    else {
        std::locale tmp(in,new num_punct_win<CharType>(lc));
        tmp = std::locale(tmp,new time_put_win<CharType>(lc));
        tmp = std::locale(tmp,new num_format<CharType>(lc));
        return tmp;
    }
}

template<typename CharType>
std::locale create_parsing_impl(std::locale const &in,winlocale const &lc)
{
    std::numpunct<CharType> *np = 0;
    if(lc.is_c())
        np = new std::numpunct_byname<CharType>("C");
    else
        np = new num_punct_win<CharType>(lc);
    std::locale tmp(in,np);
    tmp = std::locale(tmp,new util::base_num_parse<CharType>());
    return tmp;
}


std::locale create_formatting(  std::locale const &in,
                                winlocale const &lc,
                                character_facet_type type)
{
        switch(type) {
        case char_facet: 
            return create_formatting_impl<char>(in,lc);
        case wchar_t_facet:
            return create_formatting_impl<wchar_t>(in,lc);
        default:
            return in;
        }
}

std::locale create_parsing( std::locale const &in,
                            winlocale const &lc,
                            character_facet_type type)
{
        switch(type) {
        case char_facet: 
            return create_parsing_impl<char>(in,lc);
        case wchar_t_facet:
            return create_parsing_impl<wchar_t>(in,lc);
        default:
            return in;
        }
}



} // impl_std
} // locale 
} //boost



// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

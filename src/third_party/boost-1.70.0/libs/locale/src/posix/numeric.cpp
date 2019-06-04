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
#include <langinfo.h>
#include <monetary.h>
#include <errno.h>
#include "../util/numeric.hpp"
#include "all_generator.hpp"


#if defined(__linux) || defined(__APPLE__)
#define BOOST_LOCALE_HAVE_WCSFTIME_L
#endif

namespace boost {
namespace locale {
namespace impl_posix {

template<typename CharType>
class num_format : public util::base_num_format<CharType>
{
public:
    typedef typename std::num_put<CharType>::iter_type iter_type;
    typedef std::basic_string<CharType> string_type;
    typedef CharType char_type;

    num_format(boost::shared_ptr<locale_t> lc,size_t refs = 0) : 
        util::base_num_format<CharType>(refs),
        lc_(lc)
    {
    }
protected: 

    virtual iter_type do_format_currency(bool intl,iter_type out,std::ios_base &/*ios*/,char_type /*fill*/,long double val) const
    {
        char buf[4]={};
        char const *format = intl ? "%i" : "%n";
        errno=0;
        ssize_t n = strfmon_l(buf,sizeof(buf),*lc_,format,static_cast<double>(val));
        if(n >= 0) 
            return write_it(out,buf,n);
        
        for(std::vector<char> tmp(sizeof(buf)*2);tmp.size() <= 4098;tmp.resize(tmp.size()*2)) {
            n = strfmon_l(&tmp.front(),tmp.size(),*lc_,format,static_cast<double>(val));
            if(n >= 0)
                return write_it(out,&tmp.front(),n);
        }
        return out;
    }

    std::ostreambuf_iterator<char> write_it(std::ostreambuf_iterator<char> out,char const *ptr,size_t n) const
    {
        for(size_t i=0;i<n;i++)
            *out++ = *ptr++;
        return out;
    }
    
    std::ostreambuf_iterator<wchar_t> write_it(std::ostreambuf_iterator<wchar_t> out,char const *ptr,size_t n) const
    {
        std::wstring tmp = conv::to_utf<wchar_t>(ptr,ptr+n,nl_langinfo_l(CODESET,*lc_));
        for(size_t i=0;i<tmp.size();i++)
            *out++ = tmp[i];
        return out;
    }
private:

    boost::shared_ptr<locale_t> lc_;

};  /// num_format


template<typename CharType>
struct ftime_traits;

template<>
struct ftime_traits<char> {
    static std::string ftime(char const *format,const struct tm *t,locale_t lc)
    {
        char buf[16];
        size_t n=strftime_l(buf,sizeof(buf),format,t,lc);
        if(n == 0) {
            // should be big enough
            //
            // Note standard specifies that in case of the error
            // the function returns 0, however 0 may be actually
            // valid output value of for example empty format or an
            // output of %p in some locales
            //
            // Thus we try to guess that 1024 would be enough.
            std::vector<char> v(1024);
            n = strftime_l(&v.front(),1024,format,t,lc);
            return std::string(&v.front(),n);
        }
        return std::string(buf,n);
    }
};

template<>
struct ftime_traits<wchar_t> {
    static std::wstring ftime(wchar_t const *format,const struct tm *t,locale_t lc)
    {
        #ifdef HAVE_WCSFTIME_L
            wchar_t buf[16];
            size_t n=wcsftime_l(buf,sizeof(buf)/sizeof(buf[0]),format,t,lc);
            if(n == 0) {
                // should be big enough
                //
                // Note standard specifies that in case of the error
                // the function returns 0, however 0 may be actually
                // valid output value of for example empty format or an
                // output of %p in some locales
                //
                // Thus we try to guess that 1024 would be enough.
                std::vector<wchar_t> v(1024);
                n = wcsftime_l(&v.front(),1024,format,t,lc);
            }
            return std::wstring(&v.front(),n);
        #else
            std::string enc = nl_langinfo_l(CODESET,lc);
            std::string nformat = conv::from_utf<wchar_t>(format,enc);
            std::string nres = ftime_traits<char>::ftime(nformat.c_str(),t,lc);
            return conv::to_utf<wchar_t>(nres,enc);
        #endif
    }
};


template<typename CharType>
class time_put_posix : public std::time_put<CharType> {
public:
    time_put_posix(boost::shared_ptr<locale_t> lc, size_t refs = 0) : 
        std::time_put<CharType>(refs),
        lc_(lc)
    {
    }
    virtual ~time_put_posix()
    {
    }
    typedef typename std::time_put<CharType>::iter_type iter_type;
    typedef CharType char_type;
    typedef std::basic_string<char_type> string_type;

    virtual iter_type do_put(iter_type out,std::ios_base &/*ios*/,CharType /*fill*/,std::tm const *tm,char format,char modifier) const
    {
        char_type fmt[4] = { '%' , modifier != 0 ? modifier : format , modifier == 0 ? '\0' : format };
        string_type res = ftime_traits<char_type>::ftime(fmt,tm,*lc_);
        for(unsigned i=0;i<res.size();i++)
            *out++ = res[i];
        return out;
    }

private:
    boost::shared_ptr<locale_t> lc_;
};


template<typename CharType>
class ctype_posix;

template<>
class ctype_posix<char> : public std::ctype<char> {
public:

    ctype_posix(boost::shared_ptr<locale_t> lc) 
    {
        lc_ = lc;
    }
   
    bool do_is(mask m,char c) const
    {
        if((m & space) && isspace_l(c,*lc_))
            return true;
        if((m & print) && isprint_l(c,*lc_))
            return true;
        if((m & cntrl) && iscntrl_l(c,*lc_))
            return true;
        if((m & upper) && isupper_l(c,*lc_))
            return true;
        if((m & lower) && islower_l(c,*lc_))
            return true;
        if((m & alpha) && isalpha_l(c,*lc_))
            return true;
        if((m & digit) && isdigit_l(c,*lc_))
            return true;
        if((m & xdigit) && isxdigit_l(c,*lc_))
            return true;
        if((m & punct) && ispunct_l(c,*lc_))
            return true;
        return false;
    }
    char const *do_is(char const *begin,char const *end,mask *m) const
    {
        while(begin!=end) {
            char c= *begin++;
            int r=0;
            if(isspace_l(c,*lc_))
                r|=space;
            if(isprint_l(c,*lc_))
                r|=cntrl;
            if(iscntrl_l(c,*lc_))
                r|=space;
            if(isupper_l(c,*lc_))
                r|=upper;
            if(islower_l(c,*lc_))
                r|=lower;
            if(isalpha_l(c,*lc_))
                r|=alpha;
            if(isdigit_l(c,*lc_))
                r|=digit;
            if(isxdigit_l(c,*lc_))
                r|=xdigit;
            if(ispunct_l(c,*lc_))
                r|=punct;
            // r actually should be mask, but some standard
            // libraries (like STLPort)
            // do not define operator | properly so using int+cast
            *m++ = static_cast<mask>(r);
        }
        return begin;
    }
    char const *do_scan_is(mask m,char const *begin,char const *end) const
    {
        while(begin!=end)
            if(do_is(m,*begin))
                return begin;
        return begin;
    }
    char const *do_scan_not(mask m,char const *begin,char const *end) const
    {
        while(begin!=end)
            if(!do_is(m,*begin))
                return begin;
        return begin;
    }
    char toupper(char c) const
    {
        return toupper_l(c,*lc_);
    }
    char const *toupper(char *begin,char const *end) const
    {
        for(;begin!=end;begin++)
            *begin = toupper_l(*begin,*lc_);
        return begin;
    }
    char tolower(char c) const
    {
        return tolower_l(c,*lc_);
    }
    char const *tolower(char *begin,char const *end) const
    {
        for(;begin!=end;begin++)
            *begin = tolower_l(*begin,*lc_);
        return begin;
    }
private:
    boost::shared_ptr<locale_t> lc_;
};

template<>
class ctype_posix<wchar_t> : public std::ctype<wchar_t> {
public:
    ctype_posix(boost::shared_ptr<locale_t> lc) 
    {
        lc_ = lc;
    }
   
    bool do_is(mask m,wchar_t c) const
    {
        if((m & space) && iswspace_l(c,*lc_))
            return true;
        if((m & print) && iswprint_l(c,*lc_))
            return true;
        if((m & cntrl) && iswcntrl_l(c,*lc_))
            return true;
        if((m & upper) && iswupper_l(c,*lc_))
            return true;
        if((m & lower) && iswlower_l(c,*lc_))
            return true;
        if((m & alpha) && iswalpha_l(c,*lc_))
            return true;
        if((m & digit) && iswdigit_l(c,*lc_))
            return true;
        if((m & xdigit) && iswxdigit_l(c,*lc_))
            return true;
        if((m & punct) && iswpunct_l(c,*lc_))
            return true;
        return false;
    }
    wchar_t const *do_is(wchar_t const *begin,wchar_t const *end,mask *m) const
    {
        while(begin!=end) {
            wchar_t c= *begin++;
            int r=0;
            if(iswspace_l(c,*lc_))
                r|=space;
            if(iswprint_l(c,*lc_))
                r|=cntrl;
            if(iswcntrl_l(c,*lc_))
                r|=space;
            if(iswupper_l(c,*lc_))
                r|=upper;
            if(iswlower_l(c,*lc_))
                r|=lower;
            if(iswalpha_l(c,*lc_))
                r|=alpha;
            if(iswdigit_l(c,*lc_))
                r|=digit;
            if(iswxdigit_l(c,*lc_))
                r|=xdigit;
            if(iswpunct_l(c,*lc_))
                r|=punct;
            // r actually should be mask, but some standard
            // libraries (like STLPort)
            // do not define operator | properly so using int+cast
            *m++ = static_cast<mask>(r);
        }
        return begin;
    }
    wchar_t const *do_scan_is(mask m,wchar_t const *begin,wchar_t const *end) const
    {
        while(begin!=end)
            if(do_is(m,*begin))
                return begin;
        return begin;
    }
    wchar_t const *do_scan_not(mask m,wchar_t const *begin,wchar_t const *end) const
    {
        while(begin!=end)
            if(!do_is(m,*begin))
                return begin;
        return begin;
    }
    wchar_t toupper(wchar_t c) const
    {
        return towupper_l(c,*lc_);
    }
    wchar_t const *toupper(wchar_t *begin,wchar_t const *end) const
    {
        for(;begin!=end;begin++)
            *begin = towupper_l(*begin,*lc_);
        return begin;
    }
    wchar_t tolower(wchar_t c) const
    {
        return tolower_l(c,*lc_);
    }
    wchar_t const *tolower(wchar_t *begin,wchar_t const *end) const
    {
        for(;begin!=end;begin++)
            *begin = tolower_l(*begin,*lc_);
        return begin;
    }
private:
    boost::shared_ptr<locale_t> lc_;
};




struct basic_numpunct {
    std::string grouping;
    std::string thousands_sep;
    std::string decimal_point;
    basic_numpunct() : 
        decimal_point(".")
    {
    }
    basic_numpunct(locale_t lc) 
    {
    #if defined(__APPLE__) || defined(__FreeBSD__)
        lconv *cv = localeconv_l(lc);
        grouping = cv->grouping;
        thousands_sep = cv->thousands_sep;
        decimal_point = cv->decimal_point;
    #else
        thousands_sep = nl_langinfo_l(THOUSEP,lc);
        decimal_point = nl_langinfo_l(RADIXCHAR,lc);
        #ifdef GROUPING
        grouping = nl_langinfo_l(GROUPING,lc);
        #endif
    #endif
    }
};

template<typename CharType>
class num_punct_posix : public std::numpunct<CharType> {
public:
    typedef std::basic_string<CharType> string_type;
    num_punct_posix(locale_t lc,size_t refs = 0) : 
        std::numpunct<CharType>(refs)
    {
        basic_numpunct np(lc);
        to_str(np.thousands_sep,thousands_sep_,lc);
        to_str(np.decimal_point,decimal_point_,lc);
        grouping_ = np.grouping;
        if(thousands_sep_.size() > 1)
            grouping_ = std::string();
        if(decimal_point_.size() > 1)
            decimal_point_ = CharType('.');
    }
    void to_str(std::string &s1,std::string &s2,locale_t /*lc*/)
    {
        s2.swap(s1);
    }
    void to_str(std::string &s1,std::wstring &s2,locale_t lc)
    {
        s2=conv::to_utf<wchar_t>(s1,nl_langinfo_l(CODESET,lc));
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
std::locale create_formatting_impl(std::locale const &in,boost::shared_ptr<locale_t> lc)
{
    std::locale tmp = std::locale(in,new num_punct_posix<CharType>(*lc));
    tmp = std::locale(tmp,new ctype_posix<CharType>(lc));
    tmp = std::locale(tmp,new time_put_posix<CharType>(lc));
    tmp = std::locale(tmp,new num_format<CharType>(lc));
    return tmp;
}

template<typename CharType>
std::locale create_parsing_impl(std::locale const &in,boost::shared_ptr<locale_t> lc)
{
    std::locale tmp = std::locale(in,new num_punct_posix<CharType>(*lc));
    tmp = std::locale(tmp,new ctype_posix<CharType>(lc));
    tmp = std::locale(tmp,new util::base_num_parse<CharType>());
    return tmp;
}


std::locale create_formatting(  std::locale const &in,
                                boost::shared_ptr<locale_t> lc,
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
                            boost::shared_ptr<locale_t> lc,
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

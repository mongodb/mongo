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
#include <limits>
#include "formatter.hpp"
#include <boost/locale/formatting.hpp>
#include <boost/locale/hold_ptr.hpp>
#include "all_generator.hpp"
#include "cdata.hpp"
#include <algorithm>
#include "predefined_formatters.hpp"

namespace boost {
namespace locale {
namespace impl_icu {

namespace details {
    template<typename V,int n=std::numeric_limits<V>::digits,bool integer=std::numeric_limits<V>::is_integer>
    struct cast_traits;
    
    template<typename v>
    struct cast_traits<v,7,true> {
        typedef int32_t cast_type;
    };
    template<typename v>
    struct cast_traits<v,8,true> {
        typedef int32_t cast_type;
    };

    template<typename v>
    struct cast_traits<v,15,true> {
        typedef int32_t cast_type;
    };
    template<typename v>
    struct cast_traits<v,16,true> {
        typedef int32_t cast_type;
    };
    template<typename v>
    struct cast_traits<v,31,true> {
        typedef int32_t cast_type;
    };
    template<typename v>
    struct cast_traits<v,32,true> {
        typedef int64_t cast_type;
    };
    template<typename v>
    struct cast_traits<v,63,true> {
        typedef int64_t cast_type;
    };
    template<typename v>
    struct cast_traits<v,64,true> {
        typedef int64_t cast_type;
    };
    template<typename V,int u>
    struct cast_traits<V,u,false> {
        typedef double cast_type;
    };

    // ICU does not support uint64_t values so fallback
    // to POSIX formatting
    template<   typename V,
                bool Sig=std::numeric_limits<V>::is_signed,
                bool Int=std::numeric_limits<V>::is_integer,
                bool Big=(sizeof(V) >= 8)
            >
    struct use_parent_traits 
    {
        static bool use(V /*v*/) { return false; }
    };
    template<typename V>
    struct use_parent_traits<V,false,true,true>
    {
        static bool use(V v) { return static_cast<int64_t>(v) < 0; }
    };

}



class num_base {
protected:

    template<typename ValueType>
    static bool use_parent(std::ios_base &ios,ValueType v)
    {
        uint64_t flg = ios_info::get(ios).display_flags();
        if(flg == flags::posix)
            return true;
        if(details::use_parent_traits<ValueType>::use(v))
            return true;

        if(!std::numeric_limits<ValueType>::is_integer)
            return false;

        if(flg == flags::number && (ios.flags() & std::ios_base::basefield) != std::ios_base::dec) {
            return true;
        }
        return false;
    }
};


template<typename CharType>
class num_format : public std::num_put<CharType>, protected num_base
{
public:
    typedef typename std::num_put<CharType>::iter_type iter_type;
    typedef std::basic_string<CharType> string_type;
    typedef CharType char_type;
    typedef formatter<CharType> formatter_type;
    typedef hold_ptr<formatter_type> formatter_ptr;

    num_format(cdata const &d,size_t refs = 0) : 
        std::num_put<CharType>(refs),
        loc_(d.locale),
        enc_(d.encoding)
    {
    }
protected: 
    

    virtual iter_type do_put (iter_type out, std::ios_base &ios, char_type fill, long val) const
    {
        return do_real_put(out,ios,fill,val);
    }
    virtual iter_type do_put (iter_type out, std::ios_base &ios, char_type fill, unsigned long val) const
    {
        return do_real_put(out,ios,fill,val);
    }
    virtual iter_type do_put (iter_type out, std::ios_base &ios, char_type fill, double val) const
    {
        return do_real_put(out,ios,fill,val);
    }
    virtual iter_type do_put (iter_type out, std::ios_base &ios, char_type fill, long double val) const
    {
        return do_real_put(out,ios,fill,val);
    }
    
    #ifndef BOOST_NO_LONG_LONG 
    virtual iter_type do_put (iter_type out, std::ios_base &ios, char_type fill, long long val) const
    {
        return do_real_put(out,ios,fill,val);
    }
    virtual iter_type do_put (iter_type out, std::ios_base &ios, char_type fill, unsigned long long val) const
    {
        return do_real_put(out,ios,fill,val);
    }
    #endif


private:



    template<typename ValueType>
    iter_type do_real_put (iter_type out, std::ios_base &ios, char_type fill, ValueType val) const
    {
        if(use_parent<ValueType>(ios,val))
            return std::num_put<char_type>::do_put(out,ios,fill,val);

        formatter_ptr formatter(formatter_type::create(ios,loc_,enc_));

        if(formatter.get() == 0) 
            return std::num_put<char_type>::do_put(out,ios,fill,val);
        
        size_t code_points;
        typedef typename details::cast_traits<ValueType>::cast_type cast_type;
        string_type const &str = formatter->format(static_cast<cast_type>(val),code_points);
        std::streamsize on_left=0,on_right = 0,points = code_points;
        if(points < ios.width()) {
            std::streamsize n = ios.width() - points;
            
            std::ios_base::fmtflags flags = ios.flags() & std::ios_base::adjustfield;
            
            //
            // We do not really know internal point, so we assume that it does not
            // exist. So according to the standard field should be right aligned
            //
            if(flags != std::ios_base::left)
                on_left = n;
            on_right = n - on_left;
        }
        while(on_left > 0) {
            *out++ = fill;
            on_left--;
        }
        std::copy(str.begin(),str.end(),out);
        while(on_right > 0) {
            *out++ = fill;
            on_right--;
        }
        ios.width(0);
        return out;

    }

    icu::Locale loc_;
    std::string enc_;

};  /// num_format


template<typename CharType>
class num_parse : public std::num_get<CharType>, protected num_base
{
public:
    num_parse(cdata const &d,size_t refs = 0) : 
        std::num_get<CharType>(refs),
        loc_(d.locale),
        enc_(d.encoding)
    {
    }
protected: 
    typedef typename std::num_get<CharType>::iter_type iter_type;
    typedef std::basic_string<CharType> string_type;
    typedef CharType char_type;
    typedef formatter<CharType> formatter_type;
    typedef hold_ptr<formatter_type> formatter_ptr;
    typedef std::basic_istream<CharType> stream_type;

    virtual iter_type do_get(iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,long &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    virtual iter_type do_get(iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,unsigned short &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    virtual iter_type do_get(iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,unsigned int &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    virtual iter_type do_get(iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,unsigned long &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    virtual iter_type do_get(iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,float &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    virtual iter_type do_get(iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,double &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    virtual iter_type do_get (iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,long double &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    #ifndef BOOST_NO_LONG_LONG 
    virtual iter_type do_get (iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,long long &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    virtual iter_type do_get (iter_type in, iter_type end, std::ios_base &ios,std::ios_base::iostate &err,unsigned long long &val) const
    {
        return do_real_get(in,end,ios,err,val);
    }

    #endif

private:
    

    //
    // This is not really an efficient solution, but it works
    //
    template<typename ValueType>
    iter_type do_real_get(iter_type in,iter_type end,std::ios_base &ios,std::ios_base::iostate &err,ValueType &val) const
    {
        stream_type *stream_ptr = dynamic_cast<stream_type *>(&ios);
        if(!stream_ptr || use_parent<ValueType>(ios,0)) {
            return std::num_get<CharType>::do_get(in,end,ios,err,val);
        }

        formatter_ptr formatter(formatter_type::create(ios,loc_,enc_));
        if(formatter.get()==0) {
            return std::num_get<CharType>::do_get(in,end,ios,err,val);
        }

        typedef typename details::cast_traits<ValueType>::cast_type cast_type;
        string_type tmp;
        tmp.reserve(64);

        CharType c;
        while(in!=end && (((c=*in)<=32 && (c>0)) || c==127)) // Assuming that ASCII is a subset
            ++in;

        while(tmp.size() < 4096 && in!=end && *in!='\n') {
            tmp += *in++;
        }

        cast_type value;
        size_t parsed_chars;

        if((parsed_chars = formatter->parse(tmp,value))==0 || !valid<ValueType>(value)) {
            err |= std::ios_base::failbit;
        }
        else {
            val=static_cast<ValueType>(value);
        }

        for(size_t n=tmp.size();n>parsed_chars;n--) {
            stream_ptr->putback(tmp[n-1]);
        }

        in = iter_type(*stream_ptr);

        if(in==end)
            err |=std::ios_base::eofbit;
        return in;
    }

    template<typename ValueType,typename CastedType>
    bool valid(CastedType v) const
    {
        typedef std::numeric_limits<ValueType> value_limits;
        typedef std::numeric_limits<CastedType> casted_limits;
        if(v < 0 && value_limits::is_signed == false)
            return false;
        
        static const CastedType max_val = value_limits::max();

        if(sizeof(CastedType) > sizeof(ValueType) && v > max_val)
            return false;

        if(value_limits::is_integer == casted_limits::is_integer) {
            return true;
        }
        if(value_limits::is_integer) { // and casted is not
            if(static_cast<CastedType>(static_cast<ValueType>(v))!=v)
                return false;
        }
        return true;
    }
    
    icu::Locale loc_;
    std::string enc_;

};


template<typename CharType>
std::locale install_formatting_facets(std::locale const &in,cdata const &cd)
{
    std::locale tmp=std::locale(in,new num_format<CharType>(cd));
    if(!std::has_facet<icu_formatters_cache>(in)) {
        tmp=std::locale(tmp,new icu_formatters_cache(cd.locale)); 
    }
    return tmp;
}

template<typename CharType>
std::locale install_parsing_facets(std::locale const &in,cdata const &cd)
{
    std::locale tmp=std::locale(in,new num_parse<CharType>(cd));
    if(!std::has_facet<icu_formatters_cache>(in)) {
        tmp=std::locale(tmp,new icu_formatters_cache(cd.locale)); 
    }
    return tmp;
}

std::locale create_formatting(std::locale const &in,cdata const &cd,character_facet_type type)
{
        switch(type) {
        case char_facet:
            return install_formatting_facets<char>(in,cd);
        case wchar_t_facet:
            return install_formatting_facets<wchar_t>(in,cd);
        #ifdef BOOST_LOCALE_ENABLE_CHAR16_T
        case char16_t_facet:
            return install_formatting_facets<char16_t>(in,cd);
        #endif
        #ifdef BOOST_LOCALE_ENABLE_CHAR32_T
        case char32_t_facet:
            return install_formatting_facets<char32_t>(in,cd);
        #endif
        default:
            return in;
        }
}

std::locale create_parsing(std::locale const &in,cdata const &cd,character_facet_type type)
{
        switch(type) {
        case char_facet:
            return install_parsing_facets<char>(in,cd);
        case wchar_t_facet:
            return install_parsing_facets<wchar_t>(in,cd);
        #ifdef BOOST_LOCALE_ENABLE_CHAR16_T
        case char16_t_facet:
            return install_parsing_facets<char16_t>(in,cd);
        #endif
        #ifdef BOOST_LOCALE_ENABLE_CHAR32_T
        case char32_t_facet:
            return install_parsing_facets<char32_t>(in,cd);
        #endif
        default:
            return in;
        }
}


} // impl_icu

} // locale 
} //boost



// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

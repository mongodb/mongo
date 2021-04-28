//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/locale/generator.hpp>
#include <boost/locale/encoding.hpp>
#include <boost/locale/utf8_codecvt.hpp>

#include "../encoding/conv.hpp"

#include <boost/locale/util.hpp>

#ifdef BOOST_MSVC
#  pragma warning(disable : 4244 4996) // loose data 
#endif

#include <cstddef>
#include <string.h>
#include <vector>
#include <algorithm>

//#define DEBUG_CODECVT

#ifdef DEBUG_CODECVT            
#include <iostream>
#endif

namespace boost {
namespace locale {
namespace util {
    
    class utf8_converter  : public base_converter {
    public:
        virtual int max_len() const
        {
            return 4;
        }

        virtual utf8_converter *clone() const
        {
            return new utf8_converter();
        }

        bool is_thread_safe() const
        {
            return true;
        }

        virtual uint32_t to_unicode(char const *&begin,char const *end)
        {
            char const *p=begin;
                        
            utf::code_point c = utf::utf_traits<char>::decode(p,end);

            if(c==utf::illegal)
                return illegal;

            if(c==utf::incomplete)
                return incomplete;

            begin = p;
            return c;
        }

        virtual uint32_t from_unicode(uint32_t u,char *begin,char const *end) 
        {
            if(!utf::is_valid_codepoint(u))
                return illegal;
            int width = utf::utf_traits<char>::width(u);
            std::ptrdiff_t d=end-begin;
            if(d < width)
                return incomplete;
            utf::utf_traits<char>::encode(u,begin);
            return width;
        }
    }; // utf8_converter

    class simple_converter_impl {
    public:
        
        static const int hash_table_size = 1024;

        simple_converter_impl(std::string const &encoding)
        {
            for(unsigned i=0;i<128;i++)
                to_unicode_tbl_[i]=i;
            for(unsigned i=128;i<256;i++) {
                char buf[2] = { char(i) , 0 };
                uint32_t uchar=utf::illegal;
                try {
                    std::wstring const tmp = conv::to_utf<wchar_t>(buf,buf+1,encoding,conv::stop);
                    if(tmp.size() == 1) {
                        uchar = tmp[0];
                    }
                    else {
                        uchar = utf::illegal;
                    }
                }
                catch(conv::conversion_error const &/*e*/) {
                    uchar = utf::illegal;
                }
                to_unicode_tbl_[i]=uchar;
            }
            for(int i=0;i<hash_table_size;i++)
                from_unicode_tbl_[i]=0;
            for(unsigned i=1;i<256;i++) {
                if(to_unicode_tbl_[i]!=utf::illegal) {
                    unsigned pos = to_unicode_tbl_[i] % hash_table_size;
                    while(from_unicode_tbl_[pos]!=0)
                        pos = (pos + 1) % hash_table_size;
                    from_unicode_tbl_[pos] = i;
                }
            }
        }

        uint32_t to_unicode(char const *&begin,char const *end) const
        {
            if(begin==end)
                return utf::incomplete;
            unsigned char c = *begin++;
            return to_unicode_tbl_[c];
        }
        uint32_t from_unicode(uint32_t u,char *begin,char const *end) const
        {
            if(begin==end)
                return utf::incomplete;
            if(u==0) {
                *begin = 0;
                return 1;
            }
            unsigned pos = u % hash_table_size;
            unsigned char c;
            while((c=from_unicode_tbl_[pos])!=0 && to_unicode_tbl_[c]!=u)
                pos = (pos + 1) % hash_table_size;
            if(c==0)
               return utf::illegal;
            *begin = c;
            return 1;
        }
    private:
        uint32_t to_unicode_tbl_[256];
        unsigned char from_unicode_tbl_[hash_table_size];
    };

    class simple_converter : public base_converter {
    public:

        virtual ~simple_converter() 
        {
        }

        simple_converter(std::string const &encoding) : 
            cvt_(encoding)
        {
        }

        virtual int max_len() const 
        {
            return 1;
        }

        virtual bool is_thread_safe() const 
        {
            return true;
        }
        virtual base_converter *clone() const 
        {
           return new simple_converter(*this); 
        }

        virtual uint32_t to_unicode(char const *&begin,char const *end)
        {
            return cvt_.to_unicode(begin,end);
        }
        virtual uint32_t from_unicode(uint32_t u,char *begin,char const *end)
        {
            return cvt_.from_unicode(u,begin,end);
        }
    private:
        simple_converter_impl cvt_;
    };

    template<typename CharType>
    class simple_codecvt : public generic_codecvt<CharType,simple_codecvt<CharType> >
    {
    public:

        simple_codecvt(std::string const &encoding,size_t refs = 0) :
            generic_codecvt<CharType,simple_codecvt<CharType> >(refs),
            cvt_(encoding)
        {
        }

        struct state_type {};
        static state_type initial_state(generic_codecvt_base::initial_convertion_state /* unused */) 
        {
            return state_type();
        }
        static int max_encoding_length()
        {
            return 1;
        }

        utf::code_point to_unicode(state_type &,char const *&begin,char const *end) const 
        {
            return cvt_.to_unicode(begin,end);
        }

        utf::code_point from_unicode(state_type &,utf::code_point u,char *begin,char const *end) const
        {
            return cvt_.from_unicode(u,begin,end);
        }
    private:
        simple_converter_impl cvt_;        
         
    };

    namespace {
        char const *simple_encoding_table[] = {
            "cp1250",
            "cp1251",
            "cp1252",
            "cp1253",
            "cp1254",
            "cp1255",
            "cp1256",
            "cp1257",
            "iso88591",
            "iso885913",
            "iso885915",
            "iso88592",
            "iso88593",
            "iso88594",
            "iso88595",
            "iso88596",
            "iso88597",
            "iso88598",
            "iso88599",
            "koi8r",
            "koi8u",
            "usascii",
            "windows1250",
            "windows1251",
            "windows1252",
            "windows1253",
            "windows1254",
            "windows1255",
            "windows1256",
            "windows1257"
        };

        bool compare_strings(char const *l,char const *r)
        {
            return strcmp(l,r) < 0;
        }
    }

    bool check_is_simple_encoding(std::string const &encoding)
    {
        std::string norm = conv::impl::normalize_encoding(encoding.c_str());
        return std::binary_search<char const **>( simple_encoding_table,
                        simple_encoding_table + sizeof(simple_encoding_table)/sizeof(char const *),
                        norm.c_str(),
                        compare_strings);
        return 0;
    }
    
    #if !defined(BOOST_LOCALE_HIDE_AUTO_PTR) && !defined(BOOST_NO_AUTO_PTR)
    std::auto_ptr<base_converter> create_utf8_converter()
    {
        std::auto_ptr<base_converter> res(create_utf8_converter_new_ptr());
        return res;
    }
    std::auto_ptr<base_converter> create_simple_converter(std::string const &encoding)
    {
        std::auto_ptr<base_converter> res(create_simple_converter_new_ptr(encoding));
        return res;
    }
    std::locale create_codecvt(std::locale const &in,std::auto_ptr<base_converter> cvt,character_facet_type type)
    {
        return create_codecvt_from_pointer(in,cvt.release(),type);
    }
    #endif
    #ifndef BOOST_NO_CXX11_SMART_PTR
    std::unique_ptr<base_converter> create_utf8_converter_unique_ptr()
    {
        std::unique_ptr<base_converter> res(create_utf8_converter_new_ptr());
        return res;
    }
    std::unique_ptr<base_converter> create_simple_converter_unique_ptr(std::string const &encoding)
    {
        std::unique_ptr<base_converter> res(create_simple_converter_new_ptr(encoding));
        return res;
    }
    std::locale create_codecvt(std::locale const &in,std::unique_ptr<base_converter> cvt,character_facet_type type)
    {
        return create_codecvt_from_pointer(in,cvt.release(),type);
    }
    #endif

    base_converter *create_simple_converter_new_ptr(std::string const &encoding)
    {
        if(check_is_simple_encoding(encoding))
            return new simple_converter(encoding);
        return 0;
    }

    base_converter *create_utf8_converter_new_ptr()
    {
        return new utf8_converter();
    }
    
    template<typename CharType>
    class code_converter : public generic_codecvt<CharType,code_converter<CharType> >
    {
    public:
        #ifndef BOOST_NO_CXX11_SMART_PTR
        typedef std::unique_ptr<base_converter> base_converter_ptr;
        #define PTR_TRANS(x) std::move((x))
        #else
        typedef std::auto_ptr<base_converter> base_converter_ptr;
        #define PTR_TRANS(x) (x)
        #endif
        typedef base_converter_ptr state_type;
        
        code_converter(base_converter_ptr cvt,size_t refs = 0) : 
            generic_codecvt<CharType,code_converter<CharType> >(refs),
            cvt_(PTR_TRANS(cvt))
        {
            max_len_ = cvt_->max_len();
            thread_safe_ = cvt_->is_thread_safe();
        }


        int max_encoding_length() const
        {
            return max_len_;
        }

        base_converter_ptr initial_state(generic_codecvt_base::initial_convertion_state /* unused */) const
        {
            base_converter_ptr r;
            if(!thread_safe_)
                r.reset(cvt_->clone());
            return r;
        }

        utf::code_point to_unicode(base_converter_ptr &ptr,char const *&begin,char const *end) const 
        {
            if(thread_safe_)
                return cvt_->to_unicode(begin,end);
            else
                return ptr->to_unicode(begin,end);
        }

        utf::code_point from_unicode(base_converter_ptr &ptr,utf::code_point u,char *begin,char const *end) const
        {
            if(thread_safe_)
                return cvt_->from_unicode(u,begin,end);
            else
                return ptr->from_unicode(u,begin,end);
        }
        
    private:
        base_converter_ptr cvt_;
        int max_len_;
        bool thread_safe_;
    };


    std::locale create_codecvt_from_pointer(std::locale const &in,base_converter *pcvt,character_facet_type type)
    {
        code_converter<char>::base_converter_ptr cvt(pcvt);
        if(!cvt.get())
            cvt.reset(new base_converter());
        switch(type) {
        case char_facet:
            return std::locale(in,new code_converter<char>(PTR_TRANS(cvt)));
        case wchar_t_facet:
            return std::locale(in,new code_converter<wchar_t>(PTR_TRANS(cvt)));
        #if defined(BOOST_LOCALE_ENABLE_CHAR16_T) && !defined(BOOST_NO_CHAR16_T_CODECVT)
        case char16_t_facet:
            return std::locale(in,new code_converter<char16_t>(PTR_TRANS(cvt)));
        #endif
        #if defined(BOOST_LOCALE_ENABLE_CHAR32_T) && !defined(BOOST_NO_CHAR32_T_CODECVT)
        case char32_t_facet:
            return std::locale(in,new code_converter<char32_t>(PTR_TRANS(cvt)));
        #endif
        default:
            return in;
        }
    }


    /// 
    /// Install utf8 codecvt to UTF-16 or UTF-32 into locale \a in and return
    /// new locale that is based on \a in and uses new facet. 
    /// 
    std::locale create_utf8_codecvt(std::locale const &in,character_facet_type type)
    {
        switch(type) {
        case char_facet:
            return std::locale(in,new utf8_codecvt<char>());
        case wchar_t_facet:
            return std::locale(in,new utf8_codecvt<wchar_t>());
        #if defined(BOOST_LOCALE_ENABLE_CHAR16_T) && !defined(BOOST_NO_CHAR16_T_CODECVT)
        case char16_t_facet:
            return std::locale(in,new utf8_codecvt<char16_t>());
        #endif
        #if defined(BOOST_LOCALE_ENABLE_CHAR32_T) && !defined(BOOST_NO_CHAR32_T_CODECVT)
        case char32_t_facet:
            return std::locale(in,new utf8_codecvt<char32_t>());
        #endif
        default:
            return in;
        }
    }

    ///
    /// This function installs codecvt that can be used for conversion between single byte
    /// character encodings like ISO-8859-1, koi8-r, windows-1255 and Unicode code points,
    /// 
    /// Throws invalid_charset_error if the chacater set is not supported or isn't single byte character
    /// set
    std::locale create_simple_codecvt(std::locale const &in,std::string const &encoding,character_facet_type type)
    {
        if(!check_is_simple_encoding(encoding))
            throw boost::locale::conv::invalid_charset_error("Invalid simple encoding " + encoding);

        switch(type) {
        case char_facet:
            return std::locale(in,new simple_codecvt<char>(encoding));
        case wchar_t_facet:
            return std::locale(in,new simple_codecvt<wchar_t>(encoding));
        #if defined(BOOST_LOCALE_ENABLE_CHAR16_T) && !defined(BOOST_NO_CHAR16_T_CODECVT)
        case char16_t_facet:
            return std::locale(in,new simple_codecvt<char16_t>(encoding));
        #endif
        #if defined(BOOST_LOCALE_ENABLE_CHAR32_T) && !defined(BOOST_NO_CHAR32_T_CODECVT)
        case char32_t_facet:
            return std::locale(in,new simple_codecvt<char32_t>(encoding));
        #endif
        default:
            return in;
        }
    }



} // util
} // locale 
} // boost

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_LOCALE_IMPL_WCONV_CODEPAGE_HPP
#define BOOST_LOCALE_IMPL_WCONV_CODEPAGE_HPP


#include <boost/locale/encoding.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include "conv.hpp"

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>
#include <vector>


namespace boost {
namespace locale {
namespace conv {
namespace impl {
    
    struct windows_encoding {
        char const *name;
        unsigned codepage;
        unsigned was_tested;
    };

    bool operator<(windows_encoding const &l,windows_encoding const &r)
    {
        return strcmp(l.name,r.name) < 0;
    }

    windows_encoding all_windows_encodings[] = {
        { "big5",       950, 0 },
        { "cp1250",     1250, 0 },
        { "cp1251",     1251, 0 },
        { "cp1252",     1252, 0 },
        { "cp1253",     1253, 0 },
        { "cp1254",     1254, 0 },
        { "cp1255",     1255, 0 },
        { "cp1256",     1256, 0 },
        { "cp1257",     1257, 0 },
        { "cp874",      874, 0 },
        { "cp932",      932, 0 },
        { "cp936",      936, 0 },
        { "eucjp",      20932, 0 },
        { "euckr",      51949, 0 },
        { "gb18030",    54936, 0 },
        { "gb2312",     20936, 0 },
        { "gbk",        936, 0 },
        { "iso2022jp",  50220, 0 },
        { "iso2022kr",  50225, 0 },
        { "iso88591",   28591, 0 },
        { "iso885913",  28603, 0 },
        { "iso885915",  28605, 0 },
        { "iso88592",   28592, 0 },
        { "iso88593",   28593, 0 },
        { "iso88594",   28594, 0 },
        { "iso88595",   28595, 0 },
        { "iso88596",   28596, 0 },
        { "iso88597",   28597, 0 },
        { "iso88598",   28598, 0 },
        { "iso88599",   28599, 0 },
        { "koi8r",      20866, 0 },
        { "koi8u",      21866, 0 },
        { "ms936",      936, 0 },
        { "shiftjis",   932, 0 },
        { "sjis",       932, 0 },
        { "usascii",    20127, 0 },
        { "utf8",       65001, 0 },
        { "windows1250",        1250, 0 },
        { "windows1251",        1251, 0 },
        { "windows1252",        1252, 0 },
        { "windows1253",        1253, 0 },
        { "windows1254",        1254, 0 },
        { "windows1255",        1255, 0 },
        { "windows1256",        1256, 0 },
        { "windows1257",        1257, 0 },
        { "windows874",         874, 0 },
        { "windows932",         932, 0 },
        { "windows936",         936, 0 },
    };

    size_t remove_substitutions(std::vector<char> &v)
    {
        if(std::find(v.begin(),v.end(),0) == v.end()) {
            return v.size();
        }
        std::vector<char> v2;
        v2.reserve(v.size());
        for(unsigned i=0;i<v.size();i++) {
            if(v[i]!=0)
                v2.push_back(v[i]);
        }
        v.swap(v2);
        return v.size();
    }

    void multibyte_to_wide_one_by_one(int codepage,char const *begin,char const *end,std::vector<wchar_t> &buf)
    {
        buf.reserve(end-begin);
        while(begin!=end) {
            wchar_t wide_buf[4];
            int n = 0;
            int len = IsDBCSLeadByteEx(codepage,*begin) ? 2 : 1;
            if(len == 2 && begin+1==end)
                return;
            n = MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,begin,len,wide_buf,4);
            for(int i=0;i<n;i++) 
                buf.push_back(wide_buf[i]);
            begin+=len;
        }
    }

    
    void multibyte_to_wide(int codepage,char const *begin,char const *end,bool do_skip,std::vector<wchar_t> &buf)
    {
        if(begin==end)
            return;
        int n = MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,begin,end-begin,0,0);
        if(n == 0) {
            if(do_skip) {
                multibyte_to_wide_one_by_one(codepage,begin,end,buf);
                return;
            }
            throw conversion_error();
        }

        buf.resize(n,0);
        if(MultiByteToWideChar(codepage,MB_ERR_INVALID_CHARS,begin,end-begin,&buf.front(),buf.size())==0)
            throw conversion_error();
    }

    void wide_to_multibyte_non_zero(int codepage,wchar_t const *begin,wchar_t const *end,bool do_skip,std::vector<char> &buf)
    {
        if(begin==end)
            return;
        BOOL substitute = FALSE;
        BOOL *substitute_ptr = codepage == 65001 || codepage == 65000 ? 0 : &substitute;
        char subst_char = 0;
        char *subst_char_ptr = codepage == 65001 || codepage == 65000 ? 0 : &subst_char;
        
        int n = WideCharToMultiByte(codepage,0,begin,end-begin,0,0,subst_char_ptr,substitute_ptr);
        buf.resize(n);
        
        if(WideCharToMultiByte(codepage,0,begin,end-begin,&buf[0],n,subst_char_ptr,substitute_ptr)==0)
            throw conversion_error();
        if(substitute) {
            if(do_skip) 
                remove_substitutions(buf);
            else 
                throw conversion_error();
        }
    }
    
    void wide_to_multibyte(int codepage,wchar_t const *begin,wchar_t const *end,bool do_skip,std::vector<char> &buf)
    {
        if(begin==end)
            return;
        buf.reserve(end-begin);
        wchar_t const *e = std::find(begin,end,L'\0');
        wchar_t const *b = begin;
        for(;;) {
            std::vector<char> tmp;
            wide_to_multibyte_non_zero(codepage,b,e,do_skip,tmp);
            size_t osize = buf.size();
            buf.resize(osize+tmp.size());
            std::copy(tmp.begin(),tmp.end(),buf.begin()+osize);
            if(e!=end) {
                buf.push_back('\0');
                b=e+1;
                e=std::find(b,end,L'0');
            }
            else 
                break;
        }
    }

    
    int encoding_to_windows_codepage(char const *ccharset)
    {
        std::string charset = normalize_encoding(ccharset);
        windows_encoding ref;
        ref.name = charset.c_str();
        size_t n = sizeof(all_windows_encodings)/sizeof(all_windows_encodings[0]);
        windows_encoding *begin = all_windows_encodings;
        windows_encoding *end = all_windows_encodings + n;
        windows_encoding *ptr = std::lower_bound(begin,end,ref);
        if(ptr!=end && strcmp(ptr->name,charset.c_str())==0) {
            if(ptr->was_tested) {
                return ptr->codepage;
            }
            else if(IsValidCodePage(ptr->codepage)) {
                // the thread safety is not an issue, maximum
                // it would be checked more then once
                ptr->was_tested=1;
                return ptr->codepage;
            }
            else {
                return -1;
            }
        }
        return -1;
        
    }

    template<typename CharType>
    bool validate_utf16(CharType const *str,unsigned len)
    {
        CharType const *begin = str;
        CharType const *end = str+len;
        while(begin!=end) {
            utf::code_point c = utf::utf_traits<CharType,2>::template decode<CharType const *>(begin,end);
            if(c==utf::illegal || c==utf::incomplete)
                return false;
        }
        return true;
    }

    template<typename CharType,typename OutChar>
    void clean_invalid_utf16(CharType const *str,unsigned len,std::vector<OutChar> &out)
    {
        out.reserve(len);
        for(unsigned i=0;i<len;i++) {
            uint16_t c = static_cast<uint16_t>(str[i]);

            if(0xD800 <= c && c<= 0xDBFF) {
                i++;
                if(i>=len)
                    return;
                uint16_t c2=static_cast<uint16_t>(str[i]);
                if(0xDC00 <= c2 && c2 <= 0xDFFF) {
                    out.push_back(static_cast<OutChar>(c));
                    out.push_back(static_cast<OutChar>(c2));
                }
            }
            else if(0xDC00 <= c && c <=0xDFFF)
                continue;
            else
                out.push_back(static_cast<OutChar>(c));
        }
    }


    class wconv_between : public converter_between {
    public:
        wconv_between() : 
            how_(skip),
            to_code_page_ (-1),
            from_code_page_ ( -1)
        {
        }
        bool open(char const *to_charset,char const *from_charset,method_type how)
        {
            how_ = how;
            to_code_page_ = encoding_to_windows_codepage(to_charset);
            from_code_page_ = encoding_to_windows_codepage(from_charset);
            if(to_code_page_ == -1 || from_code_page_ == -1)
                return false;
            return true;
        }
        virtual std::string convert(char const *begin,char const *end)
        {
            if(to_code_page_ == 65001 && from_code_page_ == 65001)
                return utf_to_utf<char>(begin,end,how_);

            std::string res;
            
            std::vector<wchar_t> tmp;   // buffer for mb2w
            std::wstring tmps;          // buffer for utf_to_utf
            wchar_t const *wbegin=0;
            wchar_t const *wend=0;
            
            if(from_code_page_ == 65001) {
                tmps = utf_to_utf<wchar_t>(begin,end,how_);
                if(tmps.empty())
                    return res;
                wbegin = tmps.c_str();
                wend = wbegin + tmps.size();
            }
            else {
                multibyte_to_wide(from_code_page_,begin,end,how_ == skip,tmp);
                if(tmp.empty())
                    return res;
                wbegin = &tmp[0];
                wend = wbegin + tmp.size();
            }
            
            if(to_code_page_ == 65001) {
                return utf_to_utf<char>(wbegin,wend,how_);
            }

            std::vector<char> ctmp;
            wide_to_multibyte(to_code_page_,wbegin,wend,how_ == skip,ctmp);
            if(ctmp.empty())
                return res;
            res.assign(&ctmp.front(),ctmp.size());
            return res;
        }
    private:
        method_type how_;
        int to_code_page_;
        int from_code_page_;
    };
    
    template<typename CharType,int size = sizeof(CharType) >
    class wconv_to_utf;

    template<typename CharType,int size = sizeof(CharType) >
    class wconv_from_utf;

    template<>
    class wconv_to_utf<char,1> : public  converter_to_utf<char> , public wconv_between {
    public:
        virtual bool open(char const *cs,method_type how) 
        {
            return wconv_between::open("UTF-8",cs,how);
        }
        virtual std::string convert(char const *begin,char const *end)
        {
            return wconv_between::convert(begin,end);
        }
    };
    
    template<>
    class wconv_from_utf<char,1> : public  converter_from_utf<char> , public wconv_between {
    public:
        virtual bool open(char const *cs,method_type how) 
        {
            return wconv_between::open(cs,"UTF-8",how);
        }
        virtual std::string convert(char const *begin,char const *end)
        {
            return wconv_between::convert(begin,end);
        }
    };
    
    template<typename CharType>
    class wconv_to_utf<CharType,2> : public converter_to_utf<CharType> {
    public:
        typedef CharType char_type;

        typedef std::basic_string<char_type> string_type;

        wconv_to_utf() : 
            how_(skip),
            code_page_(-1)
        {
        }

        virtual bool open(char const *charset,method_type how)
        {
            how_ = how;
            code_page_ = encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        virtual string_type convert(char const *begin,char const *end) 
        {
            if(code_page_ == 65001) {
                return utf_to_utf<char_type>(begin,end,how_);
            }
            std::vector<wchar_t> tmp;
            multibyte_to_wide(code_page_,begin,end,how_ == skip,tmp);
            string_type res;
            if(!tmp.empty())
                res.assign(reinterpret_cast<char_type *>(&tmp.front()),tmp.size());
            return res;
        }

    private:
        method_type how_;
        int code_page_;
    };
  
    template<typename CharType>
    class wconv_from_utf<CharType,2> : public converter_from_utf<CharType> {
    public:
        typedef CharType char_type;

        typedef std::basic_string<char_type> string_type;

        wconv_from_utf() : 
            how_(skip),
            code_page_(-1)
        {
        }

        virtual bool open(char const *charset,method_type how)
        {
            how_ = how;
            code_page_ = encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        virtual std::string convert(CharType const *begin,CharType const *end) 
        {
            if(code_page_ == 65001) {
                return utf_to_utf<char>(begin,end,how_);
            }
            wchar_t const *wbegin = 0;
            wchar_t const *wend = 0;
            std::vector<wchar_t> buffer; // if needed
            if(begin==end)
                return std::string();
            if(validate_utf16(begin,end-begin)) {
                wbegin =  reinterpret_cast<wchar_t const *>(begin);
                wend = reinterpret_cast<wchar_t const *>(end);
            }
            else {
                if(how_ == stop) {
                        throw conversion_error();
                }
                else {
                    clean_invalid_utf16(begin,end-begin,buffer);
                    if(!buffer.empty()) {
                        wbegin = &buffer[0];
                        wend = wbegin + buffer.size();
                    }
                }
            }
            std::string res;
            if(wbegin==wend)
                return res;
            std::vector<char> ctmp;
            wide_to_multibyte(code_page_,wbegin,wend,how_ == skip,ctmp);
            if(ctmp.empty())
                return res;
            res.assign(&ctmp.front(),ctmp.size());
            return res;
        }

    private:
        method_type how_;
        int code_page_;
    };



    template<typename CharType>
    class wconv_to_utf<CharType,4> : public converter_to_utf<CharType> {
    public:
        typedef CharType char_type;

        typedef std::basic_string<char_type> string_type;

        wconv_to_utf() : 
            how_(skip),
            code_page_(-1)
        {
        }

        virtual bool open(char const *charset,method_type how)
        {
            how_ = how;
            code_page_ = encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        virtual string_type convert(char const *begin,char const *end) 
        {
            if(code_page_ == 65001) {
                return utf_to_utf<char_type>(begin,end,how_);
            }
            std::vector<wchar_t> buf;
            multibyte_to_wide(code_page_,begin,end,how_ == skip,buf);

            if(buf.empty())
                return string_type();

            return utf_to_utf<CharType>(&buf[0],&buf[0]+buf.size(),how_);
        }
    private:
        method_type how_;
        int code_page_;
    };
  
    template<typename CharType>
    class wconv_from_utf<CharType,4> : public converter_from_utf<CharType> {
    public:
        typedef CharType char_type;

        typedef std::basic_string<char_type> string_type;

        wconv_from_utf() : 
            how_(skip),
            code_page_(-1)
        {
        }

        virtual bool open(char const *charset,method_type how)
        {
            how_ = how;
            code_page_ = encoding_to_windows_codepage(charset);
            return code_page_ != -1;
        }

        virtual std::string convert(CharType const *begin,CharType const *end) 
        {
            if(code_page_ == 65001) {
                return utf_to_utf<char>(begin,end,how_);
            }
            std::wstring tmp = utf_to_utf<wchar_t>(begin,end,how_);

            std::vector<char> ctmp;
            wide_to_multibyte(code_page_,tmp.c_str(),tmp.c_str()+tmp.size(),how_ == skip,ctmp);
            std::string res;
            if(ctmp.empty())
                return res;
            res.assign(&ctmp.front(),ctmp.size());
            return res;

        }

    private:
        method_type how_;
        int code_page_;
    };





} // impl
} // conv
} // locale 
} // boost

#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

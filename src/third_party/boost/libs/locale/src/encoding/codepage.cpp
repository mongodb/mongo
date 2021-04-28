//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/config.hpp>

#if defined(BOOST_WINDOWS) || defined(__CYGWIN__)
#define BOOST_LOCALE_WITH_WCONV
#endif

#ifdef BOOST_LOCALE_WITH_ICONV
#include "iconv_codepage.ipp"
#endif
#ifdef BOOST_LOCALE_WITH_ICU
#include "uconv_codepage.ipp"
#endif
#ifdef BOOST_LOCALE_WITH_WCONV
#include "wconv_codepage.ipp"
#endif

#include <boost/locale/encoding.hpp>
#include <boost/locale/hold_ptr.hpp>

#include <string>
#include <cstring>
#include <memory>

namespace boost {
    namespace locale {
        namespace conv {
            namespace impl {
                
                std::string convert_between(char const *begin,
                                            char const *end,
                                            char const *to_charset,
                                            char const *from_charset,
                                            method_type how)
                {
                    hold_ptr<converter_between> cvt;
                    #ifdef BOOST_LOCALE_WITH_ICONV
                    cvt.reset(new iconv_between());
                    if(cvt->open(to_charset,from_charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    #ifdef BOOST_LOCALE_WITH_ICU
                    cvt.reset(new uconv_between());
                    if(cvt->open(to_charset,from_charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    #ifdef BOOST_LOCALE_WITH_WCONV
                    cvt.reset(new wconv_between());
                    if(cvt->open(to_charset,from_charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    throw invalid_charset_error(std::string(to_charset) + " or " + from_charset);
                }

                template<typename CharType>
                std::basic_string<CharType> convert_to(
                                        char const *begin,
                                        char const *end,
                                        char const *charset,
                                        method_type how)
                {
                    hold_ptr<converter_to_utf<CharType> > cvt;
                    #ifdef BOOST_LOCALE_WITH_ICONV
                    cvt.reset(new iconv_to_utf<CharType>());
                    if(cvt->open(charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    #ifdef BOOST_LOCALE_WITH_ICU
                    cvt.reset(new uconv_to_utf<CharType>());
                    if(cvt->open(charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    #ifdef BOOST_LOCALE_WITH_WCONV
                    cvt.reset(new wconv_to_utf<CharType>());
                    if(cvt->open(charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    throw invalid_charset_error(charset);
                }

                template<typename CharType>
                std::string convert_from(
                                        CharType const *begin,
                                        CharType const *end,
                                        char const *charset,
                                        method_type how)
                {
                    hold_ptr<converter_from_utf<CharType> > cvt;
                    #ifdef BOOST_LOCALE_WITH_ICONV
                    cvt.reset(new iconv_from_utf<CharType>());
                    if(cvt->open(charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    #ifdef BOOST_LOCALE_WITH_ICU
                    cvt.reset(new uconv_from_utf<CharType>());
                    if(cvt->open(charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    #ifdef BOOST_LOCALE_WITH_WCONV
                    cvt.reset(new wconv_from_utf<CharType>());
                    if(cvt->open(charset,how))
                        return cvt->convert(begin,end);
                    #endif
                    throw invalid_charset_error(charset);
                }

                std::string normalize_encoding(char const *ccharset)
                {
                    std::string charset;
                    charset.reserve(std::strlen(ccharset));
                    while(*ccharset!=0) {
                        char c=*ccharset++;
                        if('0' <= c && c<= '9')
                            charset+=c;
                        else if('a' <=c && c <='z')
                            charset+=c;
                        else if('A' <=c && c <='Z')
                            charset+=char(c-'A'+'a');
                    }
                    return charset;
                }
                

            } // impl 

            using namespace impl;
            
            std::string between(char const *begin,char const *end,
                                std::string const &to_charset,std::string const &from_charset,method_type how)
            {
                return convert_between(begin,end,to_charset.c_str(),from_charset.c_str(),how);
            }

            template<>
            std::basic_string<char> to_utf(char const *begin,char const *end,std::string const &charset,method_type how)
            {
                return convert_to<char>(begin,end,charset.c_str(),how);
            }

            template<>
            std::string from_utf(char const *begin,char const *end,std::string const &charset,method_type how)
            {
                return convert_from<char>(begin,end,charset.c_str(),how);
            }

            template<>
            std::basic_string<wchar_t> to_utf(char const *begin,char const *end,std::string const &charset,method_type how)
            {
                return convert_to<wchar_t>(begin,end,charset.c_str(),how);
            }

            template<>
            std::string from_utf(wchar_t const *begin,wchar_t const *end,std::string const &charset,method_type how)
            {
                return convert_from<wchar_t>(begin,end,charset.c_str(),how);
            }

            #ifdef BOOST_LOCALE_ENABLE_CHAR16_T
            template<>
            std::basic_string<char16_t> to_utf(char const *begin,char const *end,std::string const &charset,method_type how)
            {
                return convert_to<char16_t>(begin,end,charset.c_str(),how);
            }

            template<>
            std::string from_utf(char16_t const *begin,char16_t const *end,std::string const &charset,method_type how)
            {
                return convert_from<char16_t>(begin,end,charset.c_str(),how);
            }
            #endif

            #ifdef BOOST_LOCALE_ENABLE_CHAR32_T
            template<>
            std::basic_string<char32_t> to_utf(char const *begin,char const *end,std::string const &charset,method_type how)
            {
                return convert_to<char32_t>(begin,end,charset.c_str(),how);
            }

            template<>
            std::string from_utf(char32_t const *begin,char32_t const *end,std::string const &charset,method_type how)
            {
                return convert_from<char32_t>(begin,end,charset.c_str(),how);
            }
            #endif


        }
    }
}



// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4


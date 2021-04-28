//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_LOCALE_CONV_IMPL_HPP
#define BOOST_LOCALE_CONV_IMPL_HPP

#include <boost/locale/config.hpp>
#include <boost/locale/encoding.hpp>
namespace boost {
    namespace locale {
        namespace conv {
            namespace impl {

                template<typename CharType>
                char const *utf_name()
                {
                    union {
                        char first;
                        uint16_t u16;
                        uint32_t u32;
                    } v;

                    if(sizeof(CharType) == 1) {
                        return "UTF-8";
                    }
                    else if(sizeof(CharType) == 2) {
                        v.u16 = 1;
                        if(v.first == 1) {
                            return "UTF-16LE";
                        }
                        else {
                            return "UTF-16BE";
                        }
                    }
                    else if(sizeof(CharType) == 4) {
                        v.u32 = 1;
                        if(v.first == 1) {
                            return "UTF-32LE";
                        }
                        else {
                            return "UTF-32BE";
                        }

                    }
                    else {
                        return "Unknown Character Encoding";
                    }
                }

                std::string normalize_encoding(char const *encoding);
                
                inline int compare_encodings(char const *l,char const *r)
                {
                    return normalize_encoding(l).compare(normalize_encoding(r));
                }
            
                #if defined(BOOST_WINDOWS)  || defined(__CYGWIN__)
                int encoding_to_windows_codepage(char const *ccharset);
                #endif
            
                class converter_between {
                public:
                    typedef char char_type;

                    typedef std::string string_type;
                    
                    virtual bool open(char const *to_charset,char const *from_charset,method_type how) = 0;
                    
                    virtual std::string convert(char const *begin,char const *end) = 0;
                    
                    virtual ~converter_between()
                    {
                    }
                };

                template<typename CharType>
                class converter_from_utf {
                public:
                    typedef CharType char_type;

                    typedef std::basic_string<char_type> string_type;
                    
                    virtual bool open(char const *charset,method_type how) = 0;
                    
                    virtual std::string convert(CharType const *begin,CharType const *end) = 0;
                    
                    virtual ~converter_from_utf()
                    {
                    }
                };

                template<typename CharType>
                class converter_to_utf {
                public:
                    typedef CharType char_type;

                    typedef std::basic_string<char_type> string_type;

                    virtual bool open(char const *charset,method_type how) = 0;

                    virtual string_type convert(char const *begin,char const *end) = 0;

                    virtual ~converter_to_utf()
                    {
                    }
                };
            }
        }
    }
}

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
#endif

//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_IMPL_ICONV_CODEPAGE_HPP
#define BOOST_LOCALE_IMPL_ICONV_CODEPAGE_HPP

#include <boost/locale/encoding.hpp>
#include "../util/iconv.hpp"
#include <errno.h>
#include "conv.hpp"
#include <assert.h>
#include <vector>

namespace boost {
namespace locale {
namespace conv {
namespace impl {

class iconverter_base {
public:
    
    iconverter_base() : 
    cvt_((iconv_t)(-1))
    {
    }

    ~iconverter_base()
    {
        close();
    }

    size_t conv(char const **inbufc,size_t *inchar_left,
                char **outbuf,size_t *outchar_left)
    {
        char **inbuf = const_cast<char **>(inbufc);
        return call_iconv(cvt_,inbuf,inchar_left,outbuf,outchar_left);
    }

    bool do_open(char const *to,char const *from,method_type how)
    {
        close();
        cvt_ = iconv_open(to,from);
        how_ = how;
        return cvt_ != (iconv_t)(-1);
    }
    
    template<typename OutChar,typename InChar>
    std::basic_string<OutChar> real_convert(InChar const *ubegin,InChar const *uend)
    {
        std::basic_string<OutChar> sresult;
        
        sresult.reserve(uend - ubegin);

        OutChar result[64];

        char *out_start   = reinterpret_cast<char *>(&result[0]);
        char const *begin = reinterpret_cast<char const *>(ubegin);
        char const *end   = reinterpret_cast<char const *>(uend);
        
        enum { normal , unshifting , done } state = normal;

        while(state!=done) {

            size_t in_left = end - begin;
            size_t out_left = sizeof(result);
            
            char *out_ptr = out_start;
            size_t res = 0;
            if(in_left == 0)
                state = unshifting;

            if(state == normal) 
                res = conv(&begin,&in_left,&out_ptr,&out_left);
            else
                res = conv(0,0,&out_ptr,&out_left);

            int err = errno;

            size_t output_count = (out_ptr - out_start) / sizeof(OutChar);
            
            if(res!=0 && res!=(size_t)(-1)) {
                    if(how_ == stop) {
                        throw conversion_error();
                    }
            }

            sresult.append(&result[0],output_count);

            if(res == (size_t)(-1)) {
                if(err == EILSEQ || err == EINVAL) {
                    if(how_ == stop) {
                        throw conversion_error();
                    }

                    if(begin != end) {
                        begin+=sizeof(InChar);
                        if(begin >= end)
                            break;
                    }
                    else {
                        break;
                    }
                }
                else if (err==E2BIG) {
                    continue;
                }
                else {
                    // We should never get there
                    // but if we do
                    if(how_ == stop)
                        throw conversion_error();
                    else
                        break;
                }
            }
            if(state == unshifting)
                state = done;
        }
        return sresult;
    }


private:

    void close()
    {
        if(cvt_!=(iconv_t)(-1)) {
            iconv_close(cvt_);
            cvt_ = (iconv_t)(-1);
        }
    }
    
    iconv_t cvt_;

    method_type how_;

};

template<typename CharType>
class iconv_from_utf : public converter_from_utf<CharType>
{
public:

    typedef CharType char_type;

    virtual bool open(char const *charset,method_type how)
    {
        return self_.do_open(charset,utf_name<CharType>(),how);
    }

    virtual std::string convert(char_type const *ubegin,char_type const *uend)
    {
        return self_.template real_convert<char,char_type>(ubegin,uend);
    }
    virtual ~iconv_from_utf() {}
private:
    iconverter_base self_;
};

class iconv_between:  public converter_between
{
public:
    virtual bool open(char const *to_charset,char const *from_charset,method_type how)
    {
        return self_.do_open(to_charset,from_charset,how);
    }
    virtual std::string convert(char const *begin,char const *end)
    {
        return self_.real_convert<char,char>(begin,end);
    }
    virtual ~iconv_between() {}
private:
    iconverter_base self_;

};

template<typename CharType>
class iconv_to_utf :  public converter_to_utf<CharType>
{
public:

    typedef CharType char_type;
    typedef std::basic_string<char_type> string_type;

    virtual bool open(char const *charset,method_type how)
    {
        return self_.do_open(utf_name<CharType>(),charset,how);
    }

    virtual string_type convert(char const *begin,char const *end)
    {
        return self_.template real_convert<char_type,char>(begin,end);
    }
    virtual ~iconv_to_utf() {}
private:
    iconverter_base self_;
};



} // impl
} // conv
} // locale 
} // boost




#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

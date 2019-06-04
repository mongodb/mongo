//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/locale/encoding.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/locale/hold_ptr.hpp>
#include "../encoding/conv.hpp"
#include <boost/locale/util.hpp>
#include "all_generator.hpp"

#include <errno.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include "codecvt.hpp"

#ifdef BOOST_LOCALE_WITH_ICONV
#include "../util/iconv.hpp"
#endif

namespace boost {
namespace locale {
namespace impl_posix {

#ifdef BOOST_LOCALE_WITH_ICONV
    class mb2_iconv_converter : public util::base_converter {
    public:
       
        mb2_iconv_converter(std::string const &encoding) :
            encoding_(encoding),
            to_utf_((iconv_t)(-1)),
            from_utf_((iconv_t)(-1))
        {
            iconv_t d = (iconv_t)(-1);
            std::vector<uint32_t> first_byte_table;
            try {
                d = iconv_open(utf32_encoding(),encoding.c_str());
                if(d == (iconv_t)(-1)) {
                    throw std::runtime_error("Unsupported encoding" + encoding);
                }
                for(unsigned c=0;c<256;c++) {
                    char ibuf[2] = { char(c) , 0 };
                    char *in = ibuf;
                    size_t insize =2;
                    uint32_t obuf[2] = {illegal,illegal};
                    char *out = reinterpret_cast<char *>(obuf);
                    size_t outsize = 8;
                    // Basic sigle codepoint conversion
                    call_iconv(d,&in,&insize,&out,&outsize);
                    if(insize == 0 && outsize == 0 && obuf[1] == 0) {
                        first_byte_table.push_back(obuf[0]);
                        continue;
                    }
                    
                    // Test if this is illegal first byte or incomplete
                    in = ibuf;
                    insize = 1;
                    out = reinterpret_cast<char *>(obuf);
                    outsize = 8;
                    call_iconv(d,0,0,0,0);
                    size_t res = call_iconv(d,&in,&insize,&out,&outsize);
                    
                    // Now if this single byte starts a sequence we add incomplete 
                    // to know to ask that we need two bytes, othewise it may only be
                    // illegal

                    uint32_t point;
                    if(res == (size_t)(-1) && errno == EINVAL)
                        point = incomplete;
                    else
                        point = illegal;
                    first_byte_table.push_back(point);

                }
            }
            catch(...) {
                if(d!=(iconv_t)(-1))
                    iconv_close(d);
                throw;
            }
            iconv_close(d);
            first_byte_table_.reset(new std::vector<uint32_t>());
            first_byte_table_->swap(first_byte_table);
        }

        mb2_iconv_converter(mb2_iconv_converter const &other) :
            first_byte_table_(other.first_byte_table_),
            encoding_(other.encoding_),
            to_utf_((iconv_t)(-1)),
            from_utf_((iconv_t)(-1))
        {
        }
        
        virtual ~mb2_iconv_converter()
        {
            if(to_utf_ != (iconv_t)(-1))
                iconv_close(to_utf_);
            if(from_utf_ != (iconv_t)(-1))
                iconv_close(from_utf_);

        }

        virtual bool is_thread_safe() const
        {
            return false;
        }

        virtual mb2_iconv_converter *clone() const
        {
            return new mb2_iconv_converter(*this);
        }

        uint32_t to_unicode(char const *&begin,char const *end)
        {
            if(begin == end)
                return incomplete;
            
            unsigned char seq0 = *begin;
            uint32_t index = (*first_byte_table_)[seq0];
            if(index == illegal)
                return illegal;
            if(index != incomplete) {
                begin++;
                return index;
            }
            else if(begin+1 == end)
                return incomplete;
            
            open(to_utf_,utf32_encoding(),encoding_.c_str());

            // maybe illegal or may be double byte

            char inseq[3] = { static_cast<char>(seq0) , begin[1], 0};
            char *inbuf = inseq;
            size_t insize = 3;
            uint32_t result[2] = { illegal, illegal };
            size_t outsize = 8;
            char *outbuf = reinterpret_cast<char*>(result);
            call_iconv(to_utf_,&inbuf,&insize,&outbuf,&outsize);
            if(outsize == 0 && insize == 0 && result[1]==0 ) {
                begin+=2;
                return result[0];
            }
            return illegal;
        }

        uint32_t from_unicode(uint32_t cp,char *begin,char const *end)
        {
            if(cp == 0) {
                if(begin!=end) {
                    *begin = 0;
                    return 1;
                }
                else {
                    return incomplete;
                }
            }

            open(from_utf_,encoding_.c_str(),utf32_encoding());

            uint32_t codepoints[2] = {cp,0};
            char *inbuf = reinterpret_cast<char *>(codepoints);
            size_t insize = sizeof(codepoints);
            char outseq[3] = {0};
            char *outbuf = outseq;
            size_t outsize = 3;

            call_iconv(from_utf_,&inbuf,&insize,&outbuf,&outsize);

            if(insize != 0 || outsize > 1)
                return illegal;
            size_t len = 2 - outsize ;
            size_t reminder = end - begin;
            if(reminder < len)
                return incomplete;
            for(unsigned i=0;i<len;i++)
                *begin++ = outseq[i];
            return len;
        }

        void open(iconv_t &d,char const *to,char const *from)
        {
            if(d!=(iconv_t)(-1))
                return;
            d=iconv_open(to,from);
        }

        static char const *utf32_encoding()
        {
            union { char one; uint32_t value; } test;
            test.value = 1;
            if(test.one == 1)
                return "UTF-32LE";
            else
                return "UTF-32BE";
        }

        virtual int max_len() const
        {
            return 2;
        }

    private:
        boost::shared_ptr<std::vector<uint32_t> > first_byte_table_;
        std::string encoding_;
        iconv_t to_utf_;
        iconv_t from_utf_;
    };

    util::base_converter *create_iconv_converter(std::string const &encoding)
    {
        hold_ptr<util::base_converter> cvt;
        try {
            cvt.reset(new mb2_iconv_converter(encoding));
        }
        catch(std::exception const &e) {
            // Nothing to do, just retrun empty cvt
        }
        return cvt.release();
    }

#else // no iconv
    util::base_converter *create_iconv_converter(std::string const &/*encoding*/)
    {
        return 0;
    }
#endif

    std::locale create_codecvt(std::locale const &in,std::string const &encoding,character_facet_type type)
    {
        if(conv::impl::normalize_encoding(encoding.c_str())=="utf8")
            return util::create_utf8_codecvt(in,type);

        try {
            return util::create_simple_codecvt(in,encoding,type);
        }
        catch(conv::invalid_charset_error const &) {
            util::base_converter *cvt = create_iconv_converter(encoding);
            return util::create_codecvt_from_pointer(in,cvt,type);
        }
    }

} // impl_posix
} // locale 
} // boost

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

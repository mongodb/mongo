//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include <boost/locale/conversion.hpp>
#include "all_generator.hpp"
#include <unicode/normlzr.h>
#include <unicode/ustring.h>
#include <unicode/locid.h>
#include <unicode/uversion.h>
#if U_ICU_VERSION_MAJOR_NUM*100 + U_ICU_VERSION_MINOR_NUM >= 308
#include <unicode/ucasemap.h>
#define WITH_CASE_MAPS
#endif


#include "cdata.hpp"
#include "uconv.hpp"

#include <vector>

namespace boost {
namespace locale {
namespace impl_icu {
    
    
    namespace {
        void normalize_string(icu::UnicodeString &str,int flags)
        {
            UErrorCode code=U_ZERO_ERROR;
            UNormalizationMode mode=UNORM_DEFAULT;
            switch(flags) {
            case norm_nfd:
                mode=UNORM_NFD;
                break;
            case norm_nfc:
                mode=UNORM_NFC;
                break;
            case norm_nfkd:
                mode=UNORM_NFKD;
                break;
            case norm_nfkc:
                mode=UNORM_NFKC;
                break;
            }
            icu::UnicodeString tmp;
            icu::Normalizer::normalize(str,mode,0,tmp,code);

            check_and_throw_icu_error(code);

            str=tmp;
        }
    }


    template<typename CharType>
    class converter_impl : public converter<CharType> {
    public:
        typedef CharType char_type;
        typedef std::basic_string<char_type> string_type;

        converter_impl(cdata const &d) :
            locale_(d.locale),
            encoding_(d.encoding)
        {
        }

        virtual string_type convert(converter_base::conversion_type how,char_type const *begin,char_type const *end,int flags = 0) const
        {
            icu_std_converter<char_type> cvt(encoding_);
            icu::UnicodeString str=cvt.icu(begin,end);
            switch(how) {
            case converter_base::normalization:
                normalize_string(str,flags);
                break;
            case converter_base::upper_case:
                str.toUpper(locale_);
                break;
            case converter_base::lower_case:
                str.toLower(locale_);
                break;
            case converter_base::title_case:
                str.toTitle(0,locale_);
                break;
            case converter_base::case_folding:
                str.foldCase();
                break;
            default:
                ;
            }
            return cvt.std(str);
        }
    
    private:
        icu::Locale locale_;
        std::string encoding_;
    }; // converter_impl

    #ifdef WITH_CASE_MAPS
    class raii_casemap {
        raii_casemap(raii_casemap const &);
        void operator = (raii_casemap const&);
    public:
        raii_casemap(std::string const &locale_id) :
            map_(0)
        {
            UErrorCode err=U_ZERO_ERROR;
            map_ = ucasemap_open(locale_id.c_str(),0,&err);
            check_and_throw_icu_error(err);
            if(!map_)
                throw std::runtime_error("Failed to create UCaseMap");
        }
        template<typename Conv>
        std::string convert(Conv func,char const *begin,char const *end) const
        {
                std::vector<char> buf((end-begin)*11/10+1);
                UErrorCode err=U_ZERO_ERROR;
                int size = func(map_,&buf.front(),buf.size(),begin,end-begin,&err);
                if(err == U_BUFFER_OVERFLOW_ERROR) {
                    err = U_ZERO_ERROR;
                    buf.resize(size+1);
                    size = func(map_,&buf.front(),buf.size(),begin,end-begin,&err);
                }
                check_and_throw_icu_error(err);
                return std::string(&buf.front(),size);
        }
        ~raii_casemap()
        {
            ucasemap_close(map_);
        }
    private:
        UCaseMap *map_;
    };

    class utf8_converter_impl : public converter<char> {
    public:
        
        utf8_converter_impl(cdata const &d) :
            locale_id_(d.locale.getName()),
            map_(locale_id_)
        {
        }

        virtual std::string convert(converter_base::conversion_type how,char const *begin,char const *end,int flags = 0) const
        {
            
            if(how == converter_base::normalization) {
                icu_std_converter<char> cvt("UTF-8");
                icu::UnicodeString str=cvt.icu(begin,end);
                normalize_string(str,flags);
                return cvt.std(str);
            }
            
            switch(how) 
            {
            case converter_base::upper_case:
                return map_.convert(ucasemap_utf8ToUpper,begin,end);
            case converter_base::lower_case:
                return map_.convert(ucasemap_utf8ToLower,begin,end);
            case converter_base::title_case:
                {
                    // Non-const method, so need to create a separate map
                    raii_casemap map(locale_id_);
                    return map.convert(ucasemap_utf8ToTitle,begin,end);
                }
            case converter_base::case_folding:
                return map_.convert(ucasemap_utf8FoldCase,begin,end);
            default:
                return std::string(begin,end-begin);
            }
        }
    private:
        std::string locale_id_;
        raii_casemap map_;
    }; // converter_impl

#endif // WITH_CASE_MAPS

    std::locale create_convert(std::locale const &in,cdata const &cd,character_facet_type type)
    {
        switch(type) {
        case char_facet:
            #ifdef WITH_CASE_MAPS
            if(cd.utf8)
                return std::locale(in,new utf8_converter_impl(cd));
            #endif
            return std::locale(in,new converter_impl<char>(cd));
        case wchar_t_facet:
            return std::locale(in,new converter_impl<wchar_t>(cd));
        #ifdef BOOST_LOCALE_ENABLE_CHAR16_T
        case char16_t_facet:
            return std::locale(in,new converter_impl<char16_t>(cd));
        #endif
        #ifdef BOOST_LOCALE_ENABLE_CHAR32_T
        case char32_t_facet:
            return std::locale(in,new converter_impl<char32_t>(cd));
        #endif
        default:
            return in;
        }
    }
    

} // impl_icu
} // locale
} // boost

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4

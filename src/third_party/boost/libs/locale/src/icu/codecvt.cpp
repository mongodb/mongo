//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2025 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include "codecvt.hpp"
#include <boost/locale/encoding.hpp>
#include <boost/locale/encoding_errors.hpp>
#include <boost/locale/hold_ptr.hpp>
#include <boost/locale/util.hpp>
#include "../util/encoding.hpp"
#include "../util/make_std_unique.hpp"
#include "all_generator.hpp"
#include "icu_util.hpp"
#include "uconv.hpp"
#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>

#ifdef BOOST_MSVC
#    pragma warning(disable : 4244) // loose data
#endif

namespace boost { namespace locale { namespace impl_icu {
    class uconv_converter : public util::base_converter {
    public:
        uconv_converter(const std::string& encoding) : encoding_(encoding), cvt_(encoding, cpcvt_type::stop) {}

        bool is_thread_safe() const override { return false; }

        uconv_converter* clone() const override { return new uconv_converter(encoding_); }

        utf::code_point to_unicode(const char*& begin, const char* end) override
        {
            UErrorCode err = U_ZERO_ERROR;
            const char* tmp = begin;
            const UChar32 c = ucnv_getNextUChar(cvt_.cvt(), &tmp, end, &err);
            ucnv_reset(cvt_.cvt());
            if(err == U_TRUNCATED_CHAR_FOUND)
                return incomplete;
            if(U_FAILURE(err))
                return illegal;

            begin = tmp;
            return c;
        }

        utf::len_or_error from_unicode(utf::code_point u, char* begin, const char* end) override
        {
            UChar code_point[2] = {0};
            int len;
            if(u <= 0xFFFF) {
                if(0xD800 <= u && u <= 0xDFFF) // No surrogates
                    return illegal;
                code_point[0] = u;
                len = 1;
            } else {
                u -= 0x10000;
                code_point[0] = 0xD800 | (u >> 10);
                code_point[1] = 0xDC00 | (u & 0x3FF);
                len = 2;
            }
            UErrorCode err = U_ZERO_ERROR;
            const auto olen = ucnv_fromUChars(cvt_.cvt(), begin, end - begin, code_point, len, &err);
            ucnv_reset(cvt_.cvt());
            if(err == U_BUFFER_OVERFLOW_ERROR)
                return incomplete;
            if(U_FAILURE(err))
                return illegal;
            return olen;
        }

        int max_len() const override { return cvt_.max_char_size(); }

    private:
        std::string encoding_;
        uconv cvt_;
    };

    std::unique_ptr<util::base_converter> create_uconv_converter(const std::string& encoding)
    {
        try {
            return make_std_unique<uconv_converter>(encoding);
        } catch(const std::exception& /*e*/) {
            return nullptr;
        }
    }

    std::locale create_codecvt(const std::locale& in, const std::string& encoding, char_facet_t type)
    {
        if(util::normalize_encoding(encoding) == "utf8")
            return util::create_utf8_codecvt(in, type);

        try {
            return util::create_simple_codecvt(in, encoding, type);
        } catch(const boost::locale::conv::invalid_charset_error&) {
            return util::create_codecvt(in, create_uconv_converter(encoding), type);
        }
    }

}}} // namespace boost::locale::impl_icu

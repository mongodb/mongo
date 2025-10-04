//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_UCONV_CODEPAGE_HPP
#define BOOST_LOCALE_IMPL_UCONV_CODEPAGE_HPP
#include <boost/locale/encoding.hpp>
#include <boost/locale/hold_ptr.hpp>
#include "../icu/icu_util.hpp"
#include "../icu/uconv.hpp"
#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>

namespace boost { namespace locale { namespace conv { namespace impl {
    template<typename CharType>
    class uconv_to_utf final : public detail::utf_encoder<CharType> {
    public:
        bool open(const std::string& charset, method_type how)
        {
            try {
                using impl_icu::cpcvt_type;
                cvt_from_.reset(new from_type(charset, how == skip ? cpcvt_type::skip : cpcvt_type::stop));
                cvt_to_.reset(new to_type("UTF-8", how == skip ? cpcvt_type::skip : cpcvt_type::stop));
            } catch(const std::exception& /*e*/) {
                cvt_from_.reset();
                cvt_to_.reset();
                return false;
            }
            return true;
        }

        std::basic_string<CharType> convert(const char* begin, const char* end) override
        {
            try {
                return cvt_to_->std(cvt_from_->icu_checked(begin, end));
            } catch(const std::exception& /*e*/) {
                throw conversion_error();
            }
        }

    private:
        typedef impl_icu::icu_std_converter<char> from_type;
        typedef impl_icu::icu_std_converter<CharType> to_type;

        hold_ptr<from_type> cvt_from_;
        hold_ptr<to_type> cvt_to_;
    };

    template<typename CharType>
    class uconv_from_utf final : public detail::utf_decoder<CharType> {
    public:
        bool open(const std::string& charset, method_type how)
        {
            try {
                using impl_icu::cpcvt_type;
                cvt_from_.reset(new from_type("UTF-8", how == skip ? cpcvt_type::skip : cpcvt_type::stop));
                cvt_to_.reset(new to_type(charset, how == skip ? cpcvt_type::skip : cpcvt_type::stop));
            } catch(const std::exception& /*e*/) {
                cvt_from_.reset();
                cvt_to_.reset();
                return false;
            }
            return true;
        }

        std::string convert(const CharType* begin, const CharType* end) override
        {
            try {
                return cvt_to_->std(cvt_from_->icu_checked(begin, end));
            } catch(const std::exception& /*e*/) {
                throw conversion_error();
            }
        }

    private:
        typedef impl_icu::icu_std_converter<CharType> from_type;
        typedef impl_icu::icu_std_converter<char> to_type;

        hold_ptr<from_type> cvt_from_;
        hold_ptr<to_type> cvt_to_;
    };

    class uconv_between final : public detail::narrow_converter {
    public:
        bool open(const std::string& to_charset, const std::string& from_charset, method_type how)
        {
            try {
                using impl_icu::cpcvt_type;
                cvt_from_.reset(new from_type(from_charset, how == skip ? cpcvt_type::skip : cpcvt_type::stop));
                cvt_to_.reset(new to_type(to_charset, how == skip ? cpcvt_type::skip : cpcvt_type::stop));
            } catch(const std::exception& /*e*/) {
                cvt_from_.reset();
                cvt_to_.reset();
                return false;
            }
            return true;
        }

        std::string convert(const char* begin, const char* end) override
        {
            try {
                return cvt_to_->std(cvt_from_->icu(begin, end));
            } catch(const std::exception& /*e*/) {
                throw conversion_error();
            }
        }

    private:
        typedef impl_icu::icu_std_converter<char> from_type;
        typedef impl_icu::icu_std_converter<char> to_type;

        hold_ptr<from_type> cvt_from_;
        hold_ptr<to_type> cvt_to_;
    };

}}}} // namespace boost::locale::conv::impl

#endif

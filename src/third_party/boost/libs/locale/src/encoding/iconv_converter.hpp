//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_ICONV_CODEPAGE_HPP
#define BOOST_LOCALE_IMPL_ICONV_CODEPAGE_HPP

#include <boost/locale/encoding.hpp>
#include "../util/encoding.hpp"
#include "../util/iconv.hpp"
#include <boost/assert.hpp>
#include <cerrno>
#include <string>

namespace boost { namespace locale { namespace conv { namespace impl {

    class iconverter_base {
    public:
        bool do_open(const char* to, const char* from, method_type how)
        {
            cvt_ = iconv_open(to, from);
            how_ = how;
            return static_cast<bool>(cvt_);
        }

        template<typename OutChar, typename InChar>
        std::basic_string<OutChar> real_convert(const InChar* ubegin, const InChar* uend)
        {
            std::basic_string<OutChar> sresult;

            sresult.reserve(uend - ubegin);

            OutChar tmp_buf[64];

            char* out_start = reinterpret_cast<char*>(tmp_buf);
            const char* begin = reinterpret_cast<const char*>(ubegin);
            const char* end = reinterpret_cast<const char*>(uend);

            bool is_unshifting = false;

            for(;;) {
                size_t in_left = end - begin;
                size_t out_left = sizeof(tmp_buf);
                char* out_ptr = out_start;

                if(in_left == 0)
                    is_unshifting = true;

                const auto old_in_left = in_left;
                const size_t res = (!is_unshifting) ? conv(&begin, &in_left, &out_ptr, &out_left) :
                                                      conv(nullptr, nullptr, &out_ptr, &out_left);

                if(res != 0 && res != (size_t)(-1)) {
                    if(how_ == stop)
                        throw conversion_error();
                }

                const size_t output_count = (out_ptr - out_start) / sizeof(OutChar);
                sresult.append(tmp_buf, output_count);

                if(res == (size_t)(-1)) {
                    const int err = errno;
                    BOOST_ASSERT_MSG(err == EILSEQ || err == EINVAL || err == E2BIG, "Invalid error code from IConv");
                    if(err == EILSEQ || err == EINVAL) {
                        if(how_ == stop)
                            throw conversion_error();

                        if(begin != end) {
                            begin += sizeof(InChar);
                            if(begin >= end)
                                break;
                        } else
                            break;
                    } else if(err == E2BIG) {
                        if(in_left != old_in_left || out_ptr != out_start) // Check to avoid infinite loop
                            continue;
                        throw std::runtime_error("No progress, IConv is faulty!"); // LCOV_EXCL_LINE
                    } else                        // Invalid error code, shouldn't ever happen or iconv has a bug
                        throw conversion_error(); // LCOV_EXCL_LINE
                }
                if(is_unshifting)
                    break;
            }
            return sresult;
        }

    private:
        size_t conv(const char** inbuf, size_t* inchar_left, char** outbuf, size_t* outchar_left)
        {
            return call_iconv(cvt_, inbuf, inchar_left, outbuf, outchar_left);
        }

        iconv_handle cvt_;
        method_type how_;
    };

    template<typename CharType>
    class iconv_from_utf final : public detail::utf_decoder<CharType> {
    public:
        bool open(const std::string& charset, method_type how)
        {
            return self_.do_open(charset.c_str(), util::utf_name<CharType>(), how);
        }

        std::string convert(const CharType* ubegin, const CharType* uend) override
        {
            return self_.template real_convert<char>(ubegin, uend);
        }

    private:
        iconverter_base self_;
    };

    class iconv_between final : public detail::narrow_converter {
    public:
        bool open(const std::string& to_charset, const std::string& from_charset, method_type how)
        {
            return self_.do_open(to_charset.c_str(), from_charset.c_str(), how);
        }
        std::string convert(const char* begin, const char* end) override
        {
            return self_.real_convert<char, char>(begin, end);
        }

    private:
        iconverter_base self_;
    };

    template<typename CharType>
    class iconv_to_utf final : public detail::utf_encoder<CharType> {
    public:
        bool open(const std::string& charset, method_type how)
        {
            return self_.do_open(util::utf_name<CharType>(), charset.c_str(), how);
        }

        std::basic_string<CharType> convert(const char* begin, const char* end) override
        {
            return self_.template real_convert<CharType>(begin, end);
        }

    private:
        iconverter_base self_;
    };

}}}} // namespace boost::locale::conv::impl

#endif

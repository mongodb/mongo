//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_ICONV_FIXER_HPP
#define BOOST_LOCALE_ICONV_FIXER_HPP

#include <boost/core/exchange.hpp>
#include <iconv.h>

namespace boost { namespace locale {
    class iconv_handle {
        iconv_t h_;
        void close()
        {
            if(*this)
                iconv_close(h_);
        }

    public:
        explicit iconv_handle(iconv_t h = iconv_t(-1)) : h_(h) {}

        iconv_handle(const iconv_handle& rhs) = delete;
        iconv_handle(iconv_handle&& rhs) noexcept : h_(exchange(rhs.h_, iconv_t(-1))) {}

        iconv_handle& operator=(const iconv_handle& rhs) = delete;
        iconv_handle& operator=(iconv_handle&& rhs) noexcept
        {
            h_ = exchange(rhs.h_, iconv_t(-1));
            return *this;
        }
        iconv_handle& operator=(iconv_t h)
        {
            close();
            h_ = h;
            return *this;
        }
        ~iconv_handle() { close(); }

        operator iconv_t() const { return h_; }
        explicit operator bool() const { return h_ != iconv_t(-1); }
    };

    extern "C" {
#if defined(__ICONV_F_HIDE_INVALID) && defined(__FreeBSD__)
#    define BOOST_LOCALE_ICONV_FUNC __iconv
#    define BOOST_LOCALE_ICONV_FLAGS , __ICONV_F_HIDE_INVALID, 0

    // GNU variant
    typedef size_t (*const_iconv_ptr_type)(iconv_t, const char**, size_t*, char**, size_t*, uint32_t, size_t*);
    // POSIX variant
    typedef size_t (*nonconst_iconv_ptr_type)(iconv_t, char**, size_t*, char**, size_t*, uint32_t, size_t*);
#else
#    define BOOST_LOCALE_ICONV_FUNC iconv
#    define BOOST_LOCALE_ICONV_FLAGS

    typedef size_t (*const_iconv_ptr_type)(iconv_t, const char**, size_t*, char**, size_t*);
    typedef size_t (*nonconst_iconv_ptr_type)(iconv_t, char**, size_t*, char**, size_t*);
#endif
    }

    inline size_t
    call_iconv_impl(const_iconv_ptr_type ptr, iconv_t d, const char** in, size_t* insize, char** out, size_t* outsize)
    {
        return ptr(d, in, insize, out, outsize BOOST_LOCALE_ICONV_FLAGS);
    }
    inline size_t call_iconv_impl(nonconst_iconv_ptr_type ptr,
                                  iconv_t d,
                                  const char** in,
                                  size_t* insize,
                                  char** out,
                                  size_t* outsize)
    {
        return ptr(d, const_cast<char**>(in), insize, out, outsize BOOST_LOCALE_ICONV_FLAGS);
    }

    inline size_t call_iconv(iconv_t d, const char** in, size_t* insize, char** out, size_t* outsize)
    {
        return call_iconv_impl(BOOST_LOCALE_ICONV_FUNC, d, in, insize, out, outsize);
    }

    // Convenience overload when the adjusted in/out ptrs are not required
    inline size_t call_iconv(iconv_t d, const char* in, size_t* insize, char* out, size_t* outsize)
    {
        return call_iconv(d, &in, insize, &out, outsize);
    }
    // Disambiguation
    inline size_t call_iconv(iconv_t d, std::nullptr_t, std::nullptr_t, std::nullptr_t, std::nullptr_t)
    {
        return call_iconv_impl(BOOST_LOCALE_ICONV_FUNC, d, nullptr, nullptr, nullptr, nullptr);
    }
}} // namespace boost::locale

#endif

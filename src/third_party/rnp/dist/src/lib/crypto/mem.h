/*-
 * Copyright (c) 2021-2023 Ribose Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef CRYPTO_MEM_H_
#define CRYPTO_MEM_H_

#include "config.h"
#include <array>
#include <vector>
#if defined(CRYPTO_BACKEND_BOTAN)
#include <botan/secmem.h>
#include <botan/ffi.h>
#elif defined(CRYPTO_BACKEND_OPENSSL)
#include <openssl/crypto.h>
#endif
#include "str-utils.h"

namespace rnp {

#if defined(CRYPTO_BACKEND_BOTAN)
template <typename T> using secure_vector = Botan::secure_vector<T>;
#elif defined(CRYPTO_BACKEND_OPENSSL)
template <typename T> class ossl_allocator {
  public:
#if !defined(_MSC_VER) || !defined(_DEBUG)
    /* MSVC in debug mode uses non-integral proxy types in container types */
    static_assert(std::is_integral<T>::value, "secure_vector can hold integral types only");
#endif

    typedef T           value_type;
    typedef std::size_t size_type;

    ossl_allocator() noexcept = default;
    ossl_allocator(const ossl_allocator &) noexcept = default;
    ossl_allocator &operator=(const ossl_allocator &) noexcept = default;
    ~ossl_allocator() noexcept = default;

    template <typename U> ossl_allocator(const ossl_allocator<U> &) noexcept
    {
    }

    T *
    allocate(std::size_t n)
    {
        if (!n) {
            return nullptr;
        }

        /* attempt to use OpenSSL secure alloc */
        T *ptr = static_cast<T *>(OPENSSL_secure_zalloc(n * sizeof(T)));
        if (ptr) {
            return ptr;
        }
        /* fallback to std::alloc if failed */
        ptr = static_cast<T *>(std::calloc(n, sizeof(T)));
        if (!ptr)
            throw std::bad_alloc();
        return ptr;
    }

    void
    deallocate(T *p, std::size_t n)
    {
        if (!p) {
            return;
        }
        if (CRYPTO_secure_allocated(p)) {
            OPENSSL_secure_clear_free(p, n * sizeof(T));
            return;
        }
        OPENSSL_cleanse(p, n * sizeof(T));
        std::free(p);
    }
};

template <typename T> using secure_vector = std::vector<T, ossl_allocator<T> >;
#else
#error Unsupported backend.
#endif

using secure_bytes = secure_vector<uint8_t>;

template <typename T, std::size_t N> struct secure_array {
  private:
#if !defined(_MSC_VER) || !defined(_DEBUG)
    /* MSVC in debug mode uses non-integral proxy types in container types */
    static_assert(std::is_integral<T>::value, "secure_array can hold integral types only");
#endif

    std::array<T, N> data_;

  public:
    secure_array() : data_({0})
    {
    }

    T *
    data()
    {
        return &data_[0];
    }

    const T *
    data() const
    {
        return &data_[0];
    }

    std::size_t
    size() const
    {
        return data_.size();
    }

    T
    operator[](size_t idx) const
    {
        return data_[idx];
    }

    T &
    operator[](size_t idx)
    {
        return data_[idx];
    }

    ~secure_array()
    {
#if defined(CRYPTO_BACKEND_BOTAN)
        botan_scrub_mem(&data_[0], sizeof(data_));
#elif defined(CRYPTO_BACKEND_OPENSSL)
        OPENSSL_cleanse(&data_[0], sizeof(data_));
#else
#error "Unsupported crypto backend."
#endif
    }
};

enum class HexFormat { Lowercase, Uppercase };

bool   hex_encode(const uint8_t *buf,
                  size_t         buf_len,
                  char *         hex,
                  size_t         hex_len,
                  HexFormat      format = HexFormat::Uppercase);
size_t hex_decode(const char *hex, uint8_t *buf, size_t buf_len);

inline std::string
bin_to_hex(const uint8_t *data, size_t len, HexFormat format = HexFormat::Uppercase)
{
    std::string res(len * 2 + 1, '\0');
    (void) hex_encode(data, len, &res.front(), res.size(), format);
    res.resize(len * 2);
    return res;
}

inline std::string
bin_to_hex(const std::vector<uint8_t> &vec, HexFormat format = HexFormat::Uppercase)
{
    return bin_to_hex(vec.data(), vec.size(), format);
}

inline std::vector<uint8_t>
hex_to_bin(const std::string &str)
{
    if (str.empty() || !is_hex(str)) {
        return {};
    }
    /* 1 extra char for case of non-even input , 1 for terminating zero */
    std::vector<uint8_t> res(str.size() / 2 + 2);
    size_t               len = hex_decode(str.c_str(), res.data(), res.size());
    res.resize(len);
    return res;
}

} // namespace rnp

void secure_clear(void *vp, size_t size);

#endif // CRYPTO_MEM_H_
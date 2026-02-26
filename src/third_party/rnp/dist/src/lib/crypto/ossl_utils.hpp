/*
 * Copyright (c) 2017-2024 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_OSSL_UTILS_HPP_
#define RNP_OSSL_UTILS_HPP_

#include <cstdio>
#include <cstdint>
#include "config.h"
#include "mpi.hpp"
#include <cassert>
#include <memory>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/dsa.h>
#include <openssl/ecdsa.h>
#if defined(CRYPTO_BACKEND_OPENSSL3)
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#else
#include <openssl/rsa.h>
#include <openssl/dsa.h>
#include <openssl/ec.h>
#endif

namespace rnp {
class bn {
    BIGNUM *      _bn;
    const BIGNUM *_c_bn;

  public:
    bn(BIGNUM *val = NULL) : _bn(val), _c_bn(NULL)
    {
    }

    bn(const BIGNUM *val) : _bn(NULL), _c_bn(val)
    {
    }

    bn(const pgp::mpi &val) : bn(&val)
    {
    }
    bn(const pgp::mpi *val) : _c_bn(NULL)
    {
        if (!val) {
            _bn = NULL;
            return;
        }

        _bn = BN_new();
        if (_bn && !BN_bin2bn(val->data(), val->size(), _bn)) {
            BN_free(_bn);
            _bn = NULL;
        }
    }

    bn(bn &&src)
    {
        _bn = src._bn;
        src._bn = NULL;
        _c_bn = src._c_bn;
        src._c_bn = NULL;
    }

    bn &
    operator=(bn &&src)
    {
        if (&src == this) {
            return *this;
        }
        BN_free(_bn);
        _bn = src._bn;
        src._bn = NULL;
        _c_bn = src._c_bn;
        src._c_bn = NULL;
        return *this;
    }

    ~bn()
    {
        BN_free(_bn);
    }

    explicit operator bool() const
    {
        return c_get();
    }

    BIGNUM **
    ptr() noexcept
    {
        BN_free(_bn);
        _bn = NULL;
        return &_bn;
    }

    const BIGNUM **
    cptr() noexcept
    {
        return &_c_bn;
    }

    BIGNUM *
    get() noexcept
    {
        return _bn;
    }

    const BIGNUM *
    c_get() const noexcept
    {
        return _bn ? _bn : _c_bn;
    }

    BIGNUM *
    own() noexcept
    {
        auto res = _bn;
        _bn = NULL;
        return res;
    }

    size_t
    bytes() const noexcept
    {
        return BN_num_bytes(c_get());
    }

    bool
    bin(uint8_t *b) const noexcept
    {
        return b && BN_bn2bin(c_get(), b) >= 0;
    }

    static bool
    mpi(const BIGNUM *num, pgp::mpi &mpi) noexcept
    {
        size_t bytes = (size_t) BN_num_bytes(num);
        assert(bytes <= PGP_MPINT_SIZE);
        mpi.resize(bytes);
        if (BN_bn2bin(num, mpi.data()) < 0) {
            return false;
        }
        return true;
    }

    bool
    mpi(pgp::mpi &mpi) const noexcept
    {
        return bn::mpi(c_get(), mpi);
    }
};

namespace ossl {

struct BNCtxDeleter {
    void
    operator()(BN_CTX *ptr) const
    {
        BN_CTX_free(ptr);
    }
};

using BNCtx = std::unique_ptr<BN_CTX, BNCtxDeleter>;

struct BNRecpCtxDeleter {
    void
    operator()(BN_RECP_CTX *ptr) const
    {
        BN_RECP_CTX_free(ptr);
    }
};

using BNRecpCtx = std::unique_ptr<BN_RECP_CTX, BNRecpCtxDeleter>;

struct BNMontCtxDeleter {
    void
    operator()(BN_MONT_CTX *ptr) const
    {
        BN_MONT_CTX_free(ptr);
    }
};

using BNMontCtx = std::unique_ptr<BN_MONT_CTX, BNMontCtxDeleter>;

namespace evp {

struct PKeyDeleter {
    void
    operator()(EVP_PKEY *ptr) const
    {
        EVP_PKEY_free(ptr);
    }
};

using PKey = std::unique_ptr<EVP_PKEY, PKeyDeleter>;

struct PKeyCtxDeleter {
    void
    operator()(EVP_PKEY_CTX *ptr) const
    {
        EVP_PKEY_CTX_free(ptr);
    }
};

using PKeyCtx = std::unique_ptr<EVP_PKEY_CTX, PKeyCtxDeleter>;

struct CipherCtxDeleter {
    void
    operator()(EVP_CIPHER_CTX *ptr) const
    {
        EVP_CIPHER_CTX_free(ptr);
    }
};

using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

struct MDCtxDeleter {
    void
    operator()(EVP_MD_CTX *ptr) const
    {
        EVP_MD_CTX_free(ptr);
    }
};

using MDCtx = std::unique_ptr<EVP_MD_CTX, MDCtxDeleter>;
} // namespace evp

struct DSASigDeleter {
    void
    operator()(DSA_SIG *ptr) const
    {
        DSA_SIG_free(ptr);
    }
};

using DSASig = std::unique_ptr<DSA_SIG, DSASigDeleter>;

struct ECDSASigDeleter {
    void
    operator()(::ECDSA_SIG *ptr) const
    {
        ECDSA_SIG_free(ptr);
    }
};

using ECDSASig = std::unique_ptr<::ECDSA_SIG, ECDSASigDeleter>;

#if !defined(CRYPTO_BACKEND_OPENSSL3)
struct RSADeleter {
    void
    operator()(::RSA *ptr) const
    {
        RSA_free(ptr);
    }
};

using RSA = std::unique_ptr<::RSA, RSADeleter>;

struct DSADeleter {
    void
    operator()(::DSA *ptr) const
    {
        DSA_free(ptr);
    }
};

using DSA = std::unique_ptr<::DSA, DSADeleter>;

struct DHDeleter {
    void
    operator()(::DH *ptr) const
    {
        DH_free(ptr);
    }
};

using DH = std::unique_ptr<::DH, DHDeleter>;

struct ECKeyDeleter {
    void
    operator()(::EC_KEY *ptr) const
    {
        EC_KEY_free(ptr);
    }
};

using ECKey = std::unique_ptr<::EC_KEY, ECKeyDeleter>;

struct ECPointDeleter {
    void
    operator()(::EC_POINT *ptr) const
    {
        EC_POINT_free(ptr);
    }
};

using ECPoint = std::unique_ptr<::EC_POINT, ECPointDeleter>;
#else
struct ParamDeleter {
    void
    operator()(OSSL_PARAM *ptr) const
    {
        OSSL_PARAM_free(ptr);
    }
};

using Param = std::unique_ptr<OSSL_PARAM, ParamDeleter>;

struct ParamBldDeleter {
    void
    operator()(OSSL_PARAM_BLD *ptr) const
    {
        OSSL_PARAM_BLD_free(ptr);
    }
};

using ParamBld = std::unique_ptr<OSSL_PARAM_BLD, ParamBldDeleter>;
#endif

inline const char *
latest_err()
{
    return ERR_error_string(ERR_peek_last_error(), NULL);
}

} // namespace ossl
} // namespace rnp

#endif
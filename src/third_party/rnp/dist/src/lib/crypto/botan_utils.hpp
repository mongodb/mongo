/*
 * Copyright (c) 2024 [Ribose Inc](https://www.ribose.com).
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

#ifndef RNP_BOTAN_UTILS_HPP_
#define RNP_BOTAN_UTILS_HPP_

#include <botan/ffi.h>
#include "mpi.hpp"
#include "utils.h"

/* Self-destructing wrappers around the Botan's FFI objects */
namespace rnp {
class bn {
    botan_mp_t bn_;

  public:
    bn()
    {
        botan_mp_init(&bn_);
    }

    bn(const uint8_t *val, size_t len)
    {
        botan_mp_init(&bn_);
        if (bn_ && botan_mp_from_bin(bn_, val, len)) {
            /* LCOV_EXCL_START */
            botan_mp_destroy(bn_);
            bn_ = NULL;
            /* LCOV_EXCL_END */
        }
    }

    bn(const pgp::mpi &val) : bn(val.data(), val.size()){};

    bn(const bn &) = delete;

    ~bn()
    {
        botan_mp_destroy(bn_);
    }

    explicit operator bool() const
    {
        return bn_;
    }

    botan_mp_t
    get() noexcept
    {
        return bn_;
    }

    bool
    mpi(pgp::mpi &val) const noexcept
    {
        auto len = bytes();
        if (len > PGP_MPINT_SIZE) {
            RNP_LOG("Too large MPI.");
            val.resize(0);
            return false;
        }
        val.resize(len);
        return !botan_mp_to_bin(bn_, val.data());
    }

    bool
    bin(uint8_t *b) const noexcept
    {
        return b ? !botan_mp_to_bin(bn_, b) : false;
    }

    size_t
    bytes() const noexcept
    {
        size_t res = 0;
        if (botan_mp_num_bits(bn_, &res)) {
            RNP_LOG("botan_mp_num_bits failed.");
        }
        return BITS_TO_BYTES(res);
    }
};

namespace botan {

class Pubkey {
    botan_pubkey_t key_;

  public:
    Pubkey() : key_(NULL){};

    Pubkey(const Pubkey &) = delete;

    ~Pubkey()
    {
        botan_pubkey_destroy(key_);
    }

    botan_pubkey_t &
    get() noexcept
    {
        return key_;
    };
};

class Privkey {
    botan_privkey_t key_;

  public:
    Privkey() : key_(NULL){};

    Privkey(const Privkey &) = delete;

    ~Privkey()
    {
        botan_privkey_destroy(key_);
    }

    botan_privkey_t &
    get() noexcept
    {
        return key_;
    };

    const botan_privkey_t &
    get() const noexcept
    {
        return key_;
    };
};

namespace op {
class Encrypt {
    botan_pk_op_encrypt_t op_;

  public:
    Encrypt() : op_(NULL){};

    Encrypt(const Encrypt &) = delete;

    ~Encrypt()
    {
        botan_pk_op_encrypt_destroy(op_);
    }

    botan_pk_op_encrypt_t &
    get() noexcept
    {
        return op_;
    };
};

class Decrypt {
    botan_pk_op_decrypt_t op_;

  public:
    Decrypt() : op_(NULL){};

    Decrypt(const Decrypt &) = delete;

    ~Decrypt()
    {
        botan_pk_op_decrypt_destroy(op_);
    }

    botan_pk_op_decrypt_t &
    get() noexcept
    {
        return op_;
    };
};

class Verify {
    botan_pk_op_verify_t op_;

  public:
    Verify() : op_(NULL){};

    Verify(const Verify &) = delete;

    ~Verify()
    {
        botan_pk_op_verify_destroy(op_);
    }

    botan_pk_op_verify_t &
    get() noexcept
    {
        return op_;
    };
};

class Sign {
    botan_pk_op_sign_t op_;

  public:
    Sign() : op_(NULL){};

    Sign(const Sign &) = delete;

    ~Sign()
    {
        botan_pk_op_sign_destroy(op_);
    }

    botan_pk_op_sign_t &
    get() noexcept
    {
        return op_;
    };
};

class KeyAgreement {
    botan_pk_op_ka_t op_;

  public:
    KeyAgreement() : op_(NULL){};

    KeyAgreement(const KeyAgreement &) = delete;

    ~KeyAgreement()
    {
        botan_pk_op_key_agreement_destroy(op_);
    }

    botan_pk_op_ka_t &
    get() noexcept
    {
        return op_;
    };
};

} // namespace op
} // namespace botan
} // namespace rnp

#endif

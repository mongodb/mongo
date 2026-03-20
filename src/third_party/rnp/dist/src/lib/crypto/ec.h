/*-
 * Copyright (c) 2017-2024 Ribose Inc.
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
#ifndef EC_H_
#define EC_H_

#include "config.h"
#include <rnp/rnp_def.h>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include "crypto/mpi.hpp"
#include "crypto/mem.h"
#include "utils.h"
#include <vector>

#define MAX_CURVE_BIT_SIZE 521 // secp521r1
/* Maximal byte size of elliptic curve order (NIST P-521) */
#define MAX_CURVE_BYTELEN ((MAX_CURVE_BIT_SIZE + 7) / 8)

namespace pgp {
namespace ec {
/**
 * Structure holds description of elliptic curve
 */
class Curve {
  public:
    const pgp_curve_t          rnp_curve_id;
    const size_t               bitlen;
    const std::vector<uint8_t> OID;
#if defined(CRYPTO_BACKEND_BOTAN)
    const char *botan_name;
#endif
#if defined(CRYPTO_BACKEND_OPENSSL)
    const char *openssl_name;
#endif
    const char *pgp_name;
    /* Curve is supported for keygen/sign/encrypt operations */
    bool supported;
    /* Curve parameters below. Needed for grip calculation */
    const char *p;
    const char *a;
    const char *b;
    const char *n;
    const char *gx;
    const char *gy;
    const char *h;

    /**
     * @brief   Finds curve ID by hex representation of OID
     * @param   oid       buffer with OID in hex
     * @returns success curve ID
     *          failure PGP_CURVE_MAX is returned
     * @remarks see RFC 4880 bis 01 - 9.2 ECC Curve OID
     */
    static pgp_curve_t by_OID(const std::vector<uint8_t> &oid);

    static pgp_curve_t by_name(const char *name);

    /**
     * @brief   Returns pointer to the curve descriptor
     *
     * @param   Valid curve ID
     *
     * @returns NULL if wrong ID provided, otherwise descriptor
     *
     */
    static const Curve *get(const pgp_curve_t curve_id);

    static bool alg_allows(pgp_pubkey_alg_t alg, pgp_curve_t curve);

    /**
     * @brief Check whether curve is supported for operations.
     *        All available curves are supported for reading/parsing key data, however some of
     * them may be disabled for use, i.e. for key generation/signing/encryption.
     */
    static bool is_supported(pgp_curve_t curve);

    size_t
    bytes() const noexcept
    {
        return BITS_TO_BYTES(bitlen);
    }
};

class Signature {
  public:
    mpi r{};
    mpi s{};
};

class Key {
  public:
    pgp_curve_t curve;
    mpi         p;
    /* secret mpi */
    mpi x;
    /* ecdh params */
    pgp_hash_alg_t kdf_hash_alg; /* Hash used by kdf */
    pgp_symm_alg_t key_wrap_alg; /* Symmetric algorithm used to wrap KEK*/

    void
    clear_secret()
    {
        x.forget();
    }

    ~Key()
    {
        clear_secret();
    }

    /**
     * @brief   Generates EC key in uncompressed format
     *
     * @param   rng initialized rnp::RNG context
     * @param   alg_id ID of EC algorithm
     * @param   curve underlying ECC curve ID
     *
     * @pre     alg_id MUST be supported algorithm
     *
     * @returns RNP_ERROR_BAD_PARAMETERS unknown curve_id
     * @returns RNP_ERROR_OUT_OF_MEMORY memory allocation failed
     * @returns RNP_ERROR_KEY_GENERATION implementation error
     */
    rnp_result_t generate(rnp::RNG &             rng,
                          const pgp_pubkey_alg_t alg_id,
                          const pgp_curve_t      curve);

    /**
     * @brief   Generates x25519 ECDH key in x25519-specific format
     *
     * @param   rng initialized rnp::RNG context*
     *
     * @returns RNP_ERROR_KEY_GENERATION implementation error
     */
    rnp_result_t generate_x25519(rnp::RNG &rng);
};

} // namespace ec
} // namespace pgp

/**
 * @brief Set least significant/most significant bits of the 25519 secret key as per
 *        specification.
 *
 * @param key secret key.
 * @return true on success or false otherwise.
 */
bool x25519_tweak_bits(pgp::ec::Key &key);

/**
 * @brief Check whether least significant/most significant bits of 25519 secret key are
 *        correctly tweaked.
 *
 * @param key secret key.
 * @return true if bits are set correctly, and false otherwise.
 */
bool x25519_bits_tweaked(const pgp::ec::Key &key);

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
typedef struct pgp_ed25519_key_t {
    std::vector<uint8_t> pub;  // \  native encoding
    std::vector<uint8_t> priv; // /

    void
    clear_secret()
    {
        secure_clear(priv.data(), priv.size());
        priv.resize(0);
    }

    ~pgp_ed25519_key_t()
    {
        clear_secret();
    }
} pgp_ed25519_key_t;

typedef struct pgp_ed25519_signature_t {
    std::vector<uint8_t> sig; // native encoding
} pgp_ed25519_signature_t;

typedef struct pgp_x25519_key_t {
    std::vector<uint8_t> pub;  // \  native encoding
    std::vector<uint8_t> priv; // /

    void
    clear_secret()
    {
        secure_clear(priv.data(), priv.size());
        priv.resize(0);
    }

    ~pgp_x25519_key_t()
    {
        clear_secret();
    }
} pgp_x25519_key_t;

typedef struct pgp_x25519_encrypted_t {
    std::vector<uint8_t> eph_key;
    std::vector<uint8_t> enc_sess_key;
} pgp_x25519_encrypted_t;

/*
 * @brief   Generates EC keys in "native" or SEC1-encoded uncompressed format
 *
 * @param   rng initialized rnp::RNG context*
 * @param   privkey private key to be generated
 * @param   pubkey public key to be generated
 * @param   curve chosen curve
 * @param   alg algorithm id
 *
 * @returns RNP_ERROR_BAD_PARAMETERS if the curve or alg parameter is invalid.
 */
rnp_result_t ec_generate_native(rnp::RNG *            rng,
                                std::vector<uint8_t> &privkey,
                                std::vector<uint8_t> &pubkey,
                                pgp_curve_t           curve,
                                pgp_pubkey_alg_t      alg);
#endif

#endif

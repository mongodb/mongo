/*
 * Copyright (c) 2021-2024, [Ribose Inc](https://www.ribose.com).
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

#include <string>
#include <cassert>
#include "ecdh.h"
#include "ecdh_utils.h"
#include "ec_ossl.h"
#include "hash.hpp"
#include "symmetric.h"
#include "types.h"
#include "utils.h"
#include "logging.h"
#include "mem.h"
#include <openssl/evp.h>
#include <openssl/err.h>

namespace pgp {
namespace ecdh {

static const struct ecdh_wrap_alg_map_t {
    pgp_symm_alg_t alg;
    const char *   name;
} ecdh_wrap_alg_map[] = {{PGP_SA_AES_128, "aes128-wrap"},
                         {PGP_SA_AES_192, "aes192-wrap"},
                         {PGP_SA_AES_256, "aes256-wrap"}};

rnp_result_t
validate_key(rnp::RNG &rng, const ec::Key &key, bool secret)
{
    return ec::validate_key(key, secret);
}

static rnp_result_t
derive_kek(rnp::secure_bytes &         x,
           const ec::Key &             key,
           const std::vector<uint8_t> &fp,
           rnp::secure_bytes &         kek)
{
    auto curve_desc = ec::Curve::get(key.curve);
    if (!curve_desc) {
        RNP_LOG("unsupported curve");
        return RNP_ERROR_NOT_SUPPORTED;
    }

    // Serialize other info, see 13.5 of RFC 4880 bis
    const size_t hash_len = rnp::Hash::size(key.kdf_hash_alg);
    if (!hash_len) {
        // must not assert here as kdf/hash algs are not checked during key parsing
        /* LCOV_EXCL_START */
        RNP_LOG("Unsupported key wrap hash algorithm.");
        return RNP_ERROR_NOT_SUPPORTED;
        /* LCOV_EXCL_END */
    }
    auto other_info =
      kdf_other_info_serialize(*curve_desc, fp, key.kdf_hash_alg, key.key_wrap_alg);
    // Self-check
    assert(other_info.size() == curve_desc->OID.size() + 46);
    // Derive KEK, using the KDF from SP800-56A
    rnp::secure_array<uint8_t, PGP_MAX_HASH_SIZE> dgst;
    assert(hash_len <= PGP_MAX_HASH_SIZE);
    size_t reps = (kek.size() + hash_len - 1) / hash_len;
    // As we use AES & SHA2 we should not get more then 2 iterations
    if (reps > 2) {
        /* LCOV_EXCL_START */
        RNP_LOG("Invalid key wrap/hash alg combination.");
        return RNP_ERROR_NOT_SUPPORTED;
        /* LCOV_EXCL_END */
    }
    size_t have = 0;
    for (size_t i = 1; i <= reps; i++) {
        auto hash = rnp::Hash::create(key.kdf_hash_alg);
        hash->add(i);
        hash->add(x.data(), x.size());
        hash->add(other_info);
        hash->finish(dgst.data());
        size_t bytes = std::min(hash_len, kek.size() - have);
        memcpy(kek.data() + have, dgst.data(), bytes);
        have += bytes;
    }
    return RNP_SUCCESS;
}

static rnp_result_t
rfc3394_wrap_ctx(rnp::ossl::evp::CipherCtx &ctx,
                 pgp_symm_alg_t             wrap_alg,
                 const rnp::secure_bytes &  key,
                 bool                       decrypt)
{
    /* get OpenSSL EVP cipher for key wrap */
    const char *cipher_name = NULL;
    ARRAY_LOOKUP_BY_ID(ecdh_wrap_alg_map, alg, name, wrap_alg, cipher_name);
    if (!cipher_name) {
        /* LCOV_EXCL_START */
        RNP_LOG("Unsupported key wrap algorithm: %d", (int) wrap_alg);
        return RNP_ERROR_NOT_SUPPORTED;
        /* LCOV_EXCL_END */
    }
    const EVP_CIPHER *cipher = EVP_get_cipherbyname(cipher_name);
    if (!cipher) {
        /* LCOV_EXCL_START */
        RNP_LOG("Cipher %s is not supported by OpenSSL.", cipher_name);
        return RNP_ERROR_NOT_SUPPORTED;
        /* LCOV_EXCL_END */
    }
    ctx.reset(EVP_CIPHER_CTX_new());
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Context allocation failed : %lu", ERR_peek_last_error());
        return RNP_ERROR_OUT_OF_MEMORY;
        /* LCOV_EXCL_END */
    }
    EVP_CIPHER_CTX_set_flags(ctx.get(), EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    int res = decrypt ? EVP_DecryptInit_ex(ctx.get(), cipher, NULL, key.data(), NULL) :
                        EVP_EncryptInit_ex(ctx.get(), cipher, NULL, key.data(), NULL);
    if (res <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to initialize cipher : %lu", ERR_peek_last_error());
        ctx.reset();
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}

static rnp_result_t
rfc3394_wrap(std::vector<uint8_t> &   out,
             const rnp::secure_bytes &in,
             const rnp::secure_bytes &key,
             pgp_symm_alg_t           wrap_alg)
{
    rnp::ossl::evp::CipherCtx ctx;
    rnp_result_t              ret = rfc3394_wrap_ctx(ctx, wrap_alg, key, false);
    if (ret) {
        /* LCOV_EXCL_START */
        RNP_LOG("Wrap context initialization failed.");
        return ret;
        /* LCOV_EXCL_END */
    }
    int intlen = out.size();
    /* encrypts in one pass, no final is needed */
    int res = EVP_EncryptUpdate(ctx.get(), out.data(), &intlen, in.data(), in.size());
    if (res <= 0) {
        RNP_LOG("Failed to encrypt data : %lu", ERR_peek_last_error()); // LCOV_EXCL_LINE
        return RNP_ERROR_GENERIC;
    }
    out.resize(intlen);
    return RNP_SUCCESS;
}

static rnp_result_t
rfc3394_unwrap(rnp::secure_bytes &         out,
               const std::vector<uint8_t> &in,
               const rnp::secure_bytes &   key,
               pgp_symm_alg_t              wrap_alg)
{
    if ((in.size() < 16) || (in.size() % 8)) {
        RNP_LOG("Invalid wrapped key size.");
        return RNP_ERROR_GENERIC;
    }
    rnp::ossl::evp::CipherCtx ctx;
    rnp_result_t              ret = rfc3394_wrap_ctx(ctx, wrap_alg, key, true);
    if (ret) {
        /* LCOV_EXCL_START */
        RNP_LOG("Unwrap context initialization failed.");
        return ret;
        /* LCOV_EXCL_END */
    }
    int intlen = out.size();
    /* decrypts in one pass, no final is needed */
    int res = EVP_DecryptUpdate(ctx.get(), out.data(), &intlen, in.data(), in.size());
    if (res <= 0) {
        RNP_LOG("Failed to decrypt data : %lu", ERR_peek_last_error());
        return RNP_ERROR_GENERIC;
    }
    out.resize(intlen);
    return RNP_SUCCESS;
}

static bool
derive_secret(rnp::ossl::evp::PKey &sec, rnp::ossl::evp::PKey &peer, rnp::secure_bytes &x)
{
    rnp::ossl::evp::PKeyCtx ctx(EVP_PKEY_CTX_new(sec.get(), NULL));
    if (!ctx) {
        /* LCOV_EXCL_START */
        RNP_LOG("Context allocation failed: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_derive_init(ctx.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Key derivation init failed: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    if (EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Peer setting failed: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    x.resize(MAX_CURVE_BYTELEN + 1);
    size_t xlen = x.size();
    if (EVP_PKEY_derive(ctx.get(), x.data(), &xlen) <= 0) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to obtain shared secret size: %lu", ERR_peek_last_error());
        return false;
        /* LCOV_EXCL_END */
    }
    x.resize(xlen);
    return true;
}

static size_t
ecdh_kek_len(pgp_symm_alg_t wrap_alg)
{
    switch (wrap_alg) {
    case PGP_SA_AES_128:
    case PGP_SA_AES_192:
    case PGP_SA_AES_256:
        return pgp_key_size(wrap_alg);
    default:
        return 0;
    }
}

rnp_result_t
encrypt_pkcs5(rnp::RNG &rng, Encrypted &out, const rnp::secure_bytes &in, const ec::Key &key)
{
    if (in.size() > MAX_SESSION_KEY_SIZE) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
#if !defined(ENABLE_SM2)
    if (key.curve == PGP_CURVE_SM2_P_256) {
        RNP_LOG("SM2 curve support is disabled.");
        return RNP_ERROR_NOT_IMPLEMENTED;
    }
#endif
    /* check whether we have valid wrap_alg before doing heavy operations */
    size_t keklen = ecdh_kek_len(key.key_wrap_alg);
    if (!keklen) {
        /* LCOV_EXCL_START */
        RNP_LOG("Unsupported key wrap algorithm: %d", (int) key.key_wrap_alg);
        return RNP_ERROR_NOT_SUPPORTED;
        /* LCOV_EXCL_END */
    }
    /* load our public key */
    auto pkey = ec::load_key(key.p, NULL, key.curve);
    if (!pkey) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to load public key.");
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }

    /* generate ephemeral key */
    auto ephkey = ec::generate_pkey(PGP_PKA_ECDH, key.curve);
    if (!ephkey) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to generate ephemeral key.");
        return RNP_ERROR_KEY_GENERATION;
        /* LCOV_EXCL_END */
    }
    /* do ECDH derivation */
    rnp::secure_bytes sec;
    if (!derive_secret(ephkey, pkey, sec)) {
        /* LCOV_EXCL_START */
        RNP_LOG("ECDH derivation failed.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* here we got x value in sec, deriving kek */
    rnp::secure_bytes kek(keklen, 0);
    auto              ret = derive_kek(sec, key, out.fp, kek);
    if (ret) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to derive KEK.");
        return ret;
        /* LCOV_EXCL_END */
    }
    /* add PKCS#7 padding */
    size_t            m_padded_len = ((in.size() / 8) + 1) * 8;
    rnp::secure_bytes mpad(in.begin(), in.end());
    pad_pkcs7(mpad, m_padded_len - in.size());
    /* do RFC 3394 AES key wrap */
    out.m.resize(ECDH_WRAPPED_KEY_SIZE);
    ret = rfc3394_wrap(out.m, mpad, kek, key.key_wrap_alg);
    if (ret) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to wrap key.");
        return ret;
        /* LCOV_EXCL_END */
    }
    /* write ephemeral public key */
    if (!ec::write_pubkey(ephkey, out.p, key.curve)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to write ec key.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    return RNP_SUCCESS;
}

rnp_result_t
decrypt_pkcs5(rnp::secure_bytes &out, const Encrypted &in, const ec::Key &key)
{
    if (!key.x.size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* check whether we have valid wrap_alg before doing heavy operations */
    size_t keklen = ecdh_kek_len(key.key_wrap_alg);
    if (!keklen) {
        RNP_LOG("Unsupported key wrap algorithm: %d", (int) key.key_wrap_alg);
        return RNP_ERROR_NOT_SUPPORTED;
    }
    /* load ephemeral public key */
    auto ephkey = ec::load_key(in.p, nullptr, key.curve);
    if (!ephkey) {
        RNP_LOG("Failed to load ephemeral public key.");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    /* load our secret key */
    auto pkey = ec::load_key(key.p, &key.x, key.curve);
    if (!pkey) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to load secret key.");
        return RNP_ERROR_BAD_PARAMETERS;
        /* LCOV_EXCL_END */
    }
    /* do ECDH derivation */
    rnp::secure_bytes sec;
    if (!derive_secret(pkey, ephkey, sec)) {
        /* LCOV_EXCL_START */
        RNP_LOG("ECDH derivation failed.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    /* here we got x value in sec, deriving kek */
    rnp::secure_bytes kek(keklen, 0);
    auto              ret = derive_kek(sec, key, in.fp, kek);
    if (ret) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to derive KEK.");
        return ret;
        /* LCOV_EXCL_END */
    }
    /* do RFC 3394 AES key unwrap */
    rnp::secure_bytes mpad(MAX_SESSION_KEY_SIZE, 0);
    ret = rfc3394_unwrap(mpad, in.m, kek, key.key_wrap_alg);
    if (ret) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to unwrap key.");
        return ret;
        /* LCOV_EXCL_END */
    }
    /* remove PKCS#7 padding */
    if (!unpad_pkcs7(mpad)) {
        /* LCOV_EXCL_START */
        RNP_LOG("Failed to unpad key.");
        return RNP_ERROR_GENERIC;
        /* LCOV_EXCL_END */
    }
    out.assign(mpad.begin(), mpad.end());
    return RNP_SUCCESS;
}

} // namespace ecdh
} // namespace pgp

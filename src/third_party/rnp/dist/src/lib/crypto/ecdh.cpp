/*-
 * Copyright (c) 2017-2022 Ribose Inc.
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

#include <string.h>
#include <cassert>
#include <botan/ffi.h>
#include "hash_botan.hpp"
#include "botan_utils.hpp"
#include "ecdh.h"
#include "ec.h"
#include "ecdh_utils.h"
#include "symmetric.h"
#include "types.h"
#include "utils.h"
#include "mem.h"

namespace pgp {
namespace ecdh {
// Produces kek of size kek_len which corresponds to length of wrapping key
static bool
compute_kek(uint8_t *                   kek,
            size_t                      kek_len,
            const std::vector<uint8_t> &other_info,
            const ec::Curve *           curve_desc,
            const mpi &                 ec_pubkey,
            const rnp::botan::Privkey & ec_prvkey,
            const pgp_hash_alg_t        hash_alg)
{
    auto * p = ec_pubkey.data();
    size_t p_len = ec_pubkey.size();

    if (curve_desc->rnp_curve_id == PGP_CURVE_25519) {
        if ((p_len != 33) || (p[0] != 0x40)) {
            return false;
        }
        p++;
        p_len--;
    }

    rnp::secure_bytes            s(MAX_CURVE_BYTELEN * 2 + 1, 0);
    size_t                       s_len = s.size();
    rnp::botan::op::KeyAgreement op;
    if (botan_pk_op_key_agreement_create(&op.get(), ec_prvkey.get(), "Raw", 0) ||
        botan_pk_op_key_agreement(op.get(), s.data(), &s_len, p, p_len, NULL, 0)) {
        return false;
    }

    char kdf_name[32] = {0};
    snprintf(
      kdf_name, sizeof(kdf_name), "SP800-56A(%s)", rnp::Hash_Botan::name_backend(hash_alg));
    return !botan_kdf(
      kdf_name, kek, kek_len, s.data(), s_len, NULL, 0, other_info.data(), other_info.size());
}

static bool
load_public_key(rnp::botan::Pubkey &pubkey, const ec::Key &key)
{
    auto curve = ec::Curve::get(key.curve);
    if (!curve) {
        RNP_LOG("unknown curve");
        return false;
    }

    if (curve->rnp_curve_id == PGP_CURVE_25519) {
        if ((key.p.size() != 33) || (key.p[0] != 0x40)) {
            return false;
        }
        rnp::secure_array<uint8_t, 32> pkey;
        memcpy(pkey.data(), key.p.data() + 1, 32);
        return !botan_pubkey_load_x25519(&pubkey.get(), pkey.data());
    }

    if (!key.p.size() || (key.p[0] != 0x04)) {
        RNP_LOG("Failed to load public key");
        return false;
    }

    const size_t curve_order = curve->bytes();
    rnp::bn      px(&key.p[1], curve_order);
    rnp::bn      py(&key.p[1 + curve_order], curve_order);

    if (!px || !py) {
        return false;
    }

    if (!botan_pubkey_load_ecdh(&pubkey.get(), px.get(), py.get(), curve->botan_name)) {
        return true;
    }
    RNP_LOG("failed to load ecdh public key");
    return false;
}

static bool
load_secret_key(rnp::botan::Privkey &seckey, const ec::Key &key)
{
    auto curve = ec::Curve::get(key.curve);
    if (!curve) {
        return false;
    }

    if (curve->rnp_curve_id == PGP_CURVE_25519) {
        if (key.x.size() != 32) {
            RNP_LOG("wrong x25519 key");
            return false;
        }
        /* need to reverse byte order since in mpi we have big-endian */
        rnp::secure_array<uint8_t, 32> prkey;
        for (int i = 0; i < 32; i++) {
            prkey[i] = key.x[31 - i];
        }
        return !botan_privkey_load_x25519(&seckey.get(), prkey.data());
    }

    rnp::bn bx(key.x);
    return bx && !botan_privkey_load_ecdh(&seckey.get(), bx.get(), curve->botan_name);
}

rnp_result_t
validate_key(rnp::RNG &rng, const ec::Key &key, bool secret)
{
    auto curve_desc = ec::Curve::get(key.curve);
    if (!curve_desc) {
        return RNP_ERROR_NOT_SUPPORTED;
    }

    rnp::botan::Pubkey bpkey;
    if (!load_public_key(bpkey, key) || botan_pubkey_check_key(bpkey.get(), rng.handle(), 0)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    if (!secret) {
        return RNP_SUCCESS;
    }

    rnp::botan::Privkey bskey;
    if (!load_secret_key(bskey, key) ||
        botan_privkey_check_key(bskey.get(), rng.handle(), 0)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return RNP_SUCCESS;
}

rnp_result_t
encrypt_pkcs5(rnp::RNG &rng, Encrypted &out, const rnp::secure_bytes &in, const ec::Key &key)
{
    if (in.size() > MAX_SESSION_KEY_SIZE) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    const size_t m_padded_len = ((in.size() / 8) + 1) * 8;
    // +8 because of AES-wrap adds 8 bytes
    if (ECDH_WRAPPED_KEY_SIZE < (m_padded_len + 8)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

#if !defined(ENABLE_SM2)
    if (key.curve == PGP_CURVE_SM2_P_256) {
        RNP_LOG("SM2 curve support is disabled.");
        return RNP_ERROR_NOT_IMPLEMENTED;
    }
#endif
    auto curve_desc = ec::Curve::get(key.curve);
    if (!curve_desc) {
        RNP_LOG("unsupported curve");
        return RNP_ERROR_NOT_SUPPORTED;
    }

    // See 13.5 of RFC 4880 for definition of other_info size
    const size_t kek_len = pgp_key_size(key.key_wrap_alg);
    auto         other_info =
      kdf_other_info_serialize(*curve_desc, out.fp, key.kdf_hash_alg, key.key_wrap_alg);
    assert(other_info.size() == curve_desc->OID.size() + 46);

    rnp::botan::Privkey eph_prv_key;
    int                 res = 0;
    if (!strcmp(curve_desc->botan_name, "curve25519")) {
        res = botan_privkey_create(&eph_prv_key.get(), "Curve25519", "", rng.handle());
    } else {
        res = botan_privkey_create(
          &eph_prv_key.get(), "ECDH", curve_desc->botan_name, rng.handle());
    }
    if (res) {
        return RNP_ERROR_GENERIC;
    }

    uint8_t kek[32] = {0}; // Size of SHA-256 or smaller
    if (!compute_kek(
          kek, kek_len, other_info, curve_desc, key.p, eph_prv_key, key.kdf_hash_alg)) {
        RNP_LOG("KEK computation failed");
        return RNP_ERROR_GENERIC;
    }

    // 'm' is padded to the 8-byte granularity
    rnp::secure_bytes m = in;
    pad_pkcs7(m, m_padded_len - m.size());

    size_t mlen = ECDH_WRAPPED_KEY_SIZE;
    out.m.resize(ECDH_WRAPPED_KEY_SIZE);
#if defined(CRYPTO_BACKEND_BOTAN3)
    char name[16];
    snprintf(name, sizeof(name), "AES-%zu", 8 * kek_len);
    if (botan_nist_kw_enc(name, 0, m.data(), m.size(), kek, kek_len, out.m.data(), &mlen)) {
#else
    if (botan_key_wrap3394(m.data(), m.size(), kek, kek_len, out.m.data(), &mlen)) {
#endif
        return RNP_ERROR_GENERIC;
    }
    out.m.resize(mlen);

    /* export ephemeral public key */
    out.p.resize(MAX_CURVE_BYTELEN * 2 + 1);
    size_t plen = out.p.size();
    /* we need to prepend 0x40 for the x25519 */
    if (key.curve == PGP_CURVE_25519) {
        plen--;
        if (botan_pk_op_key_agreement_export_public(
              eph_prv_key.get(), out.p.data() + 1, &plen)) {
            return RNP_ERROR_GENERIC;
        }
        out.p[0] = 0x40;
        out.p.resize(plen + 1);
    } else {
        if (botan_pk_op_key_agreement_export_public(eph_prv_key.get(), out.p.data(), &plen)) {
            return RNP_ERROR_GENERIC;
        }
        out.p.resize(plen);
    }
    // All OK
    return RNP_SUCCESS;
}

rnp_result_t
decrypt_pkcs5(rnp::secure_bytes &out, const Encrypted &in, const ec::Key &key)
{
    if (!key.x.size()) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto curve_desc = ec::Curve::get(key.curve);
    if (!curve_desc) {
        RNP_LOG("unknown curve");
        return RNP_ERROR_NOT_SUPPORTED;
    }

    auto wrap_alg = key.key_wrap_alg;
    auto kdf_hash = key.kdf_hash_alg;
    /* Ensure that AES is used for wrapping */
    if (!pgp_is_sa_aes(wrap_alg)) {
        RNP_LOG("non-aes wrap algorithm");
        return RNP_ERROR_NOT_SUPPORTED;
    }

    // See 13.5 of RFC 4880 for definition of other_info_size
    auto other_info = kdf_other_info_serialize(*curve_desc, in.fp, kdf_hash, wrap_alg);
    assert(other_info.size() == curve_desc->OID.size() + 46);

    rnp::botan::Privkey prv_key;
    if (!load_secret_key(prv_key, key)) {
        RNP_LOG("failed to load ecdh secret key");
        return RNP_ERROR_GENERIC;
    }

    // Size of SHA-256 or smaller
    rnp::secure_array<uint8_t, MAX_SYMM_KEY_SIZE> kek;

    /* Security: Always return same error code in case compute_kek,
     *           botan_key_unwrap3394 or unpad_pkcs7 fails
     */
    size_t kek_len = pgp_key_size(wrap_alg);
    if (!compute_kek(kek.data(), kek_len, other_info, curve_desc, in.p, prv_key, kdf_hash)) {
        return RNP_ERROR_GENERIC;
    }

    size_t deckey_len = MAX_SESSION_KEY_SIZE;
    out.resize(deckey_len);
#if defined(CRYPTO_BACKEND_BOTAN3)
    char name[16];
    snprintf(name, sizeof(name), "AES-%zu", 8 * kek_len);
    if (botan_nist_kw_dec(
          name, 0, in.m.data(), in.m.size(), kek.data(), kek_len, out.data(), &deckey_len)) {
#else
    if (botan_key_unwrap3394(
          in.m.data(), in.m.size(), kek.data(), kek_len, out.data(), &deckey_len)) {
#endif
        return RNP_ERROR_GENERIC;
    }
    out.resize(deckey_len);
    if (!unpad_pkcs7(out)) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

} // namespace ecdh
} // namespace pgp

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
rnp_result_t
ecdh_kem_gen_keypair_native(rnp::RNG *            rng,
                            std::vector<uint8_t> &privkey,
                            std::vector<uint8_t> &pubkey,
                            pgp_curve_t           curve)
{
    return ec_generate_native(rng, privkey, pubkey, curve, PGP_PKA_ECDH);
}

rnp_result_t
exdsa_gen_keypair_native(rnp::RNG *            rng,
                         std::vector<uint8_t> &privkey,
                         std::vector<uint8_t> &pubkey,
                         pgp_curve_t           curve)
{
    pgp_pubkey_alg_t alg;
    switch (curve) {
    case PGP_CURVE_ED25519:
        alg = PGP_PKA_EDDSA;
        break;
    case PGP_CURVE_NIST_P_256:
        FALLTHROUGH_STATEMENT;
    case PGP_CURVE_NIST_P_384:
        FALLTHROUGH_STATEMENT;
    case PGP_CURVE_NIST_P_521:
        FALLTHROUGH_STATEMENT;
    case PGP_CURVE_BP256:
        FALLTHROUGH_STATEMENT;
    case PGP_CURVE_BP384:
        FALLTHROUGH_STATEMENT;
    case PGP_CURVE_BP512:
        FALLTHROUGH_STATEMENT;
    case PGP_CURVE_P256K1:
        alg = PGP_PKA_ECDSA;
        break;
    default:
        RNP_LOG("invalid curve for ECDSA/EDDSA");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    return ec_generate_native(rng, privkey, pubkey, curve, alg);
}

#endif

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

#include <string>
#include <cstring>
#include <botan/ffi.h>
#include "hash_botan.hpp"
#include "botan_utils.hpp"
#include "crypto/rsa.h"
#include "config.h"
#include "utils.h"

namespace pgp {
namespace rsa {

static bool
load_public_key(rnp::botan::Pubkey &bkey, const Key &key)
{
    rnp::bn n(key.n);
    rnp::bn e(key.e);

    if (!n || !e) {
        RNP_LOG("out of memory");
        return false;
    }
    return !botan_pubkey_load_rsa(&bkey.get(), n.get(), e.get());
}

static bool
load_secret_key(rnp::botan::Privkey &bkey, const Key &key)
{
    rnp::bn p(key.p);
    rnp::bn q(key.q);
    rnp::bn e(key.e);

    if (!p || !q || !e) {
        RNP_LOG("out of memory");
        return false;
    }
    /* p and q are reversed from normal usage in PGP */
    return !botan_privkey_load_rsa(&bkey.get(), q.get(), p.get(), e.get());
}

rnp_result_t
Key::validate(rnp::RNG &rng, bool secret) const noexcept
{
    /* load and check public key part */
    rnp::botan::Pubkey pkey;
    if (!load_public_key(pkey, *this) || botan_pubkey_check_key(pkey.get(), rng.handle(), 0)) {
        return RNP_ERROR_GENERIC;
    }

    if (!secret) {
        return RNP_SUCCESS;
    }

    /* load and check secret key part */
    rnp::botan::Privkey skey;
    if (!load_secret_key(skey, *this) ||
        botan_privkey_check_key(skey.get(), rng.handle(), 0)) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

rnp_result_t
Key::encrypt_pkcs1(rnp::RNG &rng, Encrypted &out, const rnp::secure_bytes &in) const noexcept
{
    rnp::botan::Pubkey rsa_key;
    if (!load_public_key(rsa_key, *this)) {
        RNP_LOG("failed to load key");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    out.m.resize(n.size());
    size_t                  mlen = out.m.size();
    rnp::botan::op::Encrypt enc_op;
    if (botan_pk_op_encrypt_create(&enc_op.get(), rsa_key.get(), "PKCS1v15", 0) ||
        botan_pk_op_encrypt(
          enc_op.get(), rng.handle(), out.m.data(), &mlen, in.data(), in.size())) {
        out.m.resize(0);
        return RNP_ERROR_GENERIC;
    }
    out.m.resize(mlen);
    return RNP_SUCCESS;
}

rnp_result_t
Key::verify_pkcs1(const Signature &        sig,
                  pgp_hash_alg_t           hash_alg,
                  const rnp::secure_bytes &hash) const noexcept
{
    rnp::botan::Pubkey rsa_key;
    if (!load_public_key(rsa_key, *this)) {
        RNP_LOG("failed to load key");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    char pad[64] = {0};
    snprintf(
      pad, sizeof(pad), "EMSA-PKCS1-v1_5(Raw,%s)", rnp::Hash_Botan::name_backend(hash_alg));

    rnp::botan::op::Verify verify_op;
    if (botan_pk_op_verify_create(&verify_op.get(), rsa_key.get(), pad, 0) ||
        botan_pk_op_verify_update(verify_op.get(), hash.data(), hash.size()) ||
        botan_pk_op_verify_finish(verify_op.get(), sig.s.data(), sig.s.size())) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    return RNP_SUCCESS;
}

rnp_result_t
Key::sign_pkcs1(rnp::RNG &               rng,
                Signature &              sig,
                pgp_hash_alg_t           hash_alg,
                const rnp::secure_bytes &hash) const noexcept
{
    if (!q.size()) {
        RNP_LOG("private key not set");
        return RNP_ERROR_GENERIC;
    }

    rnp::botan::Privkey rsa_key;
    if (!load_secret_key(rsa_key, *this)) {
        RNP_LOG("failed to load key");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    char pad[64] = {0};
    snprintf(
      pad, sizeof(pad), "EMSA-PKCS1-v1_5(Raw,%s)", rnp::Hash_Botan::name_backend(hash_alg));

    sig.s.resize(n.size());
    size_t               slen = sig.s.size();
    rnp::botan::op::Sign sign_op;
    if (botan_pk_op_sign_create(&sign_op.get(), rsa_key.get(), pad, 0) ||
        botan_pk_op_sign_update(sign_op.get(), hash.data(), hash.size()) ||
        botan_pk_op_sign_finish(sign_op.get(), rng.handle(), sig.s.data(), &slen)) {
        sig.s.resize(0);
        return RNP_ERROR_GENERIC;
    }
    sig.s.resize(slen);
    return RNP_SUCCESS;
}

rnp_result_t
Key::decrypt_pkcs1(rnp::RNG &rng, rnp::secure_bytes &out, const Encrypted &in) const noexcept
{
    if (!q.size()) {
        RNP_LOG("private key not set");
        return RNP_ERROR_GENERIC;
    }

    rnp::botan::Privkey rsa_key;
    if (!load_secret_key(rsa_key, *this)) {
        RNP_LOG("failed to load key");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    rnp::botan::op::Decrypt decrypt_op;
    if (botan_pk_op_decrypt_create(&decrypt_op.get(), rsa_key.get(), "PKCS1v15", 0)) {
        return RNP_ERROR_GENERIC;
    }
    /* Skip trailing zeroes if any as Botan3 doesn't like m.len > n.len */
    size_t skip = 0;
    while ((in.m.size() - skip > e.size()) && !in.m[skip]) {
        skip++;
    }
    out.resize(n.size());
    size_t out_len = out.size();
    if (botan_pk_op_decrypt(
          decrypt_op.get(), out.data(), &out_len, in.m.data() + skip, in.m.size() - skip)) {
        out.resize(0);
        return RNP_ERROR_GENERIC;
    }
    out.resize(out_len);
    return RNP_SUCCESS;
}

rnp_result_t
Key::generate(rnp::RNG &rng, size_t numbits) noexcept
{
    if ((numbits < 1024) || (numbits > PGP_MPINT_BITS)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::bn bn;
    rnp::bn be;
    rnp::bn bp;
    rnp::bn bq;
    rnp::bn bd;
    rnp::bn bu;
    if (!bn || !be || !bp || !bq || !bd || !bu) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    rnp::botan::Privkey rsa_key;
    if (botan_privkey_create(
          &rsa_key.get(), "RSA", std::to_string(numbits).c_str(), rng.handle()) ||
        botan_privkey_check_key(rsa_key.get(), rng.handle(), 1)) {
        return RNP_ERROR_GENERIC;
    }

    if (botan_privkey_get_field(bn.get(), rsa_key.get(), "n") ||
        botan_privkey_get_field(be.get(), rsa_key.get(), "e") ||
        botan_privkey_get_field(bd.get(), rsa_key.get(), "d") ||
        botan_privkey_get_field(bp.get(), rsa_key.get(), "p") ||
        botan_privkey_get_field(bq.get(), rsa_key.get(), "q")) {
        return RNP_ERROR_GENERIC;
    }

    /* RFC 4880, 5.5.3 tells that p < q. GnuPG relies on this. */
    int cmp = 0;
    (void) botan_mp_cmp(&cmp, bp.get(), bq.get());
    if (cmp > 0) {
        (void) botan_mp_swap(bp.get(), bq.get());
    }

    if (botan_mp_mod_inverse(bu.get(), bp.get(), bq.get()) != 0) {
        RNP_LOG("Error computing RSA u param");
        return RNP_ERROR_BAD_STATE;
    }

    bn.mpi(n);
    be.mpi(e);
    bp.mpi(p);
    bq.mpi(q);
    bd.mpi(d);
    bu.mpi(u);
    return RNP_SUCCESS;
}

} // namespace rsa
} // namespace pgp

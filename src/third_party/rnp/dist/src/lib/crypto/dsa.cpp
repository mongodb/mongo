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

#include <stdlib.h>
#include <string.h>
#include <vector>
#include <botan/ffi.h>
#include "botan_utils.hpp"
#include <rnp/rnp_def.h>
#include "dsa.h"
#include "utils.h"

namespace pgp {
namespace dsa {

rnp_result_t
Key::validate(rnp::RNG &rng, bool secret) const noexcept
{
    /* load and check public key part */
    rnp::bn bp(p);
    rnp::bn bq(q);
    rnp::bn bg(g);
    rnp::bn by(y);

    if (!bp || !bq || !bg || !by) {
        RNP_LOG("out of memory");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    rnp::botan::Pubkey pkey;
    if (botan_pubkey_load_dsa(&pkey.get(), bp.get(), bq.get(), bg.get(), by.get()) ||
        botan_pubkey_check_key(pkey.get(), rng.handle(), 0)) {
        return RNP_ERROR_GENERIC;
    }
    if (!secret) {
        return RNP_SUCCESS;
    }

    /* load and check secret key part */
    rnp::bn bx(x);
    if (!bx) {
        RNP_LOG("out of memory");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    rnp::botan::Privkey skey;
    if (botan_privkey_load_dsa(&skey.get(), bp.get(), bq.get(), bg.get(), bx.get()) ||
        botan_privkey_check_key(skey.get(), rng.handle(), 0)) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

rnp_result_t
Key::sign(rnp::RNG &rng, Signature &sig, const rnp::secure_bytes &hash) const
{
    sig = {};
    size_t q_order = q.size();
    // As 'Raw' is used we need to reduce hash size (as per FIPS-186-4, 4.6)
    size_t z_len = std::min(hash.size(), q_order);

    rnp::bn bp(p);
    rnp::bn bq(q);
    rnp::bn bg(g);
    rnp::bn bx(x);

    if (!bp || !bq || !bg || !bx) {
        RNP_LOG("out of memory");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    rnp::botan::Privkey dsa_key;
    if (botan_privkey_load_dsa(&dsa_key.get(), bp.get(), bq.get(), bg.get(), bx.get())) {
        RNP_LOG("Can't load key");
        return RNP_ERROR_SIGNING_FAILED;
    }

    rnp::botan::op::Sign sign_op;
    if (botan_pk_op_sign_create(&sign_op.get(), dsa_key.get(), "Raw", 0)) {
        return RNP_ERROR_SIGNING_FAILED;
    }

    if (botan_pk_op_sign_update(sign_op.get(), hash.data(), z_len)) {
        return RNP_ERROR_SIGNING_FAILED;
    }

    size_t               sigbuf_size = 2 * q_order;
    std::vector<uint8_t> sign_buf(sigbuf_size, 0);
    if (botan_pk_op_sign_finish(sign_op.get(), rng.handle(), sign_buf.data(), &sigbuf_size)) {
        RNP_LOG("Signing has failed");
        return RNP_ERROR_SIGNING_FAILED;
    }

    // Now load the DSA (r,s) values from the signature.
    sig.r.assign(sign_buf.data(), q_order);
    sig.s.assign(sign_buf.data() + q_order, q_order);
    return RNP_SUCCESS;
}

rnp_result_t
Key::verify(const Signature &sig, const rnp::secure_bytes &hash) const
{
    size_t q_order = q.size();
    size_t z_len = std::min(hash.size(), q_order);
    size_t r_blen = sig.r.size();
    size_t s_blen = sig.s.size();
    if ((r_blen > q_order) || (s_blen > q_order)) {
        RNP_LOG("Wrong signature");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::bn bp(p);
    rnp::bn bq(q);
    rnp::bn bg(g);
    rnp::bn by(y);

    if (!bp || !bq || !bg || !by) {
        RNP_LOG("out of memory");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    rnp::botan::Pubkey dsa_key;
    if (botan_pubkey_load_dsa(&dsa_key.get(), bp.get(), bq.get(), bg.get(), by.get())) {
        RNP_LOG("Wrong key");
        return RNP_ERROR_GENERIC;
    }

    std::vector<uint8_t> sign_buf(q_order * 2, 0);
    sig.r.copy(sign_buf.data() + q_order - r_blen);
    sig.s.copy(sign_buf.data() + 2 * q_order - s_blen);

    rnp::botan::op::Verify verify_op;
    if (botan_pk_op_verify_create(&verify_op.get(), dsa_key.get(), "Raw", 0)) {
        RNP_LOG("Can't create verifier");
        return RNP_ERROR_GENERIC;
    }

    if (botan_pk_op_verify_update(verify_op.get(), hash.data(), z_len)) {
        return RNP_ERROR_GENERIC;
    }

    if (botan_pk_op_verify_finish(verify_op.get(), sign_buf.data(), 2 * q_order)) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    return RNP_SUCCESS;
}

rnp_result_t
Key::generate(rnp::RNG &rng, size_t keylen, size_t qbits)
{
    if ((keylen < 1024) || (keylen > 3072) || (qbits < 160) || (qbits > 256)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::bn bp;
    rnp::bn bq;
    rnp::bn bg;
    rnp::bn by;
    rnp::bn bx;

    if (!bp || !bq || !bg || !by || !bx) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    rnp::botan::Privkey key_priv;
    rnp::botan::Pubkey  key_pub;
    if (botan_privkey_create_dsa(&key_priv.get(), rng.handle(), keylen, qbits) ||
        botan_privkey_check_key(key_priv.get(), rng.handle(), 1) ||
        botan_privkey_export_pubkey(&key_pub.get(), key_priv.get())) {
        RNP_LOG("Wrong parameters");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (botan_pubkey_get_field(bp.get(), key_pub.get(), "p") ||
        botan_pubkey_get_field(bq.get(), key_pub.get(), "q") ||
        botan_pubkey_get_field(bg.get(), key_pub.get(), "g") ||
        botan_pubkey_get_field(by.get(), key_pub.get(), "y") ||
        botan_privkey_get_field(bx.get(), key_priv.get(), "x")) {
        RNP_LOG("Botan FFI call failed");
        return RNP_ERROR_GENERIC;
    }

    if (!bp.mpi(p) || !bq.mpi(q) || !bg.mpi(g) || !by.mpi(y) || !bx.mpi(x)) {
        RNP_LOG("failed to copy mpi");
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

} // namespace dsa
} // namespace pgp

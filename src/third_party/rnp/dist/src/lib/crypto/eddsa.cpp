/*
 * Copyright (c) 2017-2024, [Ribose Inc](https://www.ribose.com).
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
#include "eddsa.h"
#include "utils.h"
#include "botan_utils.hpp"

namespace pgp {
namespace eddsa {

static bool
load_public_key(rnp::botan::Pubkey &pubkey, const ec::Key &keydata)
{
    if (keydata.curve != PGP_CURVE_ED25519) {
        return false;
    }
    /*
     * See draft-ietf-openpgp-rfc4880bis-01 section 13.3
     */
    if ((keydata.p.size() != 33) || (keydata.p[0] != 0x40)) {
        return false;
    }
    if (botan_pubkey_load_ed25519(&pubkey.get(), &keydata.p[1])) {
        return false;
    }
    return true;
}

static bool
load_secret_key(rnp::botan::Privkey &seckey, const ec::Key &keydata)
{
    if (keydata.curve != PGP_CURVE_ED25519) {
        return false;
    }
    size_t sz = keydata.x.size();
    if (!sz || (sz > 32)) {
        return false;
    }
    uint8_t keybuf[32] = {0};
    keydata.x.copy(keybuf + 32 - sz);
    if (botan_privkey_load_ed25519(&seckey.get(), keybuf)) {
        return false;
    }

    return true;
}

rnp_result_t
validate_key(rnp::RNG &rng, const ec::Key &key, bool secret)
{
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
generate(rnp::RNG &rng, ec::Key &key)
{
    rnp::botan::Privkey eddsa;
    if (botan_privkey_create(&eddsa.get(), "Ed25519", NULL, rng.handle())) {
        return RNP_ERROR_GENERIC;
    }

    uint8_t key_bits[64];
    if (botan_privkey_ed25519_get_privkey(eddsa.get(), key_bits)) {
        return RNP_ERROR_GENERIC;
    }

    // First 32 bytes of key_bits are the EdDSA seed (private key)
    // Second 32 bytes are the EdDSA public key
    key.x.assign(key_bits, 32);
    // insert the required 0x40 prefix on the public key
    key_bits[31] = 0x40;
    key.p.assign(key_bits + 31, 33);
    key.curve = PGP_CURVE_ED25519;
    return RNP_SUCCESS;
}

rnp_result_t
verify(const ec::Signature &sig, const rnp::secure_bytes &hash, const ec::Key &key)
{
    // Unexpected size for Ed25519 signature
    if ((sig.r.size() > 32) || (sig.s.size() > 32)) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }

    rnp::botan::Pubkey eddsa;
    if (!load_public_key(eddsa, key)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::botan::op::Verify verify_op;
    if (botan_pk_op_verify_create(&verify_op.get(), eddsa.get(), "Pure", 0) ||
        botan_pk_op_verify_update(verify_op.get(), hash.data(), hash.size())) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }

    uint8_t bn_buf[64] = {0};
    sig.r.copy(bn_buf + 32 - sig.r.size());
    sig.s.copy(bn_buf + 64 - sig.s.size());

    if (botan_pk_op_verify_finish(verify_op.get(), bn_buf, 64)) {
        return RNP_ERROR_SIGNATURE_INVALID;
    }
    return RNP_SUCCESS;
}

rnp_result_t
sign(rnp::RNG &rng, ec::Signature &sig, const rnp::secure_bytes &hash, const ec::Key &key)
{
    rnp::botan::Privkey eddsa;
    if (!load_secret_key(eddsa, key)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::botan::op::Sign sign_op;
    if (botan_pk_op_sign_create(&sign_op.get(), eddsa.get(), "Pure", 0) ||
        botan_pk_op_sign_update(sign_op.get(), hash.data(), hash.size())) {
        return RNP_ERROR_SIGNING_FAILED;
    }

    uint8_t bn_buf[64] = {0};
    size_t  sig_size = sizeof(bn_buf);
    if (botan_pk_op_sign_finish(sign_op.get(), rng.handle(), bn_buf, &sig_size)) {
        return RNP_ERROR_SIGNING_FAILED;
    }
    // Unexpected size...
    assert(sig_size == 64);
    sig.r.assign(bn_buf, 32);
    sig.s.assign(bn_buf + 32, 32);
    return RNP_SUCCESS;
}
} // namespace eddsa
} // namespace pgp

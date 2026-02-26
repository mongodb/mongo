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
#include <botan/bigint.h>
#include <botan/numthry.h>
#include <botan/reducer.h>
#include "botan_utils.hpp"
#include <rnp/rnp_def.h>
#include "elgamal.h"
#include "utils.h"

// Max supported key byte size
#define ELGAMAL_MAX_P_BYTELEN BITS_TO_BYTES(PGP_MPINT_BITS)

namespace pgp {
namespace eg {

static bool
load_public_key(rnp::botan::Pubkey &pubkey, const Key &key)
{
    // Check if provided public key byte size is not greater than ELGAMAL_MAX_P_BYTELEN.
    if (key.p.size() > ELGAMAL_MAX_P_BYTELEN) {
        return false;
    }

    rnp::bn p(key.p);
    rnp::bn g(key.g);
    rnp::bn y(key.y);

    if (!p || !g || !y) {
        return false;
    }
    return !botan_pubkey_load_elgamal(&pubkey.get(), p.get(), g.get(), y.get());
}

static bool
load_secret_key(rnp::botan::Privkey &privkey, const Key &key)
{
    // Check if provided secret key byte size is not greater than ELGAMAL_MAX_P_BYTELEN.
    if (key.p.size() > ELGAMAL_MAX_P_BYTELEN) {
        return false;
    }

    rnp::bn p(key.p);
    rnp::bn g(key.g);
    rnp::bn x(key.x);

    if (!p || !g || !x) {
        return false;
    }
    return !botan_privkey_load_elgamal(&privkey.get(), p.get(), g.get(), x.get());
}

bool
Key::validate(bool secret) const noexcept
{
    // Check if provided public key byte size is not greater than ELGAMAL_MAX_P_BYTELEN.
    if (p.size() > ELGAMAL_MAX_P_BYTELEN) {
        return false;
    }

    /* Use custom validation since we added some custom validation, and Botan has slow test for
     * prime for p */
    try {
        Botan::BigInt bp(p.data(), p.size());
        Botan::BigInt bg(g.data(), g.size());

        /* 1 < g < p */
        if ((bg.cmp_word(1) != 1) || (bg.cmp(bp) != -1)) {
            return false;
        }
        /* g ^ (p - 1) = 1 mod p */
        if (Botan::power_mod(bg, bp - 1, bp).cmp_word(1)) {
            return false;
        }
        /* check for small order subgroups */
        Botan::Modular_Reducer reducer(bp);
        Botan::BigInt          v = bg;
        for (size_t i = 2; i < (1 << 17); i++) {
            v = reducer.multiply(v, bg);
            if (!v.cmp_word(1)) {
                RNP_LOG("Small subgroup detected. Order %zu", i);
                return false;
            }
        }
        if (!secret) {
            return true;
        }
        /* check that g ^ x = y (mod p) */
        Botan::BigInt by(y.data(), y.size());
        Botan::BigInt bx(x.data(), x.size());
        return Botan::power_mod(bg, bx, bp) == by;
    } catch (const std::exception &e) {
        RNP_LOG("%s", e.what());
        return false;
    }
}

rnp_result_t
Key::encrypt_pkcs1(rnp::RNG &rng, Encrypted &out, const rnp::secure_bytes &in) const
{
    rnp::botan::Pubkey b_key;
    if (!load_public_key(b_key, *this)) {
        RNP_LOG("Failed to load public key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* Size of output buffer must be equal to twice the size of key byte len.
     * as ElGamal encryption outputs concatenation of two components, both
     * of size equal to size of public key byte len.
     * Successful call to botan's ElGamal encryption will return output that's
     * always 2*pubkey size.
     */
    size_t                  p_len = p.size() * 2;
    std::vector<uint8_t>    enc_buf(p_len, 0);
    rnp::botan::op::Encrypt op_ctx;

    if (botan_pk_op_encrypt_create(&op_ctx.get(), b_key.get(), "PKCS1v15", 0) ||
        botan_pk_op_encrypt(
          op_ctx.get(), rng.handle(), enc_buf.data(), &p_len, in.data(), in.size())) {
        RNP_LOG("Failed to create operation context");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /*
     * Botan's ElGamal formats the g^k and msg*(y^k) together into a single byte string.
     * We have to parse out the two values after encryption, as rnp stores those values
     * separately.
     *
     * We don't trim zeros from octet string as it is done before final marshalling
     * (add_packet_body_mpi)
     *
     * We must assume that botan copies even number of bytes to output buffer (to avoid
     * memory corruption)
     */
    p_len /= 2;
    out.g.assign(enc_buf.data(), p_len);
    out.m.assign(enc_buf.data() + p_len, p_len);
    return RNP_SUCCESS;
}

rnp_result_t
Key::decrypt_pkcs1(rnp::RNG &rng, rnp::secure_bytes &out, const Encrypted &in) const
{
    if (!x.size()) {
        RNP_LOG("empty secret key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // Check if provided public key byte size is not greater than ELGAMAL_MAX_P_BYTELEN.
    size_t p_len = p.size();
    size_t g_len = in.g.size();
    size_t m_len = in.m.size();

    if ((p_len > ELGAMAL_MAX_P_BYTELEN) || (g_len > p_len) || (m_len > p_len)) {
        RNP_LOG("Unsupported/wrong public key or encrypted data");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::botan::Privkey b_key;
    if (!load_secret_key(b_key, *this)) {
        RNP_LOG("Failed to load private key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /* Botan expects ciphertext to be concatenated (g^k | encrypted m). Size must
     * be equal to twice the byte size of public key, potentially prepended with zeros.
     */
    std::vector<uint8_t> enc_buf(2 * p_len, 0);
    in.g.copy(&enc_buf[p_len - g_len]);
    in.m.copy(&enc_buf[2 * p_len - m_len]);

    out.resize(p_len);
    size_t                  out_len = out.size();
    rnp::botan::op::Decrypt op_ctx;
    if (botan_pk_op_decrypt_create(&op_ctx.get(), b_key.get(), "PKCS1v15", 0) ||
        botan_pk_op_decrypt(op_ctx.get(), out.data(), &out_len, enc_buf.data(), 2 * p_len)) {
        out.resize(0);
        RNP_LOG("Decryption failed");
        return RNP_ERROR_BAD_PARAMETERS;
    }
    out.resize(out_len);
    return RNP_SUCCESS;
}

rnp_result_t
Key::generate(rnp::RNG &rng, size_t keybits)
{
    if ((keybits < 1024) || (keybits > PGP_MPINT_BITS)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    rnp::bn bp;
    rnp::bn bg;
    rnp::bn by;
    rnp::bn bx;

    if (!bp || !bg || !by || !bx) {
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    do {
        rnp::botan::Privkey key_priv;
        if (botan_privkey_create_elgamal(
              &key_priv.get(), rng.handle(), keybits, keybits - 1)) {
            RNP_LOG("Wrong parameters");
            return RNP_ERROR_BAD_PARAMETERS;
        }
        if (botan_privkey_get_field(by.get(), key_priv.get(), "y")) {
            RNP_LOG("Failed to obtain public key");
            return RNP_ERROR_GENERIC;
        }
        if (by.bytes() < BITS_TO_BYTES(keybits)) {
            /* This code line is rarely hit, so ignoring it for the coverage report: */
            continue; // LCOV_EXCL_LINE
        }

        if (botan_privkey_get_field(bp.get(), key_priv.get(), "p") ||
            botan_privkey_get_field(bg.get(), key_priv.get(), "g") ||
            botan_privkey_get_field(by.get(), key_priv.get(), "y") ||
            botan_privkey_get_field(bx.get(), key_priv.get(), "x")) {
            RNP_LOG("Botan FFI call failed");
            return RNP_ERROR_GENERIC;
        }
        break;
    } while (1);

    if (!bp.mpi(p) || !bg.mpi(g) || !by.mpi(y) || !bx.mpi(x)) {
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

} // namespace eg
} // namespace pgp

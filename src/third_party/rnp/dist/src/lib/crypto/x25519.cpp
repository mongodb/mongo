/*
 * Copyright (c) 2023, [MTG AG](https://www.mtg.de).
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

#include "x25519.h"
#include <botan/curve25519.h>
#if defined(ENABLE_CRYPTO_REFRESH)
#include "exdsa_ecdhkem.h"
#include "hkdf.hpp"
#include "utils.h"
#include "botan/rfc3394.h"

static void
x25519_hkdf(std::vector<uint8_t> &      derived_key,
            const std::vector<uint8_t> &ephemeral_pubkey_material,
            const std::vector<uint8_t> &recipient_pubkey_material,
            const std::vector<uint8_t> &shared_key)
{
    /* The shared secret is passed to HKDF (see {{RFC5869}}) using SHA256, and the
     * UTF-8-encoded string "OpenPGP X25519" as the info parameter. */
    static const std::vector<uint8_t> info = {
      'O', 'p', 'e', 'n', 'P', 'G', 'P', ' ', 'X', '2', '5', '5', '1', '9'};
    auto kdf = rnp::Hkdf::create(PGP_HASH_SHA256);
    derived_key.resize(pgp_key_size(PGP_SA_AES_128)); // 128-bit AES key wrap

    std::vector<uint8_t> kdf_input;
    kdf_input.insert(kdf_input.end(),
                     std::begin(ephemeral_pubkey_material),
                     std::end(ephemeral_pubkey_material));
    kdf_input.insert(kdf_input.end(),
                     std::begin(recipient_pubkey_material),
                     std::end(recipient_pubkey_material));
    kdf_input.insert(kdf_input.end(), std::begin(shared_key), std::end(shared_key));

    kdf->extract_expand(NULL,
                        0, // no salt
                        kdf_input.data(),
                        kdf_input.size(),
                        info.data(),
                        info.size(),
                        derived_key.data(),
                        derived_key.size());
}

rnp_result_t
x25519_native_encrypt(rnp::RNG *                  rng,
                      const std::vector<uint8_t> &pubkey,
                      const uint8_t *             in,
                      size_t                      in_len,
                      pgp_x25519_encrypted_t *    encrypted)
{
    rnp_result_t         ret;
    std::vector<uint8_t> shared_key;
    std::vector<uint8_t> derived_key;

    if (!in_len || (in_len % 8) != 0) {
        RNP_LOG("incorrect size of in, AES key wrap requires a multiple of 8 bytes");
        return RNP_ERROR_BAD_FORMAT;
    }

    /* encapsulation */
    ecdh_kem_public_key_t ecdhkem_pubkey(pubkey, PGP_CURVE_25519);
    ret = ecdhkem_pubkey.encapsulate(rng, encrypted->eph_key, shared_key);
    if (ret != RNP_SUCCESS) {
        RNP_LOG("encapsulation failed");
        return ret;
    }

    x25519_hkdf(derived_key, encrypted->eph_key, pubkey, shared_key);

    Botan::SymmetricKey kek(derived_key);
    try {
        encrypted->enc_sess_key = Botan::unlock(
          Botan::rfc3394_keywrap(Botan::secure_vector<uint8_t>(in, in + in_len), kek));
    } catch (const std::exception &e) {
        RNP_LOG("Keywrap failed: %s", e.what());
        return RNP_ERROR_ENCRYPT_FAILED;
    }

    return RNP_SUCCESS;
}

rnp_result_t
x25519_native_decrypt(rnp::RNG *                    rng,
                      const pgp_x25519_key_t &      keypair,
                      const pgp_x25519_encrypted_t *encrypted,
                      uint8_t *                     decbuf,
                      size_t *                      decbuf_len)
{
    rnp_result_t         ret;
    std::vector<uint8_t> shared_key;
    std::vector<uint8_t> derived_key;

    static const size_t x25519_pubkey_size = 32;
    if (encrypted->eph_key.size() != x25519_pubkey_size) {
        RNP_LOG("Wrong ephemeral public key size");
        return RNP_ERROR_BAD_FORMAT;
    }
    if (!encrypted->enc_sess_key.size()) {
        // TODO: could do a check for possible sizes
        RNP_LOG("No encrypted session key provided");
        return RNP_ERROR_BAD_FORMAT;
    }

    /* decapsulate */
    ecdh_kem_private_key_t ecdhkem_privkey(keypair.priv, PGP_CURVE_25519);
    ret = ecdhkem_privkey.decapsulate(rng, encrypted->eph_key, shared_key);
    if (ret != RNP_SUCCESS) {
        RNP_LOG("decapsulation failed");
        return ret;
    }

    x25519_hkdf(derived_key, encrypted->eph_key, keypair.pub, shared_key);

    Botan::SymmetricKey kek(derived_key);
    auto                tmp_out =
      Botan::rfc3394_keyunwrap(Botan::secure_vector<uint8_t>(encrypted->enc_sess_key.begin(),
                                                             encrypted->enc_sess_key.end()),
                               kek);
    if (*decbuf_len < tmp_out.size()) {
        RNP_LOG("buffer for decryption result too small");
        return RNP_ERROR_DECRYPT_FAILED;
    }
    *decbuf_len = tmp_out.size();
    memcpy(decbuf, tmp_out.data(), tmp_out.size());

    return RNP_SUCCESS;
}

rnp_result_t
x25519_validate_key_native(rnp::RNG *rng, const pgp_x25519_key_t *key, bool secret)
{
    bool valid_pub;
    bool valid_priv;

    Botan::Curve25519_PublicKey pub_key(key->priv);
    valid_pub = pub_key.check_key(*(rng->obj()), false);

    if (secret) {
        Botan::Curve25519_PrivateKey priv_key(
          Botan::secure_vector<uint8_t>(key->priv.begin(), key->priv.end()));
        valid_priv = priv_key.check_key(*(rng->obj()), false);
    } else {
        valid_priv = true;
    }

    // check key returns true for successful check
    return (valid_pub && valid_priv) ? RNP_SUCCESS : RNP_ERROR_BAD_PARAMETERS;
}

#endif

rnp_result_t
generate_x25519_native(rnp::RNG *            rng,
                       std::vector<uint8_t> &privkey,
                       std::vector<uint8_t> &pubkey)
{
    Botan::Curve25519_PrivateKey priv_key(*(rng->obj()));
    pubkey = priv_key.public_value();
    privkey = Botan::unlock(priv_key.raw_private_key_bits());

    return RNP_SUCCESS;
}

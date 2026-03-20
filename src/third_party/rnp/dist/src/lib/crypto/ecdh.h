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

#ifndef ECDH_H_
#define ECDH_H_

#include "crypto/ec.h"
#include <vector>

/* Max size of wrapped and obfuscated key size
 *
 * RNP pads a key with PKCS-5 always to 8 byte granularity,
 * then 8 bytes is added by AES-wrap (RFC3394).
 */
#define ECDH_WRAPPED_KEY_SIZE 48

/* Forward declarations */

namespace pgp {
namespace ecdh {
class Encrypted {
  public:
    mpi                  p{};
    std::vector<uint8_t> m;
    std::vector<uint8_t> fp;
};

rnp_result_t validate_key(rnp::RNG &rng, const ec::Key &key, bool secret);

/*
 * @brief   Sets hash algorithm and key wrapping algo
 *          based on curve_id
 *
 * @param   key   ec key to set parameters for
 * @param   curve       underlying ECC curve ID
 *
 * @returns false if curve is not supported, otherwise true
 */
bool set_params(ec::Key &key, pgp_curve_t curve_id);

/*
 * Encrypts session key with a KEK agreed during ECDH as specified in
 * RFC 4880 bis 01, 13.5
 *
 * @param rng initialized rnp::RNG object
 * @param out [out] resulting key wrapped in by some AES
 *        as specified in RFC 3394
 * @param in data to be encrypted
 * @param key public key to be used for encryption
 *
 * @return RNP_SUCCESS on success and output parameters are populated
 * @return RNP_ERROR_NOT_SUPPORTED unknown curve
 * @return RNP_ERROR_BAD_PARAMETERS unexpected input provided
 * @return RNP_ERROR_SHORT_BUFFER `wrapped_key_len' to small to store result
 * @return RNP_ERROR_GENERIC implementation error
 */
rnp_result_t encrypt_pkcs5(rnp::RNG &               rng,
                           Encrypted &              out,
                           const rnp::secure_bytes &in,
                           const ec::Key &          key);

/*
 * Decrypts session key with a KEK agreed during ECDH as specified in
 * RFC 4880 bis 01, 13.5
 *
 * @param session_key [out] resulting session key
 * @param session_key_len [out] length of the resulting session key
 * @param wrapped_key session key wrapped with some AES as specified
 *        in RFC 3394
 * @param wrapped_key_len length of the `wrapped_key' buffer
 * @param ephemeral_key public ephemeral ECDH key coming from
 *        encrypted packet.
 * @param seckey secret key to be used for decryption
 * @param fingerprint fingerprint of the key
 *
 * @return RNP_SUCCESS on success and output parameters are populated
 * @return RNP_ERROR_NOT_SUPPORTED unknown curve
 * @return RNP_ERROR_BAD_PARAMETERS unexpected input provided
 * @return RNP_ERROR_SHORT_BUFFER `session_key_len' to small to store result
 * @return RNP_ERROR_GENERIC decryption failed or implementation error
 */
rnp_result_t decrypt_pkcs5(rnp::secure_bytes &out, const Encrypted &in, const ec::Key &key);
} // namespace ecdh
} // namespace pgp

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
/* Generate an ECDH key pair in "native" format, i.e.,
 * no changes to the format specified in the respective standard
 * are applied (uncompressed SEC1 and RFC 7748).
 *
 * @param rng initialized rnp::RNG object
 * @param privkey [out] the generated private key
 * @param pubkey [out] the generated public key
 * @param curve the curve for which a key pair is generated
 *
 * @return RNP_SUCCESS on success and output parameters are populated
 * @return RNP_ERROR_BAD_PARAMETERS unexpected input provided
 */
rnp_result_t ecdh_kem_gen_keypair_native(rnp::RNG *            rng,
                                         std::vector<uint8_t> &privkey,
                                         std::vector<uint8_t> &pubkey,
                                         pgp_curve_t           curve);

/* Generate an ECDSA or EdDSA key pair in "native" format, i.e.,
 * no changes to the format specified in the respective standard
 * are applied (uncompressed SEC1 and RFC 7748).
 *
 * @param rng initialized rnp::RNG object
 * @param privkey [out] the generated private key
 * @param pubkey [out] the generated public key
 * @param curve the curve for which a key pair is generated
 *
 * @return RNP_SUCCESS on success and output parameters are populated
 * @return RNP_ERROR_BAD_PARAMETERS unexpected input provided
 */
rnp_result_t exdsa_gen_keypair_native(rnp::RNG *            rng,
                                      std::vector<uint8_t> &privkey,
                                      std::vector<uint8_t> &pubkey,
                                      pgp_curve_t           curve);

#endif

#endif // ECDH_H_

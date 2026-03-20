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

#ifndef X25519_H_
#define X25519_H_

#include "config.h"
#include <rnp/rnp_def.h>
#include <vector>
#include <repgp/repgp_def.h>
#include "crypto/rng.h"
#include "crypto/ec.h"

rnp_result_t generate_x25519_native(rnp::RNG *            rng,
                                    std::vector<uint8_t> &privkey,
                                    std::vector<uint8_t> &pubkey);

#if defined(ENABLE_CRYPTO_REFRESH)
rnp_result_t x25519_native_encrypt(rnp::RNG *                  rng,
                                   const std::vector<uint8_t> &pubkey,
                                   const uint8_t *             in,
                                   size_t                      in_len,
                                   pgp_x25519_encrypted_t *    encrypted);

rnp_result_t x25519_native_decrypt(rnp::RNG *                    rng,
                                   const pgp_x25519_key_t &      keypair,
                                   const pgp_x25519_encrypted_t *encrypted,
                                   uint8_t *                     decbuf,
                                   size_t *                      decbuf_len);

rnp_result_t x25519_validate_key_native(rnp::RNG *              rng,
                                        const pgp_x25519_key_t *key,
                                        bool                    secret);

#endif
#endif

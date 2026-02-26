/*
 * Copyright (c) 2021, [Ribose Inc](https://www.ribose.com).
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

#include "ecdh_utils.h"
#include "types.h"
#include "utils.h"
#include <cassert>

namespace pgp {
namespace ecdh {
/* Used by ECDH keys. Specifies which hash and wrapping algorithm
 * to be used (see point 15. of RFC 4880).
 *
 * Note: sync with ec_curves.
 */
static const struct ecdh_params_t {
    pgp_curve_t    curve;    /* Curve ID */
    pgp_hash_alg_t hash;     /* Hash used by kdf */
    pgp_symm_alg_t wrap_alg; /* Symmetric algorithm used to wrap KEK*/
} ecdh_params[] = {
  {PGP_CURVE_NIST_P_256, PGP_HASH_SHA256, PGP_SA_AES_128},
  {PGP_CURVE_NIST_P_384, PGP_HASH_SHA384, PGP_SA_AES_192},
  {PGP_CURVE_NIST_P_521, PGP_HASH_SHA512, PGP_SA_AES_256},
  {PGP_CURVE_BP256, PGP_HASH_SHA256, PGP_SA_AES_128},
  {PGP_CURVE_BP384, PGP_HASH_SHA384, PGP_SA_AES_192},
  {PGP_CURVE_BP512, PGP_HASH_SHA512, PGP_SA_AES_256},
  {PGP_CURVE_25519, PGP_HASH_SHA256, PGP_SA_AES_128},
  {PGP_CURVE_P256K1, PGP_HASH_SHA256, PGP_SA_AES_128},
};

// returns size of data written to other_info
std::vector<uint8_t>
kdf_other_info_serialize(const ec::Curve &           curve,
                         const std::vector<uint8_t> &fp,
                         const pgp_hash_alg_t        kdf_hash,
                         const pgp_symm_alg_t        wrap_alg)
{
    assert(fp.size() >= 20);
    /* KDF-OtherInfo: AlgorithmID
     *   Current implementation will always use SHA-512 and AES-256 for KEK wrapping
     */
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(curve.OID.size()));
    buf.insert(buf.end(), curve.OID.begin(), curve.OID.end());
    buf.push_back(PGP_PKA_ECDH);
    // size of following 3 params (each 1 byte)
    buf.push_back(0x03);
    // Value reserved for future use
    buf.push_back(0x01);
    // Hash used with KDF
    buf.push_back(kdf_hash);
    // Algorithm ID used for key wrapping
    buf.push_back(wrap_alg);

    /* KDF-OtherInfo: PartyUInfo
     *   20 bytes representing "Anonymous Sender "
     */
    static const std::array<uint8_t, 20> anonymous = {0x41, 0x6E, 0x6F, 0x6E, 0x79, 0x6D, 0x6F,
                                                      0x75, 0x73, 0x20, 0x53, 0x65, 0x6E, 0x64,
                                                      0x65, 0x72, 0x20, 0x20, 0x20, 0x20};
    buf.insert(buf.end(), anonymous.begin(), anonymous.end());
    // keep 20, as per spec
    buf.insert(buf.end(), fp.begin(), fp.end());
    return buf;
}

void
pad_pkcs7(rnp::secure_bytes &buf, uint8_t padding)
{
    buf.insert(buf.end(), padding, padding);
}

bool
unpad_pkcs7(rnp::secure_bytes &buf)
{
    if (buf.empty()) {
        return false;
    }

    uint8_t       err = 0;
    const uint8_t pad_byte = buf.back();
    const size_t  pad_begin = buf.size() - pad_byte;

    // TODO: Still >, <, and <=,==  are not constant time (maybe?)
    err |= (pad_byte > buf.size());
    err |= (pad_byte == 0);

    /* Check if padding is OK */
    for (size_t c = 0; c < buf.size(); c++) {
        err |= (buf[c] ^ pad_byte) * (pad_begin <= c);
    }
    buf.resize(pad_begin);
    return (err == 0);
}

bool
set_params(ec::Key &key, pgp_curve_t curve_id)
{
    for (size_t i = 0; i < ARRAY_SIZE(ecdh_params); i++) {
        if (ecdh_params[i].curve == curve_id) {
            key.kdf_hash_alg = ecdh_params[i].hash;
            key.key_wrap_alg = ecdh_params[i].wrap_alg;
            return true;
        }
    }

    return false;
}

} // namespace ecdh
} // namespace pgp

bool
x25519_tweak_bits(pgp::ec::Key &key)
{
    if (key.x.size() != 32) {
        return false;
    }
    /* MPI is big-endian, while raw x25519 key is little-endian */
    key.x[31] &= 248; // zero 3 low bits
    key.x[0] &= 127;  // zero high bit
    key.x[0] |= 64;   // set high - 1 bit
    return true;
}

bool
x25519_bits_tweaked(const pgp::ec::Key &key)
{
    if (key.x.size() != 32) {
        return false;
    }
    return !(key.x[31] & 7) && (key.x[0] < 128) && (key.x[0] >= 64);
}

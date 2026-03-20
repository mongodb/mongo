/*
 * Copyright (c) 2017, [Ribose Inc](https://www.ribose.com).
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

#include <botan/ffi.h>
#include <string.h>
#include <cassert>
#include "ec.h"
#include "types.h"
#include "utils.h"
#include "mem.h"
#include "botan_utils.hpp"
#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
#include "x25519.h"
#include "ed25519.h"
#include "botan_utils.hpp"
#include "botan/bigint.h"
#include "botan/ecdh.h"
#include <cassert>
#endif

namespace pgp {
namespace ec {

static id_str_pair ec_algo_to_botan[] = {
  {PGP_PKA_ECDH, "ECDH"},
  {PGP_PKA_ECDSA, "ECDSA"},
  {PGP_PKA_SM2, "SM2_Sig"},
  {0, NULL},
};

rnp_result_t
Key::generate_x25519(rnp::RNG &rng)
{
    rnp::botan::Privkey pr_key;
    if (botan_privkey_create(&pr_key.get(), "Curve25519", "", rng.handle())) {
        return RNP_ERROR_KEY_GENERATION;
    }

    rnp::botan::Pubkey pu_key;
    if (botan_privkey_export_pubkey(&pu_key.get(), pr_key.get())) {
        return RNP_ERROR_KEY_GENERATION;
    }

    /* botan returns key in little-endian, while mpi is big-endian */
    rnp::secure_array<uint8_t, 32> keyle;
    if (botan_privkey_x25519_get_privkey(pr_key.get(), keyle.data())) {
        return RNP_ERROR_KEY_GENERATION;
    }
    x.resize(32);
    for (int i = 0; i < 32; i++) {
        x[31 - i] = keyle[i];
    }
    /* botan doesn't tweak secret key bits, so we should do that here */
    if (!x25519_tweak_bits(*this)) {
        return RNP_ERROR_KEY_GENERATION;
    }

    p.resize(33);
    if (botan_pubkey_x25519_get_pubkey(pu_key.get(), &p[1])) {
        return RNP_ERROR_KEY_GENERATION;
    }
    p[0] = 0x40;
    return RNP_SUCCESS;
}

rnp_result_t
Key::generate(rnp::RNG &rng, const pgp_pubkey_alg_t alg_id, const pgp_curve_t curve)
{
    /**
     * Keeps "0x04 || x || y"
     * \see 13.2.  ECDSA, ECDH, SM2 Conversion Primitives
     *
     * P-521 is biggest supported curve
     */
    if (!Curve::alg_allows(alg_id, curve)) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    const char *ec_algo = id_str_pair::lookup(ec_algo_to_botan, alg_id, NULL);
    assert(ec_algo);
    auto ec_desc = Curve::get(curve);
    if (!ec_desc) {
        return RNP_ERROR_BAD_PARAMETERS;
    }

    // at this point it must succeed
    rnp::botan::Privkey pr_key;
    if (botan_privkey_create(&pr_key.get(), ec_algo, ec_desc->botan_name, rng.handle())) {
        return RNP_ERROR_KEY_GENERATION;
    }

    rnp::botan::Pubkey pu_key;
    if (botan_privkey_export_pubkey(&pu_key.get(), pr_key.get())) {
        return RNP_ERROR_KEY_GENERATION;
    }

    rnp::bn px;
    rnp::bn py;
    rnp::bn bx;

    if (!px || !py || !bx) {
        RNP_LOG("Allocation failed");
        return RNP_ERROR_OUT_OF_MEMORY;
    }

    if (botan_pubkey_get_field(px.get(), pu_key.get(), "public_x") ||
        botan_pubkey_get_field(py.get(), pu_key.get(), "public_y") ||
        botan_privkey_get_field(bx.get(), pr_key.get(), "x")) {
        return RNP_ERROR_KEY_GENERATION;
    }

    // Safety check
    size_t field_size = ec_desc->bytes();
    if ((px.bytes() > field_size) || (py.bytes() > field_size)) {
        RNP_LOG("Key generation failed");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    /*
     * Convert coordinates to MPI stored as
     * "0x04 || x || y"
     *
     *  \see 13.2.  ECDSA and ECDH Conversion Primitives
     *
     * Note: Generated pk/sk may not always have exact number of bytes
     *       which is important when converting to octet-string
     */
    p.resize(2 * field_size + 1);
    p[0] = 0x04;
    px.bin(&p[1 + field_size - px.bytes()]);
    py.bin(&p[1 + 2 * field_size - py.bytes()]);
    /* secret key value */
    bx.mpi(x);
    return RNP_SUCCESS;
}

} // namespace ec
} // namespace pgp

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
static bool
is_generic_prime_curve(pgp_curve_t curve)
{
    switch (curve) {
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
        return true;
    default:
        return false;
    }
}

static rnp_result_t
ec_generate_generic_native(rnp::RNG *            rng,
                           std::vector<uint8_t> &privkey,
                           std::vector<uint8_t> &pubkey,
                           pgp_curve_t           curve,
                           pgp_pubkey_alg_t      alg)
{
    if (!is_generic_prime_curve(curve)) {
        RNP_LOG("expected generic prime curve");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    auto         ec_desc = pgp::ec::Curve::get(curve);
    const size_t curve_order = ec_desc->bytes();

    Botan::ECDH_PrivateKey privkey_botan(*(rng->obj()), Botan::EC_Group(ec_desc->botan_name));
    Botan::BigInt          pub_x = privkey_botan.public_point().get_affine_x();
    Botan::BigInt          pub_y = privkey_botan.public_point().get_affine_y();
    Botan::BigInt          x = privkey_botan.private_value();

    // pubkey: 0x04 || X || Y
    pubkey = Botan::unlock(Botan::BigInt::encode_fixed_length_int_pair(
      pub_x, pub_y, curve_order)); // zero-pads to the given size
    pubkey.insert(pubkey.begin(), 0x04);

    privkey = std::vector<uint8_t>(curve_order);
    x.binary_encode(privkey.data(), privkey.size()); // zero-pads to the given size

    assert(pubkey.size() == 2 * curve_order + 1);
    assert(privkey.size() == curve_order);

    return RNP_SUCCESS;
}

rnp_result_t
ec_generate_native(rnp::RNG *            rng,
                   std::vector<uint8_t> &privkey,
                   std::vector<uint8_t> &pubkey,
                   pgp_curve_t           curve,
                   pgp_pubkey_alg_t      alg)
{
    if (curve == PGP_CURVE_25519) {
        return generate_x25519_native(rng, privkey, pubkey);
    } else if (curve == PGP_CURVE_ED25519) {
        return generate_ed25519_native(rng, privkey, pubkey);
    } else if (is_generic_prime_curve(curve)) {
        if (alg != PGP_PKA_ECDH && alg != PGP_PKA_ECDSA) {
            RNP_LOG("alg and curve mismatch");
            return RNP_ERROR_BAD_PARAMETERS;
        }
        return ec_generate_generic_native(rng, privkey, pubkey, curve, alg);
    } else {
        RNP_LOG("invalid curve");
        return RNP_ERROR_BAD_PARAMETERS;
    }
}
#endif

/*-
 * Copyright (c) 2021-2024 Ribose Inc.
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

#include "config.h"
#include "defaults.h"
#include "repgp/repgp_def.h"
#include "dsa.h"
#include "ecdsa.h"
#include <cstddef>

namespace pgp {
namespace dsa {

pgp_hash_alg_t
Key::get_min_hash(size_t qsize)
{
    /*
     * I'm using _broken_ SHA1 here only because
     * some old implementations may not understand keys created
     * with other hashes. If you're sure we don't have to support
     * such implementations, please be my guest and remove it.
     */
    return (qsize < 160)  ? PGP_HASH_UNKNOWN :
           (qsize == 160) ? PGP_HASH_SHA1 :
           (qsize <= 224) ? PGP_HASH_SHA224 :
           (qsize <= 256) ? PGP_HASH_SHA256 :
           (qsize <= 384) ? PGP_HASH_SHA384 :
           (qsize <= 512) ? PGP_HASH_SHA512
                            /*(qsize>512)*/ :
                            PGP_HASH_UNKNOWN;
}

size_t
Key::choose_qsize(size_t psize)
{
    return (psize == 1024) ? 160 : (psize <= 2047) ? 224 : (psize <= 3072) ? 256 : 0;
}

} // namespace dsa

namespace ecdsa {
pgp_hash_alg_t
get_min_hash(pgp_curve_t curve)
{
    switch (curve) {
    case PGP_CURVE_NIST_P_256:
    case PGP_CURVE_BP256:
    case PGP_CURVE_P256K1:
        return PGP_HASH_SHA256;
    case PGP_CURVE_NIST_P_384:
    case PGP_CURVE_BP384:
        return PGP_HASH_SHA384;
    case PGP_CURVE_NIST_P_521:
    case PGP_CURVE_BP512:
        return PGP_HASH_SHA512;
    default:
        return PGP_HASH_UNKNOWN;
    }
}
} // namespace ecdsa

} // namespace pgp

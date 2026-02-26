/*
 * Copyright (c) 2017-2021, [Ribose Inc](https://www.ribose.com).
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

#include <assert.h>
#include <botan/ffi.h>
#include "rng.h"
#include "types.h"

namespace rnp {
RNG::RNG(Type type)
{
    if (botan_rng_init(&botan_rng, type == Type::DRBG ? "user" : NULL)) {
        throw rnp::rnp_exception(RNP_ERROR_RNG);
    }
#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
    if (type == Type::DRBG) {
        botan_rng_obj.reset(new Botan::AutoSeeded_RNG);
    } else {
        botan_rng_obj.reset(new Botan::System_RNG);
    }
#endif
}

RNG::~RNG()
{
    (void) botan_rng_destroy(botan_rng);
}

void
RNG::get(uint8_t *data, size_t len)
{
    if (botan_rng_get(botan_rng, data, len)) {
        // This should never happen
        throw rnp::rnp_exception(RNP_ERROR_RNG);
    }
}

struct botan_rng_struct *
RNG::handle()
{
    return botan_rng;
}

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
Botan::RandomNumberGenerator *
RNG::obj() const
{
    return botan_rng_obj.get();
}
#endif
} // namespace rnp

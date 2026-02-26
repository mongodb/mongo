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

#ifndef RNP_RNG_H_
#define RNP_RNG_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "config.h"

#ifdef CRYPTO_BACKEND_BOTAN
typedef struct botan_rng_struct *botan_rng_t;

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
#include <botan/system_rng.h>
#include <botan/auto_rng.h>
#endif
#endif

namespace rnp {
class RNG {
  private:
#ifdef CRYPTO_BACKEND_BOTAN
    struct botan_rng_struct *botan_rng;
#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
    std::unique_ptr<Botan::RandomNumberGenerator> botan_rng_obj;
#endif
#endif
  public:
    enum Type { DRBG, System };
    /**
     * @brief Construct a new RNG object.
     *        Note: OpenSSL uses own global RNG, so this class is not needed there and left
     *        only for code-level compatibility.
     *
     * @param type indicates which random generator to initialize.
     *             Possible values for Botan backend:
     *             - DRBG will initialize HMAC_DRBG, this generator is initialized on-demand
     *               (when used for the first time)
     *             - SYSTEM will initialize /dev/(u)random
     */
    RNG(Type type = Type::DRBG);
    ~RNG();
    /**
     * @brief Get randoom bytes.
     *
     * @param data buffer where data should be stored. Cannot be NULL.
     * @param len number of bytes required.
     */
    void get(uint8_t *data, size_t len);
#ifdef CRYPTO_BACKEND_BOTAN
    /**
     * @brief   Returns internal handle to botan rng. Returned
     *          handle is always initialized. In case of
     *          internal error NULL is returned
     */
    struct botan_rng_struct *handle();

#if defined(ENABLE_CRYPTO_REFRESH) || defined(ENABLE_PQC)
    /**
     * @brief Returns the Botan RNG C++ object
     *        Note: It is planned to move away from the FFI handle.
     *        For the transition phase, both approaches are implemented.
     */
    Botan::RandomNumberGenerator *obj() const;
#endif
#endif
};
} // namespace rnp

#endif // RNP_RNG_H_

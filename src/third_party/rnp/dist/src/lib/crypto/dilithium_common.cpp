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

#include "dilithium.h"
#include "types.h"
#include "logging.h"

pgp_dilithium_public_key_t::pgp_dilithium_public_key_t(const uint8_t *       key_encoded,
                                                       size_t                key_encoded_len,
                                                       dilithium_parameter_e param)
    : key_encoded_(key_encoded, key_encoded + key_encoded_len), dilithium_param_(param),
      is_initialized_(true)
{
}

pgp_dilithium_public_key_t::pgp_dilithium_public_key_t(std::vector<uint8_t> const &key_encoded,
                                                       dilithium_parameter_e       param)
    : key_encoded_(key_encoded), dilithium_param_(param), is_initialized_(true)
{
}

pgp_dilithium_private_key_t::pgp_dilithium_private_key_t(const uint8_t *       key_encoded,
                                                         size_t                key_encoded_len,
                                                         dilithium_parameter_e param)
    : key_encoded_(key_encoded, key_encoded + key_encoded_len), dilithium_param_(param),
      is_initialized_(true)
{
}

pgp_dilithium_private_key_t::pgp_dilithium_private_key_t(
  std::vector<uint8_t> const &key_encoded, dilithium_parameter_e param)
    : key_encoded_(Botan::secure_vector<uint8_t>(key_encoded.begin(), key_encoded.end())),
      dilithium_param_(param), is_initialized_(true)
{
}

size_t
dilithium_privkey_size(dilithium_parameter_e parameter)
{
    switch (parameter) {
    case dilithium_L3:
        return 4000;
    case dilithium_L5:
        return 4864;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

size_t
dilithium_pubkey_size(dilithium_parameter_e parameter)
{
    switch (parameter) {
    case dilithium_L3:
        return 1952;
    case dilithium_L5:
        return 2592;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

size_t
dilithium_signature_size(dilithium_parameter_e parameter)
{
    switch (parameter) {
    case dilithium_L3:
        return 3293;
    case dilithium_L5:
        return 4595;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

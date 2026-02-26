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

#include "kyber_common.h"
#include "types.h"
#include "logging.h"

size_t
kyber_privkey_size(kyber_parameter_e parameter)
{
    switch (parameter) {
    case kyber_768:
        return 2400;
    case kyber_1024:
        return 3168;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

size_t
kyber_pubkey_size(kyber_parameter_e parameter)
{
    switch (parameter) {
    case kyber_768:
        return 1184;
    case kyber_1024:
        return 1568;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

size_t
kyber_keyshare_size(kyber_parameter_e parameter)
{
    switch (parameter) {
    case kyber_768:
        return 24;
    case kyber_1024:
        return 32;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

size_t
kyber_ciphertext_size(kyber_parameter_e parameter)
{
    switch (parameter) {
    case kyber_768:
        return 1088;
    case kyber_1024:
        return 1568;
    default:
        RNP_LOG("invalid parameter given");
        throw rnp::rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
}

pgp_kyber_public_key_t::pgp_kyber_public_key_t(const uint8_t *   key_encoded,
                                               size_t            key_encoded_len,
                                               kyber_parameter_e mode)
    : key_encoded_(key_encoded, key_encoded + key_encoded_len), kyber_mode_(mode),
      is_initialized_(true)
{
}

pgp_kyber_public_key_t::pgp_kyber_public_key_t(std::vector<uint8_t> const &key_encoded,
                                               kyber_parameter_e           mode)
    : key_encoded_(key_encoded), kyber_mode_(mode), is_initialized_(true)
{
}

pgp_kyber_private_key_t::pgp_kyber_private_key_t(const uint8_t *   key_encoded,
                                                 size_t            key_encoded_len,
                                                 kyber_parameter_e mode)
    : key_encoded_(key_encoded, key_encoded + key_encoded_len), kyber_mode_(mode),
      is_initialized_(true)
{
}

pgp_kyber_private_key_t::pgp_kyber_private_key_t(std::vector<uint8_t> const &key_encoded,
                                                 kyber_parameter_e           mode)
    : key_encoded_(Botan::secure_vector<uint8_t>(key_encoded.begin(), key_encoded.end())),
      kyber_mode_(mode), is_initialized_(true)
{
}

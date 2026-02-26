/*-
 * Copyright (c) 2021 Ribose Inc.
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

#include "backend_version.h"
#include "logging.h"
#if defined(CRYPTO_BACKEND_BOTAN)
#include <botan/version.h>
#elif defined(CRYPTO_BACKEND_OPENSSL)
#include <openssl/opensslv.h>
#include <openssl/crypto.h>
#if defined(CRYPTO_BACKEND_OPENSSL3)
#include <openssl/provider.h>
#endif
#include <string.h>
#include "config.h"
#include "ossl_utils.hpp"
#ifndef RNP_USE_STD_REGEX
#include <regex.h>
#else
#include <regex>
#endif
#endif
#include <cassert>

namespace rnp {

const char *
backend_string()
{
#if defined(CRYPTO_BACKEND_BOTAN)
    return "Botan";
#elif defined(CRYPTO_BACKEND_OPENSSL)
    return "OpenSSL";
#else
#error "Unknown backend"
#endif
}

const char *
backend_version()
{
#if defined(CRYPTO_BACKEND_BOTAN)
    return Botan::short_version_cstr();
#elif defined(CRYPTO_BACKEND_OPENSSL)
    /* Use regexp to retrieve version (second word) from version string
     * like "OpenSSL 1.1.1l  24 Aug 2021"
     * */
    static char version[32] = {};
    if (version[0]) {
        return version;
    }
    const char *reg = "OpenSSL (([0-9]+\\.[0-9]+\\.[0-9]+)[a-z]*(-[a-z0-9]+)*) ";
#ifndef RNP_USE_STD_REGEX
    static regex_t r;
    regmatch_t     matches[5];
    const char *   ver = OpenSSL_version(OPENSSL_VERSION);

    if (!strlen(version)) {
        if (regcomp(&r, reg, REG_EXTENDED) != 0) {
            RNP_LOG("failed to compile regexp");
            return "unknown";
        }
    }
    int res = regexec(&r, ver, 5, matches, 0);
    if (res != 0) {
        RNP_LOG("regexec() failed on %s: %d", ver, res);
        return "unknown";
    }
    assert(sizeof(version) > matches[1].rm_eo - matches[1].rm_so);
    memcpy(version, ver + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
    version[matches[1].rm_eo - matches[1].rm_so] = '\0';
#else
    static std::regex re(reg, std::regex_constants::extended);
    std::smatch       result;
    std::string       ver = OpenSSL_version(OPENSSL_VERSION);
    if (!std::regex_search(ver, result, re)) {
        RNP_LOG("std::regex_search failed on \"%s\"", ver.c_str());
        return "unknown";
    }
    assert(sizeof(version) > result[1].str().size());
    strncpy(version, result[1].str().c_str(), sizeof(version) - 1);
#endif
    return version;
#else
#error "Unknown backend"
#endif
}

#if defined(CRYPTO_BACKEND_OPENSSL3)

#if defined(ENABLE_IDEA) || defined(ENABLE_CAST5) || defined(ENABLE_BLOWFISH) || \
  (defined(ENABLE_RIPEMD160) && OPENSSL_VERSION_NUMBER < 0x30000070L)
#if !defined(CRYPTO_BACKEND_OPENSSL3_LEGACY)
#error "OpenSSL doesn't have legacy provider, however one of the features enables it's load."
#endif
#define OPENSSL_LOAD_LEGACY
#endif

typedef struct openssl3_state {
#if defined(OPENSSL_LOAD_LEGACY)
    OSSL_PROVIDER *legacy;
#endif
    OSSL_PROVIDER *def;
} openssl3_state;

bool
backend_init(void **param)
{
    if (!param) {
        return false;
    }

    *param = NULL;
    openssl3_state *state = (openssl3_state *) calloc(1, sizeof(openssl3_state));
    if (!state) {
        RNP_LOG("Allocation failure.");
        return false;
    }
    /* Load default crypto provider */
    state->def = OSSL_PROVIDER_load(NULL, "default");
    if (!state->def) {
        RNP_LOG("Failed to load default crypto provider: %s", rnp::ossl::latest_err());
        free(state);
        return false;
    }
    /* Load legacy crypto provider if needed */
#if defined(OPENSSL_LOAD_LEGACY)
    state->legacy = OSSL_PROVIDER_load(NULL, "legacy");
    if (!state->legacy) {
        RNP_LOG("Failed to load legacy crypto provider: %s", rnp::ossl::latest_err());
        OSSL_PROVIDER_unload(state->def);
        free(state);
        return false;
    }
#endif
    *param = state;
    return true;
}

void
backend_finish(void *param)
{
    if (!param) {
        return;
    }
    openssl3_state *state = (openssl3_state *) param;
    OSSL_PROVIDER_unload(state->def);
#if defined(OPENSSL_LOAD_LEGACY)
    OSSL_PROVIDER_unload(state->legacy);
#endif
    free(state);
}
#else
bool
backend_init(void **param)
{
    return true;
}

void
backend_finish(void *param)
{
    // Do nothing
}
#endif

} // namespace rnp

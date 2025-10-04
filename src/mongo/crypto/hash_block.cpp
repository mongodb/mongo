/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/crypto/hash_block.h"

namespace mongo {
#if defined(MONGO_CONFIG_SSL) && MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL && \
    OPENSSL_VERSION_NUMBER < 0x10100000L
namespace {
HMAC_CTX* HMAC_CTX_new() {
    void* ctx = OPENSSL_malloc(sizeof(HMAC_CTX));

    if (ctx != NULL) {
        memset(ctx, 0, sizeof(HMAC_CTX));
    }
    return static_cast<HMAC_CTX*>(ctx);
}

void HMAC_CTX_free(HMAC_CTX* ctx) {
    HMAC_CTX_cleanup(ctx);
    OPENSSL_free(ctx);
}
}  // namespace
#endif
#if defined(MONGO_CONFIG_SSL) && (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL)
int HmacContext::hmacCtxInitFn(const EVP_MD* md, const uint8_t* key, size_t keyLen) {
    int ret;
    if (getReuseKey() && useCount() >= 1) {
        ret = HMAC_Init_ex(get(), nullptr, 0, md, nullptr);
    }
    ret = HMAC_Init_ex(get(), key, keyLen, md, nullptr);
    if (getReuseKey()) {
        use++;
    }
    return ret;
}

HmacContext::HmacContext() {
    hmac_ctx = HMAC_CTX_new();
};

HmacContext::~HmacContext() {
    HMAC_CTX_free(hmac_ctx);
}

HMAC_CTX* HmacContext::get() {
    return hmac_ctx;
}
#endif

void HmacContext::resetCount() {
    use = 0;
}

int HmacContext::useCount() {
    return use;
}

void HmacContext::setReuseKey(bool val) {
    _reuseKey = val;
    if (val == false) {
        resetCount();
    }
}

bool HmacContext::getReuseKey() {
    return _reuseKey;
}

}  // namespace mongo

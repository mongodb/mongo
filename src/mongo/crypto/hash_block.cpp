// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    } else {
        ret = HMAC_Init_ex(get(), key, keyLen, md, nullptr);
    }
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

HashContext::HashContext() {
    _digestCtx = EVP_MD_CTX_new();
}

HashContext::~HashContext() {
    EVP_MD_CTX_free(_digestCtx);
}

EVP_MD_CTX* HashContext::get() {
    return _digestCtx;
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

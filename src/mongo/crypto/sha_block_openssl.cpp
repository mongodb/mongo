/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/data_range.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/sha512_block.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <initializer_list>
#include <memory>

#ifndef MONGO_CONFIG_SSL
#error This file should only be included in SSL-enabled builds
#endif

#include <cstdint>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>
#include <openssl/ossl_typ.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
namespace {
// Copies of OpenSSL after 1.1.0 define new EVP digest routines. We must
// polyfill used definitions to interact with older OpenSSL versions.
EVP_MD_CTX* EVP_MD_CTX_new() {
    void* ret = OPENSSL_malloc(sizeof(EVP_MD_CTX));

    if (ret != NULL) {
        memset(ret, 0, sizeof(EVP_MD_CTX));
    }
    return static_cast<EVP_MD_CTX*>(ret);
}

void EVP_MD_CTX_free(EVP_MD_CTX* ctx) {
    EVP_MD_CTX_cleanup(ctx);
    OPENSSL_free(ctx);
}

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

namespace mongo {

namespace {


/**
 * Class to load singleton instances of each SHA algorithm.
 */
#if OPENSSL_VERSION_NUMBER > 0x30000000L
class OpenSSLHashLoader {
public:
    OpenSSLHashLoader() {
        _algoSHA1 = EVP_MD_fetch(NULL, "SHA1", NULL);
        _algoSHA256 = EVP_MD_fetch(NULL, "SHA2-256", NULL);
        _algoSHA512 = EVP_MD_fetch(NULL, "SHA2-512", NULL);
    }

    ~OpenSSLHashLoader() {
        EVP_MD_free(_algoSHA1);
        EVP_MD_free(_algoSHA256);
        EVP_MD_free(_algoSHA512);
    }

    const EVP_MD* getSHA512() {
        return _algoSHA512;
    }

    const EVP_MD* getSHA256() {
        return _algoSHA256;
    }

    const EVP_MD* getSHA1() {
        return _algoSHA1;
    }

private:
    EVP_MD* _algoSHA512;
    EVP_MD* _algoSHA256;
    EVP_MD* _algoSHA1;
};
#else

class OpenSSLHashLoader {
public:
    const EVP_MD* getSHA512() {
        return EVP_sha512();
    }

    const EVP_MD* getSHA256() {
        return EVP_sha256();
    }

    const EVP_MD* getSHA1() {
        return EVP_sha1();
    }
};
#endif

static OpenSSLHashLoader& getOpenSSLHashLoader() {
    static OpenSSLHashLoader* loader = new OpenSSLHashLoader();
    return *loader;
}


/*
 * Computes a SHA hash of 'input'.
 */
template <typename HashType>
void computeHashImpl(const EVP_MD* md,
                     std::initializer_list<ConstDataRange> input,
                     HashType* const output) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digestCtx(EVP_MD_CTX_new(),
                                                                      EVP_MD_CTX_free);

    fassert(40379,
            EVP_DigestInit_ex(digestCtx.get(), md, nullptr) == 1 &&
                std::all_of(begin(input),
                            end(input),
                            [&](const auto& i) {
                                return EVP_DigestUpdate(digestCtx.get(), i.data(), i.length()) == 1;
                            }) &&
                EVP_DigestFinal_ex(digestCtx.get(), output->data(), nullptr) == 1);
}

template <typename HashType>
void computeHmacImplWithCtx(HmacContext* digestCtx,
                            const EVP_MD* md,
                            const uint8_t* key,
                            size_t keyLen,
                            std::initializer_list<ConstDataRange> input,
                            HashType* const output) {
    auto ctx = digestCtx->get();
    fassert(40380,
            digestCtx->hmacCtxInitFn(md, key, keyLen) == 1 &&
                std::all_of(begin(input),
                            end(input),
                            [&](const auto& i) {
                                return HMAC_Update(ctx,
                                                   reinterpret_cast<const unsigned char*>(i.data()),
                                                   i.length()) == 1;
                            }) &&
                HMAC_Final(ctx, output->data(), nullptr) == 1);
}

template <typename HashType>
void computeHmacImpl(const EVP_MD* md,
                     const uint8_t* key,
                     size_t keyLen,
                     std::initializer_list<ConstDataRange> input,
                     HashType* const output) {
    HmacContext hmacCtx;
    computeHmacImplWithCtx<HashType>(&hmacCtx, md, key, keyLen, input, output);
}

}  // namespace

void SHA1BlockTraits::computeHash(std::initializer_list<ConstDataRange> input,
                                  HashType* const output) {
    computeHashImpl<SHA1BlockTraits::HashType>(getOpenSSLHashLoader().getSHA1(), input, output);
}

void SHA256BlockTraits::computeHash(std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    computeHashImpl<SHA256BlockTraits::HashType>(getOpenSSLHashLoader().getSHA256(), input, output);
}

void SHA512BlockTraits::computeHash(std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    computeHashImpl<SHA512BlockTraits::HashType>(getOpenSSLHashLoader().getSHA512(), input, output);
}

void SHA1BlockTraits::computeHmac(const uint8_t* key,
                                  size_t keyLen,
                                  std::initializer_list<ConstDataRange> input,
                                  SHA1BlockTraits::HashType* const output) {
    return computeHmacImpl<SHA1BlockTraits::HashType>(
        getOpenSSLHashLoader().getSHA1(), key, keyLen, input, output);
}

void SHA1BlockTraits::computeHmacWithCtx(HmacContext* ctx,
                                         const uint8_t* key,
                                         size_t keyLen,
                                         std::initializer_list<ConstDataRange> input,
                                         HashType* const output) {
    return computeHmacImplWithCtx<SHA1BlockTraits::HashType>(
        ctx, getOpenSSLHashLoader().getSHA1(), key, keyLen, input, output);
}

void SHA256BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    std::initializer_list<ConstDataRange> input,
                                    SHA256BlockTraits::HashType* const output) {
    return computeHmacImpl<SHA256BlockTraits::HashType>(
        getOpenSSLHashLoader().getSHA256(), key, keyLen, input, output);
}

void SHA256BlockTraits::computeHmacWithCtx(HmacContext* ctx,
                                           const uint8_t* key,
                                           size_t keyLen,
                                           std::initializer_list<ConstDataRange> input,
                                           HashType* const output) {
    return computeHmacImplWithCtx<SHA256BlockTraits::HashType>(
        ctx, getOpenSSLHashLoader().getSHA256(), key, keyLen, input, output);
}

void SHA512BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    std::initializer_list<ConstDataRange> input,
                                    SHA512BlockTraits::HashType* const output) {
    return computeHmacImpl<SHA512BlockTraits::HashType>(
        getOpenSSLHashLoader().getSHA512(), key, keyLen, input, output);
}

void SHA512BlockTraits::computeHmacWithCtx(HmacContext* ctx,
                                           const uint8_t* key,
                                           size_t keyLen,
                                           std::initializer_list<ConstDataRange> input,
                                           HashType* const output) {
    return computeHmacImplWithCtx<SHA512BlockTraits::HashType>(
        ctx, getOpenSSLHashLoader().getSHA512(), key, keyLen, input, output);
}

}  // namespace mongo

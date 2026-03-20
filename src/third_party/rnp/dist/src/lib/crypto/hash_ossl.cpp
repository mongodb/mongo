/*
 * Copyright (c) 2021-2022 Ribose Inc.
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

#include "hash_ossl.hpp"
#include <stdio.h>
#include <memory>
#include <cassert>
#include <openssl/err.h>
#include "config.h"
#include "types.h"
#include "utils.h"
#include "str-utils.h"
#include "defaults.h"

static const id_str_pair openssl_alg_map[] = {
  {PGP_HASH_MD5, "md5"},
  {PGP_HASH_SHA1, "sha1"},
  {PGP_HASH_RIPEMD, "ripemd160"},
  {PGP_HASH_SHA256, "sha256"},
  {PGP_HASH_SHA384, "sha384"},
  {PGP_HASH_SHA512, "sha512"},
  {PGP_HASH_SHA224, "sha224"},
  {PGP_HASH_SM3, "sm3"},
  {PGP_HASH_SHA3_256, "sha3-256"},
  {PGP_HASH_SHA3_512, "sha3-512"},
  {0, NULL},
};

namespace rnp {
Hash_OpenSSL::Hash_OpenSSL(pgp_hash_alg_t alg) : Hash(alg)
{
    const char *  hash_name = Hash_OpenSSL::name_backend(alg);
    const EVP_MD *hash_tp = EVP_get_digestbyname(hash_name);
    if (!hash_tp) {
        RNP_LOG("Error creating hash object for '%s'", hash_name);
        throw rnp_exception(RNP_ERROR_BAD_STATE);
    }
    fn_ = EVP_MD_CTX_new();
    if (!fn_) {
        RNP_LOG("Allocation failure");
        throw rnp_exception(RNP_ERROR_OUT_OF_MEMORY);
    }
    int res = EVP_DigestInit_ex(fn_, hash_tp, NULL);
    if (res != 1) {
        RNP_LOG("Digest initializataion error %d : %lu", res, ERR_peek_last_error());
        EVP_MD_CTX_free(fn_);
        throw rnp_exception(RNP_ERROR_BAD_STATE);
    }
    assert(size_ == (size_t) EVP_MD_size(hash_tp));
}

Hash_OpenSSL::Hash_OpenSSL(const Hash_OpenSSL &src) : Hash(src.alg_)
{
    if (!src.fn_) {
        throw rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }

    fn_ = EVP_MD_CTX_new();
    if (!fn_) {
        RNP_LOG("Allocation failure");
        throw rnp_exception(RNP_ERROR_OUT_OF_MEMORY);
    }

    int res = EVP_MD_CTX_copy(fn_, src.fn_);
    if (res != 1) {
        RNP_LOG("Digest copying error %d: %lu", res, ERR_peek_last_error());
        EVP_MD_CTX_free(fn_);
        throw rnp_exception(RNP_ERROR_BAD_STATE);
    }
}

std::unique_ptr<Hash_OpenSSL>
Hash_OpenSSL::create(pgp_hash_alg_t alg)
{
    return std::unique_ptr<Hash_OpenSSL>(new Hash_OpenSSL(alg));
}

std::unique_ptr<Hash>
Hash_OpenSSL::clone() const
{
    return std::unique_ptr<Hash>(new Hash_OpenSSL(*this));
}

void
Hash_OpenSSL::add(const void *buf, size_t len)
{
    if (!fn_) {
        throw rnp_exception(RNP_ERROR_NULL_POINTER);
    }
    int res = EVP_DigestUpdate(fn_, buf, len);
    if (res != 1) {
        RNP_LOG("Digest updating error %d: %lu", res, ERR_peek_last_error());
        throw rnp_exception(RNP_ERROR_GENERIC);
    }
}

void
Hash_OpenSSL::finish(uint8_t *digest)
{
    assert(fn_);
    if (!fn_) {
        return;
    }
    int res = digest ? EVP_DigestFinal_ex(fn_, digest, NULL) : 1;
    EVP_MD_CTX_free(fn_);
    fn_ = NULL;
    if (res != 1) {
        RNP_LOG("Digest finalization error %d: %lu", res, ERR_peek_last_error());
        return;
    }
    size_ = 0;
}

Hash_OpenSSL::~Hash_OpenSSL()
{
    if (!fn_) {
        return;
    }
    EVP_MD_CTX_free(fn_);
}

const char *
Hash_OpenSSL::name_backend(pgp_hash_alg_t alg)
{
    return id_str_pair::lookup(openssl_alg_map, alg);
}
} // namespace rnp

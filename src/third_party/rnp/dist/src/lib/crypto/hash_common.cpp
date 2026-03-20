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

#include "config.h"
#include "hash.hpp"
#include "types.h"
#include "utils.h"
#include "str-utils.h"
#include "hash_sha1cd.hpp"
#if defined(CRYPTO_BACKEND_BOTAN)
#include "hash_botan.hpp"
#endif
#if defined(CRYPTO_BACKEND_OPENSSL)
#include "hash_ossl.hpp"
#include "hash_crc24.hpp"
#endif

static const struct hash_alg_map_t {
    pgp_hash_alg_t type;
    const char *   name;
    size_t         len;
} hash_alg_map[] = {{PGP_HASH_MD5, "MD5", 16},
                    {PGP_HASH_SHA1, "SHA1", 20},
                    {PGP_HASH_RIPEMD, "RIPEMD160", 20},
                    {PGP_HASH_SHA256, "SHA256", 32},
                    {PGP_HASH_SHA384, "SHA384", 48},
                    {PGP_HASH_SHA512, "SHA512", 64},
                    {PGP_HASH_SHA224, "SHA224", 28},
                    {PGP_HASH_SM3, "SM3", 32},
                    {PGP_HASH_SHA3_256, "SHA3-256", 32},
                    {PGP_HASH_SHA3_512, "SHA3-512", 64}};

namespace rnp {

pgp_hash_alg_t
Hash::alg() const
{
    return alg_;
}

size_t
Hash::size() const
{
    return Hash::size(alg_);
}

std::unique_ptr<Hash>
Hash::create(pgp_hash_alg_t alg)
{
    if (alg == PGP_HASH_SHA1) {
        return Hash_SHA1CD::create();
    }
#if !defined(ENABLE_SM2)
    if (alg == PGP_HASH_SM3) {
        RNP_LOG("SM3 hash is not available.");
        throw rnp_exception(RNP_ERROR_BAD_PARAMETERS);
    }
#endif
#if defined(CRYPTO_BACKEND_OPENSSL)
    return Hash_OpenSSL::create(alg);
#elif defined(CRYPTO_BACKEND_BOTAN)
    return Hash_Botan::create(alg);
#else
#error "Crypto backend not specified"
#endif
}

std::unique_ptr<CRC24>
CRC24::create()
{
#if defined(CRYPTO_BACKEND_OPENSSL)
    return CRC24_RNP::create();
#elif defined(CRYPTO_BACKEND_BOTAN)
    return CRC24_Botan::create();
#else
#error "Crypto backend not specified"
#endif
}

void
Hash::add(const std::vector<uint8_t> &val)
{
    add(val.data(), val.size());
}

void
Hash::add(uint32_t val)
{
    uint8_t ibuf[4];
    write_uint32(ibuf, val);
    add(ibuf, sizeof(ibuf));
}

void
Hash::add(const pgp::mpi &val)
{
    size_t len = val.size();
    size_t idx = 0;
    while ((idx < len) && (!val[idx])) {
        idx++;
    }

    if (idx >= len) {
        add(0);
        return;
    }

    add(len - idx);
    if (val[idx] & 0x80) {
        uint8_t padbyte = 0;
        add(&padbyte, 1);
    }
    add(val.data() + idx, len - idx);
}

std::vector<uint8_t>
Hash::finish()
{
    std::vector<uint8_t> res(size_, 0);
    finish(res.data());
    return res;
}

rnp::secure_bytes
Hash::sec_finish()
{
    rnp::secure_bytes res(size_, 0);
    finish(res.data());
    return res;
}

Hash::~Hash()
{
}

pgp_hash_alg_t
Hash::alg(const char *name)
{
    if (!name) {
        return PGP_HASH_UNKNOWN;
    }
    for (size_t i = 0; i < ARRAY_SIZE(hash_alg_map); i++) {
        if (rnp::str_case_eq(name, hash_alg_map[i].name)) {
            return hash_alg_map[i].type;
        }
    }
    return PGP_HASH_UNKNOWN;
}

const char *
Hash::name(pgp_hash_alg_t alg)
{
    const char *ret = NULL;
    ARRAY_LOOKUP_BY_ID(hash_alg_map, type, name, alg, ret);
    return ret;
}

size_t
Hash::size(pgp_hash_alg_t alg)
{
    size_t val = 0;
    ARRAY_LOOKUP_BY_ID(hash_alg_map, type, len, alg, val);
    return val;
}

void
HashList::add_alg(pgp_hash_alg_t alg)
{
    if (!get(alg)) {
        hashes.emplace_back(rnp::Hash::create(alg));
    }
}

const Hash *
HashList::get(pgp_hash_alg_t alg) const
{
    for (auto &hash : hashes) {
        if (hash->alg() == alg) {
            return hash.get();
        }
    }
    return nullptr;
}

void
HashList::add(const void *buf, size_t len)
{
    for (auto &hash : hashes) {
        hash->add(buf, len);
    }
}

} // namespace rnp

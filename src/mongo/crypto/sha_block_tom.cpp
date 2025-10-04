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


#include "mongo/config.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/sha512_block.h"
#include "mongo/util/assert_util.h"

#ifdef MONGO_CONFIG_SSL
#error This file should not be included if compiling with SSL support
#endif

#include "tomcrypt.h"

namespace mongo {

namespace {

/**
 * Computes a SHA hash of 'input'.
 */
template <typename HashType,
          int (*Init)(hash_state*),
          int (*Process)(hash_state*, const unsigned char*, unsigned long),
          int (*Done)(hash_state*, unsigned char*)>
void computeHashImpl(std::initializer_list<ConstDataRange> input, HashType* const output) {
    hash_state hashState;
    fassert(40381,
            Init(&hashState) == CRYPT_OK &&
                std::all_of(begin(input),
                            end(input),
                            [&](const auto& i) {
                                return Process(&hashState,
                                               reinterpret_cast<const unsigned char*>(i.data()),
                                               i.length()) == CRYPT_OK;
                            }) &&
                Done(&hashState, output->data()) == CRYPT_OK);
}

/*
 * Computes a HMAC SHA'd keyed hash of 'input' using the key 'key', writes output into 'output'.
 */
template <typename HashType>
void computeHmacImpl(const ltc_hash_descriptor* desc,
                     const uint8_t* key,
                     size_t keyLen,
                     std::initializer_list<ConstDataRange> input,
                     HashType* const output) {
    invariant(key);
    invariant(
        std::all_of(begin(input), end(input), [&](const auto& i) { return i.data() != nullptr; }));

    static const struct Magic {
        Magic(const ltc_hash_descriptor* desc) {
            register_hash(desc);
            hashId = find_hash(desc->name);
        }

        int hashId;
    } magic(desc);

    Hmac_state hmacState;
    unsigned long shaHashLen = sizeof(HashType);

    fassert(40382,
            hmac_init(&hmacState, magic.hashId, key, keyLen) == CRYPT_OK &&
                std::all_of(begin(input),
                            end(input),
                            [&](const auto& i) {
                                return hmac_process(
                                           &hmacState,
                                           reinterpret_cast<const unsigned char*>(i.data()),
                                           i.length()) == CRYPT_OK;
                            }) &&
                hmac_done(&hmacState, output->data(), &shaHashLen) == CRYPT_OK);
}

}  // namespace

void SHA1BlockTraits::computeHash(std::initializer_list<ConstDataRange> input,
                                  HashType* const output) {
    computeHashImpl<SHA1BlockTraits::HashType, sha1_init, sha1_process, sha1_done>(input, output);
}

void SHA256BlockTraits::computeHash(std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    computeHashImpl<SHA256BlockTraits::HashType, sha256_init, sha256_process, sha256_done>(input,
                                                                                           output);
}

void SHA512BlockTraits::computeHash(std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    computeHashImpl<SHA512BlockTraits::HashType, sha512_init, sha512_process, sha512_done>(input,
                                                                                           output);
}

void SHA1BlockTraits::computeHmac(const uint8_t* key,
                                  size_t keyLen,
                                  std::initializer_list<ConstDataRange> input,
                                  HashType* const output) {
    return computeHmacImpl<HashType>(&sha1_desc, key, keyLen, input, output);
}

void SHA1BlockTraits::computeHmacWithCtx(HmacContext* ctx,
                                         const uint8_t* key,
                                         size_t keyLen,
                                         std::initializer_list<ConstDataRange> input,
                                         HashType* const output) {
    return SHA1BlockTraits::computeHmac(key, keyLen, input, output);
}

void SHA256BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    return computeHmacImpl<HashType>(&sha256_desc, key, keyLen, input, output);
}

void SHA256BlockTraits::computeHmacWithCtx(HmacContext*,
                                           const uint8_t* key,
                                           size_t keyLen,
                                           std::initializer_list<ConstDataRange> input,
                                           HashType* const output) {
    return SHA256BlockTraits::computeHmac(key, keyLen, input, output);
}

void SHA512BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    return computeHmacImpl<HashType>(&sha512_desc, key, keyLen, input, output);
}

void SHA512BlockTraits::computeHmacWithCtx(HmacContext*,
                                           const uint8_t* key,
                                           size_t keyLen,
                                           std::initializer_list<ConstDataRange> input,
                                           HashType* const output) {
    return SHA512BlockTraits::computeHmac(key, keyLen, input, output);
}
}  // namespace mongo

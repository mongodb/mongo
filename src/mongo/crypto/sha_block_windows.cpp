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

#include "mongo/platform/basic.h"

#include <initializer_list>

#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/crypto/sha512_block.h"

#include "mongo/config.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

/**
 * Class to load singleton instances of each SHA algorithm.
 */
class BCryptHashLoader {
public:
    BCryptHashLoader() {
        loadAlgo(&_algoSHA1, BCRYPT_SHA1_ALGORITHM, false);
        loadAlgo(&_algoSHA256, BCRYPT_SHA256_ALGORITHM, false);
        loadAlgo(&_algoSHA512, BCRYPT_SHA512_ALGORITHM, false);

        loadAlgo(&_algoSHA1Hmac, BCRYPT_SHA1_ALGORITHM, true);
        loadAlgo(&_algoSHA256Hmac, BCRYPT_SHA256_ALGORITHM, true);
        loadAlgo(&_algoSHA512Hmac, BCRYPT_SHA512_ALGORITHM, true);
    }

    ~BCryptHashLoader() {
        invariant(BCryptCloseAlgorithmProvider(_algoSHA512, 0) == STATUS_SUCCESS);
        invariant(BCryptCloseAlgorithmProvider(_algoSHA256, 0) == STATUS_SUCCESS);
        invariant(BCryptCloseAlgorithmProvider(_algoSHA1, 0) == STATUS_SUCCESS);
        invariant(BCryptCloseAlgorithmProvider(_algoSHA512Hmac, 0) == STATUS_SUCCESS);
        invariant(BCryptCloseAlgorithmProvider(_algoSHA256Hmac, 0) == STATUS_SUCCESS);
        invariant(BCryptCloseAlgorithmProvider(_algoSHA1Hmac, 0) == STATUS_SUCCESS);
    }

    BCRYPT_ALG_HANDLE getAlgoSHA512() {
        return _algoSHA512;
    }

    BCRYPT_ALG_HANDLE getAlgoSHA256() {
        return _algoSHA256;
    }

    BCRYPT_ALG_HANDLE getAlgoSHA1() {
        return _algoSHA1;
    }

    BCRYPT_ALG_HANDLE getAlgoSHA512Hmac() {
        return _algoSHA512Hmac;
    };

    BCRYPT_ALG_HANDLE getAlgoSHA256Hmac() {
        return _algoSHA256Hmac;
    };

    BCRYPT_ALG_HANDLE getAlgoSHA1Hmac() {
        return _algoSHA1Hmac;
    };

private:
    void loadAlgo(BCRYPT_ALG_HANDLE* algo, const wchar_t* name, bool isHmac) {
        invariant(
            BCryptOpenAlgorithmProvider(
                algo, name, MS_PRIMITIVE_PROVIDER, isHmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) ==
            STATUS_SUCCESS);
    }

private:
    BCRYPT_ALG_HANDLE _algoSHA512;
    BCRYPT_ALG_HANDLE _algoSHA256;
    BCRYPT_ALG_HANDLE _algoSHA1;
    BCRYPT_ALG_HANDLE _algoSHA512Hmac;
    BCRYPT_ALG_HANDLE _algoSHA256Hmac;
    BCRYPT_ALG_HANDLE _algoSHA1Hmac;
};

static BCryptHashLoader& getBCryptHashLoader() {
    static BCryptHashLoader* loader = new BCryptHashLoader();
    return *loader;
}

/**
 * Computes a SHA hash of 'input'.
 */
template <typename HashType>
HashType computeHashImpl(BCRYPT_ALG_HANDLE algo, std::initializer_list<ConstDataRange> input) {
    HashType output;

    BCRYPT_HASH_HANDLE hHash;

    fassert(50725,
            BCryptCreateHash(algo, &hHash, NULL, 0, NULL, 0, 0) == STATUS_SUCCESS &&

                std::all_of(begin(input),
                            end(input),
                            [&](const auto& i) {
                                return BCryptHashData(
                                           hHash,
                                           reinterpret_cast<PUCHAR>(const_cast<char*>(i.data())),
                                           i.length(),
                                           0) == STATUS_SUCCESS;
                            }) &&

                BCryptFinishHash(hHash, output.data(), output.size(), 0) == STATUS_SUCCESS &&

                BCryptDestroyHash(hHash) == STATUS_SUCCESS);

    return output;
}

/**
 * Computes a HMAC SHA'd keyed hash of 'input' using the key 'key', writes output into 'output'.
 */
template <typename HashType>
void computeHmacImpl(BCRYPT_ALG_HANDLE algo,
                     const uint8_t* key,
                     size_t keyLen,
                     std::initializer_list<ConstDataRange> input,
                     HashType* const output) {
    invariant(key);

    BCRYPT_HASH_HANDLE hHash;

    fassert(50726,
            BCryptCreateHash(algo, &hHash, NULL, 0, const_cast<PUCHAR>(key), keyLen, 0) ==
                    STATUS_SUCCESS &&

                std::all_of(begin(input),
                            end(input),
                            [&](const auto& i) {
                                return BCryptHashData(
                                           hHash,
                                           reinterpret_cast<PUCHAR>(const_cast<char*>(i.data())),
                                           i.length(),
                                           0) == STATUS_SUCCESS;
                            }) &&

                BCryptFinishHash(hHash, output->data(), output->size(), 0) == STATUS_SUCCESS &&

                BCryptDestroyHash(hHash) == STATUS_SUCCESS);
}

}  // namespace

SHA1BlockTraits::HashType SHA1BlockTraits::computeHash(
    std::initializer_list<ConstDataRange> input) {
    return computeHashImpl<SHA1BlockTraits::HashType>(getBCryptHashLoader().getAlgoSHA1(),
                                                      std::move(input));
}

SHA256BlockTraits::HashType SHA256BlockTraits::computeHash(
    std::initializer_list<ConstDataRange> input) {
    return computeHashImpl<SHA256BlockTraits::HashType>(getBCryptHashLoader().getAlgoSHA256(),
                                                        std::move(input));
}

SHA512BlockTraits::HashType SHA512BlockTraits::computeHash(
    std::initializer_list<ConstDataRange> input) {
    return computeHashImpl<SHA512BlockTraits::HashType>(getBCryptHashLoader().getAlgoSHA512(),
                                                        std::move(input));
}

void SHA1BlockTraits::computeHmac(const uint8_t* key,
                                  size_t keyLen,
                                  std::initializer_list<ConstDataRange> input,
                                  HashType* const output) {
    return computeHmacImpl<HashType>(
        getBCryptHashLoader().getAlgoSHA1Hmac(), key, keyLen, input, output);
}

void SHA256BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    return computeHmacImpl<HashType>(
        getBCryptHashLoader().getAlgoSHA256Hmac(), key, keyLen, input, output);
}

void SHA512BlockTraits::computeHmac(const uint8_t* key,
                                    size_t keyLen,
                                    std::initializer_list<ConstDataRange> input,
                                    HashType* const output) {
    return computeHmacImpl<HashType>(
        getBCryptHashLoader().getAlgoSHA512Hmac(), key, keyLen, input, output);
}

}  // namespace mongo

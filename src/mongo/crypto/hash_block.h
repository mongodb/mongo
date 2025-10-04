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

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/base/secure_allocator.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"
#include "mongo/util/secure_compare_memory.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <absl/hash/hash.h>

#if defined(MONGO_CONFIG_SSL) && (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL)
#include <openssl/hmac.h>
#endif

namespace mongo {

struct BSONBinData;
class BSONObjBuilder;

/**
 * For an OpenSSL optimization where a hash needs to be computed many times in succession,
 * we can re-use the ctx object by calling init. However, this needs to be cross compatible
 * with Windows and Apple, so we define a wrapping struct that is effectively a no-op on
 * the other two platforms.
 */
class HmacContext {
public:
    /**
     * When we reuse a key with the same hmac context, we can get a performance benefit by
     * not setting the key during the HMAC_Init_ex function. To use this API, the user should
     * identify the location where we will reuse a key. Before using the key for the first time,
     * they should call setReuseKey(true). Before they change to a new key with the same
     * HmacContext object, they must call setReuseKey(false), OR if they intend to reuse the
     * next key, they may call resetCount().
     */
    void setReuseKey(bool val);
    void resetCount();
#if defined(MONGO_CONFIG_SSL) && (MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL)
    int hmacCtxInitFn(const EVP_MD* md, const uint8_t* key, size_t keyLen);
    HmacContext();
    ~HmacContext();
    HMAC_CTX* get();

private:
    // Because OpenSSL <= 1.0.2 does not define HMAC_CTX_free function,
    // we cannot add a custom deleter for the unique pointer. Instead we
    // have to manage the lifetime of the ctx object manually.
    HMAC_CTX* hmac_ctx;
#endif
private:
    bool getReuseKey();
    int useCount();
    bool _reuseKey = false;
    int use = 0;
};

/**
 * Secure allocator wrapper for HashBlock Traits.
 *
 * Usage:
 *   HashBlock<SecureTraits<SHA256BlockTraits>> secureBlock;
 * or as a convenience:
 *   SHA256Block::Secure secureBlock;
 */
template <typename Traits>
class SecureTraits {
public:
    using HashType = typename Traits::HashType;
    static constexpr auto name = Traits::name;

    SecureTraits() = default;
    SecureTraits(const SecureTraits& hash) : _hash(hash) {}
    SecureTraits(const Traits& hash) : _hash(hash) {}

    static void computeHash(std::initializer_list<ConstDataRange> input, HashType* const output) {
        Traits::computeHash(input, output);
    }

    static void computeHmac(const uint8_t* key,
                            size_t keyLen,
                            std::initializer_list<ConstDataRange> input,
                            HashType* const output) {
        Traits::computeHmac(key, keyLen, input, output);
    }

    auto data() {
        return _hash->data();
    }

    auto size() const {
        return _hash->size();
    }

private:
    using SecureHashType = SecureAllocatorDefaultDomain::SecureHandle<HashType>;

    SecureHashType _hash;
};

/**
 * Data structure with fixed sized byte array that can be used as HMAC key or result of a SHA
 * computation.
 */
template <typename Traits>
class HashBlock {
public:
    using Secure = HashBlock<SecureTraits<Traits>>;
    using HashType = typename Traits::HashType;
    static constexpr size_t kHashLength = sizeof(HashType);
    static constexpr auto kName = Traits::name;

    HashBlock() = default;

    HashBlock(HashType rawHash) : _hash(rawHash) {}

    /**
     * Constructs a HashBlock from a buffer of specified size.
     */
    static StatusWith<HashBlock> fromBuffer(const uint8_t* input, size_t inputLen) {
        if (inputLen != kHashLength) {
            return {ErrorCodes::InvalidLength,
                    str::stream() << "Unsupported " << Traits::name
                                  << " hash length: " << inputLen};
        }

        HashType newHash;
        memcpy(newHash.data(), input, inputLen);
        return HashBlock(newHash);
    }

    static StatusWith<HashBlock> fromHexStringNoThrow(StringData hex) {
        if (!hexblob::validate(hex)) {
            return {ErrorCodes::BadValue, "Hash input is not a hex string"};
        }

        BufBuilder buf;
        hexblob::decode(hex, &buf);
        return fromBuffer(reinterpret_cast<const uint8_t*>(buf.buf()), buf.len());
    }

    static HashBlock fromHexString(StringData hex) {
        return uassertStatusOK(fromHexStringNoThrow(hex));
    }

    static void computeHash(std::initializer_list<ConstDataRange> input, HashBlock* const output) {
        Traits::computeHash(input, &(output->_hash));
    }

    /**
     * Computes a hash of 'input' from multiple contigous buffers.
     */
    static HashBlock computeHash(std::initializer_list<ConstDataRange> input) {
        HashBlock ret;
        computeHash(input, &ret);
        return ret;
    }

    /**
     * Computes a hash of 'input' from one buffer.
     */
    static HashBlock computeHash(const uint8_t* input, size_t inputLen) {
        return computeHash({ConstDataRange(input, inputLen)});
    }

    /**
     * Computes a HMAC keyed hash of 'input' using the key 'key'.
     */
    static HashBlock computeHmac(const uint8_t* key,
                                 size_t keyLen,
                                 const uint8_t* input,
                                 size_t inputLen) {
        HashBlock output;
        HashBlock::computeHmac(key, keyLen, {ConstDataRange(input, inputLen)}, &output);
        return output;
    }

    /**
     * Computes a HMAC keyed hash of 'input' using the key 'key'. Writes the results into
     * a pre-allocated HashBlock. This lets us allocate HashBlocks with the SecureAllocator.
     */
    static void computeHmac(const uint8_t* key,
                            size_t keyLen,
                            const uint8_t* input,
                            size_t inputLen,
                            HashBlock* const output) {
        HashBlock::computeHmac(key, keyLen, {ConstDataRange(input, inputLen)}, output);
    }

    static HashBlock computeHmac(const uint8_t* key,
                                 size_t keyLen,
                                 std::initializer_list<ConstDataRange> input) {
        HashBlock output;
        HashBlock::computeHmac(key, keyLen, input, &output);
        return output;
    }

    static void computeHmac(const uint8_t* key,
                            size_t keyLen,
                            std::initializer_list<ConstDataRange> input,
                            HashBlock* const output) {
        Traits::computeHmac(key, keyLen, input, &(output->_hash));
    }

    /**
     * This function is an alternative to computeHmac. It provides an optimization - when
     * a single thread needs to compute a hash repeatedly on the OpenSSL platform, it can
     * provide a ctx object of its own (which is an empty object in Apple and Windows)
     * that will be re-used by being re-initialized when computing an Hmac.
     */
    static void computeHmacWithCtx(HmacContext* ctx,
                                   const uint8_t* key,
                                   size_t keyLen,
                                   std::initializer_list<ConstDataRange> input,
                                   HashBlock* const output) {
        Traits::computeHmacWithCtx(ctx, key, keyLen, input, &(output->_hash));
    }

    const uint8_t* data() const& {
        return _hash.data();
    }

    uint8_t* data() && = delete;

    ConstDataRange toCDR() && = delete;

    ConstDataRange toCDR() const& {
        return ConstDataRange(reinterpret_cast<const char*>(_hash.data()), kHashLength);
    }

    size_t size() const {
        return _hash.size();
    }

    /**
     * Make a new HashBlock from a BSON BinData value.
     */
    static StatusWith<HashBlock> fromBinData(const BSONBinData& binData) {
        if (binData.type != BinDataGeneral) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << Traits::name << " only accepts BinDataGeneral type"};
        }

        if (binData.length != kHashLength) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "Unsupported " << Traits::name
                                  << " hash length: " << binData.length};
        }

        HashType newHash;
        memcpy(newHash.data(), binData.data, binData.length);
        return HashBlock(newHash);
    }

    /**
     * Make a new HashBlock from a vector of bytes representing bindata. For IDL.
     */
    static HashBlock fromBinData(const std::vector<unsigned char>& bytes) {
        HashType newHash;
        uassert(ErrorCodes::UnsupportedFormat,
                str::stream() << "Unsupported " << Traits::name << " hash length: " << bytes.size(),
                bytes.size() == kHashLength);
        memcpy(newHash.data(), bytes.data(), bytes.size());
        return HashBlock(newHash);
    }

    /**
     * Append this to a builder using the given name as a BSON BinData type value.
     */
    void appendAsBinData(BSONObjBuilder& builder, StringData fieldName) const {
        builder.appendBinData(fieldName, _hash.size(), BinDataGeneral, _hash.data());
    }

    /**
     * Do a bitwise xor against another HashBlock and replace the current contents of this block
     * with the result.
     */
    void xorInline(const HashBlock& other) {
        for (size_t x = 0; x < _hash.size(); x++) {
            _hash[x] ^= other._hash[x];
        }
    }

    /**
     * Base64 encoding of the sha block as a string.
     */
    std::string toString() const {
        return base64::encode(
            StringData(reinterpret_cast<const char*>(_hash.data()), _hash.size()));
    }

    /**
     * Hex encoded hash block.
     */
    std::string toHexString() const {
        return hexblob::encode(_hash.data(), _hash.size());
    }

    bool operator==(const HashBlock& other) const {
        return consttimeMemEqual(this->_hash.data(), other._hash.data(), kHashLength);
    }

    bool operator!=(const HashBlock& other) const {
        return !(*this == other);
    }

    bool operator<(const HashBlock& other) const {
        return this->_hash < other._hash;
    }

    bool operator==(const HashBlock::Secure& other) const {
        return consttimeMemEqual(this->_hash.data(), other.data(), kHashLength);
    }

    bool operator!=(const HashBlock::Secure& other) const {
        return !(*this == other);
    }

    /**
     * Custom hasher so HashBlocks can be used in unordered data structures.
     *
     * ex: std::unordered_set<HashBlock, HashBlock::Hash> shaSet;
     *
     * Cryptographically secure hashes are good hashes so no need to hash them again. Just truncate
     * the hash and return it.
     */
    struct Hash {
        std::size_t operator()(const HashBlock& HashBlock) const {
            static_assert(kHashLength >= sizeof(std::size_t));

            return ConstDataView(reinterpret_cast<const char*>(HashBlock.data()))
                .read<LittleEndian<std::size_t>>();
        }
    };

    /**
     * Hash function compatible with absl::Hash for absl::unordered_{map,set}
     */
    template <typename H>
    friend H AbslHashValue(H h, const HashBlock& HashBlock) {
        static_assert(kHashLength >= sizeof(std::size_t));

        return H::combine(std::move(h), Hash()(HashBlock));
    }

private:
    // The backing array of bytes for the sha block
    HashType _hash;
};


template <typename Traits>
std::ostream& operator<<(std::ostream& os, const HashBlock<Traits>& sha) {
    return os << sha.toString();
}

}  // namespace mongo

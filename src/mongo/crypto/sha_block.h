/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/base64.h"
#include "mongo/util/secure_compare_memory.h"

namespace mongo {

struct BSONBinData;
class BSONObjBuilder;

/**
 * Data structure with fixed sized byte array that can be used as HMAC key or result of a SHA
 * computation.
 */
template <typename Traits>
class SHABlock {
public:
    using HashType = typename Traits::HashType;
    static constexpr size_t kHashLength = sizeof(HashType);

    SHABlock() = default;

    SHABlock(HashType rawHash) : _hash(rawHash) {}

    /**
     * Constructs a SHABlock from a buffer of specified size.
     */
    static StatusWith<SHABlock> fromBuffer(const uint8_t* input, size_t inputLen) {
        if (inputLen != kHashLength) {
            return {
                ErrorCodes::InvalidLength,
                str::stream() << "Unsupported " << Traits::name << " hash length: " << inputLen};
        }

        HashType newHash;
        memcpy(newHash.data(), input, inputLen);
        return SHABlock(newHash);
    }

    /**
     * Computes a hash of 'input' from multiple contigous buffers.
     */
    static SHABlock computeHash(std::initializer_list<ConstDataRange> input) {
        return SHABlock{Traits::computeHash(input)};
    }

    /**
     * Computes a hash of 'input' from one buffer.
     */
    static SHABlock computeHash(const uint8_t* input, size_t inputLen) {
        return computeHash({ConstDataRange(reinterpret_cast<const char*>(input), inputLen)});
    }

    /**
     * Computes a HMAC keyed hash of 'input' using the key 'key'.
     */
    static SHABlock computeHmac(const uint8_t* key,
                                size_t keyLen,
                                const uint8_t* input,
                                size_t inputLen) {
        SHABlock output;
        SHABlock::computeHmac(key, keyLen, input, inputLen, &output);
        return output;
    }

    /**
     * Computes a HMAC keyed hash of 'input' using the key 'key'. Writes the results into
     * a pre-allocated SHABlock. This lets us allocate SHABlocks with the SecureAllocator.
     */
    static void computeHmac(const uint8_t* key,
                            size_t keyLen,
                            const uint8_t* input,
                            size_t inputLen,
                            SHABlock* const output) {
        return Traits::computeHmac(key, keyLen, input, inputLen, &(output->_hash));
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
     * Make a new SHABlock from a BSON BinData value.
     */
    static StatusWith<SHABlock> fromBinData(const BSONBinData& binData) {
        if (binData.type != BinDataGeneral) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << Traits::name << " only accepts BinDataGeneral type"};
        }

        if (binData.length != kHashLength) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "Unsupported " << Traits::name << " hash length: "
                                  << binData.length};
        }

        HashType newHash;
        memcpy(newHash.data(), binData.data, binData.length);
        return SHABlock(newHash);
    }

    /**
     * Make a new SHABlock from a vector of bytes representing bindata. For IDL.
     */
    static SHABlock fromBinData(const std::vector<unsigned char>& bytes) {
        HashType newHash;
        uassert(ErrorCodes::UnsupportedFormat,
                str::stream() << "Unsupported " << Traits::name << " hash length: " << bytes.size(),
                bytes.size() == kHashLength);
        memcpy(newHash.data(), bytes.data(), bytes.size());
        return SHABlock(newHash);
    }

    /**
     * Append this to a builder using the given name as a BSON BinData type value.
     */
    void appendAsBinData(BSONObjBuilder& builder, StringData fieldName) const {
        builder.appendBinData(fieldName, _hash.size(), BinDataGeneral, _hash.data());
    }

    /**
     * Do a bitwise xor against another SHABlock and replace the current contents of this block
     * with the result.
     */
    void xorInline(const SHABlock& other) {
        for (size_t x = 0; x < _hash.size(); x++) {
            _hash[x] ^= other._hash[x];
        }
    }

    /**
     * Base64 encoding of the sha block as a string.
     */
    std::string toString() const {
        return base64::encode(reinterpret_cast<const char*>(_hash.data()), _hash.size());
    }

    bool operator==(const SHABlock& other) const {
        return consttimeMemEqual(this->_hash.data(), other._hash.data(), kHashLength);
    }

    bool operator!=(const SHABlock& other) const {
        return !(*this == other);
    }

private:
    // The backing array of bytes for the sha block
    HashType _hash;
};

template <typename T>
constexpr size_t SHABlock<T>::kHashLength;

template <typename Traits>
std::ostream& operator<<(std::ostream& os, const SHABlock<Traits>& sha) {
    return os << sha.toString();
}

}  // namespace mongo

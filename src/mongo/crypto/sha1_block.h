/**
*    Copyright (C) 2017 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "mongo/base/status_with.h"

namespace mongo {

struct BSONBinData;
class BSONObjBuilder;

/**
 * Data structure with fixed sized byte array that can be used as HMAC key or result of a SHA1
 * computation.
 */
class SHA1Block {
public:
    static constexpr size_t kHashLength = 20;
    using HashType = std::array<std::uint8_t, kHashLength>;

    SHA1Block() = default;
    SHA1Block(HashType rawHash);
    static StatusWith<SHA1Block> fromBuffer(const uint8_t* input, size_t inputLen);

    /**
     * Computes a SHA-1 hash of 'input'.
     */
    static SHA1Block computeHash(const uint8_t* input, size_t inputLen);

    /**
     * Computes a HMAC SHA-1 keyed hash of 'input' using the key 'key'
     */
    static SHA1Block computeHmac(const uint8_t* key,
                                 size_t keyLen,
                                 const uint8_t* input,
                                 size_t inputLen) {
        SHA1Block output;
        SHA1Block::computeHmac(key, keyLen, input, inputLen, &output);
        return output;
    }

    /**
     * Computes a HMAC SHA-1 keyed hash of 'input' using the key 'key'. Writes the results into
     * a pre-allocated SHA1Block. This lets us allocate SHA1Blocks with the SecureAllocator.
     */
    static void computeHmac(const uint8_t* key,
                            size_t keyLen,
                            const uint8_t* input,
                            size_t inputLen,
                            SHA1Block* const output);

    const uint8_t* data() const& {
        return _hash.data();
    }

    uint8_t* data() const&& = delete;

    size_t size() const {
        return _hash.size();
    }

    /**
     * Make a new SHA1Block from a BSON BinData value.
     */
    static StatusWith<SHA1Block> fromBinData(const BSONBinData& binData);

    /**
     * Append this to a builder using the given name as a BSON BinData type value.
     */
    void appendAsBinData(BSONObjBuilder& builder, StringData fieldName);

    /**
     * Do a bitwise xor against another SHA1Block and replace the current contents of this block
     * with the result.
     */
    void xorInline(const SHA1Block& other);

    std::string toString() const;
    bool operator==(const SHA1Block& rhs) const;

private:
    HashType _hash;
};

}  // namespace mongo

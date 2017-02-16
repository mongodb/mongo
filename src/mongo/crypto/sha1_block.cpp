/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/crypto/sha1_block.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/base64.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

constexpr size_t SHA1Block::kHashLength;

SHA1Block::SHA1Block(HashType hash) : _hash(std::move(hash)) {}

StatusWith<SHA1Block> SHA1Block::fromBuffer(const uint8_t* input, size_t inputLen) {
    if (inputLen != kHashLength) {
        return {ErrorCodes::InvalidLength,
                str::stream() << "Unsupported SHA1Hash hash length: " << inputLen};
    }

    HashType newHash;
    memcpy(newHash.data(), input, inputLen);
    return SHA1Block(newHash);
}

StatusWith<SHA1Block> SHA1Block::fromBinData(const BSONBinData& binData) {
    if (binData.type != BinDataGeneral) {
        return {ErrorCodes::UnsupportedFormat, "SHA1Block only accepts BinDataGeneral type"};
    }

    if (binData.length != kHashLength) {
        return {ErrorCodes::UnsupportedFormat,
                str::stream() << "Unsupported SHA1Block hash length: " << binData.length};
    }

    HashType newHash;
    memcpy(newHash.data(), binData.data, binData.length);
    return SHA1Block(newHash);
}

std::string SHA1Block::toString() const {
    return base64::encode(reinterpret_cast<const char*>(_hash.data()), _hash.size());
}

void SHA1Block::appendAsBinData(BSONObjBuilder& builder, StringData fieldName) {
    builder.appendBinData(fieldName, _hash.size(), BinDataGeneral, _hash.data());
}

void SHA1Block::xorInline(const SHA1Block& other) {
    for (size_t x = 0; x < _hash.size(); x++) {
        _hash[x] ^= other._hash[x];
    }
}

bool SHA1Block::operator==(const SHA1Block& rhs) const {
    return rhs._hash == this->_hash;
}

}  // namespace mongo

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <vector>

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/storage/column_store.h"

namespace mongo::sbe::value {
/**
 * This encoder provides a mechanism to represent BSON values as 'sbe::value' pairs, suitable for
 * use in the SBE VM.
 *
 * Values returned by 'operator()' are _unowned_ and are invalidated by the next 'operator()' call
 * or when the 'ColumnStoreEncoder' is destroyed. Additionally, modifying or destroying any string
 * or BSONElement passed to 'operator()' can also invalidate the resulting value.
 *
 * Callers that need long-lived values should make "owned" copies.
 */
struct ColumnStoreEncoder {
    using Out = boost::optional<std::pair<TypeTags, Value>>;

    std::pair<TypeTags, Value> operator()(BSONElement value) {
        return bson::convertFrom<true>(value);
    }

    std::pair<TypeTags, Value> operator()(NullLabeler) {
        return {TypeTags::Null, 0};
    }

    std::pair<TypeTags, Value> operator()(MinKeyLabeler) {
        return {TypeTags::MinKey, 0};
    }

    std::pair<TypeTags, Value> operator()(MaxKeyLabeler) {
        return {TypeTags::MaxKey, 0};
    }

    std::pair<TypeTags, Value> operator()(BSONObj value) {
        tassert(6343901, "Unexpected non-trivial object in columnar value", value.isEmpty());
        return {TypeTags::Object, bitcastFrom<const Object*>(&emptyObject)};
    }

    std::pair<TypeTags, Value> operator()(BSONArray value) {
        tassert(6343902, "Unexpected non-trivial array in columnar value", value.isEmpty());
        return {TypeTags::Array, bitcastFrom<const Array*>(&emptyArray)};
    }

    std::pair<TypeTags, Value> operator()(bool value) {
        return {TypeTags::Boolean, bitcastFrom<bool>(value)};
    }

    std::pair<TypeTags, Value> operator()(int32_t value) {
        return {TypeTags::NumberInt32, bitcastFrom<int32_t>(value)};
    }

    std::pair<TypeTags, Value> operator()(int64_t value) {
        return {TypeTags::NumberInt64, bitcastFrom<int64_t>(value)};
    }

    std::pair<TypeTags, Value> operator()(double value) {
        return {TypeTags::NumberDouble, bitcastFrom<double>(value)};
    }

    std::pair<TypeTags, Value> operator()(StringData value) {
        if (canUseSmallString(value)) {
            auto [tag, newValue] = makeSmallString(value);
            return {tag, newValue};
        } else {
            uassert(6343903,
                    "Expected large strings to be encoded as a BSONElement",
                    value.size() + 5 <= temporaryStorage.size());

            auto outputPtr = temporaryStorage.data();
            DataView(std::exchange(outputPtr, outputPtr + sizeof(int32_t)))
                .write<LittleEndian<int32_t>>(value.size() + 1);
            memcpy(
                std::exchange(outputPtr, outputPtr + value.size()), value.rawData(), value.size());
            DataView(outputPtr).write<LittleEndian<char>>('\0');

            return {TypeTags::StringBig, bitcastFrom<const char*>(temporaryStorage.data())};
        }
    }

    std::pair<TypeTags, Value> operator()(Decimal128 value) {
        DataView(temporaryStorage.data()).write(value);
        return {TypeTags::NumberDecimal, bitcastFrom<const char*>(temporaryStorage.data())};
    }

    std::pair<TypeTags, Value> operator()(const OID& value) {
        auto oidBytes = value.view().view();
        std::copy(oidBytes, oidBytes + OID::kOIDSize, temporaryStorage.begin());

        return {TypeTags::ObjectId, bitcastFrom<const char*>(temporaryStorage.data())};
    }

    std::pair<TypeTags, Value> operator()(const UUID& value) {
        // The 'ColumnStoreEncoder' returns a UUID by formatting it in temporary storage as a
        // BSONBinData object.

        // Write the payload length.
        DataView binDataView(temporaryStorage.data());
        std::ptrdiff_t offset = 0;
        binDataView.write<LittleEndian<uint32_t>>(UUID::kNumBytes,
                                                  std::exchange(offset, offset + sizeof(uint32_t)));

        // Write the BinDataType value.
        binDataView.write<char>(newUUID, std::exchange(offset, offset + 1));

        // Write the UUID payload.
        static_assert(sizeof(value.data()) == UUID::kNumBytes);
        binDataView.write(value.data(), offset);  // No need to update 'offset' for the last write.

        return {TypeTags::ObjectId, bitcastFrom<const char*>(temporaryStorage.data())};
    }

private:
    static const Object emptyObject;
    static const Array emptyArray;

    static constexpr std::size_t kSizeOfDecimal = 2 * sizeof(long long);
    static constexpr std::size_t kSizeOfUUIDBinData = sizeof(uint32_t)   // Length field
        + 1                                                              // BinData type field
        + UUID::kNumBytes;                                               // UUID payload
    static constexpr std::size_t kSizeOfStringBuffer = sizeof(uint32_t)  // Length field
        + ColumnStore::Bytes::TinySize::kStringMax                       // String
        + 1;                                                             // Null terminator
#ifdef _GLIBCXX_DEBUG
    static constexpr std::size_t kSizeOfTemporary = 69;
    static_assert(kSizeOfStringBuffer >
                  (kSizeOfUUIDBinData + kSizeOfDecimal +
                   OID::kOIDSize));  // kSizeOfStringBuffer should be the largest - this is
                                     // shorthand for writing 4 checks
    static_assert(kSizeOfStringBuffer == 69);  // kSizeOfStringBuffer is 69
#else
    static constexpr std::size_t kSizeOfTemporary = std::max<std::size_t>(
        {kSizeOfDecimal, OID::kOIDSize, kSizeOfUUIDBinData, kSizeOfStringBuffer});
#endif

    std::array<char, kSizeOfTemporary> temporaryStorage;
};
}  // namespace mongo::sbe::value

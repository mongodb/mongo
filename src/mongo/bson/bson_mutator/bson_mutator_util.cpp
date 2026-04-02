/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/bson/bson_mutator/bson_mutator.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/crypto/fle_field_schema_gen.h"

namespace mongo::bson_mutator {

/**
 * Provides an Arbitrary mongo::Date_t
 * can optionally can be seeded with a `long long` input
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryDate_T())
 *      .WithSeeds({1LL, 2LL, 3LL});
 */
fuzztest::Domain<Date_t> ArbitraryDate_T() {
    return fuzztest::ReversibleMap(
        // long long -> Date_t
        [](long long timestamp) { return Date_t::fromMillisSinceEpoch(timestamp); },
        // Date_t -> long long
        [](const Date_t& date) {
            return std::optional{std::tuple<long long>{date.toMillisSinceEpoch()}};
        },
        // Input domain
        fuzztest::Arbitrary<long long>());
}

/**
 * Provides an Arbitrary mongo::Decimal128
 * can optionally be seeded with two `uint64_t` inputs
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryDecimal128())
 *      .WithSeeds({{1,1}, {2,2}, {3,3}});
 */
fuzztest::Domain<Decimal128> ArbitraryDecimal128() {
    return fuzztest::ReversibleMap(
        [](uint64_t low64, uint64_t high64) {
            return Decimal128(Decimal128::Value{low64, high64});
        },
        [](const Decimal128& dec) {
            auto val = dec.getValue();
            return std::optional{std::tuple{val.low64, val.high64}};
        },
        fuzztest::Arbitrary<uint64_t>(),
        fuzztest::Arbitrary<uint64_t>());
}

/**
 * Provides an Arbitrary mongo::Timestamp
 * can optionally be seeded with a `unsigned long long` input
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryTimestamp())
 *      .WithSeeds({1ULL, 2ULL, 3ULL});
 */
fuzztest::Domain<Timestamp> ArbitraryTimestamp() {
    return fuzztest::ReversibleMap([](unsigned long long val) { return Timestamp(val); },
                                   [](const Timestamp& timestamp) {
                                       return std::optional{
                                           std::tuple<unsigned long long>{timestamp.asLL()}};
                                   },
                                   fuzztest::Arbitrary<unsigned long long>());
}

/**
 * Provides an Arbitrary mongo::OID
 * can optionally be seeded with a `string` of `[a-zA-Z0-9]{24}`
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryOID())
 *      .WithSeeds({"abcdefghijABCDEFGHIJ1234"});
 */
fuzztest::Domain<OID> ArbitraryOID() {
    return fuzztest::ReversibleMap(
        [](std::string arr) { return OID(arr); },
        [](const OID& oid) { return std::optional{std::tuple{oid.toString()}}; },
        fuzztest::StringOf(fuzztest::OneOf(fuzztest::InRange('a', 'f'),
                                           fuzztest::InRange('A', 'F'),
                                           fuzztest::NumericChar()))
            .WithSize(24));
}

/**
 * Provides an Arbitrary mongo::BSONRegEx
 * can optionally be seeded with a `string` without null characters and
 * a `string` of `i?l?m?s?u?x?`
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBSONRegEx())
 *      .WithSeeds({
 *          {"foo", "i"},
 *          {"bar", "ms"},
 *          {"baz", "ilmsux"},
 *      });
 */
fuzztest::Domain<BSONRegExOwned> ArbitraryBSONRegEx() {
    return fuzztest::ReversibleMap(
        [](std::string pattern, std::string flags) { return BSONRegExOwned(pattern, flags); },
        [](const BSONRegExOwned& regex) {
            return std::optional{std::tuple{regex.patternOwned, regex.flagsOwned}};
        },
        fuzztest::StringOf(fuzztest::NonZeroChar()),
        fuzztest::InRegexp("i?l?m?s?u?x?"));
}

/**
 * Provides an Arbitrary mongo::BSONSymbol
 * can optionally be seeded with a `string` input
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBSONSymbol())
 *      .WithSeeds({"foo", "bar", "baz"});
 */
fuzztest::Domain<BSONSymbolOwned> ArbitraryBSONSymbol() {
    return fuzztest::ReversibleMap(
        [](std::string sym) { return BSONSymbolOwned(sym); },
        [](const BSONSymbolOwned& symbol) { return std::optional{std::tuple{symbol.symbolOwned}}; },
        fuzztest::Arbitrary<std::string>());
}

/**
 * Provides an Arbitrary mongo::BSONCode
 * can optionally be seeded with a `string` input
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBSONCode())
 *      .WithSeeds({"foo", "bar", "baz"});
 */
fuzztest::Domain<BSONCodeOwned> ArbitraryBSONCode() {
    return fuzztest::ReversibleMap(
        [](std::string code) { return BSONCodeOwned(code); },
        [](const BSONCodeOwned& code) { return std::optional{std::tuple{code.codeOwned}}; },
        fuzztest::Arbitrary<std::string>());
}

/**
 * Provides an Arbitrary mongo::BSONCodeWScope
 * can optionally be seeded with a `string` and a bson object
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBSONCodeWScope())
 *      .WithSeeds({
 *          {"foo", fromjson("{}")}
 *      });
 */
fuzztest::Domain<BSONCodeWScopeOwned> ArbitraryBSONCodeWScope() {
    return fuzztest::ReversibleMap(
        [](std::string code, ConstSharedBuffer scope) {
            return BSONCodeWScopeOwned(code, BSONObj(scope));
        },
        [](const BSONCodeWScopeOwned& obj) {
            return std::optional{std::tuple{obj.codeOwned, obj.scope.getOwned().sharedBuffer()}};
        },
        fuzztest::Arbitrary<std::string>(),
        fuzztest::Arbitrary<ConstSharedBuffer>());
}

/**
 * Provides an Arbitrary mongo::BSONDBRef
 * can optionally be seeded with a `string` and an OID (see ArbitraryOID)
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBSONDBRef())
 *      .WithSeeds({"foo", ""abcdefghijABCDEFGHIJ1234""});
 */
fuzztest::Domain<BSONDBRefOwned> ArbitraryBSONDBRef() {
    return fuzztest::ReversibleMap([](std::string ns, OID oid) { return BSONDBRefOwned(ns, oid); },
                                   [](const BSONDBRefOwned& dbref) {
                                       return std::optional{std::tuple{dbref.nsOwned, dbref.oid}};
                                   },
                                   fuzztest::Arbitrary<std::string>(),
                                   ArbitraryOID());
};

/**
 * Provides an Arbitrary mongo::BSONArray
 * can optionally be seeded with a BSONObj
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBSONDBRef())
 *      .WithSeeds({fromjson("[\"a\",\"b\",\"c\"]")});
 */
fuzztest::Domain<BSONArray> ArbitraryBSONArray() {
    return fuzztest::ReversibleMap(
        [](ConstSharedBuffer buf) {
            auto bob = BSONArrayBuilder();
            for (auto&& element : BSONObj(buf)) {
                bob.append(element);
            }
            return bob.arr();
        },
        [](const BSONArray& arr) {
            return std::optional{std::tuple{arr.getOwned().sharedBuffer()}};
        },
        fuzztest::Arbitrary<ConstSharedBuffer>());
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::BinDataGeneral
 * can optionally be seeded with a std::vector<uint8_t>
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBinDataGeneral())
 *      .WithSeeds({
 *          {1,2,3,4,5},
 *          {6,7,8,9,10}
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryBinDataGeneral() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) { return BSONBinDataOwned(v, BinDataType::BinDataGeneral); },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::BinDataGeneral) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>());
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::Function
 * can optionally be seeded with a std::vector<uint8_t>
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryFunction())
 *      .WithSeeds({
 *          {1,2,3,4,5},
 *          {6,7,8,9,10}
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryFunction() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) { return BSONBinDataOwned(v, BinDataType::Function); },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::Function) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>());
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::ByteArrayDeprecated
 * can optionally be seeded with a std::vector<uint8_t>
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryByteArrayDeprecated())
 *      .WithSeeds({
 *          {1,2,3,4,5},
 *          {6,7,8,9,10}
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryByteArrayDeprecated() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) {
            auto length = v.size();
            auto l = reinterpret_cast<uint8_t*>(&length);
            std::vector<uint8_t> data(l, l + sizeof(length));
            data.insert(data.end(), v.begin(), v.end());
            return BSONBinDataOwned(data, BinDataType::ByteArrayDeprecated);
        },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::ByteArrayDeprecated) {
                return std::nullopt;
            }
            return std::optional{std::tuple{
                std::vector<uint8_t>(data.dataOwned.begin(), data.dataOwned.begin() + 4)}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>());
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::bdtUUID
 * can optionally be seeded with a std::vector<uint8_t> with length 16
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitrarybdtUUID())
 *      .WithSeeds({
 *          {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitrarybdtUUID() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) { return BSONBinDataOwned(v, BinDataType::bdtUUID); },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::bdtUUID) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>().WithSize(16));
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::newUUID
 * can optionally be seeded with a std::vector<uint8_t> with length 16
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitrarynewUUID())
 *      .WithSeeds({
 *          {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitrarynewUUID() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) { return BSONBinDataOwned(v, BinDataType::newUUID); },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::newUUID) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>().WithSize(16));
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::MD5Type
 * can optionally be seeded with a std::vector<uint8_t> with length 16
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryMD5Type())
 *      .WithSeeds({
 *          {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryMD5Type() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) { return BSONBinDataOwned(v, BinDataType::MD5Type); },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::MD5Type) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>().WithSize(16));
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::Column
 * can optionally be seeded with a BSONObj
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryColumn())
 *      .WithSeeds({
 *          fromjson("{}")
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryColumn() {
    return fuzztest::ReversibleMap(
        [](ConstSharedBuffer buf) -> BSONBinDataOwned {
            auto obj = BSONObj(buf);
            BSONColumnBuilder cb;
            for (auto& element : obj) {
                cb.append(element);
            }
            return BSONBinDataOwned(cb.finalize());
        },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<ConstSharedBuffer>> {
            if (data.type != BinDataType::Column) {
                return std::nullopt;
            }
            SharedBuffer buf(data.dataOwned.size());
            memcpy(buf.get(), data.dataOwned.data(), data.dataOwned.size());

            return std::optional{std::tuple{buf}};
        },
        fuzztest::Arbitrary<ConstSharedBuffer>());
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::MD5Type
 * can optionally be seeded with a std::vector<uint8_t> with length 16
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitrarySensitive())
 *      .WithSeeds({
 *          {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitrarySensitive() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) { return BSONBinDataOwned(v, BinDataType::Sensitive); },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::Sensitive) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>());
}

/**
 * Provides an Arbitrary mongo::BSONBinData with subtype BinDataType::Vector
 * can optionally be seeded with a std::vector<uint8_t>
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryVector())
 *      .WithSeeds({
 *          {1,2,3,4,5},
 *          {6,7,8,9,0},
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryVector() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v) { return BSONBinDataOwned(v, BinDataType::Vector); },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>>> {
            if (data.type != BinDataType::Vector) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>());
}

/**
 * Provides an Arbitrary mongo::BSONBinData with a subtype between 128 and 255
 * can optionally be seeded with a std::vector<uint8_t> and an integer
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryUserData())
 *      .WithSeeds({
 *          {{1,2,3,4,5}, 128},
 *          {{5,4,3,2,1}, 200},
 *          {{6,7,8,9,0}, 255},
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryUserData() {
    return fuzztest::ReversibleMap(
        [](std::vector<uint8_t> v, int i) {
            return BSONBinDataOwned(v, static_cast<BinDataType>(i));
        },
        [](const BSONBinDataOwned& data) -> std::optional<std::tuple<std::vector<uint8_t>, int>> {
            if (data.type < BinDataType::bdtCustom) {
                return std::nullopt;
            }
            return std::optional{std::tuple{data.dataOwned, data.type}};
        },
        fuzztest::Arbitrary<std::vector<uint8_t>>(),
        fuzztest::InRange(128, 255));
}

/**
 * Provides an Arbitrary mongo::BSONBinData with encrypted data
 *
 * Encrypted data sub type format
 * +-----------------------------------+----------------------+-------------------------------+
 * | 1 byte (encryptedBinDataTypeByte) | 16 bytes (Key UUID)  | 1 byte (originalBsonTypeByte) |
 * +----------------+------------------+----------------------+-------------------------------+
 * | N bytes (data) |
 * +----------------+
 *
 * Can be seeded by providing the following:
 *      encryptedBinDataTypeByte = mongo::EncryptedBinDataType (uint8_t)
 *      key UUID = std::vector<uint8_t> with length 16
 *      originalBsonTypeByte = mongo::BinDataType (uint8_t)
 *      data = std::vector<uint8_t>
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryEncrypted())
 *      .WithSeeds({
 *          {
 *              EncryptedBinDataType::kDeterministic,
 *              {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
 *              BinDataType::BinDataGeneral,
 *              {1,2,3,4,5}
 *          },
 *      });
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryEncrypted() {
    return fuzztest::ReversibleMap(
        [](uint8_t type, std::vector<uint8_t> uuid, uint8_t dataType, std::vector<uint8_t> data) {
            std::vector<uint8_t> fullData;
            fullData.push_back(type);
            fullData.insert(fullData.end(), uuid.begin(), uuid.end());
            fullData.push_back(dataType);
            fullData.insert(fullData.end(), data.begin(), data.end());
            return BSONBinDataOwned(fullData, BinDataType::Encrypt);
        },
        [](const BSONBinDataOwned& data)
            -> std::optional<
                std::tuple<uint8_t, std::vector<uint8_t>, uint8_t, std::vector<uint8_t>>> {
            if (data.type != BinDataType::Encrypt) {
                return std::nullopt;
            }

            auto& ownedData = data.dataOwned;
            if (ownedData.size() < (sizeof(uint8_t) + 16 + sizeof(uint8_t))) {
                // Doesn't have the minimum required bytes
                return std::nullopt;
            }
            uint8_t type = ownedData[0];
            std::vector<uint8_t> uuid(ownedData.begin() + 1, ownedData.begin() + 17);
            uint8_t dataType = ownedData[17];
            std::vector<uint8_t> encryptedData(ownedData.begin() + 18, ownedData.end());

            return std::optional{std::tuple{type, uuid, dataType, encryptedData}};
        },
        fuzztest::InRange<uint8_t>(  // encryptedBinDataTypeByte
            static_cast<uint8_t>(EncryptedBinDataType::kDeterministic),
            static_cast<uint8_t>(EncryptedBinDataType::kFLE2FindTextPayload)),
        fuzztest::Arbitrary<std::vector<uint8_t>>().WithSize(16),
        fuzztest::InRange<uint8_t>(  // originalBsonTypeByte
            static_cast<uint8_t>(BSONType::eoo),
            static_cast<uint8_t>(BSONType::jsTypeMax)),
        fuzztest::Arbitrary<std::vector<uint8_t>>());
}

/**
 * Provides an arbitrary mongo::BSONBinData of any subtype
 * This should be used when the subtype has no hard requirements
 *
 * Example:
 *  FUZZ_TEST(TestSuite, FuzzerTest)
 *      .WithDomains(ArbitraryBinData())
 */
fuzztest::Domain<BSONBinDataOwned> ArbitraryBinData() {
    return fuzztest::OneOf(ArbitraryBinDataGeneral(),
                           ArbitraryFunction(),
                           ArbitraryByteArrayDeprecated(),
                           ArbitrarybdtUUID(),
                           ArbitrarynewUUID(),
                           ArbitraryMD5Type(),
                           ArbitraryColumn(),
                           ArbitraryEncrypted(),
                           ArbitrarySensitive(),
                           ArbitraryVector(),
                           ArbitraryUserData());
}
}  // namespace mongo::bson_mutator

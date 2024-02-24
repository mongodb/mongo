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


#include "mongo/bson/util/bsoncolumnbuilder.h"
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <memory>
#include <random>
#include <system_error>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/storage/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using std::string;
using namespace mongo;

BSONObj toBson(const key_string::Builder& ks, Ordering ord) {
    return key_string::toBson(ks.getBuffer(), ks.getSize(), ord, ks.getTypeBits());
}

template <class T>
BSONObj toBsonAndCheckKeySize(const key_string::BuilderBase<T>& ks, Ordering ord) {
    auto KeyStringBuilderSize = ks.getSize();

    // Validate size of the key in key_string::Builder.
    ASSERT_EQUALS(
        KeyStringBuilderSize,
        key_string::getKeySize(ks.getBuffer(), KeyStringBuilderSize, ord, ks.getTypeBits()));
    return key_string::toBson(ks.getBuffer(), KeyStringBuilderSize, ord, ks.getTypeBits());
}

BSONObj toBsonAndCheckKeySize(const key_string::Value& ks, Ordering ord) {
    auto KeyStringSize = ks.getSize();

    // Validate size of the key in key_string::Value.
    ASSERT_EQUALS(KeyStringSize,
                  key_string::getKeySize(ks.getBuffer(), KeyStringSize, ord, ks.getTypeBits()));
    return key_string::toBson(ks.getBuffer(), KeyStringSize, ord, ks.getTypeBits());
}

Ordering ALL_ASCENDING = Ordering::make(BSONObj());
Ordering ONE_ASCENDING = Ordering::make(BSON("a" << 1));
Ordering ONE_DESCENDING = Ordering::make(BSON("a" << -1));

class KeyStringBuilderTest : public mongo::unittest::Test {
public:
    void run() {
        auto base = static_cast<mongo::unittest::Test*>(this);
        try {
            version = key_string::Version::V0;
            base->run();
            version = key_string::Version::V1;
            base->run();
        } catch (...) {
            LOGV2(22226,
                  "exception while testing KeyStringBuilder version "
                  "{mongo_KeyString_keyStringVersionToString_version}",
                  "mongo_KeyString_keyStringVersionToString_version"_attr =
                      mongo::key_string::keyStringVersionToString(version));
            throw;
        }
    }

protected:
    key_string::Version version;
};

template <typename T>
void checkSizeWhileAppendingTypeBits(int numOfBitsUsedForType, T&& appendBitsFunc) {
    key_string::TypeBits typeBits(key_string::Version::V1);
    const int kItems = 10000;  // Pick an arbitrary large number.
    for (int i = 0; i < kItems; i++) {
        appendBitsFunc(typeBits);
        size_t currentRawSize = ((i + 1) * numOfBitsUsedForType - 1) / 8 + 1;
        size_t currentSize = currentRawSize;
        if (currentRawSize > key_string::TypeBits::kMaxBytesForShortEncoding) {
            // Case 4: plus 1 signal byte + 4 size bytes.
            currentSize += 5;
            ASSERT(typeBits.isLongEncoding());
        } else {
            ASSERT(!typeBits.isLongEncoding());
            if (currentRawSize == 1 && !(typeBits.getBuffer()[0] & 0x80)) {  // Case 2
                currentSize = 1;
            } else {
                // Case 3: plus 1 size byte.
                currentSize += 1;
            }
        }
        ASSERT_EQ(typeBits.getSize(), currentSize);
    }
}

// This test is derived from a fuzzer suite and triggers interesting code paths and recursion
// patterns, so including it here specifically.
TEST(InvalidKeyStringTest, FuzzedCodeWithScopeNesting) {
    BufBuilder keyData;
    hexblob::decode(
        "aa00aa4200aafa00aa0200aa0a01aa02aa00aa4200aafa00aa0200aa0a01aa0200aa00aa4200aafa00aa0200aa"
        "0a01aa0200aa4200aafa00aa0200aa00aaaa00aa00aafa00aa0200aa3900aafa00aa0200aa00aa004200aafa00"
        "aaaafa00aa0200aa0a01aa0200aa4200aafa00aa0200aa00aaaa00aa00aafa00aa0200aa00aafa00aa0200aa00"
        "aa004200aafa00aa0200aa000200aafcfeaa0200aaaa00aa00aafa00aa0200aa00aaaa00aa00aa4200aafa00aa"
        "0200aa6001fa00aa0200aa0a01aa0200aa00aaaa0a01aa0200aa4200aafa00aa0200aa00aaaa00aa00aafa00aa"
        "0200aa004200aafa00aa0200aa000200aa0a0200aa0200aa4200aafa00aa0200aa00aaaa00aa00aafa00aa0200"
        "aa3900aafa00aa0200aa00aa004200aafa00aaaafa00aa0200aa0a01aa0200aa4200aafa00aa0200aa00aaaa00"
        "aa00aafa00aa0200aa00aafa00aa0200aa00aa004200aafa00aa0200aa000200aafcfeaa0200aaaa00aa00aafa"
        "00aa0200aa00aaaa00aa00aa4200aafa00aa0200aa6001fa00aa0200aa0a01aa0200aa00aaaa0a01aa0200aa42"
        "00aafa00aa0200aa00aaaa00aa00aafa00aa0200aa00aaaa00aa0200aa0200aa4200aafa00aa0200aa00aaaa00"
        "aa00aafa00aa0200aa3900aafa00aa0200aa00aa004200aafa00aaaafa00aa0200aa0a01aa0200aa4200aafa00"
        "aa0200aa00aaaa00aa00aafa00aa0200aa00aafa00aa0200aa00aa004200aafa00aa0200aa000200aafcfeaa02"
        "00aaaa00aa00aafa00aa0200aa00aaaa00aa00aa4200aafa00aa0200aa6001fa00aa0200aa0a01aa0200aa00aa"
        "aa0a01aa0200aa4200aafa00aa0200aa00aaaa00aa00aafa00aa0200aa00aaaa00aa00aafa00aa0201aa0200aa"
        "aa00aa00aafa00aa0200aa00aaaa00000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000004200aafa00aa0200aa000200aa0a0200aa00aaaa002c00aafa00aa0200aa0200"
        "aa00aaaa002c00aafa00aa0200aa004200aafa00aa0200aa000200aa0a01aa0200aaaa00aa00aafa00aa0201aa"
        "0200aaaa00aa00aafa00aa0200aa00aaaa00000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000aaaa00aa00aafa00aa0200aa00aaaa00000000000000000000000000"
        "00000000000000000000000000000000000000000000000000aafa00aa0200aa00aaaa00000000000000000400"
        "00000000000000000000000000000000"_sd,
        &keyData);
    signed char typeBitsData[] = {0, 16, 0, 0, -127, 1};
    BufReader typeBitsReader(typeBitsData, sizeof(typeBitsData));
    key_string::TypeBits typeBits =
        key_string::TypeBits::fromBuffer(key_string::Version::kLatestVersion, &typeBitsReader);
    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(keyData.buf(), keyData.len(), ALL_ASCENDING, typeBits),
        AssertionException,
        ErrorCodes::Overflow);
}

TEST(TypeBitsTest, AppendSymbol) {
    checkSizeWhileAppendingTypeBits(
        1, [](key_string::TypeBits& typeBits) -> void { typeBits.appendSymbol(); });
}
TEST(TypeBitsTest, AppendString) {
    // The typeBits should be all zeros, so numOfBitsUsedForType is set to 0 for
    // passing the test although it technically uses 1 bit.
    checkSizeWhileAppendingTypeBits(
        0, [](key_string::TypeBits& typeBits) -> void { typeBits.appendString(); });
}
TEST(typebitstest, appendDouble) {
    checkSizeWhileAppendingTypeBits(
        2, [](key_string::TypeBits& typeBits) -> void { typeBits.appendNumberDouble(); });
}
TEST(TypeBitsTest, AppendNumberLong) {
    checkSizeWhileAppendingTypeBits(
        2, [](key_string::TypeBits& typeBits) -> void { typeBits.appendNumberLong(); });
}
TEST(TypeBitsTest, AppendNumberInt) {
    // The typeBits should be all zeros, so numOfBitsUsedForType is set to 0 for
    // passing the test although it technically uses 2 bits.
    checkSizeWhileAppendingTypeBits(
        0, [](key_string::TypeBits& typeBits) -> void { typeBits.appendNumberInt(); });
}
TEST(TypeBitsTest, AppendNumberDecimal) {
    checkSizeWhileAppendingTypeBits(
        2, [](key_string::TypeBits& typeBits) -> void { typeBits.appendNumberDecimal(); });
}
TEST(TypeBitsTest, AppendLongZero) {
    checkSizeWhileAppendingTypeBits(2, [](key_string::TypeBits& typeBits) -> void {
        typeBits.appendZero(key_string::TypeBits::kLong);
    });
}
TEST(TypeBitsTest, AppendDecimalZero) {
    checkSizeWhileAppendingTypeBits(12 + 5, [](key_string::TypeBits& typeBits) -> void {
        typeBits.appendDecimalZero(key_string::TypeBits::kDecimalZero1xxx);
    });
}
TEST(TypeBitsTest, AppendDecimalExponent) {
    checkSizeWhileAppendingTypeBits(
        key_string::TypeBits::kStoredDecimalExponentBits,
        [](key_string::TypeBits& typeBits) -> void { typeBits.appendDecimalExponent(1); });
}

TEST(TypeBitsTest, UninitializedTypeBits) {
    key_string::TypeBits typeBits(key_string::Version::V1);
    ASSERT_EQ(typeBits.getSize(), 1u);
    ASSERT_EQ(typeBits.getBuffer()[0], 0);
    ASSERT(typeBits.isAllZeros());
}

TEST(TypeBitsTest, AllZerosTypeBits) {
    {
        std::string emptyBuffer = "";
        BufReader reader(emptyBuffer.c_str(), 0);
        key_string::TypeBits typeBits =
            key_string::TypeBits::fromBuffer(key_string::Version::V1, &reader);
        ASSERT_EQ(typeBits.getSize(), 1u);
        ASSERT_EQ(typeBits.getBuffer()[0], 0);
        ASSERT(typeBits.isAllZeros());
    }

    {
        char allZerosBuffer[16] = {0};
        BufReader reader(allZerosBuffer, sizeof(allZerosBuffer));
        key_string::TypeBits typeBits =
            key_string::TypeBits::fromBuffer(key_string::Version::V1, &reader);
        ASSERT_EQ(typeBits.getSize(), 1u);
        ASSERT_EQ(typeBits.getBuffer()[0], 0);
        ASSERT(typeBits.isAllZeros());
    }
}

TEST(TypeBitsTest, AppendLotsOfZeroTypeBits) {
    key_string::TypeBits typeBits(key_string::Version::V1);
    for (int i = 0; i < 100000; i++) {
        typeBits.appendString();
    }
    // TypeBits should still be in short encoding format.
    ASSERT(!typeBits.isLongEncoding());
}

TEST_F(KeyStringBuilderTest, TooManyElementsInCompoundKey) {
    // Construct a KeyString with more than the limit of 32 elements in a compound index key. Encode
    // 33 kBoolTrue ('o') values.
    // Note that this KeyString encoding is legal, but it may not be legally stored in an index.
    const char* data = "ooooooooooooooooooooooooooooooooo";
    const size_t size = 33;

    key_string::Builder ks(key_string::Version::V1);
    ks.resetFromBuffer(data, size);

    // No exceptions should be thrown.
    key_string::toBsonSafe(data, size, ALL_ASCENDING, ks.getTypeBits());
    key_string::decodeDiscriminator(ks.getBuffer(), ks.getSize(), ALL_ASCENDING, ks.getTypeBits());
    key_string::getKeySize(ks.getBuffer(), ks.getSize(), ALL_ASCENDING, ks.getTypeBits());
}

TEST_F(KeyStringBuilderTest, MaxElementsInCompoundKey) {
    // Construct a KeyString with 32 elements in a compound index key followed by an end byte.
    // Encode 32 kBoolTrue ('o') values and an end byte, 0x4.
    const char* data = "oooooooooooooooooooooooooooooooo\x4";
    const size_t size = 33;

    key_string::Builder ks(key_string::Version::V1);
    ks.resetFromBuffer(data, size);

    // No exceptions should be thrown.
    key_string::toBsonSafe(data, size, ALL_ASCENDING, ks.getTypeBits());
    key_string::decodeDiscriminator(ks.getBuffer(), ks.getSize(), ALL_ASCENDING, ks.getTypeBits());
    key_string::getKeySize(ks.getBuffer(), ks.getSize(), ALL_ASCENDING, ks.getTypeBits());
}

TEST_F(KeyStringBuilderTest, EmbeddedNullString) {
    // Construct a KeyString where \x3c defines the type kStringLike then embedded with null
    // characters and followed by \x00.
    const char* data = "\x3c\x00\xff\x00";
    const size_t size = 4;
    key_string::TypeBits typeBits(key_string::Version::kLatestVersion);

    // No exceptions should be thrown.
    ASSERT_BSONOBJ_EQ(key_string::toBson(data, size, ALL_ASCENDING, typeBits),
                      BSON("" << StringData("\x00", 1)));
};

TEST_F(KeyStringBuilderTest, ExceededBSONDepth) {
    key_string::Builder ks(key_string::Version::V1);

    // Construct an illegal KeyString encoding with excessively nested BSON arrays '80' (P).
    const auto nestedArr = std::string(BSONDepth::getMaxAllowableDepth() + 1, 'P');
    ks.resetFromBuffer(nestedArr.c_str(), nestedArr.size());
    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(ks.getBuffer(), ks.getSize(), ALL_ASCENDING, ks.getTypeBits()),
        AssertionException,
        ErrorCodes::Overflow);

    // Construct an illegal BSON object with excessive nesting.
    BSONObj nestedObj;
    for (unsigned i = 0; i < BSONDepth::getMaxAllowableDepth() + 1; i++) {
        nestedObj = BSON("" << nestedObj);
    }
    // This BSON object should not be valid.
    auto validateStatus = validateBSON(nestedObj.objdata(), nestedObj.objsize());
    ASSERT_EQ(ErrorCodes::Overflow, validateStatus.code());

    // Construct a KeyString from the invalid BSON, and confirm that it fails to convert back to
    // BSON.
    ks.resetToKey(nestedObj, ALL_ASCENDING, RecordId(1));
    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(ks.getBuffer(), ks.getSize(), ALL_ASCENDING, ks.getTypeBits()),
        AssertionException,
        ErrorCodes::Overflow);
}

TEST_F(KeyStringBuilderTest, Simple1) {
    BSONObj a = BSON("" << 5);
    BSONObj b = BSON("" << 6);

    ASSERT_BSONOBJ_LT(a, b);

    ASSERT_LESS_THAN(key_string::Builder(version, a, ALL_ASCENDING, RecordId(1)),
                     key_string::Builder(version, b, ALL_ASCENDING, RecordId(1)));
}

#define ROUNDTRIP_ORDER(version, x, order)                            \
    do {                                                              \
        const BSONObj _orig = x;                                      \
        const key_string::Builder _ks(version, _orig, order);         \
        const BSONObj _converted = toBsonAndCheckKeySize(_ks, order); \
        ASSERT_BSONOBJ_EQ(_converted, _orig);                         \
        ASSERT(_converted.binaryEqual(_orig));                        \
    } while (0)

#define ROUNDTRIP(version, x)                        \
    do {                                             \
        ROUNDTRIP_ORDER(version, x, ALL_ASCENDING);  \
        ROUNDTRIP_ORDER(version, x, ONE_DESCENDING); \
    } while (0)

#define COMPARES_SAME(_v, _x, _y)                                          \
    do {                                                                   \
        key_string::Builder _xKS(_v, _x, ONE_ASCENDING);                   \
        key_string::Builder _yKS(_v, _y, ONE_ASCENDING);                   \
        if (SimpleBSONObjComparator::kInstance.evaluate(_x == _y)) {       \
            ASSERT_EQUALS(_xKS, _yKS);                                     \
        } else if (SimpleBSONObjComparator::kInstance.evaluate(_x < _y)) { \
            ASSERT_LESS_THAN(_xKS, _yKS);                                  \
        } else {                                                           \
            ASSERT_LESS_THAN(_yKS, _xKS);                                  \
        }                                                                  \
                                                                           \
        _xKS.resetToKey(_x, ONE_DESCENDING);                               \
        _yKS.resetToKey(_y, ONE_DESCENDING);                               \
        if (SimpleBSONObjComparator::kInstance.evaluate(_x == _y)) {       \
            ASSERT_EQUALS(_xKS, _yKS);                                     \
        } else if (SimpleBSONObjComparator::kInstance.evaluate(_x < _y)) { \
            ASSERT_GREATER_THAN(_xKS, _yKS);                               \
        } else {                                                           \
            ASSERT_GREATER_THAN(_yKS, _xKS);                               \
        }                                                                  \
    } while (0)

TEST_F(KeyStringBuilderTest, DeprecatedBinData) {
    ROUNDTRIP(version, BSON("" << BSONBinData(nullptr, 0, ByteArrayDeprecated)));
}

TEST_F(KeyStringBuilderTest, ValidColumn) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    cb.append(BSON("a" << 1).getField("a"));
    cb.append(BSON("a" << 2).getField("a"));
    cb.append(BSON("a" << 1).getField("a"));
    BSONBinData columnData = cb.finalize();
    BSONObj objData = BSON("" << columnData);

    ROUNDTRIP(version, objData);
}

TEST_F(KeyStringBuilderTest, InvalidColumn) {
    const BSONObj objData = BSON("" << BSONBinData("foobar", 6, Column));
    const key_string::Builder builder(version, objData, ALL_ASCENDING);
    auto KeyStringBuilderSize = builder.getSize();
    ASSERT(KeyStringBuilderSize > 0);

    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(
            builder.getBuffer(), KeyStringBuilderSize, ALL_ASCENDING, builder.getTypeBits()),
        AssertionException,
        50833);
}

TEST_F(KeyStringBuilderTest, ActualBytesDouble) {
    // just one test like this for utter sanity

    BSONObj a = BSON("" << 5.5);
    key_string::Builder ks(version, a, ALL_ASCENDING);
    LOGV2(22227,
          "{keyStringVersionToString_version} size: {ks_getSize} hex "
          "[{toHex_ks_getBuffer_ks_getSize}]",
          "keyStringVersionToString_version"_attr = keyStringVersionToString(version),
          "ks_getSize"_attr = ks.getSize(),
          "toHex_ks_getBuffer_ks_getSize"_attr = hexblob::encode(ks.getBuffer(), ks.getSize()));

    ASSERT_EQUALS(10U, ks.getSize());

    string hex = version == key_string::Version::V0
        ? "2B"              // kNumericPositive1ByteInt
          "0B"              // (5 << 1) | 1
          "02000000000000"  // fractional bytes of double
          "04"              // kEnd
        : "2B"              // kNumericPositive1ByteInt
          "0B"              // (5 << 1) | 1
          "80000000000000"  // fractional bytes
          "04";             // kEnd

    ASSERT_EQUALS(hex, hexblob::encode(ks.getBuffer(), ks.getSize()));

    ks.resetToKey(a, Ordering::make(BSON("a" << -1)));

    ASSERT_EQUALS(10U, ks.getSize());


    // last byte (kEnd) doesn't get flipped
    string hexFlipped;
    for (size_t i = 0; i < hex.size() - 2; i += 2) {
        char c = hexblob::decodePair(StringData(hex).substr(i, 2));
        c = ~c;
        hexFlipped += hexblob::encode(StringData(&c, 1));
    }
    hexFlipped += hex.substr(hex.size() - 2);

    ASSERT_EQUALS(hexFlipped, hexblob::encode(ks.getBuffer(), ks.getSize()));
}

TEST_F(KeyStringBuilderTest, AllTypesSimple) {
    ROUNDTRIP(version, BSON("" << 5.5));
    ROUNDTRIP(version,
              BSON(""
                   << "abc"));
    ROUNDTRIP(version, BSON("" << BSON("a" << 5)));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY("a" << 5)));
    ROUNDTRIP(version, BSON("" << BSONBinData("abc", 3, bdtCustom)));
    ROUNDTRIP(version, BSON("" << BSONUndefined));
    ROUNDTRIP(version, BSON("" << OID("abcdefabcdefabcdefabcdef")));
    ROUNDTRIP(version, BSON("" << true));
    ROUNDTRIP(version, BSON("" << Date_t::fromMillisSinceEpoch(123123123)));
    ROUNDTRIP(version, BSON("" << BSONRegEx("asdf", "x")));
    ROUNDTRIP(version, BSON("" << BSONDBRef("db.c", OID("010203040506070809101112"))));
    ROUNDTRIP(version, BSON("" << BSONCode("abc_code")));
    ROUNDTRIP(version,
              BSON("" << BSONCodeWScope("def_code",
                                        BSON("x_scope"
                                             << "a"))));
    ROUNDTRIP(version, BSON("" << 5));
    ROUNDTRIP(version, BSON("" << Timestamp(123123, 123)));
    ROUNDTRIP(version, BSON("" << Timestamp(~0U, 3)));
    ROUNDTRIP(version, BSON("" << 1235123123123LL));
}

TEST_F(KeyStringBuilderTest, Array1) {
    BSONObj emptyArray = BSON("" << BSONArray());

    ASSERT_EQUALS(Array, emptyArray.firstElement().type());

    ROUNDTRIP(version, emptyArray);
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(emptyArray.firstElement())));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(1)));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(1 << 2)));
    ROUNDTRIP(version, BSON("" << BSON_ARRAY(1 << 2 << 3)));

    {
        key_string::Builder a(version, emptyArray, ALL_ASCENDING, RecordId::minLong());
        key_string::Builder b(version, emptyArray, ALL_ASCENDING, RecordId(5));
        ASSERT_LESS_THAN(a, b);
    }

    {
        key_string::Builder a(version, emptyArray, ALL_ASCENDING, RecordId(0));
        key_string::Builder b(version, emptyArray, ALL_ASCENDING, RecordId(5));
        ASSERT_LESS_THAN(a, b);
    }
}

TEST_F(KeyStringBuilderTest, SubDoc1) {
    ROUNDTRIP(version, BSON("" << BSON("foo" << 2)));
    ROUNDTRIP(version,
              BSON("" << BSON("foo" << 2 << "bar"
                                    << "asd")));
    ROUNDTRIP(version, BSON("" << BSON("foo" << BSON_ARRAY(2 << 4))));
}

TEST_F(KeyStringBuilderTest, SubDoc2) {
    BSONObj a = BSON("" << BSON("a"
                                << "foo"));
    BSONObj b = BSON("" << BSON("b" << 5.5));
    BSONObj c = BSON("" << BSON("c" << BSON("x" << 5)));
    ROUNDTRIP(version, a);
    ROUNDTRIP(version, b);
    ROUNDTRIP(version, c);

    COMPARES_SAME(version, a, b);
    COMPARES_SAME(version, a, c);
    COMPARES_SAME(version, b, c);
}


TEST_F(KeyStringBuilderTest, Compound1) {
    ROUNDTRIP(version, BSON("" << BSON("a" << 5) << "" << 1));
    ROUNDTRIP(version, BSON("" << BSON("" << 5) << "" << 1));
}

TEST_F(KeyStringBuilderTest, Undef1) {
    ROUNDTRIP(version, BSON("" << BSONUndefined));
}

TEST_F(KeyStringBuilderTest, NumberLong0) {
    double d = (1ll << 52) - 1;
    long long ll = static_cast<long long>(d);
    double d2 = static_cast<double>(ll);
    ASSERT_EQUALS(d, d2);
}

TEST_F(KeyStringBuilderTest, NumbersNearInt32Max) {
    int64_t start = std::numeric_limits<int32_t>::max();
    for (int64_t i = -1000; i < 1000; i++) {
        long long toTest = start + i;
        ROUNDTRIP(version, BSON("" << toTest));
        ROUNDTRIP(version, BSON("" << static_cast<int>(toTest)));
        ROUNDTRIP(version, BSON("" << static_cast<double>(toTest)));
    }
}

TEST_F(KeyStringBuilderTest, DecimalNumbers) {
    if (version == key_string::Version::V0) {
        LOGV2(22228, "not testing DecimalNumbers for KeyStringBuilder V0");
        return;
    }

    const auto V1 = key_string::Version::V1;

    // Zeros
    ROUNDTRIP(V1, BSON("" << Decimal128("0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("0.0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("0E5000")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-0.0000E-6172")));

    // Special numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("NaN")));
    ROUNDTRIP(V1, BSON("" << Decimal128("+Inf")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-Inf")));

    // Decimal representations of whole double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("1")));
    ROUNDTRIP(V1, BSON("" << Decimal128("2.0")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-2.0E1")));
    ROUNDTRIP(V1, BSON("" << Decimal128("1234.56E15")));
    ROUNDTRIP(V1, BSON("" << Decimal128("2.00000000000000000000000")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-9223372036854775808.00000000000000")));  // -2**63
    ROUNDTRIP(V1, BSON("" << Decimal128("973555660975280180349468061728768E1")));  // 1.875 * 2**112

    // Decimal representations of fractional double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("1.25")));
    ROUNDTRIP(V1, BSON("" << Decimal128("3.141592653584666550159454345703125")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-127.50")));

    // Decimal representations of whole int64 non-double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("243290200817664E4")));  // 20!
    ROUNDTRIP(V1, BSON("" << Decimal128("9007199254740993")));   // 2**53 + 1
    ROUNDTRIP(V1, BSON("" << Decimal128(std::numeric_limits<int64_t>::max())));
    ROUNDTRIP(V1, BSON("" << Decimal128(std::numeric_limits<int64_t>::min())));

    // Decimals in int64_t range without decimal or integer representation
    ROUNDTRIP(V1, BSON("" << Decimal128("1.23")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-1.1")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-12345.60")));
    ROUNDTRIP(V1, BSON("" << Decimal128("3.141592653589793238462643383279502")));
    ROUNDTRIP(V1, BSON("" << Decimal128("-3.141592653589793115997963468544185")));

    // Decimal representations of small double numbers
    ROUNDTRIP(V1, BSON("" << Decimal128("0.50")));
    ROUNDTRIP(V1,
              BSON("" << Decimal128("-0.3552713678800500929355621337890625E-14")));  // -2**(-48)
    ROUNDTRIP(V1,
              BSON("" << Decimal128("-0.000000000000001234567890123456789012345678901234E-99")));

    // Decimal representations of small decimals not representable as double
    ROUNDTRIP(V1, BSON("" << Decimal128("0.02")));

    // Large decimals
    ROUNDTRIP(V1, BSON("" << Decimal128("1234567890123456789012345678901234E6000")));
    ROUNDTRIP(V1,
              BSON("" << Decimal128("-19950631168.80758384883742162683585E3000")));  // -2**10000

    // Tiny, tiny decimals
    ROUNDTRIP(V1,
              BSON("" << Decimal128("0.2512388057698744585180135042133610E-6020")));  // 2**(-10000)
    ROUNDTRIP(V1, BSON("" << Decimal128("4.940656458412465441765687928682213E-324") << "" << 1));
    ROUNDTRIP(V1, BSON("" << Decimal128("-0.8289046058458094980903836776809409E-316")));

    // Decimal inside sub-doc
    ROUNDTRIP(V1, BSON("" << BSONNULL << "" << BSON("a" << Decimal128::kPositiveInfinity)));
}

TEST_F(KeyStringBuilderTest, KeyStringValue) {
    // Test that KeyStringBuilder is releasable into a Value type that is comparable. Once
    // released, it is reusable once reset.
    key_string::HeapBuilder ks1(key_string::Version::V1, BSON("" << 1), ALL_ASCENDING);
    key_string::Value data1 = ks1.release();

    key_string::HeapBuilder ks2(key_string::Version::V1, BSON("" << 2), ALL_ASCENDING);
    key_string::Value data2 = ks2.release();

    ASSERT(data2.compare(data1) > 0);
    ASSERT(data1.compare(data2) < 0);

    // Test that Value is moveable.
    key_string::Value moved = std::move(data1);
    ASSERT(data2.compare(moved) > 0);
    ASSERT(moved.compare(data2) < 0);

    // Test that Value is copyable.
    key_string::Value dataCopy = data2;
    ASSERT(data2.compare(dataCopy) == 0);
}

#define COMPARE_KS_BSON(ks, bson, order)                             \
    do {                                                             \
        const BSONObj _converted = toBsonAndCheckKeySize(ks, order); \
        ASSERT_BSONOBJ_EQ(_converted, bson);                         \
        ASSERT(_converted.binaryEqual(bson));                        \
    } while (0)

TEST_F(KeyStringBuilderTest, KeyStringValueReleaseReusableTest) {
    // Test that KeyStringBuilder is reusable once reset.
    BSONObj doc1 = BSON("fieldA" << 1 << "fieldB" << 2);
    BSONObj doc2 = BSON("fieldA" << 2 << "fieldB" << 3);
    BSONObj bson1 = BSON("" << 1 << "" << 2);
    BSONObj bson2 = BSON("" << 2 << "" << 3);
    key_string::HeapBuilder ks1(key_string::Version::V1);
    ks1.appendBSONElement(doc1["fieldA"]);
    ks1.appendBSONElement(doc1["fieldB"]);
    key_string::Value data1 = ks1.release();

    ks1.resetToEmpty();
    ks1.appendBSONElement(doc2["fieldA"]);
    ks1.appendBSONElement(doc2["fieldB"]);
    key_string::Value data2 = ks1.release();
    COMPARE_KS_BSON(data1, bson1, ALL_ASCENDING);
    COMPARE_KS_BSON(data2, bson2, ALL_ASCENDING);
}

TEST_F(KeyStringBuilderTest, KeyStringGetValueCopyTest) {
    // Test that KeyStringGetValueCopyTest creates a copy.
    BSONObj doc = BSON("fieldA" << 1);
    key_string::HeapBuilder ks(key_string::Version::V1, ALL_ASCENDING);
    ks.appendBSONElement(doc["fieldA"]);
    key_string::Value data1 = ks.getValueCopy();
    key_string::Value data2 = ks.release();

    // Assert that a copy was actually made and they don't share a buffer.
    ASSERT_NOT_EQUALS(data1.getBuffer(), data2.getBuffer());

    COMPARE_KS_BSON(data1, BSON("" << 1), ALL_ASCENDING);
    COMPARE_KS_BSON(data2, BSON("" << 1), ALL_ASCENDING);
}

TEST_F(KeyStringBuilderTest, KeyStringBuilderAppendBsonElement) {
    // Test that appendBsonElement works.
    {
        BSONObj doc = BSON("fieldA" << 1 << "fieldB" << 2);
        key_string::HeapBuilder ks(key_string::Version::V1, ALL_ASCENDING);
        ks.appendBSONElement(doc["fieldA"]);
        ks.appendBSONElement(doc["fieldB"]);
        key_string::Value data = ks.release();
        COMPARE_KS_BSON(data, BSON("" << 1 << "" << 2), ALL_ASCENDING);
    }

    {
        BSONObj doc = BSON("fieldA" << 1 << "fieldB" << 2);
        key_string::HeapBuilder ks(key_string::Version::V1, ONE_DESCENDING);
        ks.appendBSONElement(doc["fieldA"]);
        ks.appendBSONElement(doc["fieldB"]);
        key_string::Value data = ks.release();
        COMPARE_KS_BSON(data, BSON("" << 1 << "" << 2), ONE_DESCENDING);
    }

    {
        BSONObj doc = BSON("fieldA"
                           << "value1"
                           << "fieldB"
                           << "value2");
        key_string::HeapBuilder ks(key_string::Version::V1, ONE_DESCENDING);
        ks.appendBSONElement(doc["fieldA"]);
        ks.appendBSONElement(doc["fieldB"]);
        key_string::Value data = ks.release();
        COMPARE_KS_BSON(data,
                        BSON(""
                             << "value1"
                             << ""
                             << "value2"),
                        ONE_DESCENDING);
    }
}

TEST_F(KeyStringBuilderTest, KeyStringBuilderOrdering) {
    // Test that ordering works.
    BSONObj doc = BSON("fieldA" << 1);
    key_string::HeapBuilder ks1(key_string::Version::V1, ALL_ASCENDING);
    ks1.appendBSONElement(doc["fieldA"]);
    key_string::HeapBuilder ks2(key_string::Version::V1, ONE_DESCENDING);
    ks2.appendBSONElement(doc["fieldA"]);
    key_string::Value data1 = ks1.release();
    key_string::Value data2 = ks2.release();

    ASSERT_EQUALS(data1.getSize(), data2.getSize());
    // Confirm that the buffers are different, indicating that the data is stored inverted in the
    // second.
    ASSERT_NE(0, memcmp(data1.getBuffer(), data2.getBuffer(), data1.getSize()));
}

TEST_F(KeyStringBuilderTest, KeyStringBuilderDiscriminator) {
    // test that when passed in a Discriminator it gets added.
    BSONObj doc = BSON("fieldA" << 1 << "fieldB" << 2);
    key_string::HeapBuilder ks(
        key_string::Version::V1, ALL_ASCENDING, key_string::Discriminator::kExclusiveBefore);
    ks.appendBSONElement(doc["fieldA"]);
    ks.appendBSONElement(doc["fieldB"]);
    key_string::Value data = ks.release();
    uint8_t appendedDiscriminator = (uint8_t)(*(data.getBuffer() + (data.getSize() - 2)));
    uint8_t end = (uint8_t)(*(data.getBuffer() + (data.getSize() - 1)));
    ASSERT_EQ((uint8_t)'\001', appendedDiscriminator);
    ASSERT_EQ((uint8_t)'\004', end);
}

TEST_F(KeyStringBuilderTest, KeyStringValueCompareWithoutDiscriminator1) {
    // test that when passed in a Discriminator it gets added.
    BSONObj doc = BSON("fieldA" << 1 << "fieldB" << 2);

    key_string::HeapBuilder ks1(
        key_string::Version::V1, ALL_ASCENDING, key_string::Discriminator::kExclusiveBefore);
    ks1.appendBSONElement(doc["fieldA"]);
    ks1.appendBSONElement(doc["fieldB"]);
    key_string::Value data1 = ks1.release();

    key_string::HeapBuilder ks2(
        key_string::Version::V1, ALL_ASCENDING, key_string::Discriminator::kExclusiveAfter);
    ks2.appendBSONElement(doc["fieldA"]);
    ks2.appendBSONElement(doc["fieldB"]);
    key_string::Value data2 = ks2.release();

    ASSERT_EQ(data1.compareWithoutDiscriminator(data2), 0);
}

TEST_F(KeyStringBuilderTest, KeyStringValueCompareWithoutDiscriminator2) {
    // test that when passed in a Discriminator it gets added.
    BSONObj doc = BSON("fieldA" << 1 << "fieldB" << 2);

    key_string::HeapBuilder ks1(
        key_string::Version::V1, ALL_ASCENDING, key_string::Discriminator::kExclusiveBefore);
    ks1.appendBSONElement(doc["fieldA"]);
    ks1.appendBSONElement(doc["fieldB"]);
    key_string::Value data1 = ks1.release();

    key_string::HeapBuilder ks2(
        key_string::Version::V1, ALL_ASCENDING, key_string::Discriminator::kExclusiveAfter);
    ks2.appendBSONElement(doc["fieldA"]);
    ks2.appendBSONElement(doc["fieldA"]);
    key_string::Value data2 = ks2.release();

    ASSERT_EQ(data1.compareWithoutDiscriminator(data2), 1);
}

TEST_F(KeyStringBuilderTest, DoubleInvalidIntegerPartV0) {
    // Test that an illegally encoded double throws an error.
    const char* data =
        // kNumericPositive7ByteInt
        "\x31"
        // Encode a 1 bit at the lowest end to indicate that this number has a fractional part.
        // Then add the value 1 << 53 left-shifted by 1. 1 << 53 is too large to have been encoded
        // as a  double, and will cause the call to toBsonSafe to fail.
        "\x40\x00\x00\x00\x00\x00\x01";  // ((1 << 53) << 1) + 1
    const size_t size = 8;

    mongo::key_string::TypeBits tb(mongo::key_string::Version::V0);
    tb.appendNumberDouble();

    ASSERT_THROWS_CODE(
        mongo::key_string::toBsonSafe(data, size, mongo::Ordering::make(mongo::BSONObj()), tb),
        AssertionException,
        31209);
}

TEST_F(KeyStringBuilderTest, InvalidInfinityDecimalV0) {
    // Encode a Decimal positive infinity in a V1 keystring.
    mongo::key_string::Builder ks(
        mongo::key_string::Version::V1, BSON("" << Decimal128::kPositiveInfinity), ALL_ASCENDING);

    // Construct V0 type bits that indicate a NumberDecimal has been encoded.
    mongo::key_string::TypeBits tb(mongo::key_string::Version::V0);
    tb.appendNumberDecimal();

    // The conversion to BSON will fail because Decimal positive infinity cannot be encoded with V0
    // type bits.
    ASSERT_THROWS_CODE(
        mongo::key_string::toBsonSafe(ks.getBuffer(), ks.getSize(), ALL_ASCENDING, tb),
        AssertionException,
        31231);
}

TEST_F(KeyStringBuilderTest, ReasonableSize) {
    // Tests that key_string::Builders do not use an excessive amount of memory for small key
    // generation. These upper bounds were the calculated sizes of each type at the time this
    // test was written.
    key_string::Builder stackBuilder(key_string::Version::kLatestVersion, BSONObj(), ALL_ASCENDING);
    static_assert(sizeof(stackBuilder) <= 624);

    key_string::HeapBuilder heapBuilder(
        key_string::Version::kLatestVersion, BSONObj(), ALL_ASCENDING);
    static_assert(sizeof(heapBuilder) <= 104);

    // Use a small block size to ensure we do not use more. Additionally, the minimum allocation
    // size is 64.
    const auto minSize = 64;
    SharedBufferFragmentBuilder fragmentBuilder(
        minSize,
        SharedBufferFragmentBuilder::DoubleGrowStrategy(
            SharedBufferFragmentBuilder::kDefaultMaxBlockSize));
    key_string::PooledBuilder pooledBuilder(
        fragmentBuilder, key_string::Version::kLatestVersion, BSONObj(), ALL_ASCENDING);
    static_assert(sizeof(pooledBuilder) <= 104);

    // Test the dynamic memory usage reported to the sorter.
    key_string::Value value1 = stackBuilder.getValueCopy();
    ASSERT_LTE(sizeof(value1), 32);
    ASSERT_LTE(value1.memUsageForSorter(), 34);

    key_string::Value value2 = heapBuilder.getValueCopy();
    ASSERT_LTE(sizeof(value2), 32);
    ASSERT_LTE(value2.memUsageForSorter(), 34);

    key_string::Value value3 = heapBuilder.release();
    ASSERT_LTE(sizeof(value3), 32);
    ASSERT_LTE(value3.memUsageForSorter(), 64);

    key_string::Value value4 = pooledBuilder.getValueCopy();
    ASSERT_LTE(sizeof(value4), 32);
    // This is safe because we are operating on a copy of the value and it is not shared elsewhere.
    ASSERT_LTE(value4.memUsageForSorter(), 34);
    // We should still be using the initially-allocated size.
    ASSERT_LTE(fragmentBuilder.memUsage(), 64);

    // For values created with the pooledBuilder, it is invalid to call memUsageForSorter(). Instead
    // we look at the mem usage of the builder itself.
    key_string::Value value5 = pooledBuilder.release();
    ASSERT_LTE(sizeof(value5), 32);
    ASSERT_LTE(fragmentBuilder.memUsage(), 64);
}

TEST_F(KeyStringBuilderTest, DiscardIfNotReleased) {
    SharedBufferFragmentBuilder fragmentBuilder(1024);
    {
        // Intentially not released, but the data should be discarded correctly.
        key_string::PooledBuilder pooledBuilder(
            fragmentBuilder, key_string::Version::kLatestVersion, BSONObj(), ALL_ASCENDING);
    }
    {
        key_string::PooledBuilder pooledBuilder(
            fragmentBuilder, key_string::Version::kLatestVersion, BSONObj(), ALL_ASCENDING);
        pooledBuilder.release();
    }
}

TEST_F(KeyStringBuilderTest, LotsOfNumbers1) {
    for (int i = 0; i < 64; i++) {
        int64_t x = 1LL << i;
        ROUNDTRIP(version, BSON("" << static_cast<long long>(x)));
        ROUNDTRIP(version, BSON("" << static_cast<int>(x)));
        ROUNDTRIP(version, BSON("" << static_cast<double>(x)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) + .1)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) - .1)));

        ROUNDTRIP(version, BSON("" << (static_cast<long long>(x) + 1)));
        ROUNDTRIP(version, BSON("" << (static_cast<int>(x) + 1)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) + 1)));
        ROUNDTRIP(version, BSON("" << (static_cast<double>(x) + 1.1)));

        // Avoid negating signed integral minima
        if (i < 63)
            ROUNDTRIP(version, BSON("" << -static_cast<long long>(x)));

        if (i < 31)
            ROUNDTRIP(version, BSON("" << -static_cast<int>(x)));
        ROUNDTRIP(version, BSON("" << -static_cast<double>(x)));
        ROUNDTRIP(version, BSON("" << -(static_cast<double>(x) + .1)));

        ROUNDTRIP(version, BSON("" << -(static_cast<long long>(x) + 1)));
        ROUNDTRIP(version, BSON("" << -(static_cast<int>(x) + 1)));
        ROUNDTRIP(version, BSON("" << -(static_cast<double>(x) + 1)));
        ROUNDTRIP(version, BSON("" << -(static_cast<double>(x) + 1.1)));
    }
}

TEST_F(KeyStringBuilderTest, LotsOfNumbers2) {
    for (double i = -1100; i < 1100; i++) {
        double x = pow(2, i);
        ROUNDTRIP(version, BSON("" << x));
    }
    for (double i = -1100; i < 1100; i++) {
        double x = pow(2.1, i);
        ROUNDTRIP(version, BSON("" << x));
    }
}

TEST_F(KeyStringBuilderTest, RecordIdOrder1) {
    Ordering ordering = Ordering::make(BSON("a" << 1));

    key_string::Builder a(version, BSON("" << 5), ordering, RecordId::minLong());
    key_string::Builder b(version, BSON("" << 5), ordering, RecordId(2));
    key_string::Builder c(version, BSON("" << 5), ordering, RecordId(3));
    key_string::Builder d(version, BSON("" << 6), ordering, RecordId(4));
    key_string::Builder e(version, BSON("" << 6), ordering, RecordId(1));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(c, d);
    ASSERT_LESS_THAN(e, d);
}

TEST_F(KeyStringBuilderTest, RecordIdOrder2) {
    Ordering ordering = Ordering::make(BSON("a" << -1 << "b" << -1));

    key_string::Builder a(version, BSON("" << 5 << "" << 6), ordering, RecordId::minLong());
    key_string::Builder b(version, BSON("" << 5 << "" << 6), ordering, RecordId(5));
    key_string::Builder c(version, BSON("" << 5 << "" << 5), ordering, RecordId(4));
    key_string::Builder d(version, BSON("" << 3 << "" << 4), ordering, RecordId(3));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(c, d);
    ASSERT_LESS_THAN(a, c);
    ASSERT_LESS_THAN(a, d);
    ASSERT_LESS_THAN(b, d);
}

TEST_F(KeyStringBuilderTest, RecordIdOrder2Double) {
    Ordering ordering = Ordering::make(BSON("a" << -1 << "b" << -1));

    key_string::Builder a(version, BSON("" << 5.0 << "" << 6.0), ordering, RecordId::minLong());
    key_string::Builder b(version, BSON("" << 5.0 << "" << 6.0), ordering, RecordId(5));
    key_string::Builder c(version, BSON("" << 3.0 << "" << 4.0), ordering, RecordId(3));

    ASSERT_LESS_THAN(a, b);
    ASSERT_LESS_THAN(b, c);
    ASSERT_LESS_THAN(a, c);
}

TEST_F(KeyStringBuilderTest, Timestamp) {
    BSONObj a = BSON("" << Timestamp(0, 0));
    BSONObj b = BSON("" << Timestamp(1234, 1));
    BSONObj c = BSON("" << Timestamp(1234, 2));
    BSONObj d = BSON("" << Timestamp(1235, 1));
    BSONObj e = BSON("" << Timestamp(~0U, 0));

    {
        ROUNDTRIP(version, a);
        ROUNDTRIP(version, b);
        ROUNDTRIP(version, c);

        ASSERT_BSONOBJ_LT(a, b);
        ASSERT_BSONOBJ_LT(b, c);
        ASSERT_BSONOBJ_LT(c, d);

        key_string::Builder ka(version, a, ALL_ASCENDING);
        key_string::Builder kb(version, b, ALL_ASCENDING);
        key_string::Builder kc(version, c, ALL_ASCENDING);
        key_string::Builder kd(version, d, ALL_ASCENDING);
        key_string::Builder ke(version, e, ALL_ASCENDING);

        ASSERT(ka.compare(kb) < 0);
        ASSERT(kb.compare(kc) < 0);
        ASSERT(kc.compare(kd) < 0);
        ASSERT(kd.compare(ke) < 0);
    }

    {
        Ordering ALL_ASCENDING = Ordering::make(BSON("a" << -1));

        ROUNDTRIP(version, a);
        ROUNDTRIP(version, b);
        ROUNDTRIP(version, c);

        ASSERT(d.woCompare(c, ALL_ASCENDING) < 0);
        ASSERT(c.woCompare(b, ALL_ASCENDING) < 0);
        ASSERT(b.woCompare(a, ALL_ASCENDING) < 0);

        key_string::Builder ka(version, a, ALL_ASCENDING);
        key_string::Builder kb(version, b, ALL_ASCENDING);
        key_string::Builder kc(version, c, ALL_ASCENDING);
        key_string::Builder kd(version, d, ALL_ASCENDING);

        ASSERT(ka.compare(kb) > 0);
        ASSERT(kb.compare(kc) > 0);
        ASSERT(kc.compare(kd) > 0);
    }
}

TEST_F(KeyStringBuilderTest, AllTypesRoundtrip) {
    for (int i = 1; i <= JSTypeMax; i++) {
        {
            BSONObjBuilder b;
            b.appendMinForType("", i);
            BSONObj o = b.obj();
            ROUNDTRIP(version, o);
        }
        {
            BSONObjBuilder b;
            b.appendMaxForType("", i);
            BSONObj o = b.obj();
            ROUNDTRIP(version, o);
        }
    }
}

const std::vector<BSONObj>& getInterestingElements(key_string::Version version) {
    static std::vector<BSONObj> elements;
    elements.clear();

    // These are used to test strings that include NUL bytes.
    const auto ball = "ball"_sd;
    const auto ball00n = "ball\0\0n"_sd;
    const auto zeroBall = "\0ball"_sd;

    elements.push_back(BSON("" << 1));
    elements.push_back(BSON("" << 1.0));
    elements.push_back(BSON("" << 1LL));
    elements.push_back(BSON("" << 123456789123456789LL));
    elements.push_back(BSON("" << -123456789123456789LL));
    elements.push_back(BSON("" << 112353998331165715LL));
    elements.push_back(BSON("" << 112353998331165710LL));
    elements.push_back(BSON("" << 1123539983311657199LL));
    elements.push_back(BSON("" << 123456789123456789.123));
    elements.push_back(BSON("" << -123456789123456789.123));
    elements.push_back(BSON("" << 112353998331165715.0));
    elements.push_back(BSON("" << 112353998331165710.0));
    elements.push_back(BSON("" << 1123539983311657199.0));
    elements.push_back(BSON("" << 5.0));
    elements.push_back(BSON("" << 5));
    elements.push_back(BSON("" << 2));
    elements.push_back(BSON("" << -2));
    elements.push_back(BSON("" << -2.2));
    elements.push_back(BSON("" << -12312312.2123123123123));
    elements.push_back(BSON("" << 12312312.2123123123123));
    elements.push_back(BSON(""
                            << "aaa"));
    elements.push_back(BSON(""
                            << "AAA"));
    elements.push_back(BSON("" << zeroBall));
    elements.push_back(BSON("" << ball));
    elements.push_back(BSON("" << ball00n));
    elements.push_back(BSON("" << BSONSymbol(zeroBall)));
    elements.push_back(BSON("" << BSONSymbol(ball)));
    elements.push_back(BSON("" << BSONSymbol(ball00n)));
    elements.push_back(BSON("" << BSON("a" << 5)));
    elements.push_back(BSON("" << BSON("a" << 6)));
    elements.push_back(BSON("" << BSON("b" << 6)));
    elements.push_back(BSON("" << BSON_ARRAY("a" << 5)));
    elements.push_back(BSON("" << BSONNULL));
    elements.push_back(BSON("" << BSONUndefined));
    elements.push_back(BSON("" << OID("abcdefabcdefabcdefabcdef")));
    elements.push_back(BSON("" << Date_t::fromMillisSinceEpoch(123)));
    elements.push_back(BSON("" << BSONCode("abc_code")));
    elements.push_back(BSON("" << BSONCode(zeroBall)));
    elements.push_back(BSON("" << BSONCode(ball)));
    elements.push_back(BSON("" << BSONCode(ball00n)));
    elements.push_back(BSON("" << BSONCodeWScope("def_code1",
                                                 BSON("x_scope"
                                                      << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2",
                                                 BSON("x_scope"
                                                      << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2",
                                                 BSON("x_scope"
                                                      << "b"))));
    elements.push_back(BSON("" << BSONCodeWScope(zeroBall, BSON("a" << 1))));
    elements.push_back(BSON("" << BSONCodeWScope(ball, BSON("a" << 1))));
    elements.push_back(BSON("" << BSONCodeWScope(ball00n, BSON("a" << 1))));
    elements.push_back(BSON("" << true));
    elements.push_back(BSON("" << false));

    // Something that needs multiple bytes of typeBits
    elements.push_back(BSON("" << BSON_ARRAY("" << BSONSymbol("") << 0 << 0ll << 0.0 << -0.0)));
    if (version != key_string::Version::V0) {
        // Something with exceptional typeBits for Decimal
        elements.push_back(
            BSON("" << BSON_ARRAY("" << BSONSymbol("") << Decimal128::kNegativeInfinity
                                     << Decimal128::kPositiveInfinity << Decimal128::kPositiveNaN
                                     << Decimal128("0.0000000") << Decimal128("-0E1000"))));
    }

    //
    // Interesting numeric cases
    //

    elements.push_back(BSON("" << 0));
    elements.push_back(BSON("" << 0ll));
    elements.push_back(BSON("" << 0.0));
    elements.push_back(BSON("" << -0.0));
    if (version != key_string::Version::V0) {
        Decimal128("0.0.0000000");
        Decimal128("-0E1000");
    }

    elements.push_back(BSON("" << std::numeric_limits<double>::quiet_NaN()));
    elements.push_back(BSON("" << std::numeric_limits<double>::infinity()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::infinity()));
    elements.push_back(BSON("" << std::numeric_limits<double>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::max()));
    elements.push_back(BSON("" << std::numeric_limits<double>::min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::min()));
    elements.push_back(BSON("" << std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::denorm_min()));

    elements.push_back(BSON("" << std::numeric_limits<long long>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<long long>::max()));
    elements.push_back(BSON("" << std::numeric_limits<long long>::min()));

    elements.push_back(BSON("" << std::numeric_limits<int>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<int>::max()));
    elements.push_back(BSON("" << std::numeric_limits<int>::min()));

    for (int powerOfTwo = 0; powerOfTwo < 63; powerOfTwo++) {
        const long long lNum = 1ll << powerOfTwo;
        const double dNum = double(lNum);

        // All powers of two in this range can be represented exactly as doubles.
        invariant(lNum == static_cast<long long>(dNum));

        elements.push_back(BSON("" << lNum));
        elements.push_back(BSON("" << -lNum));

        elements.push_back(BSON("" << dNum));
        elements.push_back(BSON("" << -dNum));


        elements.push_back(BSON("" << (lNum + 1)));
        elements.push_back(BSON("" << (lNum - 1)));
        elements.push_back(BSON("" << (-lNum + 1)));
        elements.push_back(BSON("" << (-lNum - 1)));

        if (powerOfTwo <= 52) {  // is dNum - 0.5 representable?
            elements.push_back(BSON("" << (dNum - 0.5)));
            elements.push_back(BSON("" << -(dNum - 0.5)));
            elements.push_back(BSON("" << (dNum - 0.1)));
            elements.push_back(BSON("" << -(dNum - 0.1)));
        }

        if (powerOfTwo <= 51) {  // is dNum + 0.5 representable?
            elements.push_back(BSON("" << (dNum + 0.5)));
            elements.push_back(BSON("" << -(dNum + 0.5)));
            elements.push_back(BSON("" << (dNum + 0.1)));
            elements.push_back(BSON("" << -(dNum + -.1)));
        }

        if (version != key_string::Version::V0) {
            const Decimal128 dec(static_cast<int64_t>(lNum));
            const Decimal128 one("1");
            const Decimal128 half("0.5");
            const Decimal128 tenth("0.1");
            elements.push_back(BSON("" << dec));
            elements.push_back(BSON("" << dec.add(one)));
            elements.push_back(BSON("" << dec.subtract(one)));
            elements.push_back(BSON("" << dec.negate()));
            elements.push_back(BSON("" << dec.add(one).negate()));
            elements.push_back(BSON("" << dec.subtract(one).negate()));
            elements.push_back(BSON("" << dec.subtract(half)));
            elements.push_back(BSON("" << dec.subtract(half).negate()));
            elements.push_back(BSON("" << dec.add(half)));
            elements.push_back(BSON("" << dec.add(half).negate()));
            elements.push_back(BSON("" << dec.subtract(tenth)));
            elements.push_back(BSON("" << dec.subtract(tenth).negate()));
            elements.push_back(BSON("" << dec.add(tenth)));
            elements.push_back(BSON("" << dec.add(tenth).negate()));
        }
    }

    {
        // Numbers around +/- numeric_limits<long long>::max() which can't be represented
        // precisely as a double.
        const long long maxLL = std::numeric_limits<long long>::max();
        const double closestAbove = 9223372036854775808.0;  // 2**63
        const double closestBelow = 9223372036854774784.0;  // 2**63 - epsilon

        elements.push_back(BSON("" << maxLL));
        elements.push_back(BSON("" << (maxLL - 1)));
        elements.push_back(BSON("" << closestAbove));
        elements.push_back(BSON("" << closestBelow));

        elements.push_back(BSON("" << -maxLL));
        elements.push_back(BSON("" << -(maxLL - 1)));
        elements.push_back(BSON("" << -closestAbove));
        elements.push_back(BSON("" << -closestBelow));
    }

    {
        // Numbers around numeric_limits<long long>::min() which can be represented precisely as
        // a double, but not as a positive long long.
        const long long minLL = std::numeric_limits<long long>::min();
        const double closestBelow = -9223372036854777856.0;  // -2**63 - epsilon
        const double equal = -9223372036854775808.0;         // 2**63
        const double closestAbove = -9223372036854774784.0;  // -2**63 + epsilon

        elements.push_back(BSON("" << minLL));
        elements.push_back(BSON("" << equal));
        elements.push_back(BSON("" << closestAbove));
        elements.push_back(BSON("" << closestBelow));
    }

    if (version != key_string::Version::V0) {
        // Numbers that are hard to round to between binary and decimal.
        elements.push_back(BSON("" << 0.1));
        elements.push_back(BSON("" << Decimal128("0.100000000")));
        // Decimals closest to the double representation of 0.1.
        elements.push_back(BSON("" << Decimal128("0.1000000000000000055511151231257827")));
        elements.push_back(BSON("" << Decimal128("0.1000000000000000055511151231257828")));

        // Decimals that failed at some point during testing.
        elements.push_back(BSON("" << Decimal128("0.999999999999999")));
        elements.push_back(BSON("" << Decimal128("2.22507385850721E-308")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364428E-324")));
        elements.push_back(BSON("" << Decimal128(9223372036854776000.0)));
        elements.push_back(BSON("" << Decimal128("9223372036854776000")));

        // Numbers close to numerical underflow/overflow for double.
        elements.push_back(BSON("" << Decimal128("1.797693134862315708145274237317044E308")));
        elements.push_back(BSON("" << Decimal128("1.797693134862315708145274237317043E308")));
        elements.push_back(BSON("" << Decimal128("-1.797693134862315708145274237317044E308")));
        elements.push_back(BSON("" << Decimal128("-1.797693134862315708145274237317043E308")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364427")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364428")));
        elements.push_back(BSON("" << Decimal128("-9.881312916824930883531375857364427")));
        elements.push_back(BSON("" << Decimal128("-9.881312916824930883531375857364428")));
        elements.push_back(BSON("" << Decimal128("4.940656458412465441765687928682213E-324")));
        elements.push_back(BSON("" << Decimal128("4.940656458412465441765687928682214E-324")));
        elements.push_back(BSON("" << Decimal128("-4.940656458412465441765687928682214E-324")));
        elements.push_back(BSON("" << Decimal128("-4.940656458412465441765687928682213E-324")));

        // Non-finite values. Note: can't roundtrip negative NaNs, so not testing here.
        elements.push_back(BSON("" << Decimal128::kPositiveNaN));
        elements.push_back(BSON("" << Decimal128::kNegativeInfinity));
        elements.push_back(BSON("" << Decimal128::kPositiveInfinity));
    }

    // Tricky double precision number for binary/decimal conversion: very close to a decimal
    if (version != key_string::Version::V0)
        elements.push_back(BSON("" << Decimal128("3743626360493413E-165")));
    elements.push_back(BSON("" << 3743626360493413E-165));

    return elements;
}

void testPermutation(key_string::Version version,
                     const std::vector<BSONObj>& elementsOrig,
                     const std::vector<BSONObj>& orderings,
                     bool debug) {
    // Since key_string::Builders are compared using memcmp we can assume it provides a total
    // ordering
    // such
    // that there won't be cases where (a < b && b < c && !(a < c)). This test still needs to ensure
    // that it provides the *correct* total ordering.
    std::vector<stdx::future<void>> futures;
    for (size_t k = 0; k < orderings.size(); k++) {
        futures.push_back(
            stdx::async(stdx::launch::async, [k, version, elementsOrig, orderings, debug] {
                BSONObj orderObj = orderings[k];
                Ordering ordering = Ordering::make(orderObj);
                if (debug)
                    LOGV2(22229, "ordering: {orderObj}", "orderObj"_attr = orderObj);

                std::vector<BSONObj> elements = elementsOrig;
                BSONObjComparator bsonCmp(orderObj,
                                          BSONObjComparator::FieldNamesMode::kConsider,
                                          &simpleStringDataComparator);
                std::stable_sort(elements.begin(), elements.end(), bsonCmp.makeLessThan());

                for (size_t i = 0; i < elements.size(); i++) {
                    const BSONObj& o1 = elements[i];
                    if (debug)
                        LOGV2(22230, "\to1: {o1}", "o1"_attr = o1);
                    ROUNDTRIP_ORDER(version, o1, ordering);

                    key_string::Builder k1(version, o1, ordering);

                    if (i + 1 < elements.size()) {
                        const BSONObj& o2 = elements[i + 1];
                        if (debug)
                            LOGV2(22231, "\t\t o2: {o2}", "o2"_attr = o2);
                        key_string::Builder k2(version, o2, ordering);

                        int bsonCmp = o1.woCompare(o2, ordering);
                        invariant(bsonCmp <= 0);  // We should be sorted...

                        if (bsonCmp == 0) {
                            ASSERT_EQ(k1, k2);
                        } else {
                            ASSERT_LT(k1, k2);
                        }

                        // Test the query encodings using kLess and kGreater
                        int firstElementComp = o1.firstElement().woCompare(o2.firstElement());
                        if (ordering.descending(1))
                            firstElementComp = -firstElementComp;

                        invariant(firstElementComp <= 0);
                    }
                }
            }));
    }
    for (auto&& future : futures) {
        future.get();
    }
}

namespace {
std::random_device rd;
std::mt19937_64 seedGen(rd());

// To be used by perf test for seeding, so that the entire test is repeatable in case of error.
unsigned newSeed() {
    unsigned int seed = seedGen();  // Replace by the reported number to repeat test execution.
    LOGV2(22232, "Initializing random number generator using seed {seed}", "seed"_attr = seed);
    return seed;
};

std::vector<BSONObj> thinElements(std::vector<BSONObj> elements,
                                  unsigned seed,
                                  size_t maxElements) {
    std::mt19937_64 gen(seed);

    if (elements.size() <= maxElements)
        return elements;

    LOGV2(22233,
          "only keeping {maxElements} of {elements_size} elements using random selection",
          "maxElements"_attr = maxElements,
          "elements_size"_attr = elements.size());
    std::shuffle(elements.begin(), elements.end(), gen);
    elements.resize(maxElements);
    return elements;
}
}  // namespace

namespace {
RecordId ridFromOid(const OID& oid) {
    key_string::Builder builder(key_string::Version::kLatestVersion);
    builder.appendOID(oid);
    return RecordId(builder.getBuffer(), builder.getSize());
}
}  // namespace

TEST_F(KeyStringBuilderTest, RecordIdStr) {
    const int kSize = 12;
    for (int i = 0; i < kSize; i++) {
        unsigned char buf[kSize];
        memset(buf, 0x80, kSize);
        buf[i] = 0xFE;

        const RecordId rid = ridFromOid(OID::from(buf));

        {  // Test encoding / decoding of single RecordIds
            const key_string::Builder ks(version, rid);
            invariant(ks.getSize() == 14);

            ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);

            if (rid.isValid()) {
                ASSERT_GT(ks, key_string::Builder(version, RecordId(1)));
                ASSERT_GT(ks, key_string::Builder(version, ridFromOid(OID())));
                ASSERT_LT(ks, key_string::Builder(version, ridFromOid(OID::max())));

                char bufLt[kSize];
                memcpy(bufLt, buf, kSize);
                bufLt[kSize - 1] -= 1;
                auto ltRid = ridFromOid(OID::from(bufLt));
                ASSERT(ltRid < rid);
                ASSERT_GT(ks, key_string::Builder(version, ltRid));

                char bufGt[kSize];
                memcpy(bufGt, buf, kSize);
                bufGt[kSize - 1] += 1;
                auto gtRid = ridFromOid(OID::from(bufGt));
                ASSERT(gtRid > rid);
                ASSERT_LT(ks, key_string::Builder(version, gtRid));
            }
        }

        for (int j = 0; j < kSize; j++) {
            unsigned char otherBuf[kSize] = {0};
            otherBuf[j] = 0xFE;
            RecordId other = ridFromOid(OID::from(otherBuf));

            if (rid == other) {
                ASSERT_EQ(key_string::Builder(version, rid), key_string::Builder(version, other));
            }
            if (rid < other) {
                ASSERT_LT(key_string::Builder(version, rid), key_string::Builder(version, other));
            }
            if (rid > other) {
                ASSERT_GT(key_string::Builder(version, rid), key_string::Builder(version, other));
            }
        }
    }
}

namespace {

RecordId ridFromStr(const char* str, size_t len) {
    key_string::Builder builder(key_string::Version::kLatestVersion);
    builder.appendString(mongo::StringData(str, len));
    return RecordId(builder.getBuffer(), builder.getSize());
}
}  // namespace


TEST_F(KeyStringBuilderTest, RecordIdStrBig1SizeSegment) {
    const int pad = 3;  // kStringLike CType + StringData terminator + RecordId len
    {
        const int size = 90;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad);
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
    {
        // Max 1-byte encoded string size is 127B: 1B CType + ridStr + string terminator
        const int size = 125;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad);
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
}

TEST_F(KeyStringBuilderTest, RecordIdStrBig2SizeSegments) {
    const int pad = 3;  // kStringLike CType + StringData terminator + RecordId len
    {
        // Min 2-byte encoded string size is 128B: 1B CType + ridStr + string terminator
        const int size = 126;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad + 1);  // 1 byte with continuation bit
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
    {
        const int size = 128;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad + 1);  // 1 byte with continuation bit
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
    {
        // Max 2-byte encoded string size is 16383B: 1B CType + ridStr + string terminator
        const int size = 16381;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad + 1);  // 1 byte with continuation bit
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
}

TEST_F(KeyStringBuilderTest, RecordIdStrBig3SizeSegments) {
    const int pad = 3;  // kStringLike CType + StringData terminator + RecordId len
    {
        // Min 3-byte encoded string size is 16384B: 1B CType + ridStr + string terminator
        const int size = 16382;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad + 2);  // 2 bytes with continuation bit
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
    {
        // Max 3-byte encoded string size is 2097151B: 1B CType + ridStr + string terminator
        const int size = 2097149;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad + 2);  // 2 bytes with continuation bit
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
}

TEST_F(KeyStringBuilderTest, RecordIdStrBig4SizeSegments) {
    const int pad = 3;  // kStringLike CType + StringData terminator + RecordId len
    {
        // Min 4-byte encoded string size is 2097152B: 1B CType + ridStr + string terminator
        const int size = 2097150;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad + 3);  // 3 bytes with continuation bit
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
    {
        // Support up to RecordId::kBigStrMaxSize
        const int size = RecordId::kBigStrMaxSize - 2 /* CType + string terminator */;
        const auto ridStr = std::string(size, 'a');
        auto rid = ridFromStr(ridStr.c_str(), size);
        const key_string::Builder ks(version, rid);
        ASSERT_EQ(ks.getSize(), size + pad + 3);  // 3 bytes with continuation bit
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(0, key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
}

TEST_F(KeyStringBuilderTest, RecordIdStrBigSizeWithoutRecordIdStr) {
    const int pad = 3;  // kStringLike CType + StringData terminator + RecordId len
    const char str[] = "keyval";
    const int padStr = 3;  // kStringLike CType + string terminator + discriminator
    {
        const int ridStrlen = 90;
        const auto ridStr = std::string(ridStrlen, 'a');
        auto rid = ridFromStr(ridStr.c_str(), ridStrlen);
        key_string::Builder ks(version);
        ks.appendString(mongo::StringData(str, strlen(str)));
        ks.appendRecordId(rid);
        ASSERT_EQ(ks.getSize(), strlen(str) + padStr + ridStrlen + pad);
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(strlen(str) + padStr,
                  key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
    {
        const int ridStrlen = 260;
        const auto ridStr = std::string(ridStrlen, 'a');
        auto rid = ridFromStr(ridStr.c_str(), ridStrlen);
        key_string::Builder ks(version);
        ks.appendString(mongo::StringData(str, strlen(str)));
        ks.appendRecordId(rid);
        ASSERT_EQ(ks.getSize(),
                  strlen(str) + padStr + ridStrlen + pad + 1);  // 1 0x80 cont byte
        ASSERT_EQ(key_string::decodeRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()), rid);
        ASSERT_EQ(strlen(str) + padStr,
                  key_string::sizeWithoutRecordIdStrAtEnd(ks.getBuffer(), ks.getSize()));
    }
}

TEST_F(KeyStringBuilderTest, AllPermCompare) {
    std::vector<BSONObj> elements = getInterestingElements(version);

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(version, o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1));
    orderings.push_back(BSON("a" << -1));

    testPermutation(version, elements, orderings, false);
}

TEST_F(KeyStringBuilderTest, AllPerm2Compare) {
    std::vector<BSONObj> baseElements = getInterestingElements(version);
    auto seed = newSeed();

    // Select only a small subset of elements, as the combination is quadratic.
    // We want to select two subsets independently, so all combinations will get tested eventually.
    // kMaxPermElements is the desired number of elements to pass to testPermutation.
    const size_t kMaxPermElements = kDebugBuild ? 100'000 : 500'000;
    size_t maxElements = sqrt(kMaxPermElements);
    auto firstElements = thinElements(baseElements, seed, maxElements);
    auto secondElements = thinElements(baseElements, seed + 1, maxElements);

    std::vector<BSONObj> elements;
    for (size_t i = 0; i < firstElements.size(); i++) {
        for (size_t j = 0; j < secondElements.size(); j++) {
            BSONObjBuilder b;
            b.appendElements(firstElements[i]);
            b.appendElements(secondElements[j]);
            BSONObj o = b.obj();
            elements.push_back(o);
        }
    }

    LOGV2(22234,
          "AllPerm2Compare {keyStringVersionToString_version} size:{elements_size}",
          "keyStringVersionToString_version"_attr = keyStringVersionToString(version),
          "elements_size"_attr = elements.size());

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(version, o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1 << "b" << 1));
    orderings.push_back(BSON("a" << -1 << "b" << 1));
    orderings.push_back(BSON("a" << 1 << "b" << -1));
    orderings.push_back(BSON("a" << -1 << "b" << -1));

    testPermutation(version, elements, orderings, false);
}

#define COMPARE_HELPER(LHS, RHS) (((LHS) < (RHS)) ? -1 : (((LHS) == (RHS)) ? 0 : 1))

int compareLongToDouble(long long lhs, double rhs) {
    if (rhs >= static_cast<double>(std::numeric_limits<long long>::max()))
        return -1;
    if (rhs < std::numeric_limits<long long>::min())
        return 1;

    if (fabs(rhs) >= (1LL << 52)) {
        return COMPARE_HELPER(lhs, static_cast<long long>(rhs));
    }

    return COMPARE_HELPER(static_cast<double>(lhs), rhs);
}

int compareNumbers(const BSONElement& lhs, const BSONElement& rhs) {
    invariant(lhs.isNumber());
    invariant(rhs.isNumber());

    if (lhs.type() == NumberInt || lhs.type() == NumberLong) {
        if (rhs.type() == NumberInt || rhs.type() == NumberLong) {
            return COMPARE_HELPER(lhs.numberLong(), rhs.numberLong());
        }
        return compareLongToDouble(lhs.numberLong(), rhs.Double());
    } else {  // double
        if (rhs.type() == NumberDouble) {
            return COMPARE_HELPER(lhs.Double(), rhs.Double());
        }
        return -compareLongToDouble(rhs.numberLong(), lhs.Double());
    }
}

TEST_F(KeyStringBuilderTest, NaNs) {
    // TODO use hex floats to force distinct NaNs
    const double nan1 = std::numeric_limits<double>::quiet_NaN();
    const double nan2 = std::numeric_limits<double>::signaling_NaN();

    // Since we only output a single NaN, we can only do ROUNDTRIP testing for nan1.
    ROUNDTRIP(version, BSON("" << nan1));

    const key_string::Builder ks1a(version, BSON("" << nan1), ONE_ASCENDING);
    const key_string::Builder ks1d(version, BSON("" << nan1), ONE_DESCENDING);

    const key_string::Builder ks2a(version, BSON("" << nan2), ONE_ASCENDING);
    const key_string::Builder ks2d(version, BSON("" << nan2), ONE_DESCENDING);

    ASSERT_EQ(ks1a, ks2a);
    ASSERT_EQ(ks1d, ks2d);

    ASSERT(std::isnan(toBson(ks1a, ONE_ASCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks2a, ONE_ASCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks1d, ONE_DESCENDING)[""].Double()));
    ASSERT(std::isnan(toBson(ks2d, ONE_DESCENDING)[""].Double()));

    if (version == key_string::Version::V0)
        return;

    const auto nan3 = Decimal128::kPositiveNaN;
    const auto nan4 = Decimal128::kNegativeNaN;
    // Since we only output a single NaN, we can only do ROUNDTRIP testing for nan1.
    ROUNDTRIP(version, BSON("" << nan3));
    const key_string::Builder ks3a(version, BSON("" << nan3), ONE_ASCENDING);
    const key_string::Builder ks3d(version, BSON("" << nan3), ONE_DESCENDING);

    const key_string::Builder ks4a(version, BSON("" << nan4), ONE_ASCENDING);
    const key_string::Builder ks4d(version, BSON("" << nan4), ONE_DESCENDING);

    ASSERT_EQ(ks1a, ks4a);
    ASSERT_EQ(ks1d, ks4d);

    ASSERT(toBson(ks3a, ONE_ASCENDING)[""].Decimal().isNaN());
    ASSERT(toBson(ks4a, ONE_ASCENDING)[""].Decimal().isNaN());
    ASSERT(toBson(ks3d, ONE_DESCENDING)[""].Decimal().isNaN());
    ASSERT(toBson(ks4d, ONE_DESCENDING)[""].Decimal().isNaN());
}

TEST_F(KeyStringBuilderTest, RecordIds) {
    for (int i = 0; i < 63; i++) {
        const RecordId rid = RecordId(1ll << i);

        {  // Test encoding / decoding of single RecordIds
            const key_string::Builder ks(version, rid);
            ASSERT_GTE(ks.getSize(), 2u);
            ASSERT_LTE(ks.getSize(), 10u);

            ASSERT_EQ(key_string::decodeRecordIdLongAtEnd(ks.getBuffer(), ks.getSize()), rid);

            {
                BufReader reader(ks.getBuffer(), ks.getSize());
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), rid);
                ASSERT(reader.atEof());
            }

            if (rid.isValid()) {
                ASSERT_GTE(ks, key_string::Builder(version, RecordId(1)));
                ASSERT_GT(ks, key_string::Builder(version, RecordId::minLong()));
                ASSERT_LT(ks, key_string::Builder(version, RecordId::maxLong()));

                ASSERT_GT(ks, key_string::Builder(version, RecordId(rid.getLong() - 1)));
                ASSERT_LT(ks, key_string::Builder(version, RecordId(rid.getLong() + 1)));
            }
        }

        for (int j = 0; j < 63; j++) {
            RecordId other = RecordId(1ll << j);

            if (rid == other) {
                ASSERT_EQ(key_string::Builder(version, rid), key_string::Builder(version, other));
            }
            if (rid < other) {
                ASSERT_LT(key_string::Builder(version, rid), key_string::Builder(version, other));
            }
            if (rid > other) {
                ASSERT_GT(key_string::Builder(version, rid), key_string::Builder(version, other));
            }

            {
                // Test concatenating RecordIds like in a unique index.
                key_string::Builder ks(version);
                ks.appendRecordId(RecordId::maxLong());  // uses all bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(RecordId(0xDEADBEEF));  // uses some extra bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(RecordId(1));  // uses no extra bytes
                ks.appendRecordId(rid);
                ks.appendRecordId(other);

                ASSERT_EQ(key_string::decodeRecordIdLongAtEnd(ks.getBuffer(), ks.getSize()), other);

                // forward scan
                BufReader reader(ks.getBuffer(), ks.getSize());
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), RecordId::maxLong());
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), rid);
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), RecordId(0xDEADBEEF));
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), rid);
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), RecordId(1));
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), rid);
                ASSERT_EQ(key_string::decodeRecordIdLong(&reader), other);
                ASSERT(reader.atEof());
            }
        }
    }
}

TEST_F(KeyStringBuilderTest, KeyWithLotsOfTypeBits) {
    BSONObj obj;
    {
        BSONObjBuilder builder;
        {
            int kArrSize = 54321;
            BSONArrayBuilder array(builder.subarrayStart("x"));
            auto zero = BSON("" << 0.0);
            for (int i = 0; i < kArrSize; i++)
                array.append(zero.firstElement());
        }

        obj = BSON("" << builder.obj());
    }
    ROUNDTRIP(version, obj);
}

BSONObj buildKeyWhichWillHaveNByteOfTypeBits(size_t n, bool allZeros) {
    int numItems = n * 8 / 2 /* kInt/kDouble needs two bits */;

    BSONObj obj;
    BSONArrayBuilder array;
    for (int i = 0; i < numItems; i++)
        if (allZeros)
            array.append(123); /* kInt uses 00 */
        else
            array.append(1.2); /* kDouble uses 10 */

    obj = BSON("" << array.arr());
    return obj;
}

void checkKeyWithNByteOfTypeBits(key_string::Version version, size_t n, bool allZeros) {
    const BSONObj orig = buildKeyWhichWillHaveNByteOfTypeBits(n, allZeros);
    const key_string::Builder ks(version, orig, ALL_ASCENDING);
    const size_t typeBitsSize = ks.getTypeBits().getSize();
    if (n == 1 || allZeros) {
        // Case 1&2
        // Case 2: Since we use kDouble, TypeBits="01010101" when n=1. The size
        // is thus 1.
        ASSERT_EQ(1u, typeBitsSize);
    } else if (n <= 127) {
        // Case 3
        ASSERT_EQ(n + 1, typeBitsSize);
    } else {
        // Case 4
        ASSERT_EQ(n + 5, typeBitsSize);
    }
    const BSONObj converted = toBsonAndCheckKeySize(ks, ALL_ASCENDING);
    ASSERT_BSONOBJ_EQ(converted, orig);
    ASSERT(converted.binaryEqual(orig));

    // Also test TypeBits::fromBuffer()
    BufReader bufReader(ks.getTypeBits().getBuffer(), typeBitsSize);
    key_string::TypeBits newTypeBits = key_string::TypeBits::fromBuffer(version, &bufReader);
    ASSERT_EQ(hexblob::encode(newTypeBits.getBuffer(), newTypeBits.getSize()),
              hexblob::encode(ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize()));
}

TEST_F(KeyStringBuilderTest, KeysWithNBytesTypeBits) {
    checkKeyWithNByteOfTypeBits(version, 0, false);
    checkKeyWithNByteOfTypeBits(version, 1, false);
    checkKeyWithNByteOfTypeBits(version, 1, true);
    checkKeyWithNByteOfTypeBits(version, 127, false);
    checkKeyWithNByteOfTypeBits(version, 127, true);
    checkKeyWithNByteOfTypeBits(version, 128, false);
    checkKeyWithNByteOfTypeBits(version, 128, true);
    checkKeyWithNByteOfTypeBits(version, 129, false);
    checkKeyWithNByteOfTypeBits(version, 129, true);
}

TEST_F(KeyStringBuilderTest, VeryLargeString) {
    BSONObj obj = BSON("" << std::string(123456, 'x'));
    ROUNDTRIP(version, obj);
}

TEST_F(KeyStringBuilderTest, ToBsonSafeShouldNotTerminate) {
    key_string::TypeBits typeBits(key_string::Version::V1);

    const char invalidString[] = {
        60,  // CType::kStringLike
        55,  // Non-null terminated
    };
    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(invalidString, sizeof(invalidString), ALL_ASCENDING, typeBits),
        AssertionException,
        50816);

    const char invalidNumber[] = {
        43,  // CType::kNumericPositive1ByteInt
        1,   // Encoded integer part, least significant bit indicates there's a fractional part.
        0,   // Since the integer part is 1 byte, the next 7 bytes are expected to be the fractional
             // part and are needed to prevent the BufReader from overflowing.
        0,
        0,
        0,
        0,
        0,
        0,
    };
    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(invalidNumber, sizeof(invalidNumber), ALL_ASCENDING, typeBits),
        AssertionException,
        50810);
}

TEST_F(KeyStringBuilderTest, InvalidDecimalExponent) {
    const Decimal128 dec("1125899906842624.1");
    const key_string::Builder ks(key_string::Version::V1, BSON("" << dec), ALL_ASCENDING);

    // Overwrite the 1st byte to 0, corrupting the exponent. This is meant to reproduce
    // SERVER-34767.
    char* ksBuffer = (char*)ks.getBuffer();
    ksBuffer[1] = 0;

    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(ksBuffer, ks.getSize(), ALL_ASCENDING, ks.getTypeBits()),
        AssertionException,
        50814);
}

TEST_F(KeyStringBuilderTest, InvalidDecimalZero) {
    const key_string::Builder ks(
        key_string::Version::V1, BSON("" << Decimal128("-0")), ALL_ASCENDING);

    char* ksBuffer = (char*)ks.getBuffer();
    ksBuffer[2] = 100;

    uint8_t* typeBits = (uint8_t*)ks.getTypeBits().getBuffer();
    typeBits[1] = 147;

    ASSERT_THROWS_CODE(
        key_string::toBsonSafe(ksBuffer, ks.getSize(), ALL_ASCENDING, ks.getTypeBits()),
        AssertionException,
        50846);
}

TEST_F(KeyStringBuilderTest, InvalidDecimalContinuation) {
    auto elem = Decimal128("1.797693134862315708145274237317043E308");
    const key_string::Builder ks(key_string::Version::V1, BSON("" << elem), ALL_ASCENDING);

    uint8_t* ksBuffer = (uint8_t*)ks.getBuffer();
    ksBuffer[2] = 239;

    uint8_t* typeBits = (uint8_t*)ks.getTypeBits().getBuffer();
    typeBits[1] = 231;

    ASSERT_THROWS_CODE(
        key_string::toBsonSafe((char*)ksBuffer, ks.getSize(), ALL_ASCENDING, ks.getTypeBits()),
        AssertionException,
        50850);
}

TEST_F(KeyStringBuilderTest, RandomizedInputsForToBsonSafe) {
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<unsigned int> randomNum(std::numeric_limits<unsigned int>::min(),
                                                          std::numeric_limits<unsigned int>::max());

    const auto interestingElements = getInterestingElements(key_string::Version::V1);
    for (const auto& elem : interestingElements) {
        const key_string::Builder ks(key_string::Version::V1, elem, ALL_ASCENDING);

        auto ksBuffer = SharedBuffer::allocate(ks.getSize());
        memcpy(ksBuffer.get(), ks.getBuffer(), ks.getSize());
        auto tbBuffer = SharedBuffer::allocate(ks.getTypeBits().getSize());
        memcpy(tbBuffer.get(), ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize());

        // Select a random byte to change, except for the first byte as it will likely become an
        // invalid CType and not test anything interesting.
        auto offset = randomNum(gen) % (ks.getSize() - 1);
        uint8_t newValue = randomNum(gen) % std::numeric_limits<uint8_t>::max();
        ksBuffer.get()[offset + 1] = newValue;

        // Ditto for the type bits buffer.
        offset = randomNum(gen) % ks.getTypeBits().getSize();
        newValue = randomNum(gen) % std::numeric_limits<uint8_t>::max();
        tbBuffer.get()[offset] = newValue;

        // Build the new TypeBits.
        BufReader reader(tbBuffer.get(), ks.getTypeBits().getSize());

        try {
            auto newTypeBits = key_string::TypeBits::fromBuffer(key_string::Version::V1, &reader);
            key_string::toBsonSafe(ksBuffer.get(), ks.getSize(), ALL_ASCENDING, newTypeBits);
        } catch (const AssertionException&) {
            // The expectation is that the randomized buffer is likely an invalid
            // key_string::Builder,
            // however attempting to decode it should fail gracefully.
        }

        // Retest with descending.
        try {
            auto newTypeBits = key_string::TypeBits::fromBuffer(key_string::Version::V1, &reader);
            key_string::toBsonSafe(ksBuffer.get(), ks.getSize(), ONE_DESCENDING, newTypeBits);
        } catch (const AssertionException&) {
            // The expectation is that the randomized buffer is likely an invalid
            // key_string::Builder,
            // however attempting to decode it should fail gracefully.
        }
    }
}

namespace {
const uint64_t kMinPerfMicros = 20 * 1000;
const uint64_t kMinPerfSamples = 50 * 1000;
typedef std::vector<BSONObj> Numbers;

/**
 * Evaluates ROUNDTRIP on all items in Numbers a sufficient number of times to take at least
 * kMinPerfMicros microseconds. Logs the elapsed time per ROUNDTRIP evaluation.
 */
void perfTest(key_string::Version version, const Numbers& numbers) {
    uint64_t micros = 0;
    uint64_t iters;
    // Ensure at least 16 iterations are done and at least 50 milliseconds is timed
    for (iters = 16; iters < (1 << 30) && micros < kMinPerfMicros; iters *= 2) {
        // Measure the number of loops
        Timer t;

        for (uint64_t i = 0; i < iters; i++)
            for (const auto& item : numbers) {
                // Assuming there are sufficient invariants in the to/from key_string::Builder
                // methods
                // that calls will not be optimized away.
                const key_string::Builder ks(version, item, ALL_ASCENDING);
                const BSONObj& converted = toBson(ks, ALL_ASCENDING);
                invariant(converted.binaryEqual(item));
            }

        micros = t.micros();
    }

    auto minmax = std::minmax_element(
        numbers.begin(), numbers.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    LOGV2(22236,
          "{_1E3_micros_static_cast_double_iters_numbers_size} ns per "
          "{mongo_KeyString_keyStringVersionToString_version} roundtrip{kDebugBuild_DEBUG_BUILD} "
          "min {minmax_first}, max{minmax_second}",
          "_1E3_micros_static_cast_double_iters_numbers_size"_attr =
              1E3 * micros / static_cast<double>(iters * numbers.size()),
          "mongo_KeyString_keyStringVersionToString_version"_attr =
              mongo::key_string::keyStringVersionToString(version),
          "kDebugBuild_DEBUG_BUILD"_attr = (kDebugBuild ? " (DEBUG BUILD!)" : ""),
          "minmax_first"_attr = (*minmax.first)[""],
          "minmax_second"_attr = (*minmax.second)[""]);
}
}  // namespace

TEST_F(KeyStringBuilderTest, CommonIntPerf) {
    // Exponential distribution, so skewed towards smaller integers.
    std::mt19937 gen(newSeed());
    std::exponential_distribution<double> expReal(1e-3);

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << static_cast<int>(expReal(gen))));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, UniformInt64Perf) {
    std::vector<BSONObj> numbers;
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << uniformInt64(gen)));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, CommonDoublePerf) {
    std::mt19937 gen(newSeed());
    std::exponential_distribution<double> expReal(1e-3);

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << expReal(gen)));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, UniformDoublePerf) {
    std::vector<BSONObj> numbers;
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t u = uniformInt64(gen);
        double d;
        memcpy(&d, &u, sizeof(d));
        if (std::isnormal(d))
            numbers.push_back(BSON("" << d));
    }
    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, CommonDecimalPerf) {
    std::mt19937 gen(newSeed());
    std::exponential_distribution<double> expReal(1e-3);

    if (version == key_string::Version::V0)
        return;

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(
            BSON("" << Decimal128(
                           expReal(gen), Decimal128::kRoundTo34Digits, Decimal128::kRoundTiesToAway)
                           .quantize(Decimal128("0.01", Decimal128::kRoundTiesToAway))));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, UniformDecimalPerf) {
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    if (version == key_string::Version::V0)
        return;

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t hi = uniformInt64(gen);
        uint64_t lo = uniformInt64(gen);
        Decimal128 d(Decimal128::Value{lo, hi});
        if (!d.isZero() && !d.isNaN() && !d.isInfinite())
            numbers.push_back(BSON("" << d));
    }
    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, DecimalFromUniformDoublePerf) {
    std::vector<BSONObj> numbers;
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    if (version == key_string::Version::V0)
        return;

    // In addition to serve as a data ponit for performance, this test also generates many decimal
    // values close to binary floating point numbers, so edge cases around 15-digit approximations
    // get extra randomized coverage over time.
    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t u = uniformInt64(gen);
        double d;
        memcpy(&d, &u, sizeof(d));
        if (!std::isnan(d)) {
            Decimal128::RoundingMode mode =
                x & 1 ? Decimal128::kRoundTowardPositive : Decimal128::kRoundTowardNegative;
            Decimal128::RoundingPrecision prec =
                x & 2 ? Decimal128::kRoundTo15Digits : Decimal128::kRoundTo34Digits;
            numbers.push_back(BSON("" << Decimal128(d, prec, mode)));
        }
    }
    perfTest(version, numbers);
}

DEATH_TEST(KeyStringBuilderTest, ToBsonPromotesAssertionsToTerminate, "terminate() called") {
    const char invalidString[] = {
        60,  // CType::kStringLike
        55,  // Non-null terminated
    };
    key_string::TypeBits typeBits(key_string::Version::V1);
    key_string::toBson(invalidString, sizeof(invalidString), ALL_ASCENDING, typeBits);
}

// The following tests run last because they take a very long time.

TEST_F(KeyStringBuilderTest, LotsOfNumbers3) {
    std::vector<stdx::future<void>> futures;

    for (double k = 0; k < 8; k++) {
        futures.push_back(stdx::async(stdx::launch::async, [k, this] {
            for (double i = -1100; i < 1100; i++) {
                for (double j = 0; j < 52; j++) {
                    const auto V1 = key_string::Version::V1;
                    Decimal128::RoundingPrecision roundingPrecisions[]{
                        Decimal128::kRoundTo15Digits, Decimal128::kRoundTo34Digits};
                    Decimal128::RoundingMode roundingModes[]{Decimal128::kRoundTowardNegative,
                                                             Decimal128::kRoundTowardPositive};
                    double x = pow(2, i);
                    double y = pow(2, i - j);
                    double z = pow(2, i - 53 + k);
                    double bin = x + y - z;

                    // In general NaNs don't roundtrip as we only store a single NaN, see the NaNs
                    // test.
                    if (std::isnan(bin))
                        continue;

                    ROUNDTRIP(version, BSON("" << bin));
                    ROUNDTRIP(version, BSON("" << -bin));

                    if (version < V1)
                        continue;

                    for (auto precision : roundingPrecisions) {
                        for (auto mode : roundingModes) {
                            Decimal128 rounded = Decimal128(bin, precision, mode);
                            ROUNDTRIP(V1, BSON("" << rounded));
                            ROUNDTRIP(V1, BSON("" << rounded.negate()));
                        }
                    }
                }
            }
        }));
    }
    for (auto&& future : futures) {
        future.get();
    }
}

TEST_F(KeyStringBuilderTest, NumberOrderLots) {
    std::vector<BSONObj> numbers;
    {
        numbers.push_back(BSON("" << 0));
        numbers.push_back(BSON("" << 0.0));
        numbers.push_back(BSON("" << -0.0));

        numbers.push_back(BSON("" << std::numeric_limits<long long>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<long long>::max()));
        numbers.push_back(BSON("" << static_cast<double>(std::numeric_limits<long long>::min())));
        numbers.push_back(BSON("" << static_cast<double>(std::numeric_limits<long long>::max())));
        numbers.push_back(BSON("" << std::numeric_limits<double>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<double>::max()));
        numbers.push_back(BSON("" << std::numeric_limits<int>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<int>::max()));
        numbers.push_back(BSON("" << std::numeric_limits<short>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<short>::max()));

        for (int i = 0; i < 64; i++) {
            int64_t x = 1LL << i;
            numbers.push_back(BSON("" << static_cast<long long>(x)));
            numbers.push_back(BSON("" << static_cast<int>(x)));
            numbers.push_back(BSON("" << static_cast<double>(x)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + .1)));

            numbers.push_back(BSON("" << (static_cast<long long>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<int>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + 1.1)));

            // Avoid negating signed integral minima
            if (i < 63)
                numbers.push_back(BSON("" << -static_cast<long long>(x)));

            if (i < 31)
                numbers.push_back(BSON("" << -static_cast<int>(x)));

            numbers.push_back(BSON("" << -static_cast<double>(x)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + .1)));

            numbers.push_back(BSON("" << -(static_cast<long long>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<int>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + 1.1)));
        }

        for (double i = 0; i < 1000; i++) {
            double x = pow(2.1, i);
            numbers.push_back(BSON("" << x));
        }
    }

    Ordering ordering = Ordering::make(BSON("a" << 1));

    std::vector<std::unique_ptr<key_string::Builder>> KeyStringBuilders;
    for (size_t i = 0; i < numbers.size(); i++) {
        KeyStringBuilders.push_back(
            std::make_unique<key_string::Builder>(version, numbers[i], ordering));
    }

    for (size_t i = 0; i < numbers.size(); i++) {
        for (size_t j = 0; j < numbers.size(); j++) {
            const key_string::Builder& a = *KeyStringBuilders[i];
            const key_string::Builder& b = *KeyStringBuilders[j];
            ASSERT_EQUALS(a.compare(b), -b.compare(a));

            if (a.compare(b) !=
                compareNumbers(numbers[i].firstElement(), numbers[j].firstElement())) {
                LOGV2(22235,
                      "{numbers_i} {numbers_j}",
                      "numbers_i"_attr = numbers[i],
                      "numbers_j"_attr = numbers[j]);
            }

            ASSERT_EQUALS(a.compare(b),
                          compareNumbers(numbers[i].firstElement(), numbers[j].firstElement()));
        }
    }
}

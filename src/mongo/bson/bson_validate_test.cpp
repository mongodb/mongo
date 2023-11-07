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


#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {

using namespace mongo;
using std::endl;
using std::unique_ptr;

void appendInvalidStringElement(const char* fieldName, BufBuilder* bb) {
    // like a BSONObj string, but without a NUL terminator.
    bb->appendChar(String);
    bb->appendStr(fieldName, /*withNUL*/ true);
    bb->appendNum(4);
    bb->appendStr("asdf", /*withNUL*/ false);
}

TEST(BSONValidate, Basic) {
    BSONObj x;
    ASSERT_TRUE(validateBSON(x).isOK());

    x = BSON("x" << 1);
    ASSERT_TRUE(validateBSON(x).isOK());
}

TEST(BSONValidate, RandomData) {
    PseudoRandom r(17);

    int numValid = 0;
    int numToRun = 1000;
    long long jsonSize = 0;

    for (int i = 0; i < numToRun; i++) {
        int size = 1234;

        char* x = new char[size];
        DataView(x).write(tagLittleEndian(size));

        for (int i = 4; i < size; i++) {
            x[i] = r.nextInt32(255);
        }

        x[size - 1] = 0;

        BSONObj o(x);

        ASSERT_EQUALS(size, o.objsize());

        if (validateBSON(o).isOK()) {
            numValid++;
            jsonSize += o.jsonString().size();
            ASSERT_OK(validateBSON(o));
        } else {
            ASSERT_NOT_OK(validateBSON(o));
        }

        delete[] x;
    }

    LOGV2(20104,
          "RandomData: didn't crash valid/total: {numValid}/{numToRun} (want few valid ones) "
          "jsonSize: {jsonSize}",
          "numValid"_attr = numValid,
          "numToRun"_attr = numToRun,
          "jsonSize"_attr = jsonSize);
}

TEST(BSONValidate, MuckingData1) {
    BSONObj theObject;

    {
        BSONObjBuilder b;
        b.append("name", "eliot was here");
        b.append("yippee", "asd");
        BSONArrayBuilder a(b.subarrayStart("arr"));
        for (int i = 0; i < 100; i++) {
            a.append(BSON("x" << i << "who"
                              << "me"
                              << "asd"
                              << "asd"));
        }
        a.done();
        b.done();

        theObject = b.obj();
    }

    int numValid = 0;
    int numToRun = 1000;
    long long jsonSize = 0;

    for (int i = 4; i < theObject.objsize() - 1; i++) {
        BSONObj mine = theObject.copy();

        char* data = const_cast<char*>(mine.objdata());

        data[i] = 0xc8U;

        numToRun++;
        if (validateBSON(mine).isOK()) {
            numValid++;
            jsonSize += mine.jsonString().size();
            ASSERT_OK(validateBSON(mine));
        } else {
            ASSERT_NOT_OK(validateBSON(mine));
        }
    }

    LOGV2(20105,
          "MuckingData1: didn't crash valid/total: {numValid}/{numToRun} (want few valid ones)  "
          "jsonSize: {jsonSize}",
          "numValid"_attr = numValid,
          "numToRun"_attr = numToRun,
          "jsonSize"_attr = jsonSize);
}

TEST(BSONValidate, Fuzz) {
    int64_t seed = time(nullptr);
    LOGV2(20106, "BSONValidate Fuzz random seed: {seed}", "seed"_attr = seed);
    PseudoRandom randomSource(seed);

    BSONObj original =
        BSON("one" << 3 << "two" << 5 << "three" << BSONObj() << "four"
                   << BSON("five" << BSON("six" << 11)) << "seven"
                   << BSON_ARRAY("a"
                                 << "bb"
                                 << "ccc" << 5)
                   << "eight" << BSONDBRef("rrr", OID("01234567890123456789aaaa")) << "_id"
                   << OID("deadbeefdeadbeefdeadbeef") << "nine"
                   << BSONBinData("\x69\xb7", 2, BinDataGeneral) << "ten"
                   << Date_t::fromMillisSinceEpoch(44) << "eleven" << BSONRegEx("foooooo", "i"));

    int32_t fuzzFrequencies[] = {2, 10, 20, 100, 1000};
    for (size_t i = 0; i < sizeof(fuzzFrequencies) / sizeof(int32_t); ++i) {
        int32_t fuzzFrequency = fuzzFrequencies[i];

        // Copy the 'original' BSONObj to 'buffer'.
        unique_ptr<char[]> buffer(new char[original.objsize()]);
        memcpy(buffer.get(), original.objdata(), original.objsize());

        // Randomly flip bits in 'buffer', with probability determined by 'fuzzFrequency'. The
        // first four bytes, representing the size of the object, are excluded from bit
        // flipping.
        for (int32_t byteIdx = 4; byteIdx < original.objsize(); ++byteIdx) {
            for (int32_t bitIdx = 0; bitIdx < 8; ++bitIdx) {
                if (randomSource.nextInt32(fuzzFrequency) == 0) {
                    reinterpret_cast<unsigned char&>(buffer[byteIdx]) ^= (1U << bitIdx);
                }
            }
        }
        BSONObj fuzzed(buffer.get());

        // There is no assert here because there is no other BSON validator oracle
        // to compare outputs against (BSONObj::valid() is a wrapper for validateBSON()).
        // Thus, the reason for this test is to ensure that validateBSON() doesn't trip
        // any ASAN or UBSAN check when fed fuzzed input.
        validateBSON(fuzzed).isOK();
    }
}

TEST(BSONValidateExtended, MD5Size) {
    // 16 byte string.
    auto properSizeMD5 = "aaaaaaaaaaaaaaaa";
    BSONObj x1 = BSON("md5" << BSONBinData(properSizeMD5, 16, MD5Type));
    ASSERT_OK(validateBSON(x1, mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1, mongo::BSONValidateMode::kFull));

    // 15 byte string.
    auto improperSizeMD5 = "aaaaaaaaaaaaaaa";
    BSONObj x2 = BSON("md5" << BSONBinData(improperSizeMD5, 15, MD5Type));
    Status status = validateBSON(x2, mongo::BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(x2, mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateExtended, BSONArrayIndexes) {
    BSONObj arr = BSON("0"
                       << "a"
                       << "1"
                       << "b");
    BSONObj x1 = BSON("arr" << BSONArray(arr));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull));


    arr = BSON("a" << 1 << "b" << 2);
    x1 = BSON("nonNumericalArray" << BSONArray(arr));
    Status status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);

    arr = BSON("1"
               << "a"
               << "2"
               << "b");
    x1 = BSON("nonSequentialArray" << BSONArray(arr));
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);

    x1 = BSON("nestedArraysAndObjects" << BSONArray(BSON("0"
                                                         << "a"
                                                         << "1"
                                                         << BSONArray(BSON("0"
                                                                           << "a"
                                                                           << "2"
                                                                           << "b"))
                                                         << "2"
                                                         << "b")));
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);

    x1 = BSON("longArray" << BSONArray(BSON("0"
                                            << "a"
                                            << "1"
                                            << "b"
                                            << "2"
                                            << "c"
                                            << "3"
                                            << "d"
                                            << "4"
                                            << "e"
                                            << "5"
                                            << "f"
                                            << "6"
                                            << "g"
                                            << "7"
                                            << "h"
                                            << "8"
                                            << "i"
                                            << "9"
                                            << "j"
                                            << "10"
                                            << "k")));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull));

    x1 = BSON("longNonSequentialArray" << BSONArray(BSON("0"
                                                         << "a"
                                                         << "1"
                                                         << "b"
                                                         << "2"
                                                         << "c"
                                                         << "3"
                                                         << "d"
                                                         << "4"
                                                         << "e"
                                                         << "5"
                                                         << "f"
                                                         << "6"
                                                         << "g"
                                                         << "7"
                                                         << "h"
                                                         << "8"
                                                         << "i"
                                                         << "9"
                                                         << "j"
                                                         << "11"
                                                         << "k")));
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);

    x1 = BSON("validNestedArraysAndObjects"
              << BSON("arr" << BSONArray(BSON("0" << BSON("2" << 1 << "1" << 0 << "3"
                                                              << BSONArray(BSON("0"
                                                                                << "a"
                                                                                << "1"
                                                                                << "b"))
                                                              << "4"
                                                              << "b")))));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull));

    x1 = BSON("invalidNestedArraysAndObjects"
              << BSON("arr" << BSONArray(BSON("0" << BSON("2" << 1 << "1" << 0 << "1"
                                                              << BSONArray(BSON("0"
                                                                                << "a"
                                                                                << "2"
                                                                                << "b"))
                                                              << "1"
                                                              << "b")))));
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateExtended, BSONUTF8) {
    auto x1 = BSON("ValidString"
                   << "\x00"
                   << "ValidString2"
                   << "str");
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull));

    // Invalid UTF-8 - 10000000; leading bit cannot be set for single byte UTF-8.
    x1 = BSON("InvalidOneByteString"
              << "\x80");
    auto status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_OK(status);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);

    x1 = BSON("ValidTwoByteString"
              << "\x40\x40");
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull));

    // Invalid UTF-8 - 11011111 11001111; second bit of second byte cannot be set.
    x1 = BSON("InvalidTwoByteString"
              << "\xDF\xCF");
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_OK(status);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);

    x1 = BSON("ValidThreeByteString"
              << "\x40\x40\x40");
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull));

    // Invalid UTF-8 - 11101111 10111111 11111111 - second bit of third byte cannot be set.
    x1 = BSON("InvalidThreeByteString"
              << "\xEF\xBF\xFF");
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_OK(status);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);

    x1 = BSON("ValidFourByteString"
              << "\x40\x40\x40\x40");
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended));
    ASSERT_OK(validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull));

    // Invalid UTF-8 - 11110000 10011000 10011010 11111111 - second bit of fourth byte cannot be
    // set.
    x1 = BSON("InvalidFourByteString"
              << "\xF0\x98\x9A\xFF");
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kExtended);
    ASSERT_OK(status);
    status = validateBSON(x1.objdata(), x1.objsize(), mongo::BSONValidateMode::kFull);
    ASSERT_EQ(status, ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateFast, Empty) {
    BSONObj x;
    ASSERT_OK(validateBSON(x));
}

TEST(BSONValidateFast, RegEx) {
    BSONObjBuilder b;
    b.appendRegex("foo", "i");
    BSONObj x = b.obj();
    ASSERT_OK(validateBSON(x));
}

TEST(BSONValidateFast, Simple0) {
    BSONObj x;
    ASSERT_OK(validateBSON(x));

    x = BSON("foo" << 17 << "bar"
                   << "eliot");
    ASSERT_OK(validateBSON(x));
}

TEST(BSONValidateFast, Simple2) {
    char buf[64];
    for (int i = 1; i <= JSTypeMax; i++) {
        BSONObjBuilder b;
        sprintf(buf, "foo%d", i);
        b.appendMinForType(buf, i);
        sprintf(buf, "bar%d", i);
        b.appendMaxForType(buf, i);
        BSONObj x = b.obj();
        ASSERT_OK(validateBSON(x));
    }
}


TEST(BSONValidateFast, Simple3) {
    BSONObjBuilder b;
    char buf[64];
    for (int i = 1; i <= JSTypeMax; i++) {
        sprintf(buf, "foo%d", i);
        b.appendMinForType(buf, i);
        sprintf(buf, "bar%d", i);
        b.appendMaxForType(buf, i);
    }
    BSONObj x = b.obj();
    ASSERT_OK(validateBSON(x));
}

TEST(BSONValidateFast, NestedObject) {
    BSONObj x = BSON("a" << 1 << "b"
                         << BSON("c" << 2 << "d" << BSONArrayBuilder().obj() << "e"
                                     << BSON_ARRAY("1" << 2 << 3)));
    ASSERT_OK(validateBSON(x));
    ASSERT_NOT_OK(validateBSON(x.objdata(), x.objsize() / 2));
}

TEST(BSONValidateFast, AllTypesSimple) {
    BSONObj x = BSON(
        "1float" << 1.5  // 64-bit binary floating point
                 << "2string"
                 << "Hello"                                           // UTF-8 string
                 << "3document" << BSON("a" << 1)                     // Embedded document
                 << "4array" << BSON_ARRAY(1 << 2)                    // Array
                 << "5bindata" << BSONBinData("", 0, BinDataGeneral)  // Binary data
                 << "6undefined" << BSONUndefined  // Undefined (value) -- Deprecated
                 << "7objectid" << OID("deadbeefdeadbeefdeadbeef")  // ObjectId
                 << "8boolean" << true                              // Boolean
                 << "9datetime" << DATENOW                          // UTC datetime
                 << "10null" << BSONNULL                            // Null value
                 << "11regex" << BSONRegEx("reg.ex")                // Regular Expression
                 << "12dbref"
                 << BSONDBRef("db", OID("dbdbdbdbdbdbdbdbdbdbdbdb"))  // DBPointer -- Deprecated
                 << "13code" << BSONCode("(function(){})();")         // JavaScript code
                 << "14symbol" << BSONSymbol("symbol")                // Symbol. Deprecated
                 << "15code_w_s"
                 << BSONCodeWScope("(function(){})();", BSON("a" << 1))  // JavaScript code w/ scope
                 << "16int" << 42                                        // 32-bit integer
                 << "17timestamp" << Timestamp(1, 2)                     // Timestamp
                 << "18long" << 0x0123456789abcdefll                     // 64-bit integer
                 << "19decimal" << Decimal128("0.30")  // 128-bit decimal floating point
    );
    ASSERT_OK(validateBSON(x));
}

TEST(BSONValidateFast, ErrorWithId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    ob.append("_id", 1);
    appendInvalidStringElement("not_id", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(
        status.reason(),
        "Not null terminated string in element with field name 'not_id' in object with _id: 1");
}

TEST(BSONValidateFast, ErrorBeforeId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    appendInvalidStringElement("not_id", &bb);
    ob.append("_id", 1);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(),
                  "Not null terminated string in element with field name 'not_id' in object with "
                  "unknown _id");
}

TEST(BSONValidateFast, ErrorNoId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    appendInvalidStringElement("not_id", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(),
                  "Not null terminated string in element with field name 'not_id' in object with "
                  "unknown _id");
}

TEST(BSONValidateFast, ErrorIsInId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    appendInvalidStringElement("_id", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(
        status.reason(),
        "Not null terminated string in element with field name '_id' in object with unknown _id");
}

TEST(BSONValidateFast, NonTopLevelId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    ob.append("not_id1",
              BSON("_id"
                   << "not the real _id"));
    appendInvalidStringElement("not_id2", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(),
                  "Not null terminated string in element with field name 'not_id2' in object with "
                  "unknown _id");
}

TEST(BSONValidateFast, ErrorInNestedObjectWithId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    ob.append("x", 2.0);
    appendInvalidStringElement("invalid", &bb);
    const BSONObj nestedInvalid = ob.done();
    const BSONObj x = BSON("_id" << 1 << "nested"
                                 << BSON_ARRAY("a"
                                               << "b" << nestedInvalid));
    const Status status = validateBSON(x);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(),
                  "Not null terminated string in element with field name 'nested.2.invalid' "
                  "in object with _id: 1");
}

TEST(BSONValidateFast, StringHasSomething) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    bb.appendChar(String);
    bb.appendStr("x", /*withNUL*/ true);
    bb.appendNum(0);
    const BSONObj x = ob.done();
    ASSERT_EQUALS(5        // overhead
                      + 1  // type
                      + 2  // name
                      + 4  // size
                  ,
                  x.objsize());
    ASSERT_NOT_OK(validateBSON(x));
}

TEST(BSONValidateFast, BoolValuesAreValidated) {
    BSONObjBuilder bob;
    bob.append("x", false);
    const BSONObj obj = bob.done();
    ASSERT_OK(validateBSON(obj));
    const BSONElement x = obj["x"];
    // Legal, because we know that the BufBuilder gave
    // us back some heap memory, which isn't originally const.
    auto writable = const_cast<char*>(x.value());
    // The next few lines may look non-optimal and strange, however the assignment of a
    // signed char to int is not safe, generally speaking. Recall that the numeric_limits
    // library returns the limit as the type requested and that the signedness of char is
    // platform-dependent. We know in this case that all values of a byte need to be
    // tested, so we can hardcode the limits ourselves to sidestep casting issues and
    // warnings from clang-tidy.
    //
    // Loop iteration will be the following for platforms with:
    //    signed char: -128->127
    //  unsigned char: 128->255, 0->127
    int signed_char_min = -128;
    int signed_char_max = 127;
    for (int val = signed_char_min; val != signed_char_max + 1; ++val) {
        *writable = static_cast<char>(val);
        if ((val == 0) || (val == 1)) {
            ASSERT_OK(validateBSON(obj));
        } else {
            ASSERT_NOT_OK(validateBSON(obj));
        }
    }
}

TEST(BSONValidateFast, InvalidType) {
    // Encode an invalid BSON Object with an invalid type, x90.
    const char* buffer = "\x0c\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";

    // Constructing the object is fine, but validating should fail.
    BSONObj obj(buffer);

    // Validate fails.
    ASSERT_NOT_OK(validateBSON(obj));

    // Make sure the binary buffer above indeed has the invalid type.
    ASSERT_THROWS_CODE(obj.woCompare(BSON("A" << 1)), DBException, 10320);
}

TEST(BSONValidateFast, ValidCodeWScope) {
    BSONObj obj = BSON("a" << BSONCodeWScope("code", BSON("c" << BSONObj())));
    ASSERT_OK(validateBSON(obj));
    obj = BSON("a" << BSONCodeWScope("code", BSON("c" << BSONArray() << "d" << BSONArray())));
    ASSERT_OK(validateBSON(obj));
}

BSONObj nest(int nesting) {
    return nesting < 1 ? BSON("i" << nesting) : BSON("i" << nesting << "o" << nest(nesting - 1));
}

TEST(BSONValidateFast, MaxNestingDepth) {
    BSONObj maxNesting = nest(BSONDepth::getMaxAllowableDepth());
    ASSERT_OK(validateBSON(maxNesting));

    BSONObj tooDeepNesting = nest(BSONDepth::getMaxAllowableDepth() + 1);
    Status status = validateBSON(tooDeepNesting);
    ASSERT_EQ(status.code(), ErrorCodes::Overflow);
}

TEST(BSONValidateFast, ErrorTooShort) {
    BSONObj x;
    x = BSON("foo" << 17 << "bar"
                   << "eliot");
    ASSERT_OK(validateBSON(x));
    ASSERT_NOT_OK(validateBSON(x.objdata(), x.objsize() - 1));
    // Check if previous byte looks like EOO
    char badCopy[16384];
    memcpy(badCopy, x.objdata(), x.objsize() - 1);
    badCopy[x.objsize() - 2] = 0;
    ASSERT_NOT_OK(validateBSON(badCopy, x.objsize() - 1));
}

TEST(BSONValidateExtended, RegexOptions) {
    // Checks that RegEx with invalid options strings (either an unknown flag or not in alphabetical
    // order) throws a warning.
    std::pair<Status, Status> stats{Status::OK(), Status::OK()};
    auto fullyValidate = [&](BSONObj obj) {
        return std::pair{validateBSON(obj.objdata(), obj.objsize(), BSONValidateMode::kExtended),
                         validateBSON(obj.objdata(), obj.objsize(), BSONValidateMode::kFull)};
    };
    BSONObj obj = BSON("a" << BSONRegEx("a*.conn", "ilmsux"));
    stats = fullyValidate(obj);
    ASSERT_OK(stats.first);
    ASSERT_OK(stats.second);

    obj = BSON("a" << BSONRegEx("a*.conn", "ilmxus"));
    stats = fullyValidate(obj);
    ASSERT_EQ(stats.first, ErrorCodes::NonConformantBSON);
    ASSERT_EQ(stats.second, ErrorCodes::NonConformantBSON);

    obj = BSON("a" << BSONRegEx("a*.conn", "ikl"));
    stats = fullyValidate(obj);
    ASSERT_EQ(stats.first, ErrorCodes::NonConformantBSON);
    ASSERT_EQ(stats.second, ErrorCodes::NonConformantBSON);

    obj = BSON("a" << BSONRegEx("a*.conn", "ilmz"));
    stats = fullyValidate(obj);
    ASSERT_EQ(stats.first, ErrorCodes::NonConformantBSON);
    ASSERT_EQ(stats.second, ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateExtended, UUIDLength) {
    // Checks that an invalid UUID length (!= 16 bytes) throws a warning.
    std::pair<Status, Status> stats{Status::OK(), Status::OK()};
    auto fullyValidate = [&](BSONObj obj) {
        return std::pair{validateBSON(obj, BSONValidateMode::kExtended),
                         validateBSON(obj, BSONValidateMode::kFull)};
    };
    BSONObj x = BSON("u" << BSONBinData("de", 2, BinDataType::newUUID));
    stats = fullyValidate(x);
    ASSERT_EQ(stats.first.code(), ErrorCodes::NonConformantBSON);
    ASSERT_EQ(stats.second.code(), ErrorCodes::NonConformantBSON);
    x = BSON("u" << BSONBinData("aaaaaaaaaaaaaaaaaaaaaa", 22, BinDataType::newUUID));
    stats = fullyValidate(x);
    ASSERT_EQ(stats.first.code(), ErrorCodes::NonConformantBSON);
    ASSERT_EQ(stats.second.code(), ErrorCodes::NonConformantBSON);

    // Checks that a valid UUID does not throw any warnings.
    x = BSON("u" << BSONBinData("abcdabcdabcdabcd", 16, BinDataType::newUUID));
    stats = fullyValidate(x);
    ASSERT_OK(stats.first);
    ASSERT_OK(stats.second);
}

TEST(BSONValidateExtended, DeprecatedTypes) {
    BSONObj obj = BSON("a" << BSONUndefined);
    Status status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);

    obj = BSON("b" << BSONDBRef("db", OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);

    obj = BSON("c" << BSONSymbol("symbol"));
    status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);

    obj = BSON("d" << BSONCodeWScope("(function(){})();", BSON("a" << 1)));
    status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);

    obj = BSON("e" << BSONBinData("", 0, ByteArrayDeprecated));
    status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);

    obj = BSON("f" << BSONBinData("", 0, bdtUUID));
    status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateExtended, DuplicateFieldNames) {
    std::pair<Status, Status> stats{Status::OK(), Status::OK()};
    auto fullyValidate = [&](BSONObj obj) {
        return std::pair{validateBSON(obj.objdata(), obj.objsize(), BSONValidateMode::kExtended),
                         validateBSON(obj.objdata(), obj.objsize(), BSONValidateMode::kFull)};
    };
    BSONObj x = BSON("a" << 1 << "b" << 2);
    stats = fullyValidate(x);
    ASSERT_OK(stats.first);
    ASSERT_OK(stats.second);

    x = BSON("a" << 1 << "b" << 1 << "a" << 3);
    stats = fullyValidate(x);
    ASSERT_OK(stats.first);
    ASSERT_EQ(stats.second, ErrorCodes::NonConformantBSON);

    x = BSON("a" << 1 << "b" << BSON("a" << 1 << "b" << BSON("a" << 1) << "a" << 3) << "c" << 3);
    stats = fullyValidate(x);
    ASSERT_OK(stats.first);
    ASSERT_EQ(stats.second, ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateExtended, BSONEncryptedValue) {
    BSONObj obj;
    Status status = Status::OK();
    FleBlobHeader blob;
    BSONBinData fle;

    {
        // Successful FLE2 Unindexed Encrypted Value
        memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
        blob.originalBsonType = BSONType::String;
        blob.fleBlobSubtype =
            static_cast<int8_t>(EncryptedBinDataType::kFLE2UnindexedEncryptedValue);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, BSONValidateMode::kExtended);
        ASSERT_OK(status);
    }

    {
        // Successful FLE2 Equality Indexed Value
        memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
        blob.originalBsonType = BSONType::String;
        blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValue);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, BSONValidateMode::kExtended);
        ASSERT_OK(status);
    }

    {
        // Successful FLE2 Range Indexed Value
        memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
        blob.originalBsonType = BSONType::NumberInt;
        blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kFLE2RangeIndexedValue);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, BSONValidateMode::kExtended);
        ASSERT_OK(status);
    }

    {
        // Successful FLE2 Equality Indexed Value V2
        memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
        blob.originalBsonType = BSONType::String;
        blob.fleBlobSubtype =
            static_cast<int8_t>(EncryptedBinDataType::kFLE2EqualityIndexedValueV2);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, BSONValidateMode::kExtended);
        ASSERT_OK(status);
    }

    {
        // Successful FLE2 Range Indexed Value V2
        memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
        blob.originalBsonType = BSONType::NumberInt;
        blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kFLE2RangeIndexedValueV2);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, BSONValidateMode::kExtended);
        ASSERT_OK(status);
    }

    {
        // Successful FLE2 Unindexed Encrypted Value V2
        memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
        blob.originalBsonType = BSONType::String;
        blob.fleBlobSubtype =
            static_cast<int8_t>(EncryptedBinDataType::kFLE2UnindexedEncryptedValueV2);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, BSONValidateMode::kExtended);
        ASSERT_OK(status);
    }

    {
        // Empty Encrypted BSON Value.
        auto emptyBinData = "";
        obj = BSON("a" << BSONBinData(emptyBinData, 0, Encrypt));
        status = validateBSON(obj, mongo::BSONValidateMode::kExtended);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
        status = validateBSON(obj, mongo::BSONValidateMode::kFull);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    }

    {
        // Encrypted BSON value subtype not supposed to persist.
        blob.originalBsonType = BSONType::String;
        blob.fleBlobSubtype = static_cast<int8_t>(EncryptedBinDataType::kFLE2Placeholder);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, mongo::BSONValidateMode::kExtended);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
        status = validateBSON(obj, mongo::BSONValidateMode::kFull);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    }

    {
        // Short Encrypted BSON Value.
        blob.originalBsonType = BSONType::String;
        blob.fleBlobSubtype =
            static_cast<int8_t>(EncryptedBinDataType::kFLE2UnindexedEncryptedValue);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader) - 1, BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, mongo::BSONValidateMode::kExtended);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
        status = validateBSON(obj, mongo::BSONValidateMode::kFull);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    }

    {
        // Unsupported original BSON subtype.
        blob.originalBsonType = BSONType::MaxKey;
        blob.fleBlobSubtype =
            static_cast<int8_t>(EncryptedBinDataType::kFLE2UnindexedEncryptedValue);
        fle = BSONBinData(
            reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
        obj = BSON("a" << fle);
        status = validateBSON(obj, mongo::BSONValidateMode::kExtended);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
        status = validateBSON(obj, mongo::BSONValidateMode::kFull);
        ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    }
}

TEST(BSONValidateExtended, UnknownBinDataType) {
    BSONObj obj = BSON("unknownBinData" << BSONBinData("", 0, static_cast<BinDataType>(42)));

    Status status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnInBSON) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    cb.append(BSON("a" << 1).getField("a"));
    cb.append(BSON("a" << 2).getField("a"));
    cb.append(BSON("a" << 1).getField("a"));
    BSONBinData columnData = cb.finalize();
    BSONObj obj = BSON("a" << columnData);
    Status status = validateBSON(obj, BSONValidateMode::kDefault);
    ASSERT_OK(status);
    status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_OK(status);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_OK(status);

    // Change one important byte.
    ((char*)columnData.data)[0] = '0';
    obj = BSON("a" << columnData);
    status = validateBSON(obj, BSONValidateMode::kDefault);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kExtended);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
    status = validateBSON(obj, BSONValidateMode::kFull);
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnMissingEOO) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    // Remove final EOO
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length - 1).code(),
              ErrorCodes::InvalidBSON);
    // Remove final EOO and 0 previous byte (check no overflow)
    ((char*)columnData.data)[columnData.length - 2] = 0;
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length - 1).code(),
              ErrorCodes::InvalidBSON);
}

TEST(BSONValidateColumn, BSONColumnNoOverflowMissingAllEOOInColumn) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONBinData columnData = cb.finalize();
    for (int i = 0; i < columnData.length; ++i)
        if (((char*)columnData.data)[i] == 0)
            ((char*)columnData.data)[i] = 1;
    BSONObj obj = BSON("a" << columnData);
    ASSERT_EQ(validateBSON(obj, BSONValidateMode::kDefault).code(), ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnTrailingGarbage) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
    char badData[4096];
    memcpy(badData, columnData.data, columnData.length);
    badData[columnData.length] = 1;

    ASSERT_EQ(validateBSONColumn(badData, columnData.length + 1).code(),
              ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnNoOverflowBadContent) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    // Remove all string null terminators, expect failure but not overflow
    for (int i = 0; i < columnData.length; ++i)
        if (((char*)columnData.data)[i] == 0)
            ((char*)columnData.data)[i] = 1;
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length).code(),
              ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnNoOverflowMissingFieldname) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    ASSERT_EQ(validateBSONColumn((char*)columnData.data, 6 /* start of "a" */).code(),
              ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnNoOverflowBadFieldname) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    for (int i = 6 /* start of "a" string */; i < columnData.length; ++i)
        if (((char*)columnData.data)[i] == 0)
            ((char*)columnData.data)[i] = 1;
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length).code(),
              ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnNoOverflowBadLiteral) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    // Remove all string null terminators after string start, expect failure but not overflow
    for (int i = 6 /* start of "deadbeef" string */; i < columnData.length; ++i)
        if (((char*)columnData.data)[i] == 0)
            ((char*)columnData.data)[i] = 1;
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length).code(),
              ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnInterleavedObjectPasses) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONObj subObj1 = BSON("b"
                           << "inside");
    cb.append(subObj1);
    BSONObj subObj2 = BSON("b"
                           << "outside");
    cb.append(subObj2);
    BSONObj subObj3 = BSON("b"
                           << "gone");
    cb.append(subObj3);
    cb.append(BSON("c"
                   << "foobar")
                  .getField("c"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

TEST(BSONValidateColumn, BSONColumnInterleavedArrayPasses) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));

    BSONArrayBuilder array;
    for (int i = 0; i < 10; i++) {
        array.append(i);
    }
    array.done();
    cb.append(array.arr());
    cb.append(BSON("c"
                   << "foobar")
                  .getField("c"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

TEST(BSONValidateColumn, BSONColumnInterleavedNestedObjectPasses) {
    BSONColumnBuilder cb;
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONObj subObj1 = BSON("d"
                           << "inside");
    BSONObj subObj2 = BSON("c" << subObj1);
    BSONObj subObj3 = BSON("b" << subObj2);
    cb.append(subObj3);
    cb.append(BSON("c"
                   << "foobar")
                  .getField("c"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

TEST(BSONValidateColumn, BSONColumnInterleavedEmptyObjectPasses) {
    BSONColumnBuilder cb;
    BSONObj subObj1;
    cb.append(subObj1);
    cb.append(BSON("c"
                   << "foobar")
                  .getField("c"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

TEST(BSONValidateColumn, BSONColumnNoOverflowBlocksShort) {
    BSONColumnBuilder cb;
    for (int i = 0; i < 100; ++i)
        cb.append(BSON("a" << i).getField("a"));

    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
    /* Remove EOO and one block */
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length - 1 - 8).code(),
              ErrorCodes::NonConformantBSON);
}

TEST(BSONValidateColumn, BSONColumnBadExtendedSelector) {
    BSONColumnBuilder cb;
    for (int i = 0; i < 100; ++i)
        cb.append(BSON("a" << i).getField("a"));

    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
    /* Change extended selector on a 7 selector to 14 */
    uint64_t block = ConstDataView((char*)columnData.data + 31) /* first 7 selector */
                         .read<LittleEndian<uint64_t>>();
    ASSERT_EQ(7, block & 15);  // Check that we found a 7 selector
    block = (14 << 4)          /* 14 extended selector */
        + 7                    /* original selector */
        + ((block >> 8) << 8); /* original blocks */
    memcpy((char*)columnData.data + 31, &block, sizeof(block));
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

}  // namespace

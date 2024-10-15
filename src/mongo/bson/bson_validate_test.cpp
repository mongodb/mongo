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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/base/data_view.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/bsoncolumn_util.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/db/jsobj.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using std::endl;
using std::unique_ptr;

void appendInvalidStringElement(const char* fieldName, BufBuilder* bb) {
    // like a BSONObj string, but without a NUL terminator.
    bb->appendChar(String);
    bb->appendCStr(fieldName);
    bb->appendNum(4);
    bb->appendStrBytes("asdf");  // Missing required final NUL.
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
            ASSERT_OK(validateBSON(o.objdata(), o.objsize()));
        } else {
            ASSERT_NOT_OK(validateBSON(o.objdata(), o.objsize()));
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
            ASSERT_OK(validateBSON(mine.objdata(), mine.objsize()));
        } else {
            ASSERT_NOT_OK(validateBSON(mine.objdata(), mine.objsize()));
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
        validateBSON(fuzzed.objdata(), fuzzed.objsize()).isOK();
    }
}

TEST(BSONValidateFast, Empty) {
    BSONObj x;
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, RegEx) {
    BSONObjBuilder b;
    b.appendRegex("foo", "i");
    BSONObj x = b.obj();
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, Simple0) {
    BSONObj x;
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));

    x = BSON("foo" << 17 << "bar"
                   << "eliot");
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
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
        ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
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
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, NestedObject) {
    BSONObj x = BSON("a" << 1 << "b"
                         << BSON("c" << 2 << "d" << BSONArrayBuilder().obj() << "e"
                                     << BSON_ARRAY("1" << 2 << 3)));
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
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
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, ErrorWithId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    ob.append("_id", 1);
    appendInvalidStringElement("not_id", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x.objdata(), x.objsize());
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
    const Status status = validateBSON(x.objdata(), x.objsize());
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
    const Status status = validateBSON(x.objdata(), x.objsize());
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
    const Status status = validateBSON(x.objdata(), x.objsize());
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
    const Status status = validateBSON(x.objdata(), x.objsize());
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
    const Status status = validateBSON(x.objdata(), x.objsize());
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(),
                  "Not null terminated string in element with field name 'nested.2.invalid' "
                  "in object with _id: 1");
}

TEST(BSONValidateFast, StringHasSomething) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    bb.appendChar(String);
    bb.appendCStr("x");
    bb.appendNum(0);
    const BSONObj x = ob.done();
    ASSERT_EQUALS(5        // overhead
                      + 1  // type
                      + 2  // name
                      + 4  // size
                  ,
                  x.objsize());
    ASSERT_NOT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, BoolValuesAreValidated) {
    BSONObjBuilder bob;
    bob.append("x", false);
    const BSONObj obj = bob.done();
    ASSERT_OK(validateBSON(obj.objdata(), obj.objsize()));
    const BSONElement x = obj["x"];
    // Legal, because we know that the BufBuilder gave
    // us back some heap memory, which isn't oringinally const.
    auto writable = const_cast<char*>(x.value());
    for (int val = std::numeric_limits<char>::min();
         val != (int(std::numeric_limits<char>::max()) + 1);
         ++val) {
        *writable = static_cast<char>(val);
        if ((val == 0) || (val == 1)) {
            ASSERT_OK(validateBSON(obj.objdata(), obj.objsize()));
        } else {
            ASSERT_NOT_OK(validateBSON(obj.objdata(), obj.objsize()));
        }
    }
}

TEST(BSONValidateFast, InvalidType) {
    // Encode an invalid BSON Object with an invalid type, x90.
    const char* buffer = "\x0c\x00\x00\x00\x90\x41\x00\x10\x00\x00\x00\x00";

    // Constructing the object is fine, but validating should fail.
    BSONObj obj(buffer);

    // Validate fails.
    ASSERT_NOT_OK(validateBSON(obj.objdata(), obj.objsize()));

    // Make sure the binary buffer above indeed has the invalid type.
    ASSERT_THROWS_CODE(obj.woCompare(BSON("A" << 1)), DBException, 10320);
}

TEST(BSONValidateFast, ValidCodeWScope) {
    BSONObj obj = BSON("a" << BSONCodeWScope("code", BSON("c" << BSONObj())));
    ASSERT_OK(validateBSON(obj.objdata(), obj.objsize()));
    obj = BSON("a" << BSONCodeWScope("code", BSON("c" << BSONArray() << "d" << BSONArray())));
    ASSERT_OK(validateBSON(obj.objdata(), obj.objsize()));
}

BSONObj nest(int nesting) {
    return nesting < 1 ? BSON("i" << nesting) : BSON("i" << nesting << "o" << nest(nesting - 1));
}

TEST(BSONValidateFast, MaxNestingDepth) {
    BSONObj maxNesting = nest(BSONDepth::getMaxAllowableDepth());
    ASSERT_OK(validateBSON(maxNesting.objdata(), maxNesting.objsize()));

    BSONObj tooDeepNesting = nest(BSONDepth::getMaxAllowableDepth() + 1);
    Status status = validateBSON(tooDeepNesting.objdata(), tooDeepNesting.objsize());
    ASSERT_EQ(status.code(), ErrorCodes::Overflow);
}

TEST(BSONValidateFast, ErrorTooShort) {
    BSONObj x;
    x = BSON("foo" << 17 << "bar"
                   << "eliot");
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
    ASSERT_NOT_OK(validateBSON(x.objdata(), x.objsize() - 1));
    // Check if previous byte looks like EOO
    char badCopy[16384];
    memcpy(badCopy, x.objdata(), x.objsize() - 1);
    badCopy[x.objsize() - 2] = 0;
    ASSERT_NOT_OK(validateBSON(badCopy, x.objsize() - 1));
}

class BSONValidateColumn : public unittest::Test {
public:
    BSONElement objToElement(BSONObj val) {
        BSONObjBuilder ob;
        ob.append("0"_sd, val);
        _elementMemory.emplace_front(ob.obj());
        return _elementMemory.front().firstElement();
    }

private:
    std::forward_list<BSONObj> _elementMemory;
};

TEST_F(BSONValidateColumn, BSONColumnInBSON) {
    BSONColumnBuilder cb("");
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    cb.append(BSON("a" << 1).getField("a"));
    cb.append(BSON("a" << 2).getField("a"));
    cb.append(BSON("a" << 1).getField("a"));
    BSONBinData columnData = cb.finalize();
    BSONObj obj = BSON("a" << columnData);
    Status status = validateBSON(obj.objdata(), obj.objsize());
    ASSERT_OK(status);

    // Change one important byte.
    ((char*)columnData.data)[0] = '0';
    obj = BSON("a" << columnData);
    status = validateBSON(obj.objdata(), obj.objsize());
    ASSERT_EQ(status.code(), ErrorCodes::NonConformantBSON);
}

TEST_F(BSONValidateColumn, BSONColumnMissingEOO) {
    BSONColumnBuilder cb("");

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

TEST(BSONValidateColumn, BSONColumnFieldnameNotEmpty) {
    BSONColumnBuilder cb("");
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    char buf[1024];
    buf[0] = ((const char*)columnData.data)[0];
    buf[1] = 'f';
    buf[2] = 'o';
    buf[3] = 'o';
    memcpy(buf + 4, ((const char*)columnData.data) + 1, columnData.length - 1);

    ASSERT_EQ(validateBSONColumn(buf, columnData.length + 3).code(), ErrorCodes::NonConformantBSON);
}

TEST_F(BSONValidateColumn, BSONColumnNoOverflowMissingAllEOOInColumn) {
    BSONColumnBuilder cb("");
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONBinData columnData = cb.finalize();
    for (int i = 0; i < columnData.length; ++i)
        if (((char*)columnData.data)[i] == 0)
            ((char*)columnData.data)[i] = 1;
    BSONObj obj = BSON("a" << columnData);
    ASSERT_EQ(validateBSON(obj.objdata(), obj.objsize()).code(), ErrorCodes::NonConformantBSON);
}

TEST_F(BSONValidateColumn, BSONColumnTrailingGarbage) {
    BSONColumnBuilder cb("");
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

TEST_F(BSONValidateColumn, BSONColumnNoOverflowBadContent) {
    BSONColumnBuilder cb("");
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

TEST_F(BSONValidateColumn, BSONColumnNoOverflowMissingFieldname) {
    BSONColumnBuilder cb("");
    cb.append(objToElement(BSON("a"
                                << "deadbeef")));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    ASSERT_EQ(validateBSONColumn((char*)columnData.data, 6 /* start of "a" */).code(),
              ErrorCodes::NonConformantBSON);
}

TEST_F(BSONValidateColumn, BSONColumnNoOverflowBadFieldname) {
    BSONColumnBuilder cb("");
    cb.append(objToElement(BSON("a"
                                << "deadbeef")));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));

    for (int i = 6 /* start of "a" string */; i < columnData.length; ++i)
        if (((char*)columnData.data)[i] == 0)
            ((char*)columnData.data)[i] = 1;
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length).code(),
              ErrorCodes::NonConformantBSON);
}

TEST_F(BSONValidateColumn, BSONColumnNoOverflowBadLiteral) {
    BSONColumnBuilder cb("");
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

TEST_F(BSONValidateColumn, BSONColumnInterleavedObjectPasses) {
    BSONColumnBuilder cb("");
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONObj subObj1 = BSON("b"
                           << "inside");
    cb.append(objToElement(subObj1));
    BSONObj subObj2 = BSON("b"
                           << "outside");
    cb.append(objToElement(subObj2));
    BSONObj subObj3 = BSON("b"
                           << "gone");
    cb.append(objToElement(subObj3));
    cb.append(BSON("c"
                   << "foobar")
                  .getField("c"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

TEST_F(BSONValidateColumn, BSONColumnInterleavedNestedObjectPasses) {
    BSONColumnBuilder cb("");
    cb.append(BSON("a"
                   << "deadbeef")
                  .getField("a"));
    BSONObj subObj1 = BSON("d"
                           << "inside");
    BSONObj subObj2 = BSON("c" << subObj1);
    BSONObj subObj3 = BSON("b" << subObj2);
    cb.append(objToElement(subObj3));
    cb.append(BSON("c"
                   << "foobar")
                  .getField("c"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

TEST_F(BSONValidateColumn, BSONColumnInterleavedEmptyObjectPasses) {
    BSONColumnBuilder cb("");
    BSONObj subObj1;
    cb.append(objToElement(subObj1));
    cb.append(BSON("c"
                   << "foobar")
                  .getField("c"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

TEST(BSONValidateColumn, BSONColumnInterleavedNestedInterleaved) {
    BufBuilder buffer;
    BSONObj ref = BSON("c" << 1);

    buffer.appendChar(bsoncolumn::kInterleavedStartControlByteLegacy);
    buffer.appendBuf(ref.objdata(), ref.objsize());
    buffer.appendChar(bsoncolumn::kInterleavedStartControlByteLegacy);
    buffer.appendBuf(ref.objdata(), ref.objsize());
    buffer.appendChar(0);
    buffer.appendChar(0);

    ASSERT_EQ(validateBSONColumn(buffer.buf(), buffer.len()), ErrorCodes::NonConformantBSON);
}

TEST_F(BSONValidateColumn, BSONColumnNoOverflowBlocksShort) {
    BSONColumnBuilder cb("");
    for (int i = 0; i < 100; ++i)
        cb.append(BSON("a" << i).getField("a"));

    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
    /* Remove EOO and one block */
    ASSERT_EQ(validateBSONColumn((char*)columnData.data, columnData.length - 1 - 8).code(),
              ErrorCodes::NonConformantBSON);
}

TEST_F(BSONValidateColumn, BSONColumnBadExtendedSelector) {
    BSONColumnBuilder cb("");
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

TEST(BSONValidateColumn, BSONColumnWithCodeWScope) {
    BSONObj obj = BSON("a" << BSONCodeWScope("code", BSON("c" << 1)));
    BSONColumnBuilder cb("");
    cb.append(obj.getField("a"));
    BSONBinData columnData = cb.finalize();
    ASSERT_OK(validateBSONColumn((char*)columnData.data, columnData.length));
}

}  // namespace

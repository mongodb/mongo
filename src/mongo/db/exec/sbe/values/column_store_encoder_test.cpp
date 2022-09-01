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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/column_store_encoder.h"
#include "mongo/db/index/column_cell.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/util/md5.hpp"

namespace mongo::sbe {
TEST(SBEColumnStoreEncoder, EncodeTest) {
    //
    // Manually construct some basic examples of columnar-encoded data.
    //
    BufBuilder columnStoreCell;

    // Write 0 as an int32.
    columnStoreCell.appendChar(ColumnStore::Bytes::TinyNum::kTinyIntZero);

    // Write 5000 as an int634 (represented in 2 bytes).
    columnStoreCell.appendChar(ColumnStore::Bytes::kLong2);
    columnStoreCell.appendNum(short{5000});  // Little-endian write.

    // Write a strange message.
    const char* message = "Help, I'm trapped in a columnar index.";
    columnStoreCell.appendChar(ColumnStore::Bytes::kStringSizeMin + strlen(message));
    columnStoreCell.appendStr(message, false /* includeEndingNull */);

    Decimal128 decimalNum{"123.456"};
    columnStoreCell.appendChar(ColumnStore::Bytes::kDecimal128);
    columnStoreCell.appendNum(decimalNum);  // Little-endian write.

    auto cellView = SplitCellView::parse(
        StringData{columnStoreCell.buf(), static_cast<size_t>(columnStoreCell.len())});

    value::ColumnStoreEncoder encoder;
    auto cellCursor = cellView.subcellValuesGenerator(&encoder);

    {
        auto cellValue = cellCursor.nextValue();
        ASSERT(cellValue);

        auto [tag, val] = *cellValue;
        ASSERT_EQ(tag, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(val), 0);
    }

    {
        auto cellValue = cellCursor.nextValue();
        ASSERT(cellValue);

        auto [tag, val] = *cellValue;
        ASSERT_EQ(tag, value::TypeTags::NumberInt64);
        ASSERT_EQ(value::bitcastTo<int64_t>(val), 5000);
    }

    {
        auto cellValue = cellCursor.nextValue();
        ASSERT(cellValue);

        auto [tag, val] = *cellValue;
        ASSERT_EQ(tag, value::TypeTags::StringBig);
        ASSERT_EQ(StringData{message}, value::getStringView(tag, val));
    }

    {
        auto cellValue = cellCursor.nextValue();
        ASSERT(cellValue);

        auto [tag, val] = *cellValue;
        ASSERT_EQ(tag, value::TypeTags::NumberDecimal);
        ASSERT_EQ(decimalNum, value::bitcastTo<Decimal128>(val));
    }

    {
        auto cellValue = cellCursor.nextValue();
        ASSERT(!cellValue);
    }
}

/**
 * Exhaustively test columnar value conversion. The test begins with reference BSON values, which
 * get converted to
 *   1. columnar format, via the 'column_keygen::appendElementToCell' function;
 *   2. SBE value pairs, via a 'SplitCellView' iterator; and finally
 *   3. BSON, via the 'bson::appendValueToBsonObj' function.
 * The test expects the final BSON to exactly match the original BSON. The 'SplitCellView'
 * conversion step is the primary target for this test.
 */
TEST(SBEColumnStoreEncoder, RoundTripConversionThroughSplitCellView) {
    //
    // Set up the reference data.
    //
    auto uuid = []() {
        auto parseResult = UUID::parse("abcdefab-cdef-abcd-efab-cdefabcdefab");
        ASSERT_OK(parseResult);
        return parseResult.getValue();
    }();

    BSONObjBuilder referenceBoB;
    referenceBoB.appendNull("Null");
    referenceBoB.appendMinKey("MinKey");
    referenceBoB.appendMaxKey("MaxKey");
    referenceBoB.append("false", false);
    referenceBoB.append("true", true);
    referenceBoB.append("empty object", StringMap<long long>());
    referenceBoB.appendArray("empty array", BSONArray());
    referenceBoB.append("OID", OID("aaaaaaaaaaaaaaaaaaaaaaaa"));
    referenceBoB.append("UUID", BSONBinData(uuid.data().data(), UUID::kNumBytes, newUUID));
    referenceBoB.append("decimal", Decimal128("1.337e-10"));
    referenceBoB.append("double", double(0.125));
    referenceBoB.append("double that fits in 32-bit float", double(0.25));
    referenceBoB.append("double that fits in int8_t", double(2));
    referenceBoB.append("double with cents that fit in 1 byte", double(0.33));
    referenceBoB.append("double with cents that fits in 2 bytes", double(10.33));
    referenceBoB.append("double with cents that fits in 4 bytes", double(10000.33));
    referenceBoB.append("infinity", std::numeric_limits<double>::infinity());
    referenceBoB.append("other infinity", -std::numeric_limits<double>::infinity());
    referenceBoB.append("NaN", std::numeric_limits<double>::quiet_NaN());
    referenceBoB.append("int that fits in 1 byte", int32_t(-100));
    referenceBoB.append("int that fits in 2 bytes", int32_t(-9020));
    referenceBoB.append("int that fits in 4 bytes", int32_t(-591751049));
    referenceBoB.append("long that fits in 1 byte", int64_t(-100));
    referenceBoB.append("long that fits in 2 bytes", int64_t(-9020));
    referenceBoB.append("long that fits in 4 bytes", int64_t(-591751049));
    referenceBoB.append("long that fits in 8 bytes", int64_t(-2541551405711093505));
    referenceBoB.append("tiny int", int32_t(-2));
    referenceBoB.append("tiny long", int64_t(-2));
    referenceBoB.append("string", "|#|$============$|#|  <-- Column that has fallen on its side.");
    referenceBoB.append("small string", "I <-- Short column");
    referenceBoB.append("tiny string", "I I");
    referenceBoB.append("string with embedded null",
                        StringData("|#|$=====\0======$|#|", 20 /* Manually-counted length */));
    referenceBoB.append(
        "large string",
        "Readers of 'Index Weekly'    Some  of  New York City's    into  documents  but ins-"
        "are  sure  to  be already    most  eligible  data sets    tead into chic 'exploded'"
        "abreast  of  the  hottest    have   been   spotted  on    columnar format. (cont."
        "new trend in indexing.       Broadway   organized  not    page 11)"
        "    --Gossip column");
    auto referenceBson = referenceBoB.done();

    //
    // Convert the values in 'referenceBson' to columnar format.
    //
    BufBuilder cellBuffer;
    for (auto&& element : referenceBson) {
        column_keygen::appendElementToCell(element, &cellBuffer);
    }

    //
    // As an auxillary check, we use a checksum to verify that the columnar version of the reference
    // data has the exact same bytes every time. The columnar format is stored on disk, so it should
    // never change.
    //
    auto cellDigest = [&]() {
        md5_state_t digestState;
        md5_init(&digestState);
        md5_append(
            &digestState, reinterpret_cast<const md5_byte_t*>(cellBuffer.buf()), cellBuffer.len());

        md5digest digestBytes;
        md5_finish(&digestState, digestBytes);

        return digestToString(digestBytes);
    }();
    ASSERT_EQ("90673664965d8597ffb717cc7fa5854d", cellDigest);

    //
    // Create a 'SplitCellView' that will convert all of the columnar data into SBE value pairs.
    //
    // We iterate through the reference values via two cursors: 'referenceIt' for the BSONElement
    // reference values and 'cellCursor' for the same values that have been translated to SBE value
    // representations.
    //
    //
    auto cellView =
        SplitCellView::parse(StringData{cellBuffer.buf(), static_cast<size_t>(cellBuffer.len())});

    value::ColumnStoreEncoder encoder;
    auto cellCursor = cellView.subcellValuesGenerator(&encoder);

    auto referenceIt = referenceBson.begin();
    while (auto&& cursorResult = cellCursor.nextValue()) {
        BSONElement referenceElement = *(referenceIt++);
        ASSERT(!referenceElement.eoo());

        // This [tag, val] pair stores the value that was translated from columnar format.
        auto [tag, val] = *cursorResult;
        ASSERT_NE(value::TypeTags::Nothing, tag);

        // Convert the translated columnar format back to BSON.
        BSONObjBuilder builder;
        bson::appendValueToBsonObj(builder, referenceElement.fieldName(), tag, val);
        BSONObj roundTripBson = builder.done();

        // The result should be bit-for-bit the same as the original reference BSON.
        ASSERT(referenceElement.binaryEqualValues(roundTripBson.firstElement()))
            << "Expected: " << referenceElement << ", Actual: " << roundTripBson.firstElement();
    }
}

/**
 * A cell encoded for a columnar index can contain embedded BSON elements. We construct all possible
 * types of BSON elements and append them to a test cell.
 */
TEST(SBEColumnStoreEncoder, ColumnsWithEmbeddedBSONElements) {
    //
    // The test data consists of a reference BSON object that has each test element. Each of these
    // elements has a corresponding check in the 'testComparisons' list. The comparisons are stored
    // this way so that each check appears next to the BSONElement it will be testing.
    //
    BSONObjBuilder referenceBoB;
    std::vector<std::function<void(value::TypeTags tag, value::Value val)>> testComparisons;

    referenceBoB.append("", double{1234.5});
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::NumberDouble, tag);
        ASSERT_EQ(1234.5, value::bitcastTo<double>(val));
    });

    referenceBoB.append("", "String as embedded BSON");
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonString, tag);
        ASSERT_EQ("String as embedded BSON"_sd, getStringView(tag, val));
    });

    referenceBoB.append("", BSON("a" << 1 << "b" << 2));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonObject, tag);
        ASSERT_BSONOBJ_EQ(BSON("a" << 1 << "b" << 2), BSONObj(value::getRawPointerView(val)));
    });

    referenceBoB.append("", BSON_ARRAY(1 << 2 << 3 << 4));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonArray, tag);
        ASSERT_BSONOBJ_EQ(BSON_ARRAY(1 << 2 << 3 << 4), BSONObj(value::getRawPointerView(val)));
    });

    referenceBoB.append("", BSONBinData("data", 5 /* Manually counted length */, BinDataGeneral));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonBinData, tag);
        ASSERT_EQ(5, value::getBSONBinDataSize(tag, val));
        ASSERT_EQ(BinDataGeneral, value::getBSONBinDataSubtype(tag, val));
        ASSERT_EQ(StringData("data"),
                  StringData(reinterpret_cast<const char*>(value::getBSONBinData(tag, val))));
    });

    referenceBoB.append("", OID("aaaaaaaaaaaaaaaaaaaaaaaa"));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonObjectId, tag);
        value::ObjectIdType expected{
            0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
        ASSERT(expected == *value::getObjectIdView(val));
    });

    referenceBoB.append("", false);
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::Boolean, tag);
        ASSERT(!value::bitcastTo<bool>(val));
    });

    referenceBoB.append("", Date_t::fromMillisSinceEpoch(1000));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::Date, tag);
        ASSERT_EQ(1000, value::bitcastTo<int64_t>(val));
    });

    referenceBoB.appendNull("");
    testComparisons.push_back([](auto tag, auto val) { ASSERT_EQ(value::TypeTags::Null, tag); });

    referenceBoB.append("", BSONRegEx(".*", "g"));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonRegex, tag);
        ASSERT_EQ(".*", value::getBsonRegexView(val).pattern);
        ASSERT_EQ("g", value::getBsonRegexView(val).flags);
    });

    referenceBoB.append("", BSONDBRef("ns", OID("aaaaaaaaaaaaaaaaaaaaaaaa")));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonDBPointer, tag);
        ASSERT_EQ("ns", value::getBsonDBPointerView(val).ns);
        ASSERT_EQ("aaaaaaaaaaaaaaaaaaaaaaaa",
                  OID::from(value::getBsonDBPointerView(val).id).toString());
    });

    referenceBoB.append("", BSONCode("foo();"));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonJavascript, tag);
        ASSERT_EQ("foo();", value::getBsonJavascriptView(val));
    });

    referenceBoB.append("", BSONCodeWScope("foo();", BSON("bar" << 1)));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::bsonCodeWScope, tag);
        ASSERT_EQ("foo();", value::getBsonCodeWScopeView(val).code);
        ASSERT_BSONOBJ_EQ(BSON("bar" << 1), BSONObj(value::getBsonCodeWScopeView(val).scope));
    });

    referenceBoB.append("", int32_t(2022));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::NumberInt32, tag);
        ASSERT_EQ(2022, value::bitcastTo<int32_t>(val));
    });

    referenceBoB.append("", Timestamp(1001));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::Timestamp, tag);
        ASSERT_EQ(1001, value::bitcastTo<int64_t>(val));
    });

    referenceBoB.append("", int64_t(202220222022));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::NumberInt64, tag);
        ASSERT_EQ(202220222022, value::bitcastTo<int64_t>(val));
    });

    referenceBoB.append("", Decimal128("2022.2022"));
    testComparisons.push_back([](auto tag, auto val) {
        ASSERT_EQ(value::TypeTags::NumberDecimal, tag);
        ASSERT_EQ(Decimal128("2022.2022"), value::bitcastTo<Decimal128>(val));
    });

    auto referenceBson = referenceBoB.done();

    //
    // Copy the test BSONElements into the simulated columnar cell.
    //
    BufBuilder columnStoreCell;
    for (auto&& element : referenceBson) {
        columnStoreCell.appendBuf(element.rawdata(), element.size());
    }

    auto cellView = SplitCellView::parse(
        StringData{columnStoreCell.buf(), static_cast<size_t>(columnStoreCell.len())});

    //
    // Test the 'ColumnStoreEncoder' by using it to iterate through the columnar values and
    // validating the translated outputs their respective comparison functions.
    //
    value::ColumnStoreEncoder encoder;
    auto cellCursor = cellView.subcellValuesGenerator(&encoder);

    for (auto comparison : testComparisons) {
        auto cellValue = cellCursor.nextValue();
        ASSERT(cellValue);

        auto [tag, val] = *cellValue;
        comparison(tag, val);
    }

    ASSERT(!cellCursor.nextValue());
}
}  // namespace mongo::sbe

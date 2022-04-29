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

#include "mongo/db/index/column_cell.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

namespace mongo {
TEST(ColumnCell, AppendElementToCellTest) {
    auto referenceBson =
        BSON_ARRAY(int32_t(0) << int64_t(5000) << "Help, I'm trapped in a columnar index."
                              << Decimal128("123.456"));

    // Initialize a buffer with the expected output of translating 'referenceBson' in the columnar
    // index format.
    auto [expectedCell, expectedCellLength] = []() {
        BufBuilder expectedBuffer;

        // Write 0 as an int32.
        expectedBuffer.appendChar(ColumnStore::Bytes::TinyNum::kTinyIntZero);

        // Write 5000 as an int64 (represented in 2 bytes).
        expectedBuffer.appendChar(ColumnStore::Bytes::kLong2);
        expectedBuffer.appendNum(short(5000));

        // Write a strange message.
        const char* message = "Help, I'm trapped in a columnar index.";
        expectedBuffer.appendChar(ColumnStore::Bytes::kStringSizeMin + strlen(message));
        expectedBuffer.appendStr(message, false /* includeEndingNull */);

        Decimal128 decimalNum("123.456");
        expectedBuffer.appendChar(ColumnStore::Bytes::kDecimal128);
        expectedBuffer.appendNum(decimalNum);

        auto expectedCellLength = expectedBuffer.len();
        return std::make_pair(expectedBuffer.release(), expectedCellLength);
    }();

    // Use the 'appendElementToCell()' function, which is the function being tested, to compute the
    // transformed 'referenceBson' in columnar index format.
    BufBuilder cellBuffer;
    for (auto&& element : referenceBson) {
        column_keygen::appendElementToCell(element, &cellBuffer);
    }

    // Ensure that the output of 'appendElementToCell()' matches the expected output.
    ASSERT_EQ(hexblob::encode(cellBuffer.buf(), cellBuffer.len()),
              hexblob::encode(expectedCell.get(), expectedCellLength));

    // Repeat the test, but this time we manually construct the expected string of bytes instead of
    // building it out of the constansts in the 'ColumnStore::Bytes' class. The column cell format
    // is an on-disk format and should never change.
    ASSERT_EQ(
        "4A3D8813A648656C702C2049276D207472617070656420696E206120636F6C756D6E617220696E6465782E3040"
        "E20100000000000000000000003A30",
        hexblob::encode(cellBuffer.buf(), cellBuffer.len()));
}

namespace {
std::vector<BSONElement> getBsonElements(const BSONArray& obj) {
    std::vector<BSONElement> elementsVector;
    obj.elems(elementsVector);
    return elementsVector;
}
}  // namespace

TEST(ColumnCellAppendElement, WriteEncodedCellWithHasSubPathsTest) {
    // Test document: {a: [null, {b: 1}]}.
    auto referenceBson = BSON_ARRAY(BSONNULL);

    // What the cell would look like with "a" as the path.
    column_keygen::UnencodedCellView unencodedCell = {
        getBsonElements(referenceBson),
        "[|o"_sd,
        false,  // hasDuplicateFields
        true,   // hasSubPaths
        false,  // isSparse
        false,  // hasDoubleNestedArrays
    };

    BufBuilder cellBuffer;
    writeEncodedCell(unencodedCell, &cellBuffer);

    ASSERT_EQ("FDD2205B7C6F", hexblob::encode(cellBuffer.buf(), cellBuffer.len()));
}

TEST(ColumnCellAppendElement, WriteEncodedCellWithHasIsSparseAndHasDoubleNestedArraysTest) {
    // Test document: {a: [[{b: null}], {b: null}, {c: 1}]}
    auto referenceBson = BSON_ARRAY(BSONNULL << BSONNULL);

    // What the cell would look like with "a.b" as the path.
    column_keygen::UnencodedCellView unencodedCell = {
        getBsonElements(referenceBson),
        "[[|]"_sd,
        false,  // hasDuplicateFields
        false,  // hasSubPaths
        true,   // isSparse
        true,   // hasDoubleNestedArrays
    };

    BufBuilder cellBuffer;
    writeEncodedCell(unencodedCell, &cellBuffer);

    ASSERT_EQ("FEFFD320205B5B7C5D", hexblob::encode(cellBuffer.buf(), cellBuffer.len()));
}

TEST(ColumnCellAppendElement, WriteEncodedCellWithDuplicateFieldsTest) {
    // Test document: {a: null, a: null} (Note: INVALID document in context of index storage)
    auto referenceBson = BSON_ARRAY(BSONNULL << BSONNULL);

    // What the invalid cell would look like with "a" as the path.
    column_keygen::UnencodedCellView unencodedCell = {
        getBsonElements(referenceBson),
        "{"_sd,  // This should _not_ get included in the output.
        true,    // hasDuplicateFields
        false,   // hasSubPaths
        false,   // isSparse
        false,   // hasDoubleNestedArrays
    };

    BufBuilder cellBuffer;
    writeEncodedCell(unencodedCell, &cellBuffer);

    ASSERT_EQ("FC2020", hexblob::encode(cellBuffer.buf(), cellBuffer.len()));
}

TEST(ColumnCellAppendElement, WriteEncodedCellWithArrInfoSize1) {
    // This test does not use a realistic cell. A cell with a 100-byte arrayInfo would be great fun
    // but also needlessly complicated for a unit test.
    std::string arrayInfo(100, '{');
    column_keygen::UnencodedCellView unencodedCell = {{}, arrayInfo, false, false, false, false};

    BufBuilder cellBuffer;
    writeEncodedCell(unencodedCell, &cellBuffer);

    ASSERT_EQ(102, cellBuffer.len());
    ASSERT_EQ("ED64", hexblob::encode(cellBuffer.buf(), 2));
    ASSERT_EQ(std::string(&cellBuffer.buf()[2], 100), arrayInfo);
}

TEST(ColumnCellAppendElement, WriteEncodedCellWithArrInfoSize2) {
    // This test does not use a realistic cell. A cell with a 1KB arrayInfo would be delightful but
    // also needlessly complicated for a unit test.
    std::string arrayInfo(1024, '{');
    column_keygen::UnencodedCellView unencodedCell = {{}, arrayInfo, false, false, false, false};

    BufBuilder cellBuffer;
    writeEncodedCell(unencodedCell, &cellBuffer);

    ASSERT_EQ(1027, cellBuffer.len());
    ASSERT_EQ("EE0004", hexblob::encode(cellBuffer.buf(), 3));
    ASSERT_EQ(std::string(&cellBuffer.buf()[3], 1024), arrayInfo);
}

TEST(ColumnCellAppendElement, WriteEncodedCellWithArrInfoSize4) {
    // This test does not use a realistic cell. A cell with a 100KB arrayInfo would be hilarious but
    // also needlessly complicated for a unit test.
    std::string arrayInfo(102400, '{');
    column_keygen::UnencodedCellView unencodedCell = {{}, arrayInfo, false, false, false, false};

    BufBuilder cellBuffer;
    writeEncodedCell(unencodedCell, &cellBuffer);

    ASSERT_EQ(102405, cellBuffer.len());
    ASSERT_EQ("EF00900100", hexblob::encode(cellBuffer.buf(), 5));
    ASSERT_EQ(std::string(&cellBuffer.buf()[5], 102400), arrayInfo);
}
}  // namespace mongo

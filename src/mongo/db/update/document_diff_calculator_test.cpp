/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <functional>

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/json.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {


TEST(DocumentDiffCalculatorTest, SameObjectsNoDiff) {
    auto assertDiffEmpty = [](const BSONObj& doc) {
        auto diffOutput = doc_diff::computeDiff(doc, doc, 5, nullptr);
        ASSERT(diffOutput);
        ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, BSONObj());
    };
    assertDiffEmpty(fromjson("{field1: 1}"));
    assertDiffEmpty(fromjson("{field1: []}"));
    assertDiffEmpty(fromjson("{field1: [{}]}"));
    assertDiffEmpty(fromjson("{field1: [0, 1, 2, 3, 4]}"));
    assertDiffEmpty(fromjson("{field1: null}"));
    assertDiffEmpty(fromjson("{'0': [0]}"));
    assertDiffEmpty(fromjson("{'0': [[{'0': [[{'0': [[]]} ]]}]]}"));
}

TEST(DocumentDiffCalculatorTest, LargeDelta) {
    ASSERT_FALSE(doc_diff::computeDiff(
        fromjson("{a: 1, b: 2, c: 3}"), fromjson("{A: 1, B: 2, C: 3}"), 0, nullptr));

    // Inserting field at the beginning produces a large delta.
    ASSERT_FALSE(doc_diff::computeDiff(
        fromjson("{b: 1, c: 1, d: 1}"), fromjson("{a: 1, b: 1, c: 1, d: 1}"), 0, nullptr));

    // Empty objects.
    ASSERT_FALSE(doc_diff::computeDiff(BSONObj(), BSONObj(), 1, nullptr));

    // Full diff.
    ASSERT_FALSE(doc_diff::computeDiff(fromjson("{b: 1}"), BSONObj(), 0, nullptr));
    ASSERT_FALSE(doc_diff::computeDiff(BSONObj(), fromjson("{b: 1}"), 0, nullptr));
}

TEST(DocumentDiffCalculatorTest, DeleteAndInsertFieldAtTheEnd) {
    auto diffOutput = doc_diff::computeDiff(fromjson("{a: 1, b: 'valueString', c: 3, d: 4}"),
                                            fromjson("{b: 'valueString', c: 3, d: 4, a: 1}"),
                                            15,
                                            nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{i: {a: 1}}"));
}

TEST(DocumentDiffCalculatorTest, DeletesRecordedInAscendingOrderOfFieldNames) {
    auto diffOutput = doc_diff::computeDiff(fromjson("{b: 1, a: 2, c: 3, d: 4, e: 'valueString'}"),
                                            fromjson("{c: 3, d: 4, e: 'valueString'}"),
                                            15,
                                            nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{d: {a: false, b: false}}"));

    // Delete at the end still follow the order.
    diffOutput = doc_diff::computeDiff(
        fromjson("{b: 1, a: 2, c: 'value', d: 'valueString', e: 'valueString', g: 1, f: 1}"),
        fromjson("{c: 'value', d: 'valueString', e: 'valueString'}"),
        15,
        nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff,
                             fromjson("{d: {g: false, f: false, a: false, b: false}}"));
}

TEST(DocumentDiffCalculatorTest, DataTypeChangeRecorded) {
    const auto objWithDoubleValue =
        fromjson("{a: 'valueString', b: 2, c: {subField1: 1, subField2: 3.0}, d: 4}");
    const auto objWithIntValue =
        fromjson("{a: 'valueString', b: 2, c: {subField1: 1, subField2: 3}, d: 4}");
    const auto objWithLongValue =
        fromjson("{a: 'valueString', b: 2, c: {subField1: 1, subField2: NumberLong(3)}, d: 4}");
    auto diffOutput = doc_diff::computeDiff(objWithDoubleValue, objWithIntValue, 15, nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sc: {u: {subField2: 3}} }"));

    diffOutput = doc_diff::computeDiff(objWithIntValue, objWithLongValue, 15, nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sc: {u: {subField2: NumberLong(3)}} }"));

    diffOutput = doc_diff::computeDiff(objWithLongValue, objWithDoubleValue, 15, nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sc: {u: {subField2: 3.0}} }"));
}

TEST(DocumentDiffCalculatorTest, NullAndMissing) {
    auto diffOutput = doc_diff::computeDiff(fromjson("{a: null}"), fromjson("{}"), 15, nullptr);
    ASSERT_FALSE(diffOutput);

    diffOutput =
        doc_diff::computeDiff(fromjson("{a: null, b: undefined, c: null, d: 'someValueStr'}"),
                              fromjson("{a: null, b: undefined, c: undefined, d: 'someValueStr'}"),
                              15,
                              nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{u: {c: undefined}}"));
}

TEST(DocumentDiffCalculatorTest, FieldOrder) {
    auto diffOutput =
        doc_diff::computeDiff(fromjson("{a: 1, b: 2, c: {p: 1, q: 1, s: 1, r: 2}, d: 4}"),
                              fromjson("{a: 1, b: 2, c: {p: 1, q: 1, r: 2, s: 1}, d: 4}"),
                              10,
                              nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sc: {i: {s: 1}} }"));
}

TEST(DocumentDiffCalculatorTest, SimpleArrayPush) {
    auto diffOutput = doc_diff::computeDiff(fromjson("{field1: 'abcd', field2: [1, 2, 3]}"),
                                            fromjson("{field1: 'abcd', field2: [1, 2, 3, 4]}"),
                                            5,
                                            nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sfield2: {a: true, 'u3': 4}}"));
}

TEST(DocumentDiffCalculatorTest, NestedArray) {
    auto diffOutput = doc_diff::computeDiff(fromjson("{field1: 'abcd', field2: [1, 2, 3, [[2]]]}"),
                                            fromjson("{field1: 'abcd', field2: [1, 2, 3, [[4]]]}"),
                                            0,
                                            nullptr);
    ASSERT(diffOutput);
    // When the sub-array delta is larger than the size of the sub-array, we record it as an update
    // operation.
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sfield2: {a: true, 'u3': [[4]]}}"));

    diffOutput = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [2], 4, 5, 6], 5, 5, 5]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [4], 4], 5, 6]}"),
        0,
        nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(
        diffOutput->diff,
        fromjson("{sfield2: {a: true, l: 6, 's3': {a: true, l: 4, 'u2': [4]}, 'u5': 6}}"));
}

TEST(DocumentDiffCalculatorTest, SubObjHasEmptyFieldName) {
    auto diffOutput = doc_diff::computeDiff(
        fromjson("{'': '1', field2: [1, 2, 3, {'': {'': 1, padding: 'largeFieldValue'}}]}"),
        fromjson("{'': '2', field2: [1, 2, 3, {'': {'': 2, padding: 'largeFieldValue'}}]}"),
        15,
        nullptr);

    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(
        diffOutput->diff, fromjson("{u: {'': '2'}, sfield2: {a: true, s3: {s: {u: {'': 2}}} }}"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubArrayUpdateElements) {
    auto diffOutput = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, "
                 "{field3: ['veryLargeStringValueveryLargeStringValue', 2, 3, 4]}]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': "
                 "['veryLargeStringValueveryLargeStringValue', 2, 4, 3, 5]}]}"),
        0,
        nullptr);

    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(
        diffOutput->diff,
        fromjson("{sfield2: {a: true, s3: {sfield3: {a: true, u2: 4, u3: 3, u4: 5}} }}"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubArrayDeleteElements) {
    auto diffOutput = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': ['largeString', 2, 3, 4, 5]}]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': ['largeString', 2, 3, 5]}]}"),
        15,
        nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(
        diffOutput->diff,
        fromjson("{sfield2: {a: true, 's3': {sfield3: {a: true, l: 4, 'u3': 5}} }}"));
}

TEST(DocumentDiffCalculatorTest, NestedSubObjs) {
    auto diffOutput = doc_diff::computeDiff(
        fromjson("{level1Field1: 'abcd', level1Field2: {level2Field1: {level3Field1: {p: 1}, "
                 "level3Field2: {q: 1}}, level2Field2: 2}, level1Field3: 3, level1Field4: "
                 "{level2Field1: {level3Field1: {p: 1}, level3Field2: {q: 1}}} }"),
        fromjson("{level1Field1: 'abcd', level1Field2: {level2Field1: {level3Field1: {q: 1}, "
                 "level3Field2: {q: 1}}, level2Field2: 2}, level1Field3: '3', level1Field4: "
                 "{level2Field1: {level3Field1: {q: 1}, level3Field2: {q: 1}}} }"),
        15,
        nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff,
                             fromjson("{u: {level1Field3: '3'}, slevel1Field2: {slevel2Field1: {u: "
                                      "{level3Field1: {q: 1}}}}, slevel1Field4: {slevel2Field1: "
                                      "{u: {level3Field1: {q: 1}}}} }"));
}

TEST(DocumentDiffCalculatorTest, SubArrayInSubArrayLargeDelta) {
    auto diffOutput = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {field3: [2, 3, 4, 5]}]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {field3: [1, 2, 3, 4, 5]}]}"),
        15,
        nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff,
                             fromjson("{sfield2: {a: true, 'u3': {field3: [1, 2, 3, 4, 5]}} }"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubArrayLargeDelta) {
    auto diffOutput =
        doc_diff::computeDiff(fromjson("{field1: [1, 2, 3, 4, 5, 6, {a: 1, b: 2, c: 3, d: 4}, 7]}"),
                              fromjson("{field1: [1, 2, 3, 4, 5, 6, {p: 1, q: 2}, 7]}"),
                              0,
                              nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff,
                             fromjson("{sfield1: {a: true, 'u6': {p: 1, q: 2}} }"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubObjLargeDelta) {
    auto diffOutput = doc_diff::computeDiff(
        fromjson("{field: {p: 'someString', q: 2, r: {a: 1, b: 2, c: 3, 'd': 4}, s: 3}}"),
        fromjson("{field: {p: 'someString', q: 2, r: {p: 1, q: 2}, s: 3}}"),
        0,
        nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sfield: {u: {r: {p: 1, q: 2} }} }"));
}

TEST(DocumentDiffCalculatorTest, SubArrayInSubObjLargeDelta) {
    auto diffOutput =
        doc_diff::computeDiff(fromjson("{field: {p: 'someString', q: 2, r: [1, 3, 4, 5], s: 3}}"),
                              fromjson("{field: {p: 'someString', q: 2, r: [1, 2, 3, 4], s: 3}}"),
                              0,
                              nullptr);
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{sfield: {u: {r: [1, 2, 3, 4]}} }"));
}

void buildDeepObj(BSONObjBuilder* builder,
                  StringData fieldName,
                  int depth,
                  int maxDepth,
                  std::function<void(BSONObjBuilder*, int, int)> function) {
    if (depth >= maxDepth) {
        return;
    }
    BSONObjBuilder subObj = builder->subobjStart(fieldName);
    function(&subObj, depth, maxDepth);

    buildDeepObj(&subObj, fieldName, depth + 1, maxDepth, function);
}

TEST(DocumentDiffCalculatorTest, DeeplyNestObjectGenerateDiff) {
    const auto largeValue = std::string(50, 'a');
    const auto maxDepth = BSONDepth::getMaxDepthForUserStorage();
    auto functionToApply = [&largeValue](BSONObjBuilder* builder, int depth, int maxDepth) {
        builder->append("largeField", largeValue);
    };

    BSONObjBuilder preBob;
    preBob.append("largeField", largeValue);
    buildDeepObj(&preBob, "subObj", 0, maxDepth, functionToApply);
    auto preObj = preBob.done();
    ASSERT(preObj.valid());

    BSONObjBuilder postBob;
    postBob.append("largeField", largeValue);
    auto diffOutput = doc_diff::computeDiff(preObj, postBob.done(), 0, nullptr);

    // Just deleting the deeply nested sub-object should give the post object.
    ASSERT(diffOutput);
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, fromjson("{d: {subObj: false}}"));

    BSONObjBuilder postBob2;
    postBob2.append("largeField", largeValue);
    buildDeepObj(&postBob2, "subObj", 0, maxDepth - 1, functionToApply);

    // Deleting the deepest field should give the post object.
    diffOutput = doc_diff::computeDiff(preObj, postBob2.done(), 0, nullptr);
    ASSERT(diffOutput);
    ASSERT(diffOutput->diff.valid());

    BSONObjBuilder expectedOutputBuilder;
    buildDeepObj(&expectedOutputBuilder,
                 "ssubObj",
                 0,
                 maxDepth - 1,
                 [](BSONObjBuilder* builder, int depth, int maxDepth) {
                     if (depth == maxDepth - 1) {
                         builder->append("d", BSON("subObj" << false));
                     }
                 });
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, expectedOutputBuilder.obj());
}

TEST(DocumentDiffCalculatorTest, DeepestObjectSubDiff) {
    BSONObjBuilder bob1;
    const auto largeValue = std::string(50, 'a');
    int value;
    auto functionToApply = [&value, &largeValue](BSONObjBuilder* builder, int depth, int maxDepth) {
        builder->append("largeField", largeValue);
        if (depth == maxDepth - 1) {
            builder->append("field", value);
        }
    };

    value = 1;
    buildDeepObj(&bob1, "subObj", 0, BSONDepth::getMaxDepthForUserStorage(), functionToApply);
    auto preObj = bob1.done();
    ASSERT(preObj.valid());

    BSONObjBuilder postBob;
    value = 2;
    buildDeepObj(&postBob, "subObj", 0, BSONDepth::getMaxDepthForUserStorage(), functionToApply);
    auto postObj = postBob.done();
    ASSERT(postObj.valid());

    auto diffOutput = doc_diff::computeDiff(preObj, postObj, 0, nullptr);
    ASSERT(diffOutput);
    ASSERT(diffOutput->diff.valid());

    BSONObjBuilder expectedOutputBuilder;
    buildDeepObj(&expectedOutputBuilder,
                 "ssubObj",
                 0,
                 BSONDepth::getMaxDepthForUserStorage(),
                 [](BSONObjBuilder* builder, int depth, int maxDepth) {
                     if (depth == maxDepth - 1) {
                         builder->append("u", BSON("field" << 2));
                     }
                 });
    ASSERT_BSONOBJ_BINARY_EQ(diffOutput->diff, expectedOutputBuilder.obj());
}

}  // namespace
}  // namespace mongo

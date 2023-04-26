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
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

void assertBsonObjEqualUnordered(const BSONObj& lhs, const BSONObj& rhs) {
    UnorderedFieldsBSONObjComparator comparator;
    ASSERT_EQ(comparator.compare(lhs, rhs), 0);
}


TEST(DocumentDiffCalculatorTest, SameObjectsNoDiff) {
    auto assertDiffEmpty = [](const BSONObj& doc) {
        auto oplogDiff = doc_diff::computeOplogDiff(doc, doc, 5);
        ASSERT(oplogDiff);
        ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, BSONObj());
        auto inlineDiff = doc_diff::computeInlineDiff(doc, doc);
        ASSERT(inlineDiff);
        ASSERT_BSONOBJ_BINARY_EQ(*inlineDiff, BSONObj());
    };
    assertDiffEmpty(fromjson("{field1: 1}"));
    assertDiffEmpty(fromjson("{field1: []}"));
    assertDiffEmpty(fromjson("{field1: [{}]}"));
    assertDiffEmpty(fromjson("{field1: [0, 1, 2, 3, 4]}"));
    assertDiffEmpty(fromjson("{field1: null}"));
    assertDiffEmpty(fromjson("{'0': [0]}"));
    assertDiffEmpty(fromjson("{'0': [[{'0': [[{'0': [[]]} ]]}]]}"));
}

TEST(DocumentDiffCalculatorTest, EmptyObjsNoDiff) {
    auto preObj = BSONObj();
    auto postObj = BSONObj();
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT_FALSE(oplogDiff);
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, BSONObj());
}

TEST(DocumentDiffCalculatorTest, SimpleInsert) {
    auto preObj = fromjson("{a: {b: 1}}");
    auto postObj = fromjson("{a: {b: 1}, c: 1}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{i: {c: 1}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: 'i'}"));
}

TEST(DocumentDiffCalculatorTest, SimpleUpdate) {
    auto preObj = fromjson("{a: {b: 1}, c: 1}}");
    auto postObj = fromjson("{a: {b: 1}, c: 2}}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{u: {c: 2}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: 'u'}"));
}

TEST(DocumentDiffCalculatorTest, SimpleDelete) {
    auto preObj = fromjson("{a: {b: 1}, c: 1}}");
    auto postObj = fromjson("{a: {b: 1}}}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{d: {c: false}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: 'd'}"));
}

TEST(DocumentDiffCalculatorTest, SimpleInsertNestedSingle) {
    auto preObj = fromjson("{a: {}, e: 1, f: 1}");
    auto postObj = fromjson("{a: {b: {c: {d: 1}}}, e: 1, f: 1}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{u: {a: {b: {c: {d: 1}}}}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: {b: {c: {d: 'i'}}}}"));
}

TEST(DocumentDiffCalculatorTest, SimpleInsertNestedMultiple) {
    auto preObj = fromjson("{a: 1, g: 1, h: 1}");
    auto postObj = fromjson("{a: {b: 2, c: [2], d: {e: 2}, f: 1}, g: 1, h: 1}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{u: {a: {b: 2, c: [2], d: {e: 2}, f: 1}}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff,
                                fromjson("{a: {b: 'i', c: 'i', d: {e: 'i'}, f: 'i'}}"));
}

TEST(DocumentDiffCalculatorTest, SimpleUpdateNestedObj) {
    auto preObj = fromjson("{a: {b: {c: {d: 1, e: 1, f: 1}}}}");
    auto postObj = fromjson("{a: {b: {c: {d: 2, e: 1, f: 1}}}}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sa: {sb: {sc: {u: {d: 2}}}}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: {b: {c: {d: 'u'}}}}"));
}

TEST(DocumentDiffCalculatorTest, SimpleUpdateNestedArray) {
    auto preObj = fromjson("{a: {b: {c: {d: [1], e: 1, f: 1}}}}");
    auto postObj = fromjson("{a: {b: {c: {d: [2], e: 1, f: 1}}}}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sa: {sb: {sc: {u: {d: [2]}}}}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: {b: {c: {d: 'u'}}}}"));
}

TEST(DocumentDiffCalculatorTest, SimpleDeleteNestedObj) {
    auto preObj = fromjson("{a: {b: {c: {d: 1}, e: 1, f: 1}}}");
    auto postObj = fromjson("{a: {b: {c: {}, e: 1, f: 1}}}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sa: {sb: {u: {c: {}}}}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: {b: {c: {d: 'd'}}}}"));
}

TEST(DocumentDiffCalculatorTest, ReplaceAllFieldsLargeDelta) {
    auto preObj = fromjson("{a: 1, b: 2, c: 3}");
    auto postObj = fromjson("{A: 1, B: 2, C: 3}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT_FALSE(oplogDiff);
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff,
                                fromjson("{a: 'd', b: 'd', c: 'd', A: 'i', B: 'i', C: 'i'}}"));
}

TEST(DocumentDiffCalculatorTest, InsertFrontFieldLargeDelta) {
    auto preObj = fromjson("{b: 1, c: 1, d: 1}");
    auto postObj = fromjson("{a: 1, b: 1, c: 1, d: 1}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT_FALSE(oplogDiff);
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: 'i', b: 'i', c: 'i', d: 'i'}"));
}

TEST(DocumentDiffCalculatorTest, EmptyPostObjLargeDelta) {
    {
        auto preObj = fromjson("{b: 1}");
        auto postObj = BSONObj();
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{b: 'd'}"));
    }
    {
        auto preObj = fromjson("{a: {b: 1}}");
        auto postObj = fromjson("{}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: 'd'}"));
    }
    {
        auto preObj = fromjson("{b: [1, 2, 3]}");
        auto postObj = BSONObj();
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{b: 'd'}"));
    }
    {
        auto preObj = fromjson("{b: {}}");
        auto postObj = BSONObj();
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{b: 'd'}"));
    }
}

TEST(DocumentDiffCalculatorTest, EmptyPreObjLargeDelta) {
    {
        auto preObj = BSONObj();
        auto postObj = fromjson("{b: 1}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{b: 'i'}"));
    }
    {
        auto preObj = fromjson("{}");
        auto postObj = fromjson("{a: {b: 1}}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: {b: 'i'}}"));
    }
    {
        auto preObj = BSONObj();
        auto postObj = fromjson("{b: [1, 2, 3]}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{b: 'i'}"));
    }
    {
        auto preObj = BSONObj();
        auto postObj = fromjson("{b: {}}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{b: 'i'}"));
    }
}

TEST(DocumentDiffCalculatorTest, BSONSizeLimitLargeDelta) {
    auto preObj = BSON(std::string(BSONObjMaxUserSize, 'a') << 1);
    auto postObj = BSONObj();
    ASSERT_FALSE(doc_diff::computeOplogDiff(preObj, postObj, 0));
    ASSERT_FALSE(doc_diff::computeInlineDiff(preObj, postObj));
}

TEST(DocumentDiffCalculatorTest, DeleteAndInsertFieldAtTheEnd) {
    auto preObj = fromjson("{a: 1, b: 'valueString', c: 3, d: 4}");
    auto postObj = fromjson("{b: 'valueString', c: 3, d: 4, a: 1}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{i: {a: 1}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: 'i'}"));
}

TEST(DocumentDiffCalculatorTest, DeletesRecordedInAscendingOrderOfFieldNames) {
    {
        auto preObj = fromjson("{b: 1, a: 2, c: 3, d: 4, e: 'valueString'}");
        auto postObj = fromjson("{c: 3, d: 4, e: 'valueString'}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
        ASSERT(oplogDiff);
        ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{d: {a: false, b: false}}"));
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: 'd', b: 'd'}"));
    }

    // Delete at the end still follow the order.
    {
        auto preObj =
            fromjson("{b: 1, a: 2, c: 'value', d: 'valueString', e: 'valueString', g: 1, f: 1}");
        auto postObj = fromjson("{c: 'value', d: 'valueString', e: 'valueString'}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
        ASSERT(oplogDiff);
        ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff,
                                 fromjson("{d: {g: false, f: false, a: false, b: false}}"));
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: 'd', b: 'd', f: 'd', g: 'd'}"));
    }
}

TEST(DocumentDiffCalculatorTest, DataTypeChangeRecorded) {
    const auto objWithDoubleValue =
        fromjson("{a: 'valueString', b: 2, c: {subField1: 1, subField2: 3.0}, d: 4}");
    const auto objWithIntValue =
        fromjson("{a: 'valueString', b: 2, c: {subField1: 1, subField2: 3}, d: 4}");
    const auto objWithLongValue =
        fromjson("{a: 'valueString', b: 2, c: {subField1: 1, subField2: NumberLong(3)}, d: 4}");
    auto oplogDiff = doc_diff::computeOplogDiff(objWithDoubleValue, objWithIntValue, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sc: {u: {subField2: 3}} }"));
    auto inlineDiff = doc_diff::computeInlineDiff(objWithDoubleValue, objWithIntValue);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: {subField2: 'u'}}"));

    oplogDiff = doc_diff::computeOplogDiff(objWithIntValue, objWithLongValue, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sc: {u: {subField2: NumberLong(3)}} }"));
    inlineDiff = doc_diff::computeInlineDiff(objWithIntValue, objWithLongValue);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: {subField2: 'u'}}"));

    oplogDiff = doc_diff::computeOplogDiff(objWithLongValue, objWithDoubleValue, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sc: {u: {subField2: 3.0}} }"));
    inlineDiff = doc_diff::computeInlineDiff(objWithLongValue, objWithDoubleValue);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: {subField2: 'u'}}"));
}

TEST(DocumentDiffCalculatorTest, NullAndMissing) {
    {
        auto preObj = fromjson("{a: null}");
        auto postObj = fromjson("{}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
        ASSERT_FALSE(oplogDiff);
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{a: 'd'}"));
    }

    {
        auto preObj = fromjson("{a: null, b: undefined, c: null, d: 'someValueStr'}");
        auto postObj = fromjson("{a: null, b: undefined, c: undefined, d: 'someValueStr'}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
        ASSERT(oplogDiff);
        ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{u: {c: undefined}}"));
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: 'u'}"));
    }
}

TEST(DocumentDiffCalculatorTest, FieldOrder) {
    auto preObj = fromjson("{a: 1, b: 2, c: {p: 1, q: 1, s: 1, r: 2}, d: 4}");
    auto postObj = fromjson("{a: 1, b: 2, c: {p: 1, q: 1, r: 2, s: 1}, d: 4}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 10);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sc: {i: {s: 1}} }"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{c: {s: 'i'}}"));
}

TEST(DocumentDiffCalculatorTest, SimpleArrayPush) {
    auto preObj = fromjson("{field1: 'abcd', field2: [1, 2, 3]}");
    auto postObj = fromjson("{field1: 'abcd', field2: [1, 2, 3, 4]}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 5);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sfield2: {a: true, 'u3': 4}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field2: 'u'}"));
}

TEST(DocumentDiffCalculatorTest, NestedArray) {
    {
        auto preObj = fromjson("{field1: 'abcd', field2: [1, 2, 3, [[2]]]}");
        auto postObj = fromjson("{field1: 'abcd', field2: [1, 2, 3, [[4]]]}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT(oplogDiff);
        // When the sub-array delta is larger than the size of the sub-array, we record it as an
        // update operation.
        ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sfield2: {a: true, 'u3': [[4]]}}"));
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field2: 'u'}"));
    }

    {
        auto preObj = fromjson(
            "{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [2], 4, 5, 6], 5, 5, 5]}");
        auto postObj =
            fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [4], 4], 5, 6]}");
        auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
        ASSERT(oplogDiff);
        ASSERT_BSONOBJ_BINARY_EQ(
            *oplogDiff,
            fromjson("{sfield2: {a: true, l: 6, 's3': {a: true, l: 4, 'u2': [4]}, 'u5': 6}}"));
        auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
        ASSERT(inlineDiff);
        assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field2: 'u'}"));
    }
}

TEST(DocumentDiffCalculatorTest, SubObjHasEmptyFieldName) {
    auto preObj =
        fromjson("{'': '1', field2: [1, 2, 3, {'': {'': 1, padding: 'largeFieldValue'}}]}");
    auto postObj =
        fromjson("{'': '2', field2: [1, 2, 3, {'': {'': 2, padding: 'largeFieldValue'}}]}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *oplogDiff, fromjson("{u: {'': '2'}, sfield2: {a: true, s3: {s: {u: {'': 2}}} }}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{'': 'u', field2: 'u'}"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubArrayUpdateElements) {
    auto preObj = fromjson(
        "{field1: 'abcd', field2: [1, 2, 3, "
        "{field3: ['veryLargeStringValueveryLargeStringValue', 2, 3, 4]}]}");
    auto postObj = fromjson(
        "{field1: 'abcd', field2: [1, 2, 3, {'field3': "
        "['veryLargeStringValueveryLargeStringValue', 2, 4, 3, 5]}]}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *oplogDiff,
        fromjson("{sfield2: {a: true, s3: {sfield3: {a: true, u2: 4, u3: 3, u4: 5}} }}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field2: 'u'}"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubArrayDeleteElements) {
    auto preObj =
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': ['largeString', 2, 3, 4, 5]}]}");
    auto postObj =
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': ['largeString', 2, 3, 5]}]}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *oplogDiff, fromjson("{sfield2: {a: true, 's3': {sfield3: {a: true, l: 4, 'u3': 5}} }}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field2: 'u'}"));
}

TEST(DocumentDiffCalculatorTest, NestedSubObjs) {
    auto preObj = fromjson(
        "{level1Field1: 'abcd', level1Field2: {level2Field1: {level3Field1: {p: 1}, "
        "level3Field2: {q: 1}}, level2Field2: 2}, level1Field3: 3, level1Field4: "
        "{level2Field1: {level3Field1: {p: 1}, level3Field2: {q: 1}}} }");
    auto postObj = fromjson(
        "{level1Field1: 'abcd', level1Field2: {level2Field1: {level3Field1: {q: 1}, "
        "level3Field2: {q: 1}}, level2Field2: 2}, level1Field3: '3', level1Field4: "
        "{level2Field1: {level3Field1: {q: 1}, level3Field2: {q: 1}}} }");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff,
                             fromjson("{u: {level1Field3: '3'}, slevel1Field2: {slevel2Field1: {u: "
                                      "{level3Field1: {q: 1}}}}, slevel1Field4: {slevel2Field1: "
                                      "{u: {level3Field1: {q: 1}}}} }"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(
        *inlineDiff,
        fromjson(
            "{level1Field3: 'u', level1Field2: {level2Field1: {level3Field1: {p: 'd', q:'i'}}}, "
            "level1Field4: {level2Field1: {level3Field1: {p: 'd', q:'i'}}}}"));
}

TEST(DocumentDiffCalculatorTest, SubArrayInSubArrayLargeDelta) {
    auto preObj = fromjson("{field1: 'abcd', field2: [1, 2, 3, {field3: [2, 3, 4, 5]}]}");
    auto postObj = fromjson("{field1: 'abcd', field2: [1, 2, 3, {field3: [1, 2, 3, 4, 5]}]}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 15);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff,
                             fromjson("{sfield2: {a: true, 'u3': {field3: [1, 2, 3, 4, 5]}} }"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field2: 'u'}"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubArrayLargeDelta) {
    auto preObj = fromjson("{field1: [1, 2, 3, 4, 5, 6, {a: 1, b: 2, c: 3, d: 4}, 7]}");
    auto postObj = fromjson("{field1: [1, 2, 3, 4, 5, 6, {p: 1, q: 2}, 7]}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sfield1: {a: true, 'u6': {p: 1, q: 2}} }"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field1: 'u'}"));
}

TEST(DocumentDiffCalculatorTest, SubObjInSubObjLargeDelta) {
    auto preObj = fromjson("{field: {p: 'someString', q: 2, r: {a: 1, b: 2, c: 3, 'd': 4}, s: 3}}");
    auto postObj = fromjson("{field: {p: 'someString', q: 2, r: {p: 1, q: 2}, s: 3}}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sfield: {u: {r: {p: 1, q: 2} }} }"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(
        *inlineDiff, fromjson("{field: {r: {q: 'i', p: 'i', a: 'd', b: 'd', c: 'd', d: 'd'}}}"));
}

TEST(DocumentDiffCalculatorTest, SubArrayInSubObjLargeDelta) {
    auto preObj = fromjson("{field: {p: 'someString', q: 2, r: [1, 3, 4, 5], s: 3}}");
    auto postObj = fromjson("{field: {p: 'someString', q: 2, r: [1, 2, 3, 4], s: 3}}");
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{sfield: {u: {r: [1, 2, 3, 4]}} }"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{field: {r: 'u'}}"));
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
    ASSERT_OK(validateBSON(preObj));

    BSONObjBuilder postBob;
    postBob.append("largeField", largeValue);
    auto postObj = postBob.done();

    // Just deleting the deeply nested sub-object should give the post object.
    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, fromjson("{d: {subObj: false}}"));
    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    assertBsonObjEqualUnordered(*inlineDiff, fromjson("{subObj: 'd'}"));

    BSONObjBuilder postBob2;
    postBob2.append("largeField", largeValue);
    buildDeepObj(&postBob2, "subObj", 0, maxDepth - 1, functionToApply);
    auto postObj2 = postBob2.done();

    // Deleting the deepest field should give the post object.
    oplogDiff = doc_diff::computeOplogDiff(preObj, postObj2, 0);
    ASSERT(oplogDiff);
    ASSERT_OK(validateBSON(*oplogDiff));
    BSONObjBuilder expectedOplogDiffBuilder;
    buildDeepObj(&expectedOplogDiffBuilder,
                 "ssubObj",
                 0,
                 maxDepth - 1,
                 [](BSONObjBuilder* builder, int depth, int maxDepth) {
                     if (depth == maxDepth - 1) {
                         builder->append("d", BSON("subObj" << false));
                     }
                 });
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, expectedOplogDiffBuilder.obj());

    inlineDiff = doc_diff::computeInlineDiff(preObj, postObj2);
    ASSERT(inlineDiff);
    ASSERT_OK(validateBSON(*inlineDiff));
    BSONObjBuilder expectedCompactDiffBuilder;
    buildDeepObj(&expectedCompactDiffBuilder,
                 "subObj",
                 0,
                 maxDepth - 1,
                 [](BSONObjBuilder* builder, int depth, int maxDepth) {
                     if (depth == maxDepth - 1) {
                         builder->append("subObj", "d");
                     }
                 });
    ASSERT_BSONOBJ_BINARY_EQ(*inlineDiff, expectedCompactDiffBuilder.obj());
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
    ASSERT_OK(validateBSON(preObj));

    BSONObjBuilder postBob;
    value = 2;
    buildDeepObj(&postBob, "subObj", 0, BSONDepth::getMaxDepthForUserStorage(), functionToApply);
    auto postObj = postBob.done();
    ASSERT_OK(validateBSON(postObj));

    auto oplogDiff = doc_diff::computeOplogDiff(preObj, postObj, 0);
    ASSERT(oplogDiff);
    ASSERT_OK(validateBSON(*oplogDiff));
    BSONObjBuilder expectedOplogDiffBuilder;
    buildDeepObj(&expectedOplogDiffBuilder,
                 "ssubObj",
                 0,
                 BSONDepth::getMaxDepthForUserStorage(),
                 [](BSONObjBuilder* builder, int depth, int maxDepth) {
                     if (depth == maxDepth - 1) {
                         builder->append("u", BSON("field" << 2));
                     }
                 });
    ASSERT_BSONOBJ_BINARY_EQ(*oplogDiff, expectedOplogDiffBuilder.obj());

    auto inlineDiff = doc_diff::computeInlineDiff(preObj, postObj);
    ASSERT(inlineDiff);
    ASSERT_OK(validateBSON(*inlineDiff));
    BSONObjBuilder expectedCompactDiffBuilder;
    buildDeepObj(&expectedCompactDiffBuilder,
                 "subObj",
                 0,
                 BSONDepth::getMaxDepthForUserStorage(),
                 [](BSONObjBuilder* builder, int depth, int maxDepth) {
                     if (depth == maxDepth - 1) {
                         builder->append("field", "u");
                     }
                 });
    ASSERT_BSONOBJ_BINARY_EQ(*inlineDiff, expectedCompactDiffBuilder.obj());
}
}  // namespace
}  // namespace mongo

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

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>

namespace mongo {
namespace {

TEST(DottedPathSupport, CompareObjectsAccordingToSort) {
    ASSERT_LT(bson::compareObjectsAccordingToSort(
                  BSON("a" << 1), BSON("a" << 2), BSON("b" << 1 << "a" << 1)),
              0);
    ASSERT_EQ(
        bson::compareObjectsAccordingToSort(BSON("a" << BSONNULL), BSON("b" << 1), BSON("a" << 1)),
        0);
}

TEST(DottedPathSupport, ExtractElementAtPath) {
    BSONObj obj = BSON("a" << 1 << "b" << BSON("a" << 2) << "c"
                           << BSON_ARRAY(BSON("a" << 3) << BSON("a" << 4)));
    ASSERT_EQUALS(1, bson::extractElementAtDottedPath(obj, "a").numberInt());
    ASSERT_EQUALS(2, bson::extractElementAtDottedPath(obj, "b.a").numberInt());
    ASSERT_EQUALS(3, bson::extractElementAtDottedPath(obj, "c.0.a").numberInt());
    ASSERT_EQUALS(4, bson::extractElementAtDottedPath(obj, "c.1.a").numberInt());

    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "x").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "a.x").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "x.y").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, ".").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "..").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "...").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "a.").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, ".a").eoo());
    ASSERT_TRUE(bson::extractElementAtDottedPath(obj, "b.a.").eoo());
}

TEST(DottedPathSupport, ExtractElementsBasedOnTemplate) {
    BSONObj obj = BSON("a" << 10 << "b" << 11);

    ASSERT_EQ(BSON("a" << 10).woCompare(bson::extractElementsBasedOnTemplate(obj, BSON("a" << 1))),
              0);
    ASSERT_EQ(BSON("b" << 11).woCompare(bson::extractElementsBasedOnTemplate(obj, BSON("b" << 1))),
              0);
    ASSERT_EQ(obj.woCompare(bson::extractElementsBasedOnTemplate(obj, BSON("a" << 1 << "b" << 1))),
              0);

    ASSERT_EQ(bson::extractElementsBasedOnTemplate(obj, BSON("a" << 1 << "c" << 1))
                  .firstElement()
                  .fieldNameStringData(),
              "a");
}

/**
 * We added bson::extractNullForAllFieldsBasedOnTemplate() to avoid some allocations inside
 * $sortArray. It is used instead of extractElementsBasedOnTemplate(). Before the change, when
 * $sortArray compared Values, it would first perform a Value to BSON transformation, then use
 * extractElementsBasedOnTemplate() to produce the BSON document to be used for the comparison.
 *
 * For non-document 'Value's, all field accesses in extractElementsBasedOnTemplate() are null.
 *
 * To avoid the dummy BSON document allocation, we directly transforms Values into the BSON document
 * with null values for each field.
 *
 * This test asserts these two processes produce the same BSON document, so as to not change sort
 * orders in sorted arrays.
 */
TEST(DottedPathSupport, ExtractElementsFromValueAndBSONObjBasedOnTemplate) {
    auto compareExtractFromValueWithExtractFromBson = [](Value&& v, BSONObj&& pattern) {
        BSONObj lhs = bson::extractElementsBasedOnTemplate(v.isObject() ? v.getDocument().toBson()
                                                                        : v.wrap(""),
                                                           pattern,
                                                           true /*useNullIfMissing*/);
        BSONObj rhs = v.isObject() ? lhs : bson::extractNullForAllFieldsBasedOnTemplate(pattern);
        return lhs.woCompare(rhs);
    };
    ASSERT_EQ(compareExtractFromValueWithExtractFromBson(Value(true), fromjson("{a: 1}")), 0);
    ASSERT_EQ(compareExtractFromValueWithExtractFromBson(Value(true), fromjson("{}")), 0);
    ASSERT_EQ(compareExtractFromValueWithExtractFromBson(Value(2), fromjson("{a: 1}")), 0);
    ASSERT_EQ(
        compareExtractFromValueWithExtractFromBson(Value(std::string("a")), fromjson("{a: 1}")), 0);
    std::vector<std::string> jsons = {
        "{a: 1, b: 1}",
        "{a: 2, b: 1}",
        "{a: [1, 2]}",
        "{a: []}",
        "{a: [{b:1}, {b:2}]}",
        "{a: [{b:1}, {}]}",
        "{a: null, b: 1}",
        "{a: null}",
        "{a: {a: []}}",
        "{a: {a: {a: []}}}",
        "{a: {a: {a: [null]}}}",
        "{a: {a: {a: [{a: 1}, {b: 1}]}}}",
        "{a: {a: {a: null}}}",
        "{a: {a: {b: []}}}",
        "{a: {a: {b: [null]}}}",
        "{a: {a: {b: [{a: 1}, {b: 1}]}}}",
        "{a: {a: {b: null}}}",
        "{a: {b: {a: []}}}",
        "{a: {b: {a: [null]}}}",
        "{a: {b: {a: [{a: 1}, {b: 1}]}}}",
        "{a: {b: {a: null}}}",
        "{b: {a: {b: []}}}",
        "{b: {a: {b: [null]}}}",
        "{b: {a: {b: [{a: 1}, {b: 1}]}}}",
        "{b: {a: {b: null}}}",
        "{b: {b: {a: []}}}",
        "{b: {b: {a: [null]}}}",
        "{b: {b: {a: [{a: 1}, {b: 1}]}}}",
        "{b: {b: {a: null}}}",
        "{a: {a: []}}",
        "{a: {a: [null]}}",
        "{a: {a: [{}]}}",
        "{a: {a: null}}",
        "{a: {b: 1}, c: 1}",
        "{a: {b: 1}}",
        "{a: {b: 1}}",
        "{a: {}}",
        "{}",
    };
    for (const std::string& lhs : jsons) {
        for (const std::string& rhs : jsons) {
            ASSERT_EQ(
                compareExtractFromValueWithExtractFromBson(Value(fromjson(lhs)), fromjson(rhs)), 0);
            ASSERT_EQ(compareExtractFromValueWithExtractFromBson(
                          Value({fromjson(lhs), fromjson(rhs)}), fromjson(rhs)),
                      0);
        }
    }
}

TEST(ExtractElementAtPathOrArrayAlongPath, ReturnsArrayEltWithEmptyPathWhenArrayIsAtEndOfPath) {
    BSONObj obj(fromjson("{a: {b: {c: [1, 2, 3]}}}"));
    StringData path("a.b.c");
    const char* pathData = path.data();
    auto resultElt = bson::extractElementAtOrArrayAlongDottedPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, fromjson("{c: [1, 2, 3]}").firstElement());
    ASSERT(StringData(pathData).empty());
}

TEST(ExtractElementAtPathOrArrayAlongPath, ReturnsArrayEltWithNonEmptyPathForArrayInMiddleOfPath) {
    BSONObj obj(fromjson("{a: {b: [{c: 1}, {c: 2}]}}"));
    StringData path("a.b.c");
    const char* pathData = path.data();
    auto resultElt = bson::extractElementAtOrArrayAlongDottedPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, fromjson("{b: [{c: 1}, {c: 2}]}").firstElement());
    ASSERT_EQ(StringData(pathData), StringData("c"));
}

TEST(ExtractElementAtPathOrArrayAlongPath, NumericalPathElementNotTreatedAsArrayIndex) {
    BSONObj obj(fromjson("{a: [{'0': 'foo'}]}"));
    StringData path("a.0");
    const char* pathData = path.data();
    auto resultElt = bson::extractElementAtOrArrayAlongDottedPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, obj.firstElement());
    ASSERT_EQ(StringData(pathData), StringData("0"));
}

TEST(ExtractElementAtPathOrArrayAlongPath, NumericalPathElementTreatedAsFieldNameForNestedObject) {
    BSONObj obj(fromjson("{a: {'0': 'foo'}}"));
    StringData path("a.0");
    const char* pathData = path.data();
    auto resultElt = bson::extractElementAtOrArrayAlongDottedPath(obj, pathData);
    ASSERT_BSONELT_EQ(resultElt, fromjson("{'0': 'foo'}").firstElement());
    ASSERT(StringData(pathData).empty());
}

}  // namespace
}  // namespace mongo

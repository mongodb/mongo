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

/**
 * This file contains tests for mongo/db/exec/projection_exec.cpp
 */

#include "boost/optional.hpp"
#include "boost/optional/optional_io.hpp"
#include <memory>

#include "mongo/db/exec/projection_exec.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/stdx/variant.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;
using namespace std::string_literals;

namespace {

/**
 * Utility function to create a MatchExpression.
 */
std::unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression status = MatchExpressionParser::parse(obj, std::move(expCtx));
    ASSERT_TRUE(status.isOK());
    return std::move(status.getValue());
}

/**
 * Test encapsulation for single call to ProjectionExec::project() or
 * ProjectionExec::projectCovered().
 */
boost::optional<std::string> project(
    const char* specStr,
    const char* queryStr,
    const stdx::variant<const char*, const IndexKeyDatum> objStrOrDatum,
    const boost::optional<const CollatorInterface&> collator = boost::none,
    const BSONObj& sortKey = BSONObj(),
    const double textScore = 0.0) {
    // Create projection exec object.
    BSONObj spec = fromjson(specStr);
    BSONObj query = fromjson(queryStr);
    std::unique_ptr<MatchExpression> queryExpression = parseMatchExpression(query);
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    ProjectionExec exec(opCtx.get(), spec, queryExpression.get(), collator.get_ptr());

    auto objStr = stdx::get_if<const char*>(&objStrOrDatum);
    auto projected = objStr
        ? exec.project(fromjson(*objStr), boost::none, Value{}, sortKey, textScore)
        : exec.projectCovered({stdx::get<const IndexKeyDatum>(objStrOrDatum)},
                              boost::none,
                              Value{},
                              sortKey,
                              textScore);

    if (!projected.isOK())
        return boost::none;
    else
        return boost::make_optional(projected.getValue().toString());
}

//
// position $
//

TEST(ProjectionExecTest, TransformPositionalDollar) {
    // Valid position $ projections.
    ASSERT_EQ(boost::make_optional("{ a: [ 10 ] }"s),
              project("{'a.$': 1}", "{a: 10}", "{a: [10, 20, 30]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 20 ] }"s),
              project("{'a.$': 1}", "{a: 20}", "{a: [10, 20, 30]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 5 ] }"s),
              project("{'a.$': 1}", "{a: {$gt: 4}}", "{a: [5]}"));

    // Invalid position $ projections.
    ASSERT_EQ(boost::none, project("{'a.$': 1}", "{a: {$size: 1}}", "{a: [5]}"));
}

//
// $elemMatch
//

TEST(ProjectionExecTest, TransformElemMatch) {
    const char* s = "{a: [{x: 1, y: 10}, {x: 1, y: 20}, {x: 2, y: 10}]}";

    // Valid $elemMatch projections.
    ASSERT_EQ(boost::make_optional("{ a: [ { x: 1, y: 10 } ] }"s),
              project("{a: {$elemMatch: {x: 1}}}", "{}", s));
    ASSERT_EQ(boost::make_optional("{ a: [ { x: 1, y: 20 } ] }"s),
              project("{a: {$elemMatch: {x: 1, y: 20}}}", "{}", s));
    ASSERT_EQ(boost::make_optional("{ a: [ { x: 2, y: 10 } ] }"s),
              project("{a: {$elemMatch: {x: 2}}}", "{}", s));
    ASSERT_EQ(boost::make_optional("{}"s), project("{a: {$elemMatch: {x: 3}}}", "{}", s));

    // $elemMatch on unknown field z
    ASSERT_EQ(boost::make_optional("{}"s), project("{a: {$elemMatch: {z: 1}}}", "{}", s));
}

TEST(ProjectionExecTest, ElemMatchProjectionRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ASSERT_EQ(boost::make_optional("{ a: [ \"zdd\" ] }"s),
              project("{a: {$elemMatch: {$gte: 'abc'}}}",
                      "{}",
                      "{a: ['zaa', 'zbb', 'zdd', 'zee']}",
                      collator));
}

//
// $slice
//

TEST(ProjectionExecTest, TransformSliceCount) {
    // Valid $slice projections using format {$slice: count}.
    ASSERT_EQ(boost::make_optional("{ a: [ 4, 6, 8 ] }"s),
              project("{a: {$slice: -10}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 4, 6, 8 ] }"s),
              project("{a: {$slice: -3}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 8 ] }"s),
              project("{a: {$slice: -1}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [] }"s),
              project("{a: {$slice: 0}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 4 ] }"s),
              project("{a: {$slice: 1}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 4, 6, 8 ] }"s),
              project("{a: {$slice: 3}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 4, 6, 8 ] }"s),
              project("{a: {$slice: 10}}", "{}", "{a: [4, 6, 8]}"));
}

TEST(ProjectionExecTest, TransformSliceSkipLimit) {
    // Valid $slice projections using format {$slice: [skip, limit]}.
    // Non-positive limits are rejected at the query parser and therefore not handled by
    // the projection execution stage. In fact, it will abort on an invalid limit.
    ASSERT_EQ(boost::make_optional("{ a: [ 4, 6, 8 ] }"s),
              project("{a: {$slice: [-10, 10]}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 4, 6, 8 ] }"s),
              project("{a: {$slice: [-3, 5]}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 8 ] }"s),
              project("{a: {$slice: [-1, 1]}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 4, 6 ] }"s),
              project("{a: {$slice: [0, 2]}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 4 ] }"s),
              project("{a: {$slice: [0, 1]}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [ 6 ] }"s),
              project("{a: {$slice: [1, 1]}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [] }"s),
              project("{a: {$slice: [3, 5]}}", "{}", "{a: [4, 6, 8]}"));
    ASSERT_EQ(boost::make_optional("{ a: [] }"s),
              project("{a: {$slice: [10, 10]}}", "{}", "{a: [4, 6, 8]}"));
}

//
// Dotted projections.
//

TEST(ProjectionExecTest, TransformCoveredDottedProjection) {
    BSONObj keyPattern = fromjson("{a: 1, 'b.c': 1, 'b.d': 1, 'b.f.g': 1, 'b.f.h': 1}");
    BSONObj keyData = fromjson("{'': 1, '': 2, '': 3, '': 4, '': 5}");
    ASSERT_EQ(boost::make_optional("{ b: { c: 2, d: 3, f: { g: 4, h: 5 } } }"s),
              project("{'b.c': 1, 'b.d': 1, 'b.f.g': 1, 'b.f.h': 1}",
                      "{}",
                      IndexKeyDatum(keyPattern, keyData, nullptr)));
}

TEST(ProjectionExecTest, TransformNonCoveredDottedProjection) {
    ASSERT_EQ(boost::make_optional("{ b: { c: 2, d: 3, f: { g: 4, h: 5 } } }"s),
              project("{'b.c': 1, 'b.d': 1, 'b.f.g': 1, 'b.f.h': 1}",
                      "{}",
                      "{a: 1, b: {c: 2, d: 3, f: {g: 4, h: 5}}}"));
}

//
// $meta
// $meta projections add computed values to the projected object.
//

TEST(ProjectionExecTest, TransformMetaTextScore) {
    // Query {} is ignored.
    ASSERT_EQ(boost::make_optional("{ a: \"hello\", b: 100.0 }"s),
              project("{b: {$meta: 'textScore'}}",
                      "{}",
                      "{a: 'hello'}",
                      boost::none,  // collator
                      BSONObj(),    // sortKey
                      100.0));      // textScore
    // Projected meta field should overwrite existing field.
    ASSERT_EQ(boost::make_optional("{ a: \"hello\", b: 100.0 }"s),
              project("{b: {$meta: 'textScore'}}",
                      "{}",
                      "{a: 'hello', b: -1}",
                      boost::none,  // collator
                      BSONObj(),    // sortKey
                      100.0));      // textScore
}

TEST(ProjectionExecTest, TransformMetaSortKey) {
    // Query {} is ignored.
    ASSERT_EQ(boost::make_optional("{ a: \"hello\", b: { : 99 } }"s),
              project("{b: {$meta: 'sortKey'}}",
                      "{}",
                      "{a: 'hello'}",
                      boost::none,       // collator
                      BSON("" << 99)));  // sortKey

    // Projected meta field should overwrite existing field.
    ASSERT_EQ(boost::make_optional("{ a: { : 99 } }"s),
              project("{a: {$meta: 'sortKey'}}",
                      "{}",
                      "{a: 'hello'}",
                      boost::none,       // collator
                      BSON("" << 99)));  // sortKey
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredNormal) {
    ASSERT_EQ(boost::make_optional("{ a: 5, b: { : 5 } }"s),
              project("{_id: 0, a: 1, b: {$meta: 'sortKey'}}",
                      "{}",
                      IndexKeyDatum(BSON("a" << 1), BSON("" << 5), nullptr),
                      boost::none,      // collator
                      BSON("" << 5)));  // sortKey
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredOverwrite) {
    ASSERT_EQ(boost::make_optional("{ a: { : 5 } }"s),
              project("{_id: 0, a: 1, a: {$meta: 'sortKey'}}",
                      "{}",
                      IndexKeyDatum(BSON("a" << 1), BSON("" << 5), nullptr),
                      boost::none,      // collator
                      BSON("" << 5)));  // sortKey
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredAdditionalData) {
    ASSERT_EQ(boost::make_optional("{ a: 5, c: 6, b: { : 5 } }"s),
              project("{_id: 0, a: 1, b: {$meta: 'sortKey'}, c: 1}",
                      "{}",
                      IndexKeyDatum(BSON("a" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr),
                      boost::none,      // collator
                      BSON("" << 5)));  // sortKey
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredCompound) {
    ASSERT_EQ(boost::make_optional("{ a: 5, b: { : 5, : 6 } }"s),
              project("{_id: 0, a: 1, b: {$meta: 'sortKey'}}",
                      "{}",
                      IndexKeyDatum(BSON("a" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr),
                      boost::none,                 // collator
                      BSON("" << 5 << "" << 6)));  // sortKey
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredCompound2) {
    ASSERT_EQ(boost::make_optional("{ a: 5, c: 4, b: { : 5, : 6 } }"s),
              project("{_id: 0, a: 1, c: 1, b: {$meta: 'sortKey'}}",
                      "{}",
                      IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1),
                                    BSON("" << 5 << "" << 6 << "" << 4),
                                    nullptr),
                      boost::none,                 // collator
                      BSON("" << 5 << "" << 6)));  // sortKey
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredCompound3) {
    ASSERT_EQ(boost::make_optional("{ c: 4, d: 9000, b: { : 6, : 4 } }"s),
              project("{_id: 0, c: 1, d: 1, b: {$meta: 'sortKey'}}",
                      "{}",
                      IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1),
                                    BSON("" << 5 << "" << 6 << "" << 4 << "" << 9000),
                                    nullptr),
                      boost::none,                 // collator
                      BSON("" << 6 << "" << 4)));  // sortKey
}

}  // namespace

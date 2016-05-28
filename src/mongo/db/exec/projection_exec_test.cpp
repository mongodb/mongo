/**
 *    Copyright (C) 2013 mongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/db/exec/projection_exec.h"

#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include <memory>

using namespace mongo;

namespace {

using std::unique_ptr;

/**
 * Utility function to create MatchExpression
 */
unique_ptr<MatchExpression> parseMatchExpression(const BSONObj& obj) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(status.isOK());
    return std::move(status.getValue());
}

//
// transform tests
//

/**
 * test function to verify results of transform()
 * on a working set member.
 *
 * specStr - projection specification
 * queryStr - query
 * objStr - object to run projection on
 * data - computed data. Owned by working set member created in this function if not null.
 * expectedStatusOK - expected status of transformation
 * expectedObjStr - expected object after successful projection.
 *                  Ignored if expectedStatusOK is false.
 */

void testTransform(const char* specStr,
                   const char* queryStr,
                   const char* objStr,
                   WorkingSetComputedData* data,
                   const CollatorInterface* collator,
                   bool expectedStatusOK,
                   const char* expectedObjStr) {
    // Create projection exec object.
    BSONObj spec = fromjson(specStr);
    BSONObj query = fromjson(queryStr);
    unique_ptr<MatchExpression> queryExpression = parseMatchExpression(query);
    ProjectionExec exec(
        spec, queryExpression.get(), collator, ExtensionsCallbackDisallowExtensions());

    // Create working set member.
    WorkingSetMember wsm;
    wsm.obj = Snapshotted<BSONObj>(SnapshotId(), fromjson(objStr));
    if (data) {
        wsm.addComputed(data);
    }
    wsm.transitionToOwnedObj();

    // Transform object
    Status status = exec.transform(&wsm);

    // There are fewer checks to perform if we are expected a failed status.
    if (!expectedStatusOK) {
        if (status.isOK()) {
            mongoutils::str::stream ss;
            ss << "expected transform() to fail but got success instead."
               << "\nprojection spec: " << specStr << "\nquery: " << queryStr
               << "\nobject before projection: " << objStr;
            FAIL(ss);
        }
        return;
    }

    // If we are expecting a successful transformation but got a failed status instead,
    // print out status message in assertion message.
    if (!status.isOK()) {
        mongoutils::str::stream ss;
        ss << "transform() test failed: unexpected failed status: " << status.toString()
           << "\nprojection spec: " << specStr << "\nquery: " << queryStr
           << "\nobject before projection: " << objStr
           << "\nexpected object after projection: " << expectedObjStr;
        FAIL(ss);
    }

    // Finally, we compare the projected object.
    const BSONObj& obj = wsm.obj.value();
    BSONObj expectedObj = fromjson(expectedObjStr);
    if (obj != expectedObj) {
        mongoutils::str::stream ss;
        ss << "transform() test failed: unexpected projected object."
           << "\nprojection spec: " << specStr << "\nquery: " << queryStr
           << "\nobject before projection: " << objStr
           << "\nexpected object after projection: " << expectedObjStr
           << "\nactual object after projection: " << obj.toString();
        FAIL(ss);
    }
}

/**
 * testTransform without computed data or collator arguments.
 */
void testTransform(const char* specStr,
                   const char* queryStr,
                   const char* objStr,
                   bool expectedStatusOK,
                   const char* expectedObjStr) {
    testTransform(specStr, queryStr, objStr, nullptr, nullptr, expectedStatusOK, expectedObjStr);
}

/**
 * Test function to verify the results of projecting the $meta sortKey while under a covered
 * projection. In particular, it tests that ProjectionExec can take a WorkingSetMember in
 * RID_AND_IDX state and use the sortKey along with the index data to generate the final output
 * document. For SERVER-20117.
 *
 * sortKey - The sort key in BSONObj form.
 * projSpec - The JSON representation of the proj spec BSONObj.
 * ikd - The data stored in the index.
 *
 * Returns the BSON representation of the actual output, to be checked against the expected output.
 */
BSONObj transformMetaSortKeyCovered(const BSONObj& sortKey,
                                    const char* projSpec,
                                    const IndexKeyDatum& ikd) {
    WorkingSet ws;
    WorkingSetID wsid = ws.allocate();
    WorkingSetMember* wsm = ws.get(wsid);
    wsm->keyData.push_back(ikd);
    wsm->addComputed(new SortKeyComputedData(sortKey));
    ws.transitionToRecordIdAndIdx(wsid);

    ProjectionExec projExec(
        fromjson(projSpec), nullptr, nullptr, ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(projExec.transform(wsm));

    return wsm->obj.value();
}

//
// position $
//

TEST(ProjectionExecTest, TransformPositionalDollar) {
    // Valid position $ projections.
    testTransform("{'a.$': 1}", "{a: 10}", "{a: [10, 20, 30]}", true, "{a: [10]}");
    testTransform("{'a.$': 1}", "{a: 20}", "{a: [10, 20, 30]}", true, "{a: [20]}");
    testTransform("{'a.$': 1}", "{a: 30}", "{a: [10, 20, 30]}", true, "{a: [30]}");
    testTransform("{'a.$': 1}", "{a: {$gt: 4}}", "{a: [5]}", true, "{a: [5]}");

    // Invalid position $ projections.
    testTransform("{'a.$': 1}", "{a: {$size: 1}}", "{a: [5]}", false, "");

    // Ambigous position $ projections.
    testTransform("{'a.$': 1}", "{$and: [{a: 1}, {a: 2}]}", "{a: [1, 2]}", false, "");
    testTransform("{'a.$': 1}", "{a: 1, b: 2}", "{a: [1], b: [2]}", false, "");
    testTransform("{'a.$': 1}", "{a: {$elemMatch: {$lt: 2}}, b: 2}", "{a: [1], b: [2]}", false, "");
    testTransform("{'a.$': 1}", "{'a.b': 1, 'a.c': 2}", "{a: [{b: 1}, {c: 2}]}", false, "");
}

//
// $elemMatch
//

TEST(ProjectionExecTest, TransformElemMatch) {
    const char* s = "{a: [{x: 1, y: 10}, {x: 1, y: 20}, {x: 2, y: 10}]}";

    // Valid $elemMatch projections.
    testTransform("{a: {$elemMatch: {x: 1}}}", "{}", s, true, "{a: [{x: 1, y: 10}]}");
    testTransform("{a: {$elemMatch: {x: 1, y: 20}}}", "{}", s, true, "{a: [{x: 1, y: 20}]}");
    testTransform("{a: {$elemMatch: {x: 2}}}", "{}", s, true, "{a: [{x: 2, y: 10}]}");
    testTransform("{a: {$elemMatch: {x: 3}}}", "{}", s, true, "{}");

    // $elemMatch on unknown field z
    testTransform("{a: {$elemMatch: {z: 1}}}", "{}", s, true, "{}");
}

TEST(ProjectionExecTest, ElemMatchProjectionRespectsCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    testTransform("{a: {$elemMatch: {$gte: 'abc'}}}",
                  "{}",
                  "{a: ['zaa', 'zbb', 'zdd', 'zee']}",
                  nullptr,  // WSM computed data
                  &collator,
                  true,
                  "{a: ['zdd']}");
}

//
// $slice
//

TEST(ProjectionExecTest, TransformSliceCount) {
    // Valid $slice projections using format {$slice: count}.
    testTransform("{a: {$slice: -10}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4, 6, 8]}");
    testTransform("{a: {$slice: -3}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4, 6, 8]}");
    testTransform("{a: {$slice: -1}}", "{}", "{a: [4, 6, 8]}", true, "{a: [8]}");
    testTransform("{a: {$slice: 0}}", "{}", "{a: [4, 6, 8]}", true, "{a: []}");
    testTransform("{a: {$slice: 1}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4]}");
    testTransform("{a: {$slice: 3}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4, 6, 8]}");
    testTransform("{a: {$slice: 10}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4, 6, 8]}");
}

TEST(ProjectionExecTest, TransformSliceSkipLimit) {
    // Valid $slice projections using format {$slice: [skip, limit]}.
    // Non-positive limits are rejected at the query parser and therefore not handled by
    // the projection execution stage. In fact, it will abort on an invalid limit.
    testTransform("{a: {$slice: [-10, 10]}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4, 6, 8]}");
    testTransform("{a: {$slice: [-3, 5]}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4, 6, 8]}");
    testTransform("{a: {$slice: [-1, 1]}}", "{}", "{a: [4, 6, 8]}", true, "{a: [8]}");
    testTransform("{a: {$slice: [0, 2]}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4, 6]}");
    testTransform("{a: {$slice: [0, 1]}}", "{}", "{a: [4, 6, 8]}", true, "{a: [4]}");
    testTransform("{a: {$slice: [1, 1]}}", "{}", "{a: [4, 6, 8]}", true, "{a: [6]}");
    testTransform("{a: {$slice: [3, 5]}}", "{}", "{a: [4, 6, 8]}", true, "{a: []}");
    testTransform("{a: {$slice: [10, 10]}}", "{}", "{a: [4, 6, 8]}", true, "{a: []}");
}

//
// $meta
// $meta projections add computed values to the projected object.
//

TEST(ProjectionExecTest, TransformMetaTextScore) {
    // Query {} is ignored.
    testTransform("{b: {$meta: 'textScore'}}",
                  "{}",
                  "{a: 'hello'}",
                  new mongo::TextScoreComputedData(100),
                  nullptr,  // collator
                  true,
                  "{a: 'hello', b: 100}");
    // Projected meta field should overwrite existing field.
    testTransform("{b: {$meta: 'textScore'}}",
                  "{}",
                  "{a: 'hello', b: -1}",
                  new mongo::TextScoreComputedData(100),
                  nullptr,  // collator
                  true,
                  "{a: 'hello', b: 100}");
}

TEST(ProjectionExecTest, TransformMetaSortKey) {
    testTransform("{b: {$meta: 'sortKey'}}",
                  "{}",
                  "{a: 'hello'}",
                  new mongo::SortKeyComputedData(BSON("" << 99)),
                  nullptr,  // collator
                  true,
                  "{a: 'hello', b: {'': 99}}");

    // Projected meta field should overwrite existing field.
    testTransform("{a: {$meta: 'sortKey'}}",
                  "{}",
                  "{a: 'hello'}",
                  new mongo::SortKeyComputedData(BSON("" << 99)),
                  nullptr,  // collator
                  true,
                  "{a: {'': 99}}");
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredNormal) {
    BSONObj actualOut =
        transformMetaSortKeyCovered(BSON("" << 5),
                                    "{_id: 0, a: 1, b: {$meta: 'sortKey'}}",
                                    IndexKeyDatum(BSON("a" << 1), BSON("" << 5), nullptr));
    BSONObj expectedOut = BSON("a" << 5 << "b" << BSON("" << 5));
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredOverwrite) {
    BSONObj actualOut =
        transformMetaSortKeyCovered(BSON("" << 5),
                                    "{_id: 0, a: 1, a: {$meta: 'sortKey'}}",
                                    IndexKeyDatum(BSON("a" << 1), BSON("" << 5), nullptr));
    BSONObj expectedOut = BSON("a" << BSON("" << 5));
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredAdditionalData) {
    BSONObj actualOut = transformMetaSortKeyCovered(
        BSON("" << 5),
        "{_id: 0, a: 1, b: {$meta: 'sortKey'}, c: 1}",
        IndexKeyDatum(BSON("a" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr));
    BSONObj expectedOut = BSON("a" << 5 << "c" << 6 << "b" << BSON("" << 5));
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredCompound) {
    BSONObj actualOut = transformMetaSortKeyCovered(
        BSON("" << 5 << "" << 6),
        "{_id: 0, a: 1, b: {$meta: 'sortKey'}}",
        IndexKeyDatum(BSON("a" << 1 << "c" << 1), BSON("" << 5 << "" << 6), nullptr));
    BSONObj expectedOut = BSON("a" << 5 << "b" << BSON("" << 5 << "" << 6));
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredCompound2) {
    BSONObj actualOut = transformMetaSortKeyCovered(
        BSON("" << 5 << "" << 6),
        "{_id: 0, a: 1, c: 1, b: {$meta: 'sortKey'}}",
        IndexKeyDatum(
            BSON("a" << 1 << "b" << 1 << "c" << 1), BSON("" << 5 << "" << 6 << "" << 4), nullptr));
    BSONObj expectedOut = BSON("a" << 5 << "c" << 4 << "b" << BSON("" << 5 << "" << 6));
    ASSERT_EQ(actualOut, expectedOut);
}

TEST(ProjectionExecTest, TransformMetaSortKeyCoveredCompound3) {
    BSONObj actualOut = transformMetaSortKeyCovered(
        BSON("" << 6 << "" << 4),
        "{_id: 0, c: 1, d: 1, b: {$meta: 'sortKey'}}",
        IndexKeyDatum(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1),
                      BSON("" << 5 << "" << 6 << "" << 4 << "" << 9000),
                      nullptr));
    BSONObj expectedOut = BSON("c" << 4 << "d" << 9000 << "b" << BSON("" << 6 << "" << 4));
    ASSERT_EQ(actualOut, expectedOut);
}

}  // namespace

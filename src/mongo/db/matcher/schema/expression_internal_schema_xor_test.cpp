/**
 *    Copyright (C) 2012 10gen Inc.
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

/** Unit tests for MatchMatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/query/collation/collator_interface_mock.h"

namespace mongo {

using std::unique_ptr;



// TEST(InternalSchemaXorOp, NoClauses) {
//     InternalSchemaXorMatchExpression internalSchemaXorOp;
//     ASSERT(internalSchemaXorOp.matchesBSON(BSONObj(), NULL));
// }

// TEST(InternalSchemaXorOp, MatchesSingleClause) {
//     BSONObj baseOperand = BSON("$ne" << 5);
//     unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression());
//     ASSERT(eq->init("a", baseOperand["$ne"]).isOK());
//     unique_ptr<NotMatchExpression> ne(new NotMatchExpression());
//     ASSERT(ne->init(eq.release()).isOK());

//     InternalSchemaXorMatchExpression internalSchemaXorOp;
//     internalSchemaXorOp.add(ne.release());

//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("a" << 4), NULL));
//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), NULL));
//     ASSERT(internalSchemaXorOp.matchesBSON(BSON("a" << 5), NULL));
//     ASSERT(internalSchemaXorOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), NULL));
// }

// TEST(InternalSchemaXorOp, MatchesThreeClauses) {
//     BSONObj baseOperand1 = BSON("$gt" << 10);
//     BSONObj baseOperand2 = BSON("$lt" << 0);
//     BSONObj baseOperand3 = BSON("b" << 100);

//     unique_ptr<ComparisonMatchExpression> sub1(new GTMatchExpression());
//     ASSERT(sub1->init("a", baseOperand1["$gt"]).isOK());
//     unique_ptr<ComparisonMatchExpression> sub2(new LTMatchExpression());
//     ASSERT(sub2->init("a", baseOperand2["$lt"]).isOK());
//     unique_ptr<ComparisonMatchExpression> sub3(new EqualityMatchExpression());
//     ASSERT(sub3->init("b", baseOperand3["b"]).isOK());

//     InternalSchemaXorMatchExpression internalSchemaXorOp;
//     internalSchemaXorOp.add(sub1.release());
//     internalSchemaXorOp.add(sub2.release());
//     internalSchemaXorOp.add(sub3.release());

//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("a" << -1), NULL));
//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("a" << 11), NULL));
//     ASSERT(internalSchemaXorOp.matchesBSON(BSON("a" << 5), NULL));
//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("b" << 100), NULL));
//     ASSERT(internalSchemaXorOp.matchesBSON(BSON("b" << 101), NULL));
//     ASSERT(internalSchemaXorOp.matchesBSON(BSONObj(), NULL));
//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("a" << 11 << "b" << 100), NULL));
// }

// TEST(InternalSchemaXorOp, ElemMatchKey) {
//     BSONObj baseOperand1 = BSON("a" << 1);
//     BSONObj baseOperand2 = BSON("b" << 2);
//     unique_ptr<ComparisonMatchExpression> sub1(new EqualityMatchExpression());
//     ASSERT(sub1->init("a", baseOperand1["a"]).isOK());
//     unique_ptr<ComparisonMatchExpression> sub2(new EqualityMatchExpression());
//     ASSERT(sub2->init("b", baseOperand2["b"]).isOK());

//     InternalSchemaXorMatchExpression internalSchemaXorOp;
//     internalSchemaXorOp.add(sub1.release());
//     internalSchemaXorOp.add(sub2.release());

//     MatchDetails details;
//     details.requestElemMatchKey();
//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("a" << 1), &details));
//     ASSERT(!details.hasElemMatchKey());
//     ASSERT(!internalSchemaXorOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(10)), &details));
//     ASSERT(!details.hasElemMatchKey());
//     ASSERT(internalSchemaXorOp.matchesBSON(BSON("a" << BSON_ARRAY(3) << "b" << BSON_ARRAY(4)), &details));
//     // The elem match key feature is not implemented for $internalSchemaXor.
//     ASSERT(!details.hasElemMatchKey());
// }


// TEST(InternalSchemaXorOp, Equivalent) {
//     BSONObj baseOperand1 = BSON("a" << 1);
//     BSONObj baseOperand2 = BSON("b" << 2);
//     EqualityMatchExpression sub1;
//     ASSERT(sub1.init("a", baseOperand1["a"]).isOK());
//     EqualityMatchExpression sub2;
//     ASSERT(sub2.init("b", baseOperand2["b"]).isOK());

//     InternalSchemaXorMatchExpression e1;
//     e1.add(sub1.shallowClone().release());
//     e1.add(sub2.shallowClone().release());

//     InternalSchemaXorMatchExpression e2;
//     e2.add(sub1.shallowClone().release());

//     ASSERT(e1.equivalent(&e1));
//     ASSERT(!e1.equivalent(&e2));
// }
}

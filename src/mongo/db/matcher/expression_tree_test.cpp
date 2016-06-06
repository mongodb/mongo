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

namespace mongo {

using std::unique_ptr;

TEST(NotMatchExpression, MatchesScalar) {
    BSONObj baseOperand = BSON("$lt" << 5);
    unique_ptr<ComparisonMatchExpression> lt(new LTMatchExpression());
    ASSERT(lt->init("a", baseOperand["$lt"]).isOK());
    NotMatchExpression notOp;
    ASSERT(notOp.init(lt.release()).isOK());
    ASSERT(notOp.matchesBSON(BSON("a" << 6), NULL));
    ASSERT(!notOp.matchesBSON(BSON("a" << 4), NULL));
}

TEST(NotMatchExpression, MatchesArray) {
    BSONObj baseOperand = BSON("$lt" << 5);
    unique_ptr<ComparisonMatchExpression> lt(new LTMatchExpression());
    ASSERT(lt->init("a", baseOperand["$lt"]).isOK());
    NotMatchExpression notOp;
    ASSERT(notOp.init(lt.release()).isOK());
    ASSERT(notOp.matchesBSON(BSON("a" << BSON_ARRAY(6)), NULL));
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    // All array elements must match.
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5 << 6)), NULL));
}

TEST(NotMatchExpression, ElemMatchKey) {
    BSONObj baseOperand = BSON("$lt" << 5);
    unique_ptr<ComparisonMatchExpression> lt(new LTMatchExpression());
    ASSERT(lt->init("a", baseOperand["$lt"]).isOK());
    NotMatchExpression notOp;
    ASSERT(notOp.init(lt.release()).isOK());
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(1)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(notOp.matchesBSON(BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(notOp.matchesBSON(BSON("a" << BSON_ARRAY(6)), &details));
    // elemMatchKey is not implemented for negative match operators.
    ASSERT(!details.hasElemMatchKey());
}
/*
  TEST( NotMatchExpression, MatchesIndexKey ) {
  BSONObj baseOperand = BSON( "$lt" << 5 );
  unique_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
  ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
  NotMatchExpression notOp;
  ASSERT( notOp.init( lt.release() ).isOK() );
  IndexSpec indexSpec( BSON( "a" << 1 ) );
  BSONObj indexKey = BSON( "" << "7" );
  ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
  notOp.matchesIndexKey( indexKey, indexSpec ) );
  }
*/

/**
TEST( AndOp, MatchesElementSingleClause ) {
    BSONObj baseOperand = BSON( "$lt" << 5 );
    BSONObj match = BSON( "a" << 4 );
    BSONObj notMatch = BSON( "a" << 5 );
    unique_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
    ASSERT( lt->init( "", baseOperand[ "$lt" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( lt.release() );
    AndOp andOp;
    ASSERT( andOp.init( &subMatchExpressions ).isOK() );
    ASSERT( andOp.matchesSingleElement( match[ "a" ] ) );
    ASSERT( !andOp.matchesSingleElement( notMatch[ "a" ] ) );
}
*/

TEST(AndOp, NoClauses) {
    AndMatchExpression andMatchExpression;
    ASSERT(andMatchExpression.matchesBSON(BSONObj(), NULL));
}

TEST(AndOp, MatchesElementThreeClauses) {
    BSONObj baseOperand1 = BSON("$lt"
                                << "z1");
    BSONObj baseOperand2 = BSON("$gt"
                                << "a1");
    BSONObj match = BSON("a"
                         << "r1");
    BSONObj notMatch1 = BSON("a"
                             << "z1");
    BSONObj notMatch2 = BSON("a"
                             << "a1");
    BSONObj notMatch3 = BSON("a"
                             << "r");

    unique_ptr<ComparisonMatchExpression> sub1(new LTMatchExpression());
    ASSERT(sub1->init("a", baseOperand1["$lt"]).isOK());
    unique_ptr<ComparisonMatchExpression> sub2(new GTMatchExpression());
    ASSERT(sub2->init("a", baseOperand2["$gt"]).isOK());
    unique_ptr<RegexMatchExpression> sub3(new RegexMatchExpression());
    ASSERT(sub3->init("a", "1", "").isOK());

    AndMatchExpression andOp;
    andOp.add(sub1.release());
    andOp.add(sub2.release());
    andOp.add(sub3.release());

    ASSERT(andOp.matchesBSON(match));
    ASSERT(!andOp.matchesBSON(notMatch1));
    ASSERT(!andOp.matchesBSON(notMatch2));
    ASSERT(!andOp.matchesBSON(notMatch3));
}

TEST(AndOp, MatchesSingleClause) {
    BSONObj baseOperand = BSON("$ne" << 5);
    unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression());
    ASSERT(eq->init("a", baseOperand["$ne"]).isOK());
    unique_ptr<NotMatchExpression> ne(new NotMatchExpression());
    ASSERT(ne->init(eq.release()).isOK());

    AndMatchExpression andOp;
    andOp.add(ne.release());

    ASSERT(andOp.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(andOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), NULL));
    ASSERT(!andOp.matchesBSON(BSON("a" << 5), NULL));
    ASSERT(!andOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), NULL));
}

TEST(AndOp, MatchesThreeClauses) {
    BSONObj baseOperand1 = BSON("$gt" << 1);
    BSONObj baseOperand2 = BSON("$lt" << 10);
    BSONObj baseOperand3 = BSON("$lt" << 100);

    unique_ptr<ComparisonMatchExpression> sub1(new GTMatchExpression());
    ASSERT(sub1->init("a", baseOperand1["$gt"]).isOK());

    unique_ptr<ComparisonMatchExpression> sub2(new LTMatchExpression());
    ASSERT(sub2->init("a", baseOperand2["$lt"]).isOK());

    unique_ptr<ComparisonMatchExpression> sub3(new LTMatchExpression());
    ASSERT(sub3->init("b", baseOperand3["$lt"]).isOK());

    AndMatchExpression andOp;
    andOp.add(sub1.release());
    andOp.add(sub2.release());
    andOp.add(sub3.release());

    ASSERT(andOp.matchesBSON(BSON("a" << 5 << "b" << 6), NULL));
    ASSERT(!andOp.matchesBSON(BSON("a" << 5), NULL));
    ASSERT(!andOp.matchesBSON(BSON("b" << 6), NULL));
    ASSERT(!andOp.matchesBSON(BSON("a" << 1 << "b" << 6), NULL));
    ASSERT(!andOp.matchesBSON(BSON("a" << 10 << "b" << 6), NULL));
}

TEST(AndOp, ElemMatchKey) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);

    unique_ptr<ComparisonMatchExpression> sub1(new EqualityMatchExpression());
    ASSERT(sub1->init("a", baseOperand1["a"]).isOK());

    unique_ptr<ComparisonMatchExpression> sub2(new EqualityMatchExpression());
    ASSERT(sub2->init("b", baseOperand2["b"]).isOK());

    AndMatchExpression andOp;
    andOp.add(sub1.release());
    andOp.add(sub2.release());

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!andOp.matchesBSON(BSON("a" << BSON_ARRAY(1)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!andOp.matchesBSON(BSON("b" << BSON_ARRAY(2)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(andOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(1 << 2)), &details));
    ASSERT(details.hasElemMatchKey());
    // The elem match key for the second $and clause is recorded.
    ASSERT_EQUALS("1", details.elemMatchKey());
}

/**
TEST( AndOp, MatchesIndexKeyWithoutUnknown ) {
    BSONObj baseOperand1 = BSON( "$gt" << 1 );
    BSONObj baseOperand2 = BSON( "$lt" << 5 );
    unique_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
    ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
    ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( sub1.release() );
    subMatchExpressions.mutableVector().push_back( sub2.release() );
    AndOp andOp;
    ASSERT( andOp.init( &subMatchExpressions ).isOK() );
    IndexSpec indexSpec( BSON( "a" << 1 ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_True ==
            andOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_False ==
            andOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_False ==
            andOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
}

TEST( AndOp, MatchesIndexKeyWithUnknown ) {
    BSONObj baseOperand1 = BSON( "$gt" << 1 );
    BSONObj baseOperand2 = BSON( "$lt" << 5 );
    // This part will return PartialMatchResult_Unknown.
    BSONObj baseOperand3 = BSON( "$ne" << 5 );
    unique_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
    ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
    ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
    unique_ptr<NeOp> sub3( new NeOp() );
    ASSERT( sub3->init( "a", baseOperand3[ "$ne" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( sub1.release() );
    subMatchExpressions.mutableVector().push_back( sub2.release() );
    subMatchExpressions.mutableVector().push_back( sub3.release() );
    AndOp andOp;
    ASSERT( andOp.init( &subMatchExpressions ).isOK() );
    IndexSpec indexSpec( BSON( "a" << 1 ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
            andOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_False ==
            andOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_False ==
            andOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
}
*/

/**
TEST( OrOp, MatchesElementSingleClause ) {
    BSONObj baseOperand = BSON( "$lt" << 5 );
    BSONObj match = BSON( "a" << 4 );
    BSONObj notMatch = BSON( "a" << 5 );
    unique_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
    ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( lt.release() );
    OrOp orOp;
    ASSERT( orOp.init( &subMatchExpressions ).isOK() );
    ASSERT( orOp.matchesSingleElement( match[ "a" ] ) );
    ASSERT( !orOp.matchesSingleElement( notMatch[ "a" ] ) );
}
*/

TEST(OrOp, NoClauses) {
    OrMatchExpression orOp;
    ASSERT(!orOp.matchesBSON(BSONObj(), NULL));
}
/*
TEST( OrOp, MatchesElementThreeClauses ) {
    BSONObj baseOperand1 = BSON( "$lt" << 0 );
    BSONObj baseOperand2 = BSON( "$gt" << 10 );
    BSONObj baseOperand3 = BSON( "a" << 5 );
    BSONObj match1 = BSON( "a" << -1 );
    BSONObj match2 = BSON( "a" << 11 );
    BSONObj match3 = BSON( "a" << 5 );
    BSONObj notMatch = BSON( "a" << "6" );
    unique_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
    ASSERT( sub1->init( "a", baseOperand1[ "$lt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
    ASSERT( sub2->init( "a", baseOperand2[ "$gt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub3( new ComparisonMatchExpression() );
    ASSERT( sub3->init( "a", baseOperand3[ "a" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( sub1.release() );
    subMatchExpressions.mutableVector().push_back( sub2.release() );
    subMatchExpressions.mutableVector().push_back( sub3.release() );
    OrOp orOp;
    ASSERT( orOp.init( &subMatchExpressions ).isOK() );
    ASSERT( orOp.matchesSingleElement( match1[ "a" ] ) );
    ASSERT( orOp.matchesSingleElement( match2[ "a" ] ) );
    ASSERT( orOp.matchesSingleElement( match3[ "a" ] ) );
    ASSERT( !orOp.matchesSingleElement( notMatch[ "a" ] ) );
}
*/
TEST(OrOp, MatchesSingleClause) {
    BSONObj baseOperand = BSON("$ne" << 5);
    unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression());
    ASSERT(eq->init("a", baseOperand["$ne"]).isOK());
    unique_ptr<NotMatchExpression> ne(new NotMatchExpression());
    ASSERT(ne->init(eq.release()).isOK());

    OrMatchExpression orOp;
    orOp.add(ne.release());

    ASSERT(orOp.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(orOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), NULL));
    ASSERT(!orOp.matchesBSON(BSON("a" << 5), NULL));
    ASSERT(!orOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), NULL));
}

TEST(OrOp, MatchesThreeClauses) {
    BSONObj baseOperand1 = BSON("$gt" << 10);
    BSONObj baseOperand2 = BSON("$lt" << 0);
    BSONObj baseOperand3 = BSON("b" << 100);
    unique_ptr<ComparisonMatchExpression> sub1(new GTMatchExpression());
    ASSERT(sub1->init("a", baseOperand1["$gt"]).isOK());
    unique_ptr<ComparisonMatchExpression> sub2(new LTMatchExpression());
    ASSERT(sub2->init("a", baseOperand2["$lt"]).isOK());
    unique_ptr<ComparisonMatchExpression> sub3(new EqualityMatchExpression());
    ASSERT(sub3->init("b", baseOperand3["b"]).isOK());

    OrMatchExpression orOp;
    orOp.add(sub1.release());
    orOp.add(sub2.release());
    orOp.add(sub3.release());

    ASSERT(orOp.matchesBSON(BSON("a" << -1), NULL));
    ASSERT(orOp.matchesBSON(BSON("a" << 11), NULL));
    ASSERT(!orOp.matchesBSON(BSON("a" << 5), NULL));
    ASSERT(orOp.matchesBSON(BSON("b" << 100), NULL));
    ASSERT(!orOp.matchesBSON(BSON("b" << 101), NULL));
    ASSERT(!orOp.matchesBSON(BSONObj(), NULL));
    ASSERT(orOp.matchesBSON(BSON("a" << 11 << "b" << 100), NULL));
}

TEST(OrOp, ElemMatchKey) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    unique_ptr<ComparisonMatchExpression> sub1(new EqualityMatchExpression());
    ASSERT(sub1->init("a", baseOperand1["a"]).isOK());
    unique_ptr<ComparisonMatchExpression> sub2(new EqualityMatchExpression());
    ASSERT(sub2->init("b", baseOperand2["b"]).isOK());

    OrMatchExpression orOp;
    orOp.add(sub1.release());
    orOp.add(sub2.release());

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!orOp.matchesBSON(BSONObj(), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!orOp.matchesBSON(BSON("a" << BSON_ARRAY(10) << "b" << BSON_ARRAY(10)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(orOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(1 << 2)), &details));
    // The elem match key feature is not implemented for $or.
    ASSERT(!details.hasElemMatchKey());
}

/**
TEST( OrOp, MatchesIndexKeyWithoutUnknown ) {
    BSONObj baseOperand1 = BSON( "$gt" << 5 );
    BSONObj baseOperand2 = BSON( "$lt" << 1 );
    unique_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
    ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
    ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( sub1.release() );
    subMatchExpressions.mutableVector().push_back( sub2.release() );
    OrOp orOp;
    ASSERT( orOp.init( &subMatchExpressions ).isOK() );
    IndexSpec indexSpec( BSON( "a" << 1 ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_False ==
            orOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_True ==
            orOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_True ==
            orOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
}

TEST( OrOp, MatchesIndexKeyWithUnknown ) {
    BSONObj baseOperand1 = BSON( "$gt" << 5 );
    BSONObj baseOperand2 = BSON( "$lt" << 1 );
    // This part will return PartialMatchResult_Unknown.
    BSONObj baseOperand3 = BSON( "$ne" << 5 );
    unique_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
    ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
    ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
    unique_ptr<NeOp> sub3( new NeOp() );
    ASSERT( sub3->init( "a", baseOperand3[ "$ne" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( sub1.release() );
    subMatchExpressions.mutableVector().push_back( sub2.release() );
    subMatchExpressions.mutableVector().push_back( sub3.release() );
    OrOp orOp;
    ASSERT( orOp.init( &subMatchExpressions ).isOK() );
    IndexSpec indexSpec( BSON( "a" << 1 ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
            orOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_True ==
            orOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
    ASSERT( MatchMatchExpression::PartialMatchResult_True ==
            orOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
}
*/

/**
TEST( NorOp, MatchesElementSingleClause ) {
    BSONObj baseOperand = BSON( "$lt" << 5 );
    BSONObj match = BSON( "a" << 5 );
    BSONObj notMatch = BSON( "a" << 4 );
    unique_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
    ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( lt.release() );
    NorOp norOp;
    ASSERT( norOp.init( &subMatchExpressions ).isOK() );
    ASSERT( norOp.matchesSingleElement( match[ "a" ] ) );
    ASSERT( !norOp.matchesSingleElement( notMatch[ "a" ] ) );
}
*/

TEST(NorOp, NoClauses) {
    NorMatchExpression norOp;
    ASSERT(norOp.matchesBSON(BSONObj(), NULL));
}
/*
TEST( NorOp, MatchesElementThreeClauses ) {
    BSONObj baseOperand1 = BSON( "$lt" << 0 );
    BSONObj baseOperand2 = BSON( "$gt" << 10 );
    BSONObj baseOperand3 = BSON( "a" << 5 );
    BSONObj notMatch1 = BSON( "a" << -1 );
    BSONObj notMatch2 = BSON( "a" << 11 );
    BSONObj notMatch3 = BSON( "a" << 5 );
    BSONObj match = BSON( "a" << "6" );
    unique_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
    ASSERT( sub1->init( "a", baseOperand1[ "$lt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
    ASSERT( sub2->init( "a", baseOperand2[ "$gt" ] ).isOK() );
    unique_ptr<ComparisonMatchExpression> sub3( new ComparisonMatchExpression() );
    ASSERT( sub3->init( "a", baseOperand3[ "a" ] ).isOK() );
    OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
    subMatchExpressions.mutableVector().push_back( sub1.release() );
    subMatchExpressions.mutableVector().push_back( sub2.release() );
    subMatchExpressions.mutableVector().push_back( sub3.release() );
    NorOp norOp;
    ASSERT( norOp.init( &subMatchExpressions ).isOK() );
    ASSERT( !norOp.matchesSingleElement( notMatch1[ "a" ] ) );
    ASSERT( !norOp.matchesSingleElement( notMatch2[ "a" ] ) );
    ASSERT( !norOp.matchesSingleElement( notMatch3[ "a" ] ) );
    ASSERT( norOp.matchesSingleElement( match[ "a" ] ) );
}
*/

TEST(NorOp, MatchesSingleClause) {
    BSONObj baseOperand = BSON("$ne" << 5);
    unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression());
    ASSERT(eq->init("a", baseOperand["$ne"]).isOK());
    unique_ptr<NotMatchExpression> ne(new NotMatchExpression());
    ASSERT(ne->init(eq.release()).isOK());

    NorMatchExpression norOp;
    norOp.add(ne.release());

    ASSERT(!norOp.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(!norOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), NULL));
    ASSERT(norOp.matchesBSON(BSON("a" << 5), NULL));
    ASSERT(norOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), NULL));
}

TEST(NorOp, MatchesThreeClauses) {
    BSONObj baseOperand1 = BSON("$gt" << 10);
    BSONObj baseOperand2 = BSON("$lt" << 0);
    BSONObj baseOperand3 = BSON("b" << 100);

    unique_ptr<ComparisonMatchExpression> sub1(new GTMatchExpression());
    ASSERT(sub1->init("a", baseOperand1["$gt"]).isOK());
    unique_ptr<ComparisonMatchExpression> sub2(new LTMatchExpression());
    ASSERT(sub2->init("a", baseOperand2["$lt"]).isOK());
    unique_ptr<ComparisonMatchExpression> sub3(new EqualityMatchExpression());
    ASSERT(sub3->init("b", baseOperand3["b"]).isOK());

    NorMatchExpression norOp;
    norOp.add(sub1.release());
    norOp.add(sub2.release());
    norOp.add(sub3.release());

    ASSERT(!norOp.matchesBSON(BSON("a" << -1), NULL));
    ASSERT(!norOp.matchesBSON(BSON("a" << 11), NULL));
    ASSERT(norOp.matchesBSON(BSON("a" << 5), NULL));
    ASSERT(!norOp.matchesBSON(BSON("b" << 100), NULL));
    ASSERT(norOp.matchesBSON(BSON("b" << 101), NULL));
    ASSERT(norOp.matchesBSON(BSONObj(), NULL));
    ASSERT(!norOp.matchesBSON(BSON("a" << 11 << "b" << 100), NULL));
}

TEST(NorOp, ElemMatchKey) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    unique_ptr<ComparisonMatchExpression> sub1(new EqualityMatchExpression());
    ASSERT(sub1->init("a", baseOperand1["a"]).isOK());
    unique_ptr<ComparisonMatchExpression> sub2(new EqualityMatchExpression());
    ASSERT(sub2->init("b", baseOperand2["b"]).isOK());

    NorMatchExpression norOp;
    norOp.add(sub1.release());
    norOp.add(sub2.release());

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!norOp.matchesBSON(BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!norOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(10)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(norOp.matchesBSON(BSON("a" << BSON_ARRAY(3) << "b" << BSON_ARRAY(4)), &details));
    // The elem match key feature is not implemented for $nor.
    ASSERT(!details.hasElemMatchKey());
}


TEST(NorOp, Equivalent) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    EqualityMatchExpression sub1;
    ASSERT(sub1.init("a", baseOperand1["a"]).isOK());
    EqualityMatchExpression sub2;
    ASSERT(sub2.init("b", baseOperand2["b"]).isOK());

    NorMatchExpression e1;
    e1.add(sub1.shallowClone().release());
    e1.add(sub2.shallowClone().release());

    NorMatchExpression e2;
    e2.add(sub1.shallowClone().release());

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
}
}

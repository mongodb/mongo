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
 */

/** Unit tests for MatchMatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    TEST( NotMatchExpression, MatchesScalar ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "a", ComparisonMatchExpression::LT, baseOperand[ "$lt" ] ).isOK() );
        NotMatchExpression notOp;
        ASSERT( notOp.init( lt.release() ).isOK() );
        ASSERT( notOp.matches( BSON( "a" << 6 ), NULL ) );
        ASSERT( !notOp.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( NotMatchExpression, MatchesArray ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "a", ComparisonMatchExpression::LT, baseOperand[ "$lt" ] ).isOK() );
        NotMatchExpression notOp;
        ASSERT( notOp.init( lt.release() ).isOK() );
        ASSERT( notOp.matches( BSON( "a" << BSON_ARRAY( 6 ) ), NULL ) );
        ASSERT( !notOp.matches( BSON( "a" << BSON_ARRAY( 4 ) ), NULL ) );
        // All array elements must match.
        ASSERT( !notOp.matches( BSON( "a" << BSON_ARRAY( 4 << 5 << 6 ) ), NULL ) );
    }

    TEST( NotMatchExpression, ElemMatchKey ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "a", ComparisonMatchExpression::LT, baseOperand[ "$lt" ] ).isOK() );
        NotMatchExpression notOp;
        ASSERT( notOp.init( lt.release() ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !notOp.matches( BSON( "a" << BSON_ARRAY( 1 ) ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( notOp.matches( BSON( "a" << 6 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( notOp.matches( BSON( "a" << BSON_ARRAY( 6 ) ), &details ) );
        // elemMatchKey is not implemented for negative match operators.
        ASSERT( !details.hasElemMatchKey() );
    }
    /*
      TEST( NotMatchExpression, MatchesIndexKey ) {
      BSONObj baseOperand = BSON( "$lt" << 5 );
      auto_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
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
        auto_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "", baseOperand[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
        subMatchExpressions.mutableVector().push_back( lt.release() );
        AndOp andOp;
        ASSERT( andOp.init( &subMatchExpressions ).isOK() );
        ASSERT( andOp.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !andOp.matchesSingleElement( notMatch[ "a" ] ) );
    }
    */

    TEST( AndOp, NoClauses ) {
        AndMatchExpression andMatchExpression;
        ASSERT( andMatchExpression.matches( BSONObj(), NULL ) );
    }

    TEST( AndOp, MatchesElementThreeClauses ) {
        BSONObj baseOperand1 = BSON( "$lt" << "z1" );
        BSONObj baseOperand2 = BSON( "$gt" << "a1" );
        BSONObj match = BSON( "a" << "r1" );
        BSONObj notMatch1 = BSON( "a" << "z1" );
        BSONObj notMatch2 = BSON( "a" << "a1" );
        BSONObj notMatch3 = BSON( "a" << "r" );

        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", ComparisonMatchExpression::LT, baseOperand1[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", ComparisonMatchExpression::GT, baseOperand2[ "$gt" ] ).isOK() );
        auto_ptr<RegexMatchExpression> sub3( new RegexMatchExpression() );
        ASSERT( sub3->init( "a", "1", "" ).isOK() );

        AndMatchExpression andOp;
        andOp.add( sub1.release() );
        andOp.add( sub2.release() );
        andOp.add( sub3.release() );

        ASSERT( andOp.matches( match ) );
        ASSERT( !andOp.matches( notMatch1 ) );
        ASSERT( !andOp.matches( notMatch2 ) );
        ASSERT( !andOp.matches( notMatch3 ) );
    }

    TEST( AndOp, MatchesSingleClause ) {
        BSONObj baseOperand = BSON( "$ne" << 5 );
        auto_ptr<ComparisonMatchExpression> ne( new ComparisonMatchExpression() );
        ASSERT( ne->init( "a", ComparisonMatchExpression::NE, baseOperand[ "$ne" ] ).isOK() );

        AndMatchExpression andOp;
        andOp.add( ne.release() );

        ASSERT( andOp.matches( BSON( "a" << 4 ), NULL ) );
        ASSERT( andOp.matches( BSON( "a" << BSON_ARRAY( 4 << 6 ) ), NULL ) );
        ASSERT( !andOp.matches( BSON( "a" << 5 ), NULL ) );
        ASSERT( !andOp.matches( BSON( "a" << BSON_ARRAY( 4 << 5 ) ), NULL ) );
    }

    TEST( AndOp, MatchesThreeClauses ) {
        BSONObj baseOperand1 = BSON( "$gt" << 1 );
        BSONObj baseOperand2 = BSON( "$lt" << 10 );
        BSONObj baseOperand3 = BSON( "$lt" << 100 );

        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", ComparisonMatchExpression::GT, baseOperand1[ "$gt" ] ).isOK() );

        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", ComparisonMatchExpression::LT, baseOperand2[ "$lt" ] ).isOK() );

        auto_ptr<ComparisonMatchExpression> sub3( new ComparisonMatchExpression() );
        ASSERT( sub3->init( "b", ComparisonMatchExpression::LT, baseOperand3[ "$lt" ] ).isOK() );

        AndMatchExpression andOp;
        andOp.add( sub1.release() );
        andOp.add( sub2.release() );
        andOp.add( sub3.release() );

        ASSERT( andOp.matches( BSON( "a" << 5 << "b" << 6 ), NULL ) );
        ASSERT( !andOp.matches( BSON( "a" << 5 ), NULL ) );
        ASSERT( !andOp.matches( BSON( "b" << 6 ), NULL ) );
        ASSERT( !andOp.matches( BSON( "a" << 1 << "b" << 6 ), NULL ) );
        ASSERT( !andOp.matches( BSON( "a" << 10 << "b" << 6 ), NULL ) );
    }

    TEST( AndOp, ElemMatchKey ) {
        BSONObj baseOperand1 = BSON( "a" << 1 );
        BSONObj baseOperand2 = BSON( "b" << 2 );

        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", ComparisonMatchExpression::EQ, baseOperand1[ "a" ] ).isOK() );

        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "b", ComparisonMatchExpression::EQ, baseOperand2[ "b" ] ).isOK() );

        AndMatchExpression andOp;
        andOp.add( sub1.release() );
        andOp.add( sub2.release() );

        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !andOp.matches( BSON( "a" << BSON_ARRAY( 1 ) ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( !andOp.matches( BSON( "b" << BSON_ARRAY( 2 ) ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( andOp.matches( BSON( "a" << BSON_ARRAY( 1 ) << "b" << BSON_ARRAY( 1 << 2 ) ),
                               &details ) );
        ASSERT( details.hasElemMatchKey() );
        // The elem match key for the second $and clause is recorded.
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    /**
    TEST( AndOp, MatchesIndexKeyWithoutUnknown ) {
        BSONObj baseOperand1 = BSON( "$gt" << 1 );
        BSONObj baseOperand2 = BSON( "$lt" << 5 );
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
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
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<NeOp> sub3( new NeOp() );
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
        auto_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
        subMatchExpressions.mutableVector().push_back( lt.release() );
        OrOp orOp;
        ASSERT( orOp.init( &subMatchExpressions ).isOK() );
        ASSERT( orOp.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !orOp.matchesSingleElement( notMatch[ "a" ] ) );
    }
    */

    TEST( OrOp, NoClauses ) {
        OrMatchExpression orOp;
        ASSERT( !orOp.matches( BSONObj(), NULL ) );
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
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub3( new ComparisonMatchExpression() );
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
    TEST( OrOp, MatchesSingleClause ) {
        BSONObj baseOperand = BSON( "$ne" << 5 );
        auto_ptr<ComparisonMatchExpression> ne( new ComparisonMatchExpression() );
        ASSERT( ne->init( "a", ComparisonMatchExpression::NE, baseOperand[ "$ne" ] ).isOK() );

        OrMatchExpression orOp;
        orOp.add( ne.release() );

        ASSERT( orOp.matches( BSON( "a" << 4 ), NULL ) );
        ASSERT( orOp.matches( BSON( "a" << BSON_ARRAY( 4 << 6 ) ), NULL ) );
        ASSERT( !orOp.matches( BSON( "a" << 5 ), NULL ) );
        ASSERT( !orOp.matches( BSON( "a" << BSON_ARRAY( 4 << 5 ) ), NULL ) );
    }

    TEST( OrOp, MatchesThreeClauses ) {
        BSONObj baseOperand1 = BSON( "$gt" << 10 );
        BSONObj baseOperand2 = BSON( "$lt" << 0 );
        BSONObj baseOperand3 = BSON( "b" << 100 );
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", ComparisonMatchExpression::GT, baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", ComparisonMatchExpression::LT, baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub3( new ComparisonMatchExpression() );
        ASSERT( sub3->init( "b", ComparisonMatchExpression::EQ, baseOperand3[ "b" ] ).isOK() );

        OrMatchExpression orOp;
        orOp.add( sub1.release() );
        orOp.add( sub2.release() );
        orOp.add( sub3.release() );

        ASSERT( orOp.matches( BSON( "a" << -1 ), NULL ) );
        ASSERT( orOp.matches( BSON( "a" << 11 ), NULL ) );
        ASSERT( !orOp.matches( BSON( "a" << 5 ), NULL ) );
        ASSERT( orOp.matches( BSON( "b" << 100 ), NULL ) );
        ASSERT( !orOp.matches( BSON( "b" << 101 ), NULL ) );
        ASSERT( !orOp.matches( BSONObj(), NULL ) );
        ASSERT( orOp.matches( BSON( "a" << 11 << "b" << 100 ), NULL ) );
    }

    TEST( OrOp, ElemMatchKey ) {
        BSONObj baseOperand1 = BSON( "a" << 1 );
        BSONObj baseOperand2 = BSON( "b" << 2 );
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", ComparisonMatchExpression::EQ, baseOperand1[ "a" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "b", ComparisonMatchExpression::EQ, baseOperand2[ "b" ] ).isOK() );

        OrMatchExpression orOp;
        orOp.add( sub1.release() );
        orOp.add( sub2.release() );

        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !orOp.matches( BSONObj(), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( !orOp.matches( BSON( "a" << BSON_ARRAY( 10 ) << "b" << BSON_ARRAY( 10 ) ),
                               &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( orOp.matches( BSON( "a" << BSON_ARRAY( 1 ) << "b" << BSON_ARRAY( 1 << 2 ) ),
                              &details ) );
        // The elem match key feature is not implemented for $or.
        ASSERT( !details.hasElemMatchKey() );
    }

    /**
    TEST( OrOp, MatchesIndexKeyWithoutUnknown ) {
        BSONObj baseOperand1 = BSON( "$gt" << 5 );
        BSONObj baseOperand2 = BSON( "$lt" << 1 );
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
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
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<NeOp> sub3( new NeOp() );
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
        auto_ptr<ComparisonMatchExpression> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
        subMatchExpressions.mutableVector().push_back( lt.release() );
        NorOp norOp;
        ASSERT( norOp.init( &subMatchExpressions ).isOK() );
        ASSERT( norOp.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !norOp.matchesSingleElement( notMatch[ "a" ] ) );
    }
    */

    TEST( NorOp, NoClauses ) {
        NorMatchExpression norOp;
        ASSERT( norOp.matches( BSONObj(), NULL ) );
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
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub3( new ComparisonMatchExpression() );
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

    TEST( NorOp, MatchesSingleClause ) {
        BSONObj baseOperand = BSON( "$ne" << 5 );
        auto_ptr<ComparisonMatchExpression> ne( new ComparisonMatchExpression() );
        ASSERT( ne->init( "a", ComparisonMatchExpression::NE, baseOperand[ "$ne" ] ).isOK() );

        NorMatchExpression norOp;
        norOp.add( ne.release() );

        ASSERT( !norOp.matches( BSON( "a" << 4 ), NULL ) );
        ASSERT( !norOp.matches( BSON( "a" << BSON_ARRAY( 4 << 6 ) ), NULL ) );
        ASSERT( norOp.matches( BSON( "a" << 5 ), NULL ) );
        ASSERT( norOp.matches( BSON( "a" << BSON_ARRAY( 4 << 5 ) ), NULL ) );
    }

    TEST( NorOp, MatchesThreeClauses ) {
        BSONObj baseOperand1 = BSON( "$gt" << 10 );
        BSONObj baseOperand2 = BSON( "$lt" << 0 );
        BSONObj baseOperand3 = BSON( "b" << 100 );

        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", ComparisonMatchExpression::GT, baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "a", ComparisonMatchExpression::LT, baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub3( new ComparisonMatchExpression() );
        ASSERT( sub3->init( "b", ComparisonMatchExpression::EQ, baseOperand3[ "b" ] ).isOK() );

        NorMatchExpression norOp;
        norOp.add( sub1.release() );
        norOp.add( sub2.release() );
        norOp.add( sub3.release() );

        ASSERT( !norOp.matches( BSON( "a" << -1 ), NULL ) );
        ASSERT( !norOp.matches( BSON( "a" << 11 ), NULL ) );
        ASSERT( norOp.matches( BSON( "a" << 5 ), NULL ) );
        ASSERT( !norOp.matches( BSON( "b" << 100 ), NULL ) );
        ASSERT( norOp.matches( BSON( "b" << 101 ), NULL ) );
        ASSERT( norOp.matches( BSONObj(), NULL ) );
        ASSERT( !norOp.matches( BSON( "a" << 11 << "b" << 100 ), NULL ) );
    }

    TEST( NorOp, ElemMatchKey ) {
        BSONObj baseOperand1 = BSON( "a" << 1 );
        BSONObj baseOperand2 = BSON( "b" << 2 );
        auto_ptr<ComparisonMatchExpression> sub1( new ComparisonMatchExpression() );
        ASSERT( sub1->init( "a", ComparisonMatchExpression::EQ, baseOperand1[ "a" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> sub2( new ComparisonMatchExpression() );
        ASSERT( sub2->init( "b", ComparisonMatchExpression::EQ, baseOperand2[ "b" ] ).isOK() );

        NorMatchExpression norOp;
        norOp.add( sub1.release() );
        norOp.add( sub2.release() );

        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !norOp.matches( BSON( "a" << 1 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( !norOp.matches( BSON( "a" << BSON_ARRAY( 1 ) << "b" << BSON_ARRAY( 10 ) ),
                                &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( norOp.matches( BSON( "a" << BSON_ARRAY( 3 ) << "b" << BSON_ARRAY( 4 ) ),
                               &details ) );
        // The elem match key feature is not implemented for $nor.
        ASSERT( !details.hasElemMatchKey() );
    }

    /**
    TEST( NorOp, MatchesIndexKey ) {
        BSONObj baseOperand = BSON( "a" << 5 );
        auto_ptr<ComparisonMatchExpression> eq( new ComparisonMatchExpression() );
        ASSERT( eq->init( "a", baseOperand[ "a" ] ).isOK() );
        OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
        subMatchExpressions.mutableVector().push_back( eq.release() );
        NorOp norOp;
        ASSERT( norOp.init( &subMatchExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "7" );
        ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
                norOp.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

}

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

/** Unit tests for MatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    TEST( NotExpression, MatchesScalar ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
        ASSERT( lt->init( "a", ComparisonExpression::LT, baseOperand[ "$lt" ] ).isOK() );
        NotExpression notOp;
        ASSERT( notOp.init( lt.release() ).isOK() );
        ASSERT( notOp.matches( BSON( "a" << 6 ), NULL ) );
        ASSERT( !notOp.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( NotExpression, MatchesArray ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
        ASSERT( lt->init( "a", ComparisonExpression::LT, baseOperand[ "$lt" ] ).isOK() );
        NotExpression notOp;
        ASSERT( notOp.init( lt.release() ).isOK() );
        ASSERT( notOp.matches( BSON( "a" << BSON_ARRAY( 6 ) ), NULL ) );
        ASSERT( !notOp.matches( BSON( "a" << BSON_ARRAY( 4 ) ), NULL ) );
        // All array elements must match.
        ASSERT( !notOp.matches( BSON( "a" << BSON_ARRAY( 4 << 5 << 6 ) ), NULL ) );
    }

    TEST( NotExpression, ElemMatchKey ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
        ASSERT( lt->init( "a", ComparisonExpression::LT, baseOperand[ "$lt" ] ).isOK() );
        NotExpression notOp;
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
      TEST( NotExpression, MatchesIndexKey ) {
      BSONObj baseOperand = BSON( "$lt" << 5 );
      auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
      ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
      NotExpression notOp;
      ASSERT( notOp.init( lt.release() ).isOK() );
      IndexSpec indexSpec( BSON( "a" << 1 ) );
      BSONObj indexKey = BSON( "" << "7" );
      ASSERT( MatchExpression::PartialMatchResult_Unknown ==
      notOp.matchesIndexKey( indexKey, indexSpec ) );
      }
    */

    /**
    TEST( AndOp, MatchesElementSingleClause ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        BSONObj match = BSON( "a" << 4 );
        BSONObj notMatch = BSON( "a" << 5 );
        auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
        ASSERT( lt->init( "", baseOperand[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( lt.release() );
        AndOp andOp;
        ASSERT( andOp.init( &subExpressions ).isOK() );
        ASSERT( andOp.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !andOp.matchesSingleElement( notMatch[ "a" ] ) );
    }
    */

    TEST( AndOp, NoClauses ) {
        AndExpression andExpression;
        ASSERT( andExpression.matches( BSONObj(), NULL ) );
    }

    TEST( AndOp, MatchesElementThreeClauses ) {
        BSONObj baseOperand1 = BSON( "$lt" << "z1" );
        BSONObj baseOperand2 = BSON( "$gt" << "a1" );
        BSONObj match = BSON( "a" << "r1" );
        BSONObj notMatch1 = BSON( "a" << "z1" );
        BSONObj notMatch2 = BSON( "a" << "a1" );
        BSONObj notMatch3 = BSON( "a" << "r" );

        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", ComparisonExpression::LT, baseOperand1[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", ComparisonExpression::GT, baseOperand2[ "$gt" ] ).isOK() );
        auto_ptr<RegexExpression> sub3( new RegexExpression() );
        ASSERT( sub3->init( "a", "1", "" ).isOK() );

        AndExpression andOp;
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
        auto_ptr<ComparisonExpression> ne( new ComparisonExpression() );
        ASSERT( ne->init( "a", ComparisonExpression::NE, baseOperand[ "$ne" ] ).isOK() );

        AndExpression andOp;
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

        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", ComparisonExpression::GT, baseOperand1[ "$gt" ] ).isOK() );

        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", ComparisonExpression::LT, baseOperand2[ "$lt" ] ).isOK() );

        auto_ptr<ComparisonExpression> sub3( new ComparisonExpression() );
        ASSERT( sub3->init( "b", ComparisonExpression::LT, baseOperand3[ "$lt" ] ).isOK() );

        AndExpression andOp;
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

        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", ComparisonExpression::EQ, baseOperand1[ "a" ] ).isOK() );

        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "b", ComparisonExpression::EQ, baseOperand2[ "b" ] ).isOK() );

        AndExpression andOp;
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
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( sub1.release() );
        subExpressions.mutableVector().push_back( sub2.release() );
        AndOp andOp;
        ASSERT( andOp.init( &subExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                andOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                andOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                andOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
    }

    TEST( AndOp, MatchesIndexKeyWithUnknown ) {
        BSONObj baseOperand1 = BSON( "$gt" << 1 );
        BSONObj baseOperand2 = BSON( "$lt" << 5 );
        // This part will return PartialMatchResult_Unknown.
        BSONObj baseOperand3 = BSON( "$ne" << 5 );
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<NeOp> sub3( new NeOp() );
        ASSERT( sub3->init( "a", baseOperand3[ "$ne" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( sub1.release() );
        subExpressions.mutableVector().push_back( sub2.release() );
        subExpressions.mutableVector().push_back( sub3.release() );
        AndOp andOp;
        ASSERT( andOp.init( &subExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                andOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                andOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                andOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
    }
    */

    /**
    TEST( OrOp, MatchesElementSingleClause ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        BSONObj match = BSON( "a" << 4 );
        BSONObj notMatch = BSON( "a" << 5 );
        auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( lt.release() );
        OrOp orOp;
        ASSERT( orOp.init( &subExpressions ).isOK() );
        ASSERT( orOp.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !orOp.matchesSingleElement( notMatch[ "a" ] ) );
    }
    */

    TEST( OrOp, NoClauses ) {
        OrExpression orOp;
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
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub3( new ComparisonExpression() );
        ASSERT( sub3->init( "a", baseOperand3[ "a" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( sub1.release() );
        subExpressions.mutableVector().push_back( sub2.release() );
        subExpressions.mutableVector().push_back( sub3.release() );
        OrOp orOp;
        ASSERT( orOp.init( &subExpressions ).isOK() );
        ASSERT( orOp.matchesSingleElement( match1[ "a" ] ) );
        ASSERT( orOp.matchesSingleElement( match2[ "a" ] ) );
        ASSERT( orOp.matchesSingleElement( match3[ "a" ] ) );
        ASSERT( !orOp.matchesSingleElement( notMatch[ "a" ] ) );
    }
    */
    TEST( OrOp, MatchesSingleClause ) {
        BSONObj baseOperand = BSON( "$ne" << 5 );
        auto_ptr<ComparisonExpression> ne( new ComparisonExpression() );
        ASSERT( ne->init( "a", ComparisonExpression::NE, baseOperand[ "$ne" ] ).isOK() );

        OrExpression orOp;
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
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", ComparisonExpression::GT, baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", ComparisonExpression::LT, baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub3( new ComparisonExpression() );
        ASSERT( sub3->init( "b", ComparisonExpression::EQ, baseOperand3[ "b" ] ).isOK() );

        OrExpression orOp;
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
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", ComparisonExpression::EQ, baseOperand1[ "a" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "b", ComparisonExpression::EQ, baseOperand2[ "b" ] ).isOK() );

        OrExpression orOp;
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
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( sub1.release() );
        subExpressions.mutableVector().push_back( sub2.release() );
        OrOp orOp;
        ASSERT( orOp.init( &subExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                orOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                orOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                orOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
    }
    
    TEST( OrOp, MatchesIndexKeyWithUnknown ) {
        BSONObj baseOperand1 = BSON( "$gt" << 5 );
        BSONObj baseOperand2 = BSON( "$lt" << 1 );
        // This part will return PartialMatchResult_Unknown.
        BSONObj baseOperand3 = BSON( "$ne" << 5 );
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<NeOp> sub3( new NeOp() );
        ASSERT( sub3->init( "a", baseOperand3[ "$ne" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( sub1.release() );
        subExpressions.mutableVector().push_back( sub2.release() );
        subExpressions.mutableVector().push_back( sub3.release() );
        OrOp orOp;
        ASSERT( orOp.init( &subExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                orOp.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                orOp.matchesIndexKey( BSON( "" << 0 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                orOp.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
    }
    */

    /**
    TEST( NorOp, MatchesElementSingleClause ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        BSONObj match = BSON( "a" << 5 );
        BSONObj notMatch = BSON( "a" << 4 );
        auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( lt.release() );
        NorOp norOp;
        ASSERT( norOp.init( &subExpressions ).isOK() );
        ASSERT( norOp.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !norOp.matchesSingleElement( notMatch[ "a" ] ) );
    }
    */

    TEST( NorOp, NoClauses ) {
        NorExpression norOp;
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
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", baseOperand1[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", baseOperand2[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub3( new ComparisonExpression() );
        ASSERT( sub3->init( "a", baseOperand3[ "a" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( sub1.release() );
        subExpressions.mutableVector().push_back( sub2.release() );
        subExpressions.mutableVector().push_back( sub3.release() );
        NorOp norOp;
        ASSERT( norOp.init( &subExpressions ).isOK() );
        ASSERT( !norOp.matchesSingleElement( notMatch1[ "a" ] ) );
        ASSERT( !norOp.matchesSingleElement( notMatch2[ "a" ] ) );
        ASSERT( !norOp.matchesSingleElement( notMatch3[ "a" ] ) );
        ASSERT( norOp.matchesSingleElement( match[ "a" ] ) );
    }
    */

    TEST( NorOp, MatchesSingleClause ) {
        BSONObj baseOperand = BSON( "$ne" << 5 );
        auto_ptr<ComparisonExpression> ne( new ComparisonExpression() );
        ASSERT( ne->init( "a", ComparisonExpression::NE, baseOperand[ "$ne" ] ).isOK() );

        NorExpression norOp;
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

        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", ComparisonExpression::GT, baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "a", ComparisonExpression::LT, baseOperand2[ "$lt" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub3( new ComparisonExpression() );
        ASSERT( sub3->init( "b", ComparisonExpression::EQ, baseOperand3[ "b" ] ).isOK() );

        NorExpression norOp;
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
        auto_ptr<ComparisonExpression> sub1( new ComparisonExpression() );
        ASSERT( sub1->init( "a", ComparisonExpression::EQ, baseOperand1[ "a" ] ).isOK() );
        auto_ptr<ComparisonExpression> sub2( new ComparisonExpression() );
        ASSERT( sub2->init( "b", ComparisonExpression::EQ, baseOperand2[ "b" ] ).isOK() );

        NorExpression norOp;
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
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "a", baseOperand[ "a" ] ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( eq.release() );
        NorOp norOp;
        ASSERT( norOp.init( &subExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "7" );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                norOp.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

}

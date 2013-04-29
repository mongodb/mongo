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
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    TEST( EqOp, MatchesElement ) {
        BSONObj operand = BSON( "a" << 5 );
        BSONObj match = BSON( "a" << 5.0 );
        BSONObj notMatch = BSON( "a" << 6 );

        ComparisonExpression eq;
        eq.init( "", ComparisonExpression::EQ, operand["a"] );
        ASSERT( eq.matchesSingleElement( match.firstElement() ) );
        ASSERT( !eq.matchesSingleElement( notMatch.firstElement() ) );
    }


    TEST( EqOp, InvalidEooOperand ) {
        BSONObj operand;
        ComparisonExpression eq;
        ASSERT( !eq.init( "", ComparisonExpression::EQ, operand.firstElement() ).isOK() );
    }

    TEST( EqOp, MatchesScalar ) {
        BSONObj operand = BSON( "a" << 5 );
        ComparisonExpression eq;
        eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] );
        ASSERT( eq.matches( BSON( "a" << 5.0 ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( EqOp, MatchesArrayValue ) {
        BSONObj operand = BSON( "a" << 5 );
        ComparisonExpression eq;
        eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] );
        ASSERT( eq.matches( BSON( "a" << BSON_ARRAY( 5.0 << 6 ) ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << BSON_ARRAY( 6 << 7 ) ), NULL ) );
    }

    TEST( EqOp, MatchesReferencedObjectValue ) {
        BSONObj operand = BSON( "a.b" << 5 );
        ComparisonExpression eq;
        eq.init( "a.b", ComparisonExpression::EQ, operand[ "a.b" ] );
        ASSERT( eq.matches( BSON( "a" << BSON( "b" << 5 ) ), NULL ) );
        ASSERT( eq.matches( BSON( "a" << BSON( "b" << BSON_ARRAY( 5 ) ) ), NULL ) );
        ASSERT( eq.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << 5 ) ) ), NULL ) );
    }

    TEST( EqOp, MatchesReferencedArrayValue ) {
        BSONObj operand = BSON( "a.0" << 5 );
        ComparisonExpression eq;
        eq.init( "a.0", ComparisonExpression::EQ, operand[ "a.0" ] );
        ASSERT( eq.matches( BSON( "a" << BSON_ARRAY( 5 ) ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << BSON_ARRAY( BSON_ARRAY( 5 ) ) ), NULL ) );
    }

    TEST( EqOp, MatchesNull ) {
        BSONObj operand = BSON( "a" << BSONNULL );
        ComparisonExpression eq;
        eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] );
        ASSERT( eq.matches( BSONObj(), NULL ) );
        ASSERT( eq.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( EqOp, MatchesMinKey ) {
        BSONObj operand = BSON( "a" << MinKey );
        ComparisonExpression eq;
        eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] );
        ASSERT( eq.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << 4 ), NULL ) );
    }



    TEST( EqOp, MatchesMaxKey ) {
        BSONObj operand = BSON( "a" << MaxKey );
        ComparisonExpression eq;
        ASSERT( eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] ).isOK() );
        ASSERT( eq.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( EqOp, MatchesFullArray ) {
        BSONObj operand = BSON( "a" << BSON_ARRAY( 1 << 2 ) );
        ComparisonExpression eq;
        ASSERT( eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] ).isOK() );
        ASSERT( eq.matches( BSON( "a" << BSON_ARRAY( 1 << 2 ) ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 3 ) ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << BSON_ARRAY( 1 ) ), NULL ) );
        ASSERT( !eq.matches( BSON( "a" << 1 ), NULL ) );
    }

    TEST( EqOp, ElemMatchKey ) {
        BSONObj operand = BSON( "a" << 5 );
        ComparisonExpression eq;
        ASSERT( eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !eq.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( eq.matches( BSON( "a" << 5 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( eq.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 5 ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "2", details.elemMatchKey() );
    }

    /**
       TEST( EqOp, MatchesIndexKeyScalar ) {
       BSONObj operand = BSON( "a" << 6 );
       ComparisonExpression eq;
       ASSERT( eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       eq.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       eq.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       eq.matchesIndexKey( BSON( "" << BSON_ARRAY( 6 ) ), indexSpec ) );
       }

       TEST( EqOp, MatchesIndexKeyMissing ) {
       BSONObj operand = BSON( "a" << 6 );
       ComparisonExpression eq;
       ASSERT( eq.init( "a", ComparisonExpression::EQ, operand[ "a" ] ).isOK() );
       IndexSpec indexSpec( BSON( "b" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       eq.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       eq.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       eq.matchesIndexKey( BSON( "" << BSON_ARRAY( 8 << 6 ) ), indexSpec ) );
       }

       TEST( EqOp, MatchesIndexKeyArray ) {
       BSONObj operand = BSON( "a" << BSON_ARRAY( 4 << 5 ) );
       ComparisonExpression eq
       ASSERT( eq.init( "a", operand[ "a" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       eq.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
       }

       TEST( EqOp, MatchesIndexKeyArrayValue ) {
       BSONObj operand = BSON( "a" << 6 );
       ComparisonExpression eq
       ASSERT( eq.init( "a", operand[ "a" ] ).isOK() );
       IndexSpec indexSpec( BSON( "loc" << "mockarrayvalue" << "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       eq.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       eq.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 4 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       eq.matchesIndexKey( BSON( "" << "dummygeohash" <<
       "" << BSON_ARRAY( 8 << 6 ) ), indexSpec ) );
       }
    */
    TEST( LtOp, MatchesElement ) {
        BSONObj operand = BSON( "$lt" << 5 );
        BSONObj match = BSON( "a" << 4.5 );
        BSONObj notMatch = BSON( "a" << 6 );
        BSONObj notMatchEqual = BSON( "a" << 5 );
        BSONObj notMatchWrongType = BSON( "a" << "foo" );
        ComparisonExpression lt;
        ASSERT( lt.init( "", ComparisonExpression::LT, operand[ "$lt" ] ).isOK() );
        ASSERT( lt.matchesSingleElement( match.firstElement() ) );
        ASSERT( !lt.matchesSingleElement( notMatch.firstElement() ) );
        ASSERT( !lt.matchesSingleElement( notMatchEqual.firstElement() ) );
        ASSERT( !lt.matchesSingleElement( notMatchWrongType.firstElement() ) );
    }

    TEST( LtOp, InvalidEooOperand ) {
        BSONObj operand;
        ComparisonExpression lt;
        ASSERT( !lt.init( "", ComparisonExpression::LT, operand.firstElement() ).isOK() );
    }

    TEST( LtOp, MatchesScalar ) {
        BSONObj operand = BSON( "$lt" << 5 );
        ComparisonExpression lt;
        ASSERT( lt.init( "a", ComparisonExpression::LT, operand[ "$lt" ] ).isOK() );
        ASSERT( lt.matches( BSON( "a" << 4.5 ), NULL ) );
        ASSERT( !lt.matches( BSON( "a" << 6 ), NULL ) );
    }

    TEST( LtOp, MatchesArrayValue ) {
        BSONObj operand = BSON( "$lt" << 5 );
        ComparisonExpression lt;
        ASSERT( lt.init( "a", ComparisonExpression::LT, operand[ "$lt" ] ).isOK() );
        ASSERT( lt.matches( BSON( "a" << BSON_ARRAY( 6 << 4.5 ) ), NULL ) );
        ASSERT( !lt.matches( BSON( "a" << BSON_ARRAY( 6 << 7 ) ), NULL ) );
    }

    TEST( LtOp, MatchesWholeArray ) {
        BSONObj operand = BSON( "$lt" << BSON_ARRAY( 5 ) );
        ComparisonExpression lt;
        ASSERT( lt.init( "a", ComparisonExpression::LT, operand[ "$lt" ] ).isOK() );
        // Arrays are not comparable as inequalities.
        ASSERT( !lt.matches( BSON( "a" << BSON_ARRAY( 4 ) ), NULL ) );
    }

    TEST( LtOp, MatchesNull ) {
        BSONObj operand = BSON( "$lt" << BSONNULL );
        ComparisonExpression lt;
        ASSERT( lt.init( "a", ComparisonExpression::LT, operand[ "$lt" ] ).isOK() );
        ASSERT( !lt.matches( BSONObj(), NULL ) );
        ASSERT( !lt.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !lt.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( LtOp, MatchesMinKey ) {
        BSONObj operand = BSON( "a" << MinKey );
        ComparisonExpression lt;
        ASSERT( lt.init( "a", ComparisonExpression::LT, operand[ "a" ] ).isOK() );
        ASSERT( !lt.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !lt.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !lt.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( LtOp, MatchesMaxKey ) {
        BSONObj operand = BSON( "a" << MaxKey );
        ComparisonExpression lt;
        ASSERT( lt.init( "a", ComparisonExpression::LT, operand[ "a" ] ).isOK() );
        ASSERT( !lt.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( lt.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( lt.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( LtOp, ElemMatchKey ) {
        BSONObj operand = BSON( "$lt" << 5 );
        ComparisonExpression lt;
        ASSERT( lt.init( "a", ComparisonExpression::LT, operand[ "$lt" ] ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !lt.matches( BSON( "a" << 6 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( lt.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( lt.matches( BSON( "a" << BSON_ARRAY( 6 << 2 << 5 ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    /**
       TEST( LtOp, MatchesIndexKeyScalar ) {
       BSONObj operand = BSON( "$lt" << 6 );
       LtOp lt;
       ASSERT( lt.init( "a", operand[ "$lt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       lt.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       lt.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       lt.matchesIndexKey( BSON( "" << BSON_ARRAY( 5 ) ), indexSpec ) );
       }

       TEST( LtOp, MatchesIndexKeyMissing ) {
       BSONObj operand = BSON( "$lt" << 6 );
       LtOp lt;
       ASSERT( lt.init( "a", operand[ "$lt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "b" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lt.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lt.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lt.matchesIndexKey( BSON( "" << BSON_ARRAY( 8 << 6 ) ), indexSpec ) );
       }

       TEST( LtOp, MatchesIndexKeyArray ) {
       BSONObj operand = BSON( "$lt" << BSON_ARRAY( 4 << 5 ) );
       LtOp lt;
       ASSERT( lt.init( "a", operand[ "$lt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lt.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
       }
    
       TEST( LtOp, MatchesIndexKeyArrayValue ) {
       BSONObj operand = BSON( "$lt" << 6 );
       LtOp lt;
       ASSERT( lt.init( "a", operand[ "$lt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "loc" << "mockarrayvalue" << "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       lt.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 3 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       lt.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       lt.matchesIndexKey( BSON( "" << "dummygeohash" <<
       "" << BSON_ARRAY( 8 << 6 << 4 ) ), indexSpec ) );
       }
    */
    TEST( LteOp, MatchesElement ) {
        BSONObj operand = BSON( "$lte" << 5 );
        BSONObj match = BSON( "a" << 4.5 );
        BSONObj equalMatch = BSON( "a" << 5 );
        BSONObj notMatch = BSON( "a" << 6 );
        BSONObj notMatchWrongType = BSON( "a" << "foo" );
        ComparisonExpression lte;
        ASSERT( lte.init( "", ComparisonExpression::LTE, operand[ "$lte" ] ).isOK() );
        ASSERT( lte.matchesSingleElement( match.firstElement() ) );
        ASSERT( lte.matchesSingleElement( equalMatch.firstElement() ) );
        ASSERT( !lte.matchesSingleElement( notMatch.firstElement() ) );
        ASSERT( !lte.matchesSingleElement( notMatchWrongType.firstElement() ) );
    }

    TEST( LteOp, InvalidEooOperand ) {
        BSONObj operand;
        ComparisonExpression lte;
        ASSERT( !lte.init( "", ComparisonExpression::LTE, operand.firstElement() ).isOK() );
    }

    TEST( LteOp, MatchesScalar ) {
        BSONObj operand = BSON( "$lte" << 5 );
        ComparisonExpression lte;
        ASSERT( lte.init( "a", ComparisonExpression::LTE, operand[ "$lte" ] ).isOK() );
        ASSERT( lte.matches( BSON( "a" << 4.5 ), NULL ) );
        ASSERT( !lte.matches( BSON( "a" << 6 ), NULL ) );
    }

    TEST( LteOp, MatchesArrayValue ) {
        BSONObj operand = BSON( "$lte" << 5 );
        ComparisonExpression lte;
        ASSERT( lte.init( "a", ComparisonExpression::LTE, operand[ "$lte" ] ).isOK() );
        ASSERT( lte.matches( BSON( "a" << BSON_ARRAY( 6 << 4.5 ) ), NULL ) );
        ASSERT( !lte.matches( BSON( "a" << BSON_ARRAY( 6 << 7 ) ), NULL ) );
    }

    TEST( LteOp, MatchesWholeArray ) {
        BSONObj operand = BSON( "$lte" << BSON_ARRAY( 5 ) );
        ComparisonExpression lte;
        ASSERT( lte.init( "a", ComparisonExpression::LTE, operand[ "$lte" ] ).isOK() );
        // Arrays are not comparable as inequalities.
        ASSERT( !lte.matches( BSON( "a" << BSON_ARRAY( 4 ) ), NULL ) );
    }

    TEST( LteOp, MatchesNull ) {
        BSONObj operand = BSON( "$lte" << BSONNULL );
        ComparisonExpression lte;
        ASSERT( lte.init( "a", ComparisonExpression::LTE, operand[ "$lte" ] ).isOK() );
        ASSERT( lte.matches( BSONObj(), NULL ) );
        ASSERT( lte.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !lte.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( LteOp, MatchesMinKey ) {
        BSONObj operand = BSON( "a" << MinKey );
        ComparisonExpression lte;
        ASSERT( lte.init( "a", ComparisonExpression::LTE, operand[ "a" ] ).isOK() );
        ASSERT( lte.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !lte.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !lte.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( LteOp, MatchesMaxKey ) {
        BSONObj operand = BSON( "a" << MaxKey );
        ComparisonExpression lte;
        ASSERT( lte.init( "a", ComparisonExpression::LTE, operand[ "a" ] ).isOK() );
        ASSERT( lte.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( lte.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( lte.matches( BSON( "a" << 4 ), NULL ) );
    }


    TEST( LteOp, ElemMatchKey ) {
        BSONObj operand = BSON( "$lte" << 5 );
        ComparisonExpression lte;
        ASSERT( lte.init( "a", ComparisonExpression::LTE, operand[ "$lte" ] ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !lte.matches( BSON( "a" << 6 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( lte.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( lte.matches( BSON( "a" << BSON_ARRAY( 6 << 2 << 5 ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    /**
       TEST( LteOp, MatchesIndexKeyScalar ) {
       BSONObj operand = BSON( "$lte" << 6 );
       LteOp lte;
       ASSERT( lte.init( "a", operand[ "$lte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       lte.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       lte.matchesIndexKey( BSON( "" << 7 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       lte.matchesIndexKey( BSON( "" << BSON_ARRAY( 5 ) ), indexSpec ) );
       }

       TEST( LteOp, MatchesIndexKeyMissing ) {
       BSONObj operand = BSON( "$lte" << 6 );
       LteOp lte;
       ASSERT( lte.init( "a", operand[ "$lte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "b" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lte.matchesIndexKey( BSON( "" << 7 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lte.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lte.matchesIndexKey( BSON( "" << BSON_ARRAY( 8 << 6 ) ), indexSpec ) );
       }

       TEST( LteOp, MatchesIndexKeyArray ) {
       BSONObj operand = BSON( "$lte" << BSON_ARRAY( 4 << 5 ) );
       LteOp lte;
       ASSERT( lte.init( "a", operand[ "$lte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       lte.matchesIndexKey( BSON( "" << 3 ), indexSpec ) );
       }

       TEST( LteOp, MatchesIndexKeyArrayValue ) {
       BSONObj operand = BSON( "$lte" << 6 );
       LteOp lte;
       ASSERT( lte.init( "a", operand[ "$lte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "loc" << "mockarrayvalue" << "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       lte.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 3 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       lte.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 7 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       lte.matchesIndexKey( BSON( "" << "dummygeohash" <<
       "" << BSON_ARRAY( 8 << 6 << 4 ) ), indexSpec ) );
       }

       TEST( GtOp, MatchesElement ) {
       BSONObj operand = BSON( "$gt" << 5 );
       BSONObj match = BSON( "a" << 5.5 );
       BSONObj notMatch = BSON( "a" << 4 );
       BSONObj notMatchEqual = BSON( "a" << 5 );
       BSONObj notMatchWrongType = BSON( "a" << "foo" );
       GtOp gt;
       ASSERT( gt.init( "", operand[ "$gt" ] ).isOK() );
       ASSERT( gt.matchesSingleElement( match.firstElement() ) );
       ASSERT( !gt.matchesSingleElement( notMatch.firstElement() ) );
       ASSERT( !gt.matchesSingleElement( notMatchEqual.firstElement() ) );
       ASSERT( !gt.matchesSingleElement( notMatchWrongType.firstElement() ) );
       }
    */

    TEST( GtOp, InvalidEooOperand ) {
        BSONObj operand;
        ComparisonExpression gt;
        ASSERT( !gt.init( "", ComparisonExpression::GT, operand.firstElement() ).isOK() );
    }

    TEST( GtOp, MatchesScalar ) {
        BSONObj operand = BSON( "$gt" << 5 );
        ComparisonExpression gt;
        ASSERT( gt.init( "a", ComparisonExpression::GT, operand[ "$gt" ] ).isOK() );
        ASSERT( gt.matches( BSON( "a" << 5.5 ), NULL ) );
        ASSERT( !gt.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( GtOp, MatchesArrayValue ) {
        BSONObj operand = BSON( "$gt" << 5 );
        ComparisonExpression gt;
        ASSERT( gt.init( "a", ComparisonExpression::GT, operand[ "$gt" ] ).isOK() );
        ASSERT( gt.matches( BSON( "a" << BSON_ARRAY( 3 << 5.5 ) ), NULL ) );
        ASSERT( !gt.matches( BSON( "a" << BSON_ARRAY( 2 << 4 ) ), NULL ) );
    }

    TEST( GtOp, MatchesWholeArray ) {
        BSONObj operand = BSON( "$gt" << BSON_ARRAY( 5 ) );
        ComparisonExpression gt;
        ASSERT( gt.init( "a", ComparisonExpression::GT, operand[ "$gt" ] ).isOK() );
        // Arrays are not comparable as inequalities.
        ASSERT( !gt.matches( BSON( "a" << BSON_ARRAY( 6 ) ), NULL ) );
    }

    TEST( GtOp, MatchesNull ) {
        BSONObj operand = BSON( "$gt" << BSONNULL );
        ComparisonExpression gt;
        ASSERT( gt.init( "a", ComparisonExpression::GT, operand[ "$gt" ] ).isOK() );
        ASSERT( !gt.matches( BSONObj(), NULL ) );
        ASSERT( !gt.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !gt.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( GtOp, MatchesMinKey ) {
        BSONObj operand = BSON( "a" << MinKey );
        ComparisonExpression gt;
        ASSERT( gt.init( "a", ComparisonExpression::GT, operand[ "a" ] ).isOK() );
        ASSERT( !gt.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( gt.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( gt.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( GtOp, MatchesMaxKey ) {
        BSONObj operand = BSON( "a" << MaxKey );
        ComparisonExpression gt;
        ASSERT( gt.init( "a", ComparisonExpression::GT, operand[ "a" ] ).isOK() );
        ASSERT( !gt.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !gt.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !gt.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( GtOp, ElemMatchKey ) {
        BSONObj operand = BSON( "$gt" << 5 );
        ComparisonExpression gt;
        ASSERT( gt.init( "a", ComparisonExpression::GT, operand[ "$gt" ] ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !gt.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( gt.matches( BSON( "a" << 6 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( gt.matches( BSON( "a" << BSON_ARRAY( 2 << 6 << 5 ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    /**
       TEST( GtOp, MatchesIndexKeyScalar ) {
       BSONObj operand = BSON( "$gt" << 6 );
       GtOp gt;
       ASSERT( gt.init( "a", operand[ "$gt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       gt.matchesIndexKey( BSON( "" << 7 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       gt.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       gt.matchesIndexKey( BSON( "" << BSON_ARRAY( 9 ) ), indexSpec ) );
       }

       TEST( GtOp, MatchesIndexKeyMissing ) {
       BSONObj operand = BSON( "$gt" << 6 );
       GtOp gt;
       ASSERT( gt.init( "a", operand[ "$gt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "b" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gt.matchesIndexKey( BSON( "" << 7 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gt.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gt.matchesIndexKey( BSON( "" << BSON_ARRAY( 8 << 6 ) ), indexSpec ) );
       }

       TEST( GtOp, MatchesIndexKeyArray ) {
       BSONObj operand = BSON( "$gt" << BSON_ARRAY( 4 << 5 ) );
       GtOp gt;
       ASSERT( gt.init( "a", operand[ "$gt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gt.matchesIndexKey( BSON( "" << 8 ), indexSpec ) );
       }
    
       TEST( GtOp, MatchesIndexKeyArrayValue ) {
       BSONObj operand = BSON( "$gt" << 6 );
       GtOp gt;
       ASSERT( gt.init( "a", operand[ "$gt" ] ).isOK() );
       IndexSpec indexSpec( BSON( "loc" << "mockarrayvalue" << "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       gt.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 7 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       gt.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 3 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       gt.matchesIndexKey( BSON( "" << "dummygeohash" <<
       "" << BSON_ARRAY( 8 << 6 << 4 ) ), indexSpec ) );
       }
    */

    TEST( ComparisonExpression, MatchesElement ) {
        BSONObj operand = BSON( "$gte" << 5 );
        BSONObj match = BSON( "a" << 5.5 );
        BSONObj equalMatch = BSON( "a" << 5 );
        BSONObj notMatch = BSON( "a" << 4 );
        BSONObj notMatchWrongType = BSON( "a" << "foo" );
        ComparisonExpression gte;
        ASSERT( gte.init( "", ComparisonExpression::GTE, operand[ "$gte" ] ).isOK() );
        ASSERT( gte.matchesSingleElement( match.firstElement() ) );
        ASSERT( gte.matchesSingleElement( equalMatch.firstElement() ) );
        ASSERT( !gte.matchesSingleElement( notMatch.firstElement() ) );
        ASSERT( !gte.matchesSingleElement( notMatchWrongType.firstElement() ) );
    }

    TEST( ComparisonExpression, InvalidEooOperand ) {
        BSONObj operand;
        ComparisonExpression gte;
        ASSERT( !gte.init( "", ComparisonExpression::GTE, operand.firstElement() ).isOK() );
    }

    TEST( ComparisonExpression, MatchesScalar ) {
        BSONObj operand = BSON( "$gte" << 5 );
        ComparisonExpression gte;
        ASSERT( gte.init( "a", ComparisonExpression::GTE, operand[ "$gte" ] ).isOK() );
        ASSERT( gte.matches( BSON( "a" << 5.5 ), NULL ) );
        ASSERT( !gte.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( ComparisonExpression, MatchesArrayValue ) {
        BSONObj operand = BSON( "$gte" << 5 );
        ComparisonExpression gte;
        ASSERT( gte.init( "a", ComparisonExpression::GTE, operand[ "$gte" ] ).isOK() );
        ASSERT( gte.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 ) ), NULL ) );
        ASSERT( !gte.matches( BSON( "a" << BSON_ARRAY( 1 << 2 ) ), NULL ) );
    }

    TEST( ComparisonExpression, MatchesWholeArray ) {
        BSONObj operand = BSON( "$gte" << BSON_ARRAY( 5 ) );
        ComparisonExpression gte;
        ASSERT( gte.init( "a", ComparisonExpression::GTE, operand[ "$gte" ] ).isOK() );
        // Arrays are not comparable as inequalities.
        ASSERT( !gte.matches( BSON( "a" << BSON_ARRAY( 6 ) ), NULL ) );
    }

    TEST( ComparisonExpression, MatchesNull ) {
        BSONObj operand = BSON( "$gte" << BSONNULL );
        ComparisonExpression gte;
        ASSERT( gte.init( "a", ComparisonExpression::GTE, operand[ "$gte" ] ).isOK() );
        ASSERT( gte.matches( BSONObj(), NULL ) );
        ASSERT( gte.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !gte.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( ComparisonExpression, MatchesMinKey ) {
        BSONObj operand = BSON( "a" << MinKey );
        ComparisonExpression gte;
        ASSERT( gte.init( "a", ComparisonExpression::GTE, operand[ "a" ] ).isOK() );
        ASSERT( gte.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( gte.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( gte.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( ComparisonExpression, MatchesMaxKey ) {
        BSONObj operand = BSON( "a" << MaxKey );
        ComparisonExpression gte;
        ASSERT( gte.init( "a", ComparisonExpression::GTE, operand[ "a" ] ).isOK() );
        ASSERT( gte.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !gte.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !gte.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( ComparisonExpression, ElemMatchKey ) {
        BSONObj operand = BSON( "$gte" << 5 );
        ComparisonExpression gte;
        ASSERT( gte.init( "a", ComparisonExpression::GTE, operand[ "$gte" ] ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !gte.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( gte.matches( BSON( "a" << 6 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( gte.matches( BSON( "a" << BSON_ARRAY( 2 << 6 << 5 ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    /**
       TEST( GteOp, MatchesIndexKeyScalar ) {
       BSONObj operand = BSON( "$gte" << 6 );
       GteOp gte;
       ASSERT( gte.init( "a", operand[ "$gte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       gte.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       gte.matchesIndexKey( BSON( "" << 5 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       gte.matchesIndexKey( BSON( "" << BSON_ARRAY( 7 ) ), indexSpec ) );
       }

       TEST( GteOp, MatchesIndexKeyMissing ) {
       BSONObj operand = BSON( "$gte" << 6 );
       GteOp gte;
       ASSERT( gte.init( "a", operand[ "$gte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "b" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gte.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gte.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gte.matchesIndexKey( BSON( "" << BSON_ARRAY( 8 << 6 ) ), indexSpec ) );
       }

       TEST( GteOp, MatchesIndexKeyArray ) {
       BSONObj operand = BSON( "$gte" << BSON_ARRAY( 4 << 5 ) );
       GteOp gte;
       ASSERT( gte.init( "a", operand[ "$gte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       gte.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
       }

       TEST( GteOp, MatchesIndexKeyArrayValue ) {
       BSONObj operand = BSON( "$gte" << 6 );
       GteOp gte;
       ASSERT( gte.init( "a", operand[ "$gte" ] ).isOK() );
       IndexSpec indexSpec( BSON( "loc" << "mockarrayvalue" << "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       gte.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 6 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       gte.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 3 ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       gte.matchesIndexKey( BSON( "" << "dummygeohash" <<
       "" << BSON_ARRAY( 8 << 6 << 4 ) ), indexSpec ) );
       }
    */

    TEST( NeOp, MatchesElement ) {
        BSONObj operand = BSON( "$ne" << 5 );
        BSONObj match = BSON( "a" << 6 );
        BSONObj notMatch = BSON( "a" << 5 );
        ComparisonExpression ne;
        ASSERT( ne.init( "", ComparisonExpression::NE, operand[ "$ne" ] ).isOK() );
        ASSERT( ne.matchesSingleElement( match.firstElement() ) );
        ASSERT( !ne.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( NeOp, InvalidEooOperand ) {
        BSONObj operand;
        ComparisonExpression ne;
        ASSERT( !ne.init( "", ComparisonExpression::NE, operand.firstElement() ).isOK() );
    }

    TEST( NeOp, MatchesScalar ) {
        BSONObj operand = BSON( "$ne" << 5 );
        ComparisonExpression ne;
        ASSERT( ne.init( "a", ComparisonExpression::NE, operand[ "$ne" ] ).isOK() );
        ASSERT( ne.matches( BSON( "a" << 4 ), NULL ) );
        ASSERT( !ne.matches( BSON( "a" << 5 ), NULL ) );
    }

    TEST( NeOp, MatchesArrayValue ) {
        BSONObj operand = BSON( "$ne" << 5 );
        ComparisonExpression ne;
        ASSERT( ne.init( "a", ComparisonExpression::NE, operand[ "$ne" ] ).isOK() );
        ASSERT( ne.matches( BSON( "a" << BSON_ARRAY( 4 << 6 ) ), NULL ) );
        ASSERT( !ne.matches( BSON( "a" << BSON_ARRAY( 4 << 5 ) ), NULL ) );
        ASSERT( !ne.matches( BSON( "a" << BSON_ARRAY( 5 << 5 ) ), NULL ) );
    }

    TEST( NeOp, MatchesNull ) {
        BSONObj operand = BSON( "$ne" << BSONNULL );
        ComparisonExpression ne;
        ASSERT( ne.init( "a", ComparisonExpression::NE, operand[ "$ne" ] ).isOK() );
        ASSERT( ne.matches( BSON( "a" << 4 ), NULL ) );
        ASSERT( !ne.matches( BSONObj(), NULL ) );
        ASSERT( !ne.matches( BSON( "a" << BSONNULL ), NULL ) );
    }

    TEST( NeOp, ElemMatchKey ) {
        BSONObj operand = BSON( "$ne" << 5 );
        ComparisonExpression ne;
        ASSERT( ne.init( "a", ComparisonExpression::NE, operand[ "$ne" ] ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !ne.matches( BSON( "a" << BSON_ARRAY( 2 << 6 << 5 ) ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( ne.matches( BSON( "a" << BSON_ARRAY( 2 << 6 ) ), &details ) );
        // The elemMatchKey feature is not implemented for $ne.
        ASSERT( !details.hasElemMatchKey() );
    }

    /**
       TEST( NeOp, MatchesIndexKey ) {
       BSONObj operand = BSON( "$ne" << 5 );
       NeOp ne;
       ASSERT( ne.init( "a", operand[ "$ne" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       BSONObj indexKey = BSON( "" << 1 );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       ne.matchesIndexKey( indexKey, indexSpec ) );
       }
    */

    TEST( RegexExpression, MatchesElementExact ) {
        BSONObj match = BSON( "a" << "b" );
        BSONObj notMatch = BSON( "a" << "c" );
        RegexExpression regex;
        ASSERT( regex.init( "", "b", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, TooLargePattern ) {
        string tooLargePattern( 50 * 1000, 'z' );
        RegexExpression regex;
        ASSERT( !regex.init( "a", tooLargePattern, "" ).isOK() );
    }

    TEST( RegexExpression, MatchesElementSimplePrefix ) {
        BSONObj match = BSON( "x" << "abc" );
        BSONObj notMatch = BSON( "x" << "adz" );
        RegexExpression regex;
        ASSERT( regex.init( "", "^ab", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementCaseSensitive ) {
        BSONObj match = BSON( "x" << "abc" );
        BSONObj notMatch = BSON( "x" << "ABC" );
        RegexExpression regex;
        ASSERT( regex.init( "", "abc", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementCaseInsensitive ) {
        BSONObj match = BSON( "x" << "abc" );
        BSONObj matchUppercase = BSON( "x" << "ABC" );
        BSONObj notMatch = BSON( "x" << "abz" );
        RegexExpression regex;
        ASSERT( regex.init( "", "abc", "i" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( regex.matchesSingleElement( matchUppercase.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementMultilineOff ) {
        BSONObj match = BSON( "x" << "az" );
        BSONObj notMatch = BSON( "x" << "\naz" );
        RegexExpression regex;
        ASSERT( regex.init( "", "^a", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementMultilineOn ) {
        BSONObj match = BSON( "x" << "az" );
        BSONObj matchMultiline = BSON( "x" << "\naz" );
        BSONObj notMatch = BSON( "x" << "\n\n" );
        RegexExpression regex;
        ASSERT( regex.init( "", "^a", "m" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( regex.matchesSingleElement( matchMultiline.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementExtendedOff ) {
        BSONObj match = BSON( "x" << "a b" );
        BSONObj notMatch = BSON( "x" << "ab" );
        RegexExpression regex;
        ASSERT( regex.init( "", "a b", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementExtendedOn ) {
        BSONObj match = BSON( "x" << "ab" );
        BSONObj notMatch = BSON( "x" << "a b" );
        RegexExpression regex;
        ASSERT( regex.init( "", "a b", "x" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementDotAllOff ) {
        BSONObj match = BSON( "x" << "a b" );
        BSONObj notMatch = BSON( "x" << "a\nb" );
        RegexExpression regex;
        ASSERT( regex.init( "", "a.b", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementDotAllOn ) {
        BSONObj match = BSON( "x" << "a b" );
        BSONObj matchDotAll = BSON( "x" << "a\nb" );
        BSONObj notMatch = BSON( "x" << "ab" );
        RegexExpression regex;
        ASSERT( regex.init( "", "a.b", "s" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( regex.matchesSingleElement( matchDotAll.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementMultipleFlags ) {
        BSONObj matchMultilineDotAll = BSON( "x" << "\na\nb" );
        RegexExpression regex;
        ASSERT( regex.init( "", "^a.b", "ms" ).isOK() );
        ASSERT( regex.matchesSingleElement( matchMultilineDotAll.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementRegexType ) {
        BSONObj match = BSONObjBuilder().appendRegex( "x", "yz", "i" ).obj();
        BSONObj notMatchPattern = BSONObjBuilder().appendRegex( "x", "r", "i" ).obj();
        BSONObj notMatchFlags = BSONObjBuilder().appendRegex( "x", "yz", "s" ).obj();
        RegexExpression regex;
        ASSERT( regex.init( "", "yz", "i" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatchPattern.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatchFlags.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementSymbolType ) {
        BSONObj match = BSONObjBuilder().appendSymbol( "x", "yz" ).obj();
        BSONObj notMatch = BSONObjBuilder().appendSymbol( "x", "gg" ).obj();
        RegexExpression regex;
        ASSERT( regex.init( "", "yz", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( match.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementWrongType ) {
        BSONObj notMatchInt = BSON( "x" << 1 );
        BSONObj notMatchBool = BSON( "x" << true );
        RegexExpression regex;
        ASSERT( regex.init( "", "1", "" ).isOK() );
        ASSERT( !regex.matchesSingleElement( notMatchInt.firstElement() ) );
        ASSERT( !regex.matchesSingleElement( notMatchBool.firstElement() ) );
    }

    TEST( RegexExpression, MatchesElementUtf8 ) {
        BSONObj multiByteCharacter = BSON( "x" << "\xc2\xa5" );
        RegexExpression regex;
        ASSERT( regex.init( "", "^.$", "" ).isOK() );
        ASSERT( regex.matchesSingleElement( multiByteCharacter.firstElement() ) );
    }

    TEST( RegexExpression, MatchesScalar ) {
        RegexExpression regex;
        ASSERT( regex.init( "a", "b", "" ).isOK() );
        ASSERT( regex.matches( BSON( "a" << "b" ), NULL ) );
        ASSERT( !regex.matches( BSON( "a" << "c" ), NULL ) );
    }

    TEST( RegexExpression, MatchesArrayValue ) {
        RegexExpression regex;
        ASSERT( regex.init( "a", "b", "" ).isOK() );
        ASSERT( regex.matches( BSON( "a" << BSON_ARRAY( "c" << "b" ) ), NULL ) );
        ASSERT( !regex.matches( BSON( "a" << BSON_ARRAY( "d" << "c" ) ), NULL ) );
    }

    TEST( RegexExpression, MatchesNull ) {
        RegexExpression regex;
        ASSERT( regex.init( "a", "b", "" ).isOK() );
        ASSERT( !regex.matches( BSONObj(), NULL ) );
        ASSERT( !regex.matches( BSON( "a" << BSONNULL ), NULL ) );
    }

    TEST( RegexExpression, ElemMatchKey ) {
        RegexExpression regex;
        ASSERT( regex.init( "a", "b", "" ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !regex.matches( BSON( "a" << "c" ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( regex.matches( BSON( "a" << "b" ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( regex.matches( BSON( "a" << BSON_ARRAY( "c" << "b" ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    /**
       TEST( RegexExpression, MatchesIndexKeyScalar ) {
       RegexExpression regex;
       ASSERT( regex.init( "a", "xyz", "" ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       regex.matchesIndexKey( BSON( "" << "z xyz" ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       regex.matchesIndexKey( BSON( "" << "xy" ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       regex.matchesIndexKey( BSON( "" << BSON_ARRAY( "xyz" ) ), indexSpec ) );
       }

       TEST( RegexExpression, MatchesIndexKeyMissing ) {
       RegexExpression regex;
       ASSERT( regex.init( "a", "xyz", "" ).isOK() );
       IndexSpec indexSpec( BSON( "b" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       regex.matchesIndexKey( BSON( "" << "z xyz" ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       regex.matchesIndexKey( BSON( "" << "xy" ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       regex.matchesIndexKey( BSON( "" << BSON_ARRAY( 8 << "xyz" ) ), indexSpec ) );
       }

       TEST( RegexExpression, MatchesIndexKeyArrayValue ) {
       RegexExpression regex;
       ASSERT( regex.init( "a", "xyz", "" ).isOK() );
       IndexSpec indexSpec( BSON( "loc" << "mockarrayvalue" << "a" << 1 ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       regex.matchesIndexKey( BSON( "" << "dummygeohash" << "" << "xyz" ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_False ==
       regex.matchesIndexKey( BSON( "" << "dummygeohash" << "" << "z" ), indexSpec ) );
       ASSERT( MatchExpression::PartialMatchResult_True ==
       regex.matchesIndexKey( BSON( "" << "dummygeohash" <<
       "" << BSON_ARRAY( "r" << 6 << "xyz" ) ), indexSpec ) );
       }
    */

    TEST( ModExpression, MatchesElement ) {
        BSONObj match = BSON( "a" << 1 );
        BSONObj largerMatch = BSON( "a" << 4.0 );
        BSONObj longLongMatch = BSON( "a" << 68719476736LL );
        BSONObj notMatch = BSON( "a" << 6 );
        BSONObj negativeNotMatch = BSON( "a" << -2 );
        ModExpression mod;
        ASSERT( mod.init( "", 3, 1 ).isOK() );
        ASSERT( mod.matchesSingleElement( match.firstElement() ) );
        ASSERT( mod.matchesSingleElement( largerMatch.firstElement() ) );
        ASSERT( mod.matchesSingleElement( longLongMatch.firstElement() ) );
        ASSERT( !mod.matchesSingleElement( notMatch.firstElement() ) );
        ASSERT( !mod.matchesSingleElement( negativeNotMatch.firstElement() ) );
    }

    TEST( ModExpression, ZeroDivisor ) {
        ModExpression mod;
        ASSERT( !mod.init( "", 0, 1 ).isOK() );
    }

    TEST( ModExpression, MatchesScalar ) {
        ModExpression mod;
        ASSERT( mod.init( "a", 5, 2 ).isOK() );
        ASSERT( mod.matches( BSON( "a" << 7.0 ), NULL ) );
        ASSERT( !mod.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( ModExpression, MatchesArrayValue ) {
        ModExpression mod;
        ASSERT( mod.init( "a", 5, 2 ).isOK() );
        ASSERT( mod.matches( BSON( "a" << BSON_ARRAY( 5 << 12LL ) ), NULL ) );
        ASSERT( !mod.matches( BSON( "a" << BSON_ARRAY( 6 << 8 ) ), NULL ) );
    }

    TEST( ModExpression, MatchesNull ) {
        ModExpression mod;
        ASSERT( mod.init( "a", 5, 2 ).isOK() );
        ASSERT( !mod.matches( BSONObj(), NULL ) );
        ASSERT( !mod.matches( BSON( "a" << BSONNULL ), NULL ) );
    }

    TEST( ModExpression, ElemMatchKey ) {
        ModExpression mod;
        ASSERT( mod.init( "a", 5, 2 ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !mod.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( mod.matches( BSON( "a" << 2 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( mod.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 5 ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }
    /**
       TEST( ModExpression, MatchesIndexKey ) {
       BSONObj operand = BSON( "$mod" << BSON_ARRAY( 2 << 1 ) );
       ModExpression mod;
       ASSERT( mod.init( "a", operand[ "$mod" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       BSONObj indexKey = BSON( "" << 1 );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       mod.matchesIndexKey( indexKey, indexSpec ) );
       }
    */

    TEST( ExistsExpression, MatchesElement ) {
        BSONObj existsInt = BSON( "a" << 5 );
        BSONObj existsNull = BSON( "a" << BSONNULL );
        BSONObj doesntExist = BSONObj();
        ExistsExpression exists;
        ASSERT( exists.init( "", true ).isOK() );
        ASSERT( exists.matchesSingleElement( existsInt.firstElement() ) );
        ASSERT( exists.matchesSingleElement( existsNull.firstElement() ) );
        ASSERT( !exists.matchesSingleElement( doesntExist.firstElement() ) );
    }

    TEST( ExistsExpression, MatchesElementExistsFalse ) {
        BSONObj existsInt = BSON( "a" << 5 );
        BSONObj existsNull = BSON( "a" << BSONNULL );
        BSONObj doesntExist = BSONObj();
        ExistsExpression exists;
        ASSERT( exists.init( "", false ).isOK() );
        ASSERT( !exists.matchesSingleElement( existsInt.firstElement() ) );
        ASSERT( !exists.matchesSingleElement( existsNull.firstElement() ) );
        ASSERT( exists.matchesSingleElement( doesntExist.firstElement() ) );
    }

    TEST( ExistsExpression, MatchesElementExistsTrueValue ) {
        BSONObj exists = BSON( "a" << 5 );
        BSONObj missing = BSONObj();
        ExistsExpression existsTrueValue;
        ExistsExpression existsFalseValue;
        ASSERT( existsTrueValue.init( "", true ).isOK() );
        ASSERT( existsFalseValue.init( "", false ).isOK() );
        ASSERT( existsTrueValue.matchesSingleElement( exists.firstElement() ) );
        ASSERT( !existsFalseValue.matchesSingleElement( exists.firstElement() ) );
        ASSERT( !existsTrueValue.matchesSingleElement( missing.firstElement() ) );
        ASSERT( existsFalseValue.matchesSingleElement( missing.firstElement() ) );
    }

    TEST( ExistsExpression, MatchesScalar ) {
        ExistsExpression exists;
        ASSERT( exists.init( "a", true ).isOK() );
        ASSERT( exists.matches( BSON( "a" << 1 ), NULL ) );
        ASSERT( exists.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !exists.matches( BSON( "b" << 1 ), NULL ) );
    }

    TEST( ExistsExpression, MatchesScalarFalse ) {
        ExistsExpression exists;
        ASSERT( exists.init( "a", false ).isOK() );
        ASSERT( !exists.matches( BSON( "a" << 1 ), NULL ) );
        ASSERT( !exists.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( exists.matches( BSON( "b" << 1 ), NULL ) );
    }

    TEST( ExistsExpression, MatchesArray ) {
        ExistsExpression exists;
        ASSERT( exists.init( "a", true ).isOK() );
        ASSERT( exists.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 ) ), NULL ) );
    }

    TEST( ExistsExpression, ElemMatchKey ) {
        ExistsExpression exists;
        ASSERT( exists.init( "a.b", true ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !exists.matches( BSON( "a" << 1 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( exists.matches( BSON( "a" << BSON( "b" << 6 ) ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( exists.matches( BSON( "a" << BSON_ARRAY( 2 << BSON( "b" << 7 ) ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }
    /**
       TEST( ExistsExpression, MatchesIndexKey ) {
       BSONObj operand = BSON( "$exists" << true );
       ExistsExpression exists;
       ASSERT( exists.init( "a", operand[ "$exists" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       BSONObj indexKey = BSON( "" << 1 );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       exists.matchesIndexKey( indexKey, indexSpec ) );
       }
    */



    TEST( TypeExpression, MatchesElementStringType ) {
        BSONObj match = BSON( "a" << "abc" );
        BSONObj notMatch = BSON( "a" << 5 );
        TypeExpression type;
        ASSERT( type.init( "", String ).isOK() );
        ASSERT( type.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !type.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( TypeExpression, MatchesElementNullType ) {
        BSONObj match = BSON( "a" << BSONNULL );
        BSONObj notMatch = BSON( "a" << "abc" );
        TypeExpression type;
        ASSERT( type.init( "", jstNULL ).isOK() );
        ASSERT( type.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !type.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( TypeExpression, InvalidTypeExpressionerand ) {
        // If the provided type number is not a valid BSONType, it is not a parse error.  The
        // operator will simply not match anything.
        BSONObj notMatch1 = BSON( "a" << BSONNULL );
        BSONObj notMatch2 = BSON( "a" << "abc" );
        TypeExpression type;
        ASSERT( type.init( "", JSTypeMax + 1 ).isOK() );
        ASSERT( !type.matchesSingleElement( notMatch1[ "a" ] ) );
        ASSERT( !type.matchesSingleElement( notMatch2[ "a" ] ) );
    }

    TEST( TypeExpression, MatchesScalar ) {
        TypeExpression type;
        ASSERT( type.init( "a", Bool ).isOK() );
        ASSERT( type.matches( BSON( "a" << true ), NULL ) );
        ASSERT( !type.matches( BSON( "a" << 1 ), NULL ) );
    }

    TEST( TypeExpression, MatchesArray ) {
        TypeExpression type;
        ASSERT( type.init( "a", NumberInt ).isOK() );
        ASSERT( type.matches( BSON( "a" << BSON_ARRAY( 4 ) ), NULL ) );
        ASSERT( type.matches( BSON( "a" << BSON_ARRAY( 4 << "a" ) ), NULL ) );
        ASSERT( type.matches( BSON( "a" << BSON_ARRAY( "a" << 4 ) ), NULL ) );
        ASSERT( !type.matches( BSON( "a" << BSON_ARRAY( "a" ) ), NULL ) );
        ASSERT( !type.matches( BSON( "a" << BSON_ARRAY( BSON_ARRAY( 4 ) ) ), NULL ) );
    }

    TEST( TypeExpression, MatchesOuterArray ) {
        TypeExpression type;
        ASSERT( type.init( "a", Array ).isOK() );
        // The outer array is not matched.
        ASSERT( !type.matches( BSON( "a" << BSONArray() ), NULL ) );
        ASSERT( !type.matches( BSON( "a" << BSON_ARRAY( 4 << "a" ) ), NULL ) );
        ASSERT( type.matches( BSON( "a" << BSON_ARRAY( BSONArray() << 2 ) ), NULL ) );
        ASSERT( !type.matches( BSON( "a" << "bar" ), NULL ) );
    }

    TEST( TypeExpression, MatchesNull ) {
        TypeExpression type;
        ASSERT( type.init( "a", jstNULL ).isOK() );
        ASSERT( type.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !type.matches( BSON( "a" << 4 ), NULL ) );
        ASSERT( !type.matches( BSONObj(), NULL ) );
    }

    TEST( TypeExpression, ElemMatchKey ) {
        TypeExpression type;
        ASSERT( type.init( "a.b", String ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !type.matches( BSON( "a" << 1 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( type.matches( BSON( "a" << BSON( "b" << "string" ) ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( type.matches( BSON( "a" << BSON( "b" << BSON_ARRAY( "string" ) ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "0", details.elemMatchKey() );
        ASSERT( type.matches( BSON( "a" <<
                                    BSON_ARRAY( 2 <<
                                                BSON( "b" << BSON_ARRAY( "string" ) ) ) ),
                              &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }
    /**
       TEST( TypeExpression, MatchesIndexKey ) {
       BSONObj operand = BSON( "$type" << 2 );
       TypeExpression type;
       ASSERT( type.init( "a", operand[ "$type" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       BSONObj indexKey = BSON( "" << "q" );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       type.matchesIndexKey( indexKey, indexSpec ) );
       }
    */


    TEST( InExpression, MatchesElementSingle ) {
        BSONArray operand = BSON_ARRAY( 1 );
        BSONObj match = BSON( "a" << 1 );
        BSONObj notMatch = BSON( "a" << 2 );
        InExpression in;
        in.getArrayFilterEntries()->addEquality( operand.firstElement() );
        ASSERT( in.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !in.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( InExpression, MatchesEmpty ) {
        InExpression in;
        in.init( "a" );

        BSONObj notMatch = BSON( "a" << 2 );
        ASSERT( !in.matchesSingleElement( notMatch[ "a" ] ) );
        ASSERT( !in.matches( BSON( "a" << 1 ), NULL ) );
        ASSERT( !in.matches( BSONObj(), NULL ) );
    }

    TEST( InExpression, MatchesElementMultiple ) {
        BSONObj operand = BSON_ARRAY( 1 << "r" << true << 1 );
        InExpression in;
        in.getArrayFilterEntries()->addEquality( operand[0] );
        in.getArrayFilterEntries()->addEquality( operand[1] );
        in.getArrayFilterEntries()->addEquality( operand[2] );
        in.getArrayFilterEntries()->addEquality( operand[3] );

        BSONObj matchFirst = BSON( "a" << 1 );
        BSONObj matchSecond = BSON( "a" << "r" );
        BSONObj matchThird = BSON( "a" << true );
        BSONObj notMatch = BSON( "a" << false );
        ASSERT( in.matchesSingleElement( matchFirst[ "a" ] ) );
        ASSERT( in.matchesSingleElement( matchSecond[ "a" ] ) );
        ASSERT( in.matchesSingleElement( matchThird[ "a" ] ) );
        ASSERT( !in.matchesSingleElement( notMatch[ "a" ] ) );
    }


    TEST( InExpression, MatchesScalar ) {
        BSONObj operand = BSON_ARRAY( 5 );
        InExpression in;
        in.init( "a" );
        in.getArrayFilterEntries()->addEquality( operand.firstElement() );

        ASSERT( in.matches( BSON( "a" << 5.0 ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( InExpression, MatchesArrayValue ) {
        BSONObj operand = BSON_ARRAY( 5 );
        InExpression in;
        in.init( "a" );
        in.getArrayFilterEntries()->addEquality( operand.firstElement() );

        ASSERT( in.matches( BSON( "a" << BSON_ARRAY( 5.0 << 6 ) ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << BSON_ARRAY( 6 << 7 ) ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << BSON_ARRAY( BSON_ARRAY( 5 ) ) ), NULL ) );
    }

    TEST( InExpression, MatchesNull ) {
        BSONObj operand = BSON_ARRAY( BSONNULL );

        InExpression in;
        in.init( "a" );
        in.getArrayFilterEntries()->addEquality( operand.firstElement() );

        ASSERT( in.matches( BSONObj(), NULL ) );
        ASSERT( in.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( InExpression, MatchesMinKey ) {
        BSONObj operand = BSON_ARRAY( MinKey );
        InExpression in;
        in.init( "a" );
        in.getArrayFilterEntries()->addEquality( operand.firstElement() );

        ASSERT( in.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( InExpression, MatchesMaxKey ) {
        BSONObj operand = BSON_ARRAY( MaxKey );
        InExpression in;
        in.init( "a" );
        in.getArrayFilterEntries()->addEquality( operand.firstElement() );

        ASSERT( in.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( InExpression, MatchesFullArray ) {
        BSONObj operand = BSON_ARRAY( BSON_ARRAY( 1 << 2 ) << 4 << 5 );
        InExpression in;
        in.init( "a" );
        in.getArrayFilterEntries()->addEquality( operand[0] );
        in.getArrayFilterEntries()->addEquality( operand[1] );
        in.getArrayFilterEntries()->addEquality( operand[2] );

        ASSERT( in.matches( BSON( "a" << BSON_ARRAY( 1 << 2 ) ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 3 ) ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << BSON_ARRAY( 1 ) ), NULL ) );
        ASSERT( !in.matches( BSON( "a" << 1 ), NULL ) );
    }

    TEST( InExpression, ElemMatchKey ) {
        BSONObj operand = BSON_ARRAY( 5 << 2 );
        InExpression in;
        in.init( "a" );
        in.getArrayFilterEntries()->addEquality( operand[0] );
        in.getArrayFilterEntries()->addEquality( operand[1] );

        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !in.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( in.matches( BSON( "a" << 5 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( in.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 5 ) ), &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    /**
    TEST( InExpression, MatchesIndexKeyScalar ) {
        BSONObj operand = BSON( "$in" << BSON_ARRAY( 6 << 5 ) );
        InExpression in;
        ASSERT( in.init( "a", operand[ "$in" ] ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                in.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                in.matchesIndexKey( BSON( "" << 5 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                in.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                in.matchesIndexKey( BSON( "" << BSON_ARRAY( 6 ) ), indexSpec ) );
    }

    TEST( InExpression, MatchesIndexKeyMissing ) {
        BSONObj operand = BSON( "$in" << BSON_ARRAY( 6 ) );
        ComparisonExpression eq
            ASSERT( eq.init( "a", operand[ "$in" ] ).isOK() );
        IndexSpec indexSpec( BSON( "b" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                eq.matchesIndexKey( BSON( "" << 6 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                eq.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                eq.matchesIndexKey( BSON( "" << BSON_ARRAY( 8 << 6 ) ), indexSpec ) );
    }

    TEST( InExpression, MatchesIndexKeyArray ) {
        BSONObj operand = BSON( "$in" << BSON_ARRAY( 4 << BSON_ARRAY( 5 ) ) );
        InExpression in;
        ASSERT( in.init( "a", operand[ "$in" ] ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                in.matchesIndexKey( BSON( "" << 4 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                in.matchesIndexKey( BSON( "" << 5 ), indexSpec ) );
    }
    
    TEST( InExpression, MatchesIndexKeyArrayValue ) {
        BSONObjBuilder inArray;
        inArray.append( "0", 4 ).append( "1", 5 ).appendRegex( "2", "abc", "" );
        BSONObj operand = BSONObjBuilder().appendArray( "$in", inArray.obj() ).obj();
        InExpression in;
        ASSERT( in.init( "a", operand[ "$in" ] ).isOK() );
        IndexSpec indexSpec( BSON( "loc" << "mockarrayvalue" << "a" << 1 ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 4 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" << "" << 6 ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" << "" << "abcd" ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                in.matchesIndexKey( BSONObjBuilder()
                                    .append( "", "dummygeohash" )
                                    .appendRegex( "", "abc", "" ).obj(),
                                    indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" << "" << "ab" ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" <<
                                          "" << BSON_ARRAY( 8 << 5 ) ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" <<
                                          "" << BSON_ARRAY( 8 << 9 ) ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_True ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" <<
                                          "" << BSON_ARRAY( 8 << "abc" ) ), indexSpec ) );
        ASSERT( MatchExpression::PartialMatchResult_False ==
                in.matchesIndexKey( BSON( "" << "dummygeohash" <<
                                          "" << BSON_ARRAY( 8 << "ac" ) ), indexSpec ) );
    }
    */

    TEST( NinExpression, MatchesElementSingle ) {
        BSONObj operands = BSON_ARRAY( 1 );
        BSONObj match = BSON( "a" << 2 );
        BSONObj notMatch = BSON( "a" << 1 );
        NinExpression nin;
        nin.getArrayFilterEntries()->addEquality( operands.firstElement() );
        ASSERT( nin.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !nin.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( NinExpression, MatchesElementMultiple ) {
        BSONArray operand = BSON_ARRAY( 1 << "r" << true << 1 );
        BSONObj match = BSON( "a" << false );
        BSONObj notMatchFirst = BSON( "a" << 1 );
        BSONObj notMatchSecond = BSON( "a" << "r" );
        BSONObj notMatchThird = BSON( "a" << true );
        NinExpression nin;
        nin.getArrayFilterEntries()->addEquality( operand[0] );
        nin.getArrayFilterEntries()->addEquality( operand[1] );
        nin.getArrayFilterEntries()->addEquality( operand[2] );
        nin.getArrayFilterEntries()->addEquality( operand[3] );
        ASSERT( nin.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !nin.matchesSingleElement( notMatchFirst[ "a" ] ) );
        ASSERT( !nin.matchesSingleElement( notMatchSecond[ "a" ] ) );
        ASSERT( !nin.matchesSingleElement( notMatchThird[ "a" ] ) );
    }

    TEST( NinExpression, MatchesScalar ) {
        BSONObj operand = BSON_ARRAY( 5 );
        NinExpression nin;
        nin.init( "a" );
        nin.getArrayFilterEntries()->addEquality( operand.firstElement() );
        ASSERT( nin.matches( BSON( "a" << 4 ), NULL ) );
        ASSERT( !nin.matches( BSON( "a" << 5 ), NULL ) );
        ASSERT( !nin.matches( BSON( "a" << 5.0 ), NULL ) );
    }

    TEST( NinExpression, MatchesArrayValue ) {
        BSONObj operand = BSON_ARRAY( 5 );
        NinExpression nin;
        nin.init( "a" );
        nin.getArrayFilterEntries()->addEquality( operand.firstElement() );
        ASSERT( !nin.matches( BSON( "a" << BSON_ARRAY( 5.0 << 6 ) ), NULL ) );
        ASSERT( nin.matches( BSON( "a" << BSON_ARRAY( 6 << 7 ) ), NULL ) );
        ASSERT( nin.matches( BSON( "a" << BSON_ARRAY( BSON_ARRAY( 5 ) ) ), NULL ) );
    }

    TEST( NinExpression, MatchesNull ) {
        BSONObjBuilder ninArray;
        ninArray.appendNull( "0" );
        BSONObj operand = ninArray.obj();
        NinExpression nin;
        nin.init( "a" );
        nin.getArrayFilterEntries()->addEquality( operand.firstElement() );
        ASSERT( !nin.matches( BSONObj(), NULL ) );
        ASSERT( !nin.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( nin.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( NinExpression, MatchesFullArray ) {
        BSONArray operand = BSON_ARRAY( BSON_ARRAY( 1 << 2 ) << 4 << 5 );
        NinExpression nin;
        nin.init( "a" );
        nin.getArrayFilterEntries()->addEquality( operand[0] );
        nin.getArrayFilterEntries()->addEquality( operand[1] );
        nin.getArrayFilterEntries()->addEquality( operand[2] );
        ASSERT( !nin.matches( BSON( "a" << BSON_ARRAY( 1 << 2 ) ), NULL ) );
        ASSERT( nin.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 3 ) ), NULL ) );
        ASSERT( nin.matches( BSON( "a" << BSON_ARRAY( 1 ) ), NULL ) );
        ASSERT( nin.matches( BSON( "a" << 1 ), NULL ) );
    }

    TEST( NinExpression, ElemMatchKey ) {
        BSONArray operand = BSON_ARRAY( 5 << 2 );
        NinExpression nin;
        nin.init( "a" );
        nin.getArrayFilterEntries()->addEquality( operand[0] );
        nin.getArrayFilterEntries()->addEquality( operand[1] );

        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !nin.matches( BSON( "a" << 2 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( nin.matches( BSON( "a" << 3 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( nin.matches( BSON( "a" << BSON_ARRAY( 1 << 3 << 6 ) ), &details ) );
        // The elemMatchKey feature is not implemented for $nin.
        ASSERT( !details.hasElemMatchKey() );
    }

    /**
    TEST( NinExpression, MatchesIndexKey ) {
        BSONObj operand = BSON( "$nin" << BSON_ARRAY( 5 ) );
        NinExpression nin;
        ASSERT( nin.init( "a", operand[ "$nin" ] ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "7" );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                nin.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

}

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
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_tree.h"

namespace mongo {

    TEST( AllMatchExpression, MatchesElementSingle ) {
        BSONObj operand = BSON_ARRAY( 1 << 1 );
        BSONObj match = BSON( "a" << 1 );
        BSONObj notMatch = BSON( "a" << 2 );
        AllMatchExpression all;
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );

        ASSERT( all.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !all.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( AllMatchExpression, MatchesEmpty ) {

        BSONObj notMatch = BSON( "a" << 2 );
        AllMatchExpression all;

        ASSERT( !all.matchesSingleElement( notMatch[ "a" ] ) );
        ASSERT( !all.matches( BSON( "a" << 1 ), NULL ) );
        ASSERT( !all.matches( BSONObj(), NULL ) );
    }

    TEST( AllMatchExpression, MatchesElementMultiple ) {
        BSONObj operand = BSON_ARRAY( 1 << "r" );
        AllMatchExpression all;
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );

        BSONObj notMatchFirst = BSON( "a" << 1 );
        BSONObj notMatchSecond = BSON( "a" << "r" );
        BSONObj notMatchArray = BSON( "a" << BSON_ARRAY( 1 << "s" ) ); // XXX

        ASSERT( !all.matchesSingleElement( notMatchFirst[ "a" ] ) );
        ASSERT( !all.matchesSingleElement( notMatchSecond[ "a" ] ) );
        ASSERT( !all.matchesSingleElement( notMatchArray[ "a" ] ) );
    }

    TEST( AllMatchExpression, MatchesScalar ) {
        BSONObj operand = BSON_ARRAY( 5 );
        AllMatchExpression all;
        all.init( "a" );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << 5.0 ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( AllMatchExpression, MatchesArrayValue ) {
        BSONObj operand = BSON_ARRAY( 5 );
        AllMatchExpression all;
        all.init( "a" );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << BSON_ARRAY( 5.0 << 6 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 6 << 7 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( BSON_ARRAY( 5 ) ) ), NULL ) );
    }

    TEST( AllMatchExpression, MatchesNonArrayMultiValues ) {
        BSONObj operand = BSON_ARRAY( 5 << 6 );
        AllMatchExpression all;
        all.init( "a.b" );
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );

        ASSERT( all.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << 5.0 ) << BSON( "b" << 6 ) ) ),
                             NULL ) );
        ASSERT( all.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 5.0 << 7 ) ) <<
                                                      BSON( "b" << BSON_ARRAY( 10 << 6 ) ) ) ),
                             NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << 5.0 ) << BSON( "c" << 6 ) ) ),
                              NULL ) );
    }

    TEST( AllMatchExpression, MatchesArrayAndNonArrayMultiValues ) {
        BSONObj operand = BSON_ARRAY( 1 << 2 << 3 << 4 );
        AllMatchExpression all;
        all.init( "a.b" );
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );
        all.getArrayFilterEntries()->addEquality( operand[2] );
        all.getArrayFilterEntries()->addEquality( operand[3] );

        ASSERT( all.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 4 << 5 << 2 ) ) <<
                                                      BSON( "b" << 3 ) <<
                                                      BSON( "b" << BSONArray() ) <<
                                                      BSON( "b" << BSON_ARRAY( 1 ) ) ) ),
                             NULL ) );
    }

    TEST( AllMatchExpression, MatchesNull ) {
        BSONObjBuilder allArray;
        allArray.appendNull( "0" );
        BSONObj operand = allArray.obj();

        AllMatchExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSONObj(), NULL ) );
        ASSERT( all.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( AllMatchExpression, MatchesFullArray ) {
        BSONObj operand = BSON_ARRAY( BSON_ARRAY( 1 << 2 ) << 1 );
        AllMatchExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );

        // $all does not match full arrays.
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 1 << 2 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 3 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 1 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 1 ), NULL ) );
    }

    TEST( AllMatchExpression, ElemMatchKey ) {
        BSONObj operand = BSON_ARRAY( 5 );
        AllMatchExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !all.matches( BSON( "a" << 4 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( all.matches( BSON( "a" << 5 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( all.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 5 ) ), &details ) );
        // The elemMatchKey feature is not implemented for $all.
        ASSERT( !details.hasElemMatchKey() );
    }

    TEST( AllMatchExpression, MatchesMinKey ) {
        BSONObj operand = BSON_ARRAY( MinKey );
        AllMatchExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( AllMatchExpression, MatchesMaxKey ) {
        BSONObj operand = BSON_ARRAY( MaxKey );
        AllMatchExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( AllMatchExpression, Equivalent ) {
        BSONObj operand = BSON_ARRAY( 5 << 2 );

        AllMatchExpression e1;
        AllMatchExpression e2;
        AllMatchExpression e3;

        e1.init( "a" );
        e1.getArrayFilterEntries()->addEquality( operand[0] );
        e1.getArrayFilterEntries()->addEquality( operand[1] );

        e2.init( "a" );
        e2.getArrayFilterEntries()->addEquality( operand[0] );

        e3.init( "b" );
        e3.getArrayFilterEntries()->addEquality( operand[0] );
        e3.getArrayFilterEntries()->addEquality( operand[1] );

        ASSERT( e1.equivalent( &e1 ) );
        ASSERT( !e1.equivalent( &e2 ) );
        ASSERT( !e1.equivalent( &e3 ) );

    }


    /**
    TEST( AllMatchExpression, MatchesIndexKey ) {
        BSONObj operand = BSON( "$all" << BSON_ARRAY( 5 ) );
        AllMatchExpression all;
        ASSERT( all.init( "a", operand[ "$all" ] ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "7" );
        ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
                all.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( ElemMatchObjectMatchExpression, MatchesElementSingle ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        BSONObj match = BSON( "a" << BSON_ARRAY( BSON( "b" << 5.0 ) ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( BSON( "b" << 6 ) ) );
        auto_ptr<ComparisonMatchExpression> eq( new EqualityMatchExpression() );
        ASSERT( eq->init( "b", baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( ElemMatchObjectMatchExpression, MatchesElementArray ) {
        BSONObj baseOperand = BSON( "1" << 5 );
        BSONObj match = BSON( "a" << BSON_ARRAY( BSON_ARRAY( 's' << 5.0 ) ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( BSON_ARRAY( 5 << 6 ) ) );
        auto_ptr<ComparisonMatchExpression> eq( new EqualityMatchExpression() );
        ASSERT( eq->init( "1", baseOperand[ "1" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( ElemMatchObjectMatchExpression, MatchesElementMultiple ) {
        BSONObj baseOperand1 = BSON( "b" << 5 );
        BSONObj baseOperand2 = BSON( "b" << 6 );
        BSONObj baseOperand3 = BSON( "c" << 7 );
        BSONObj notMatch1 = BSON( "a" << BSON_ARRAY( BSON( "b" << 5 << "c" << 7 ) ) );
        BSONObj notMatch2 = BSON( "a" << BSON_ARRAY( BSON( "b" << 6 << "c" << 7 ) ) );
        BSONObj notMatch3 = BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 5 << 6 ) ) ) );
        BSONObj match =
            BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 5 << 6 ) << "c" << 7 ) ) );
        auto_ptr<ComparisonMatchExpression> eq1( new EqualityMatchExpression() );
        ASSERT( eq1->init( "b", baseOperand1[ "b" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> eq2( new EqualityMatchExpression() );
        ASSERT( eq2->init( "b", baseOperand2[ "b" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> eq3( new EqualityMatchExpression() );
        ASSERT( eq3->init( "c", baseOperand3[ "c" ] ).isOK() );

        auto_ptr<AndMatchExpression> andOp( new AndMatchExpression() );
        andOp->add( eq1.release() );
        andOp->add( eq2.release() );
        andOp->add( eq3.release() );

        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a", andOp.release() ).isOK() );
        ASSERT( !op.matchesSingleElement( notMatch1[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch2[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch3[ "a" ] ) );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
    }

    TEST( ElemMatchObjectMatchExpression, MatchesNonArray ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        auto_ptr<ComparisonMatchExpression> eq( new EqualityMatchExpression() );
        ASSERT( eq->init( "b", baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        // Directly nested objects are not matched with $elemMatch.  An intervening array is
        // required.
        ASSERT( !op.matches( BSON( "a" << BSON( "b" << 5 ) ), NULL ) );
        ASSERT( !op.matches( BSON( "a" << BSON( "0" << ( BSON( "b" << 5 ) ) ) ), NULL ) );
        ASSERT( !op.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( ElemMatchObjectMatchExpression, MatchesArrayObject ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        auto_ptr<ComparisonMatchExpression> eq( new EqualityMatchExpression() );
        ASSERT( eq->init( "b", baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << 5 ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( 4 << BSON( "b" << 5 ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSONObj() << BSON( "b" << 5 ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << 6 ) << BSON( "b" << 5 ) ) ),
                            NULL ) );
    }

    TEST( ElemMatchObjectMatchExpression, MatchesMultipleNamedValues ) {
        BSONObj baseOperand = BSON( "c" << 5 );
        auto_ptr<ComparisonMatchExpression> eq( new EqualityMatchExpression() );
        ASSERT( eq->init( "c", baseOperand[ "c" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a.b", eq.release() ).isOK() );
        ASSERT( op.matches( BSON( "a" <<
                                  BSON_ARRAY( BSON( "b" <<
                                                    BSON_ARRAY( BSON( "c" <<
                                                                      5 ) ) ) ) ),
                            NULL ) );
        ASSERT( op.matches( BSON( "a" <<
                                  BSON_ARRAY( BSON( "b" <<
                                                    BSON_ARRAY( BSON( "c" <<
                                                                      1 ) ) ) <<
                                              BSON( "b" <<
                                                    BSON_ARRAY( BSON( "c" <<
                                                                      5 ) ) ) ) ),
                            NULL ) );
    }

    TEST( ElemMatchObjectMatchExpression, ElemMatchKey ) {
        BSONObj baseOperand = BSON( "c" << 6 );
        auto_ptr<ComparisonMatchExpression> eq( new EqualityMatchExpression() );
        ASSERT( eq->init( "c", baseOperand[ "c" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a.b", eq.release() ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !op.matches( BSONObj(), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( !op.matches( BSON( "a" << BSON( "b" << BSON_ARRAY( BSON( "c" << 7 ) ) ) ),
                             &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( op.matches( BSON( "a" << BSON( "b" << BSON_ARRAY( 3 << BSON( "c" << 6 ) ) ) ),
                            &details ) );
        ASSERT( details.hasElemMatchKey() );
        // The entry within the $elemMatch array is reported.
        ASSERT_EQUALS( "1", details.elemMatchKey() );
        ASSERT( op.matches( BSON( "a" <<
                                  BSON_ARRAY( 1 << 2 <<
                                              BSON( "b" << BSON_ARRAY( 3 <<
                                                                       5 <<
                                                                       BSON( "c" << 6 ) ) ) ) ),
                            &details ) );
        ASSERT( details.hasElemMatchKey() );
        // The entry within a parent of the $elemMatch array is reported.
        ASSERT_EQUALS( "2", details.elemMatchKey() );
    }

    /**
    TEST( ElemMatchObjectMatchExpression, MatchesIndexKey ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        auto_ptr<ComparisonMatchExpression> eq( new ComparisonMatchExpression() );
        ASSERT( eq->init( "b", baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        IndexSpec indexSpec( BSON( "a.b" << 1 ) );
        BSONObj indexKey = BSON( "" << "5" );
        ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
                op.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( ElemMatchValueMatchExpression, MatchesElementSingle ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        BSONObj match = BSON( "a" << BSON_ARRAY( 6 ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( 4 ) );
        auto_ptr<ComparisonMatchExpression> gt( new GTMatchExpression() );
        ASSERT( gt->init( "", baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueMatchExpression op;
        ASSERT( op.init( "a", gt.release() ).isOK() );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( ElemMatchValueMatchExpression, MatchesElementMultiple ) {
        BSONObj baseOperand1 = BSON( "$gt" << 1 );
        BSONObj baseOperand2 = BSON( "$lt" << 10 );
        BSONObj notMatch1 = BSON( "a" << BSON_ARRAY( 0 << 1 ) );
        BSONObj notMatch2 = BSON( "a" << BSON_ARRAY( 10 << 11 ) );
        BSONObj match = BSON( "a" << BSON_ARRAY( 0 << 5 << 11 ) );
        auto_ptr<ComparisonMatchExpression> gt( new GTMatchExpression() );
        ASSERT( gt->init( "", baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonMatchExpression> lt( new LTMatchExpression() );
        ASSERT( lt->init( "", baseOperand2[ "$lt" ] ).isOK() );

        ElemMatchValueMatchExpression op;
        ASSERT( op.init( "a" ).isOK() );
        op.add( gt.release() );
        op.add( lt.release() );

        ASSERT( !op.matchesSingleElement( notMatch1[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch2[ "a" ] ) );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
    }

    TEST( ElemMatchValueMatchExpression, MatchesNonArray ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        auto_ptr<ComparisonMatchExpression> gt( new GTMatchExpression() );
        ASSERT( gt->init( "", baseOperand[ "$gt" ] ).isOK() );
        ElemMatchObjectMatchExpression op;
        ASSERT( op.init( "a", gt.release() ).isOK() );
        // Directly nested objects are not matched with $elemMatch.  An intervening array is
        // required.
        ASSERT( !op.matches( BSON( "a" << 6 ), NULL ) );
        ASSERT( !op.matches( BSON( "a" << BSON( "0" << 6 ) ), NULL ) );
    }

    TEST( ElemMatchValueMatchExpression, MatchesArrayScalar ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        auto_ptr<ComparisonMatchExpression> gt( new GTMatchExpression() );
        ASSERT( gt->init( "", baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueMatchExpression op;
        ASSERT( op.init( "a", gt.release() ).isOK() );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( 6 ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( 4 << 6 ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSONObj() << 7 ) ), NULL ) );
    }

    TEST( ElemMatchValueMatchExpression, MatchesMultipleNamedValues ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        auto_ptr<ComparisonMatchExpression> gt( new GTMatchExpression() );
        ASSERT( gt->init( "", baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueMatchExpression op;
        ASSERT( op.init( "a.b", gt.release() ).isOK() );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 6 ) ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" <<
                                  BSON_ARRAY( BSON( "b" << BSON_ARRAY( 4 ) ) <<
                                              BSON( "b" << BSON_ARRAY( 4 << 6 ) ) ) ),
                            NULL ) );
    }

    TEST( ElemMatchValueMatchExpression, ElemMatchKey ) {
        BSONObj baseOperand = BSON( "$gt" << 6 );
        auto_ptr<ComparisonMatchExpression> gt( new GTMatchExpression() );
        ASSERT( gt->init( "", baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueMatchExpression op;
        ASSERT( op.init( "a.b", gt.release() ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !op.matches( BSONObj(), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( !op.matches( BSON( "a" << BSON( "b" << BSON_ARRAY( 2 ) ) ),
                             &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( op.matches( BSON( "a" << BSON( "b" << BSON_ARRAY( 3 << 7 ) ) ),
                            &details ) );
        ASSERT( details.hasElemMatchKey() );
        // The entry within the $elemMatch array is reported.
        ASSERT_EQUALS( "1", details.elemMatchKey() );
        ASSERT( op.matches( BSON( "a" <<
                                  BSON_ARRAY( 1 << 2 <<
                                              BSON( "b" << BSON_ARRAY( 3 << 7 ) ) ) ),
                            &details ) );
        ASSERT( details.hasElemMatchKey() );
        // The entry within a parent of the $elemMatch array is reported.
        ASSERT_EQUALS( "2", details.elemMatchKey() );
    }

    /**
    TEST( ElemMatchValueMatchExpression, MatchesIndexKey ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<LtOp> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        ElemMatchValueMatchExpression op;
        ASSERT( op.init( "a", lt.release() ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "3" );
        ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
                op.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( AllElemMatchOp, MatchesElement ) {


        BSONObj baseOperanda1 = BSON( "a" << 1 );
        auto_ptr<ComparisonMatchExpression> eqa1( new EqualityMatchExpression() );
        ASSERT( eqa1->init( "a", baseOperanda1[ "a" ] ).isOK() );

        BSONObj baseOperandb1 = BSON( "b" << 1 );
        auto_ptr<ComparisonMatchExpression> eqb1( new EqualityMatchExpression() );
        ASSERT( eqb1->init( "b", baseOperandb1[ "b" ] ).isOK() );

        auto_ptr<AndMatchExpression> and1( new AndMatchExpression() );
        and1->add( eqa1.release() );
        and1->add( eqb1.release() );
        // and1 = { a : 1, b : 1 }

        auto_ptr<ElemMatchObjectMatchExpression> elemMatch1( new ElemMatchObjectMatchExpression() );
        elemMatch1->init( "x", and1.release() );
        // elemMatch1 = { x : { $elemMatch : { a : 1, b : 1 } } }

        BSONObj baseOperanda2 = BSON( "a" << 2 );
        auto_ptr<ComparisonMatchExpression> eqa2( new EqualityMatchExpression() );
        ASSERT( eqa2->init( "a", baseOperanda2[ "a" ] ).isOK() );

        BSONObj baseOperandb2 = BSON( "b" << 2 );
        auto_ptr<ComparisonMatchExpression> eqb2( new EqualityMatchExpression() );
        ASSERT( eqb2->init( "b", baseOperandb2[ "b" ] ).isOK() );

        auto_ptr<AndMatchExpression> and2( new AndMatchExpression() );
        and2->add( eqa2.release() );
        and2->add( eqb2.release() );

        auto_ptr<ElemMatchObjectMatchExpression> elemMatch2( new ElemMatchObjectMatchExpression() );
        elemMatch2->init( "x", and2.release() );
        // elemMatch2 = { x : { $elemMatch : { a : 2, b : 2 } } }

        AllElemMatchOp op;
        op.init( "" );
        op.add( elemMatch1.release() );
        op.add( elemMatch2.release() );

        BSONObj nonArray = BSON( "x" << 4 );
        ASSERT( !op.matchesSingleElement( nonArray[ "x" ] ) );
        BSONObj emptyArray = BSON( "x" << BSONArray() );
        ASSERT( !op.matchesSingleElement( emptyArray[ "x" ] ) );
        BSONObj nonObjArray = BSON( "x" << BSON_ARRAY( 4 ) );
        ASSERT( !op.matchesSingleElement( nonObjArray[ "x" ] ) );
        BSONObj singleObjMatch = BSON( "x" << BSON_ARRAY( BSON( "a" << 1 << "b" << 1 ) ) );
        ASSERT( !op.matchesSingleElement( singleObjMatch[ "x" ] ) );
        BSONObj otherObjMatch = BSON( "x" << BSON_ARRAY( BSON( "a" << 2 << "b" << 2 ) ) );
        ASSERT( !op.matchesSingleElement( otherObjMatch[ "x" ] ) );
        BSONObj bothObjMatch = BSON( "x" << BSON_ARRAY( BSON( "a" << 1 << "b" << 1 ) <<
                                                        BSON( "a" << 2 << "b" << 2 ) ) );
        ASSERT( op.matchesSingleElement( bothObjMatch[ "x" ] ) );
        BSONObj noObjMatch = BSON( "x" << BSON_ARRAY( BSON( "a" << 1 << "b" << 2 ) <<
                                                      BSON( "a" << 2 << "b" << 1 ) ) );
        ASSERT( !op.matchesSingleElement( noObjMatch[ "x" ] ) );
    }


    TEST( AllElemMatchOp, Matches ) {
        BSONObj baseOperandgt1 = BSON( "$gt" << 1 );
        auto_ptr<ComparisonMatchExpression> gt1( new GTMatchExpression() );
        ASSERT( gt1->init( "", baseOperandgt1[ "$gt" ] ).isOK() );

        BSONObj baseOperandlt1 = BSON( "$lt" << 10 );
        auto_ptr<ComparisonMatchExpression> lt1( new LTMatchExpression() );
        ASSERT( lt1->init( "", baseOperandlt1[ "$lt" ] ).isOK() );

        auto_ptr<ElemMatchValueMatchExpression> elemMatch1( new ElemMatchValueMatchExpression() );
        elemMatch1->init( "x" );
        elemMatch1->add( gt1.release() );
        elemMatch1->add( lt1.release() );

        BSONObj baseOperandgt2 = BSON( "$gt" << 101 );
        auto_ptr<ComparisonMatchExpression> gt2( new GTMatchExpression() );
        ASSERT( gt2->init( "", baseOperandgt2[ "$gt" ] ).isOK() );

        BSONObj baseOperandlt2 = BSON( "$lt" << 110 );
        auto_ptr<ComparisonMatchExpression> lt2( new LTMatchExpression() );
        ASSERT( lt2->init( "", baseOperandlt2[ "$lt" ] ).isOK() );

        auto_ptr<ElemMatchValueMatchExpression> elemMatch2( new ElemMatchValueMatchExpression() );
        elemMatch2->init( "x" );
        elemMatch2->add( gt2.release() );
        elemMatch2->add( lt2.release() );

        AllElemMatchOp op;
        op.init( "x" );
        op.add( elemMatch1.release() );
        op.add( elemMatch2.release() );


        BSONObj nonArray = BSON( "x" << 4 );
        ASSERT( !op.matches( nonArray, NULL ) );
        BSONObj emptyArray = BSON( "x" << BSONArray() );
        ASSERT( !op.matches( emptyArray, NULL ) );
        BSONObj nonNumberArray = BSON( "x" << BSON_ARRAY( "q" ) );
        ASSERT( !op.matches( nonNumberArray, NULL ) );
        BSONObj singleMatch = BSON( "x" << BSON_ARRAY( 5 ) );
        ASSERT( !op.matches( singleMatch, NULL ) );
        BSONObj otherMatch = BSON( "x" << BSON_ARRAY( 105 ) );
        ASSERT( !op.matches( otherMatch, NULL ) );
        BSONObj bothMatch = BSON( "x" << BSON_ARRAY( 5 << 105 ) );
        ASSERT( op.matches( bothMatch, NULL ) );
        BSONObj neitherMatch = BSON( "x" << BSON_ARRAY( 0 << 200 ) );
        ASSERT( !op.matches( neitherMatch, NULL ) );
    }

    /**
    TEST( AllElemMatchOp, MatchesIndexKey ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<LtOp> lt( new ComparisonMatchExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        auto_ptr<ElemMatchValueMatchExpression> elemMatchValueOp( new ElemMatchValueMatchExpression() );
        ASSERT( elemMatchValueOp->init( "a", lt.release() ).isOK() );
        OwnedPointerVector<MatchMatchExpression> subMatchExpressions;
        subMatchExpressions.mutableVector().push_back( elemMatchValueOp.release() );
        AllElemMatchOp allElemMatchOp;
        ASSERT( allElemMatchOp.init( &subMatchExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "3" );
        ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
                allElemMatchOp.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( SizeMatchExpression, MatchesElement ) {
        BSONObj match = BSON( "a" << BSON_ARRAY( 5 << 6 ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( 5 ) );
        SizeMatchExpression size;
        ASSERT( size.init( "", 2 ).isOK() );
        ASSERT( size.matchesSingleElement( match.firstElement() ) );
        ASSERT( !size.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( SizeMatchExpression, MatchesNonArray ) {
        // Non arrays do not match.
        BSONObj stringValue = BSON( "a" << "z" );
        BSONObj numberValue = BSON( "a" << 0 );
        BSONObj arrayValue = BSON( "a" << BSONArray() );
        SizeMatchExpression size;
        ASSERT( size.init( "", 0 ).isOK() );
        ASSERT( !size.matchesSingleElement( stringValue.firstElement() ) );
        ASSERT( !size.matchesSingleElement( numberValue.firstElement() ) );
        ASSERT( size.matchesSingleElement( arrayValue.firstElement() ) );
    }

    TEST( SizeMatchExpression, MatchesArray ) {
        SizeMatchExpression size;
        ASSERT( size.init( "a", 2 ).isOK() );
        ASSERT( size.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 ) ), NULL ) );
        // Arrays are not unwound to look for matching subarrays.
        ASSERT( !size.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 << BSON_ARRAY( 1 << 2 ) ) ),
                               NULL ) );
    }

    TEST( SizeMatchExpression, MatchesNestedArray ) {
        SizeMatchExpression size;
        ASSERT( size.init( "a.2", 2 ).isOK() );
        // A numerically referenced nested array is matched.
        ASSERT( size.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 << BSON_ARRAY( 1 << 2 ) ) ),
                              NULL ) );
    }

    TEST( SizeMatchExpression, ElemMatchKey ) {
        SizeMatchExpression size;
        ASSERT( size.init( "a.b", 3 ).isOK() );
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT( !size.matches( BSON( "a" << 1 ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( size.matches( BSON( "a" << BSON( "b" << BSON_ARRAY( 1 << 2 << 3 ) ) ), &details ) );
        ASSERT( !details.hasElemMatchKey() );
        ASSERT( size.matches( BSON( "a" <<
                                    BSON_ARRAY( 2 <<
                                                BSON( "b" << BSON_ARRAY( 1 << 2 << 3 ) ) ) ),
                              &details ) );
        ASSERT( details.hasElemMatchKey() );
        ASSERT_EQUALS( "1", details.elemMatchKey() );
    }

    TEST( SizeMatchExpression, Equivalent ) {
        SizeMatchExpression e1;
        SizeMatchExpression e2;
        SizeMatchExpression e3;

        e1.init( "a", 5 );
        e2.init( "a", 6 );
        e3.init( "v", 5 );

        ASSERT( e1.equivalent( &e1 ) );
        ASSERT( !e1.equivalent( &e2 ) );
        ASSERT( !e1.equivalent( &e3 ) );
    }

    /**
       TEST( SizeMatchExpression, MatchesIndexKey ) {
       BSONObj operand = BSON( "$size" << 4 );
       SizeMatchExpression size;
       ASSERT( size.init( "a", operand[ "$size" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       BSONObj indexKey = BSON( "" << 1 );
       ASSERT( MatchMatchExpression::PartialMatchResult_Unknown ==
       size.matchesIndexKey( indexKey, indexSpec ) );
       }
    */

} // namespace mongo

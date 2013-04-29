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
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_tree.h"

namespace mongo {

    TEST( AllExpression, MatchesElementSingle ) {
        BSONObj operand = BSON_ARRAY( 1 << 1 );
        BSONObj match = BSON( "a" << 1 );
        BSONObj notMatch = BSON( "a" << 2 );
        AllExpression all;
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );

        ASSERT( all.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !all.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( AllExpression, MatchesEmpty ) {

        BSONObj notMatch = BSON( "a" << 2 );
        AllExpression all;

        ASSERT( !all.matchesSingleElement( notMatch[ "a" ] ) );
        ASSERT( !all.matches( BSON( "a" << 1 ), NULL ) );
        ASSERT( !all.matches( BSONObj(), NULL ) );
    }

    TEST( AllExpression, MatchesElementMultiple ) {
        BSONObj operand = BSON_ARRAY( 1 << "r" );
        AllExpression all;
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );

        BSONObj notMatchFirst = BSON( "a" << 1 );
        BSONObj notMatchSecond = BSON( "a" << "r" );
        BSONObj notMatchArray = BSON( "a" << BSON_ARRAY( 1 << "s" ) ); // XXX

        ASSERT( !all.matchesSingleElement( notMatchFirst[ "a" ] ) );
        ASSERT( !all.matchesSingleElement( notMatchSecond[ "a" ] ) );
        ASSERT( !all.matchesSingleElement( notMatchArray[ "a" ] ) );
    }

    TEST( AllExpression, MatchesScalar ) {
        BSONObj operand = BSON_ARRAY( 5 );
        AllExpression all;
        all.init( "a" );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << 5.0 ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( AllExpression, MatchesArrayValue ) {
        BSONObj operand = BSON_ARRAY( 5 );
        AllExpression all;
        all.init( "a" );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << BSON_ARRAY( 5.0 << 6 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 6 << 7 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( BSON_ARRAY( 5 ) ) ), NULL ) );
    }

    TEST( AllExpression, MatchesNonArrayMultiValues ) {
        BSONObj operand = BSON_ARRAY( 5 << 6 );
        AllExpression all;
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

    TEST( AllExpression, MatchesArrayAndNonArrayMultiValues ) {
        BSONObj operand = BSON_ARRAY( 1 << 2 << 3 << 4 );
        AllExpression all;
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

    TEST( AllExpression, MatchesNull ) {
        BSONObjBuilder allArray;
        allArray.appendNull( "0" );
        BSONObj operand = allArray.obj();

        AllExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSONObj(), NULL ) );
        ASSERT( all.matches( BSON( "a" << BSONNULL ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( AllExpression, MatchesFullArray ) {
        BSONObj operand = BSON_ARRAY( BSON_ARRAY( 1 << 2 ) << 1 );
        AllExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );
        all.getArrayFilterEntries()->addEquality( operand[1] );

        // $all does not match full arrays.
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 1 << 2 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 1 << 2 << 3 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << BSON_ARRAY( 1 ) ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 1 ), NULL ) );
    }

    TEST( AllExpression, ElemMatchKey ) {
        BSONObj operand = BSON_ARRAY( 5 );
        AllExpression all;
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

    TEST( AllExpression, MatchesMinKey ) {
        BSONObj operand = BSON_ARRAY( MinKey );
        AllExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( AllExpression, MatchesMaxKey ) {
        BSONObj operand = BSON_ARRAY( MaxKey );
        AllExpression all;
        ASSERT( all.init( "a" ).isOK() );
        all.getArrayFilterEntries()->addEquality( operand[0] );

        ASSERT( all.matches( BSON( "a" << MaxKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << MinKey ), NULL ) );
        ASSERT( !all.matches( BSON( "a" << 4 ), NULL ) );
    }

    /**
    TEST( AllExpression, MatchesIndexKey ) {
        BSONObj operand = BSON( "$all" << BSON_ARRAY( 5 ) );
        AllExpression all;
        ASSERT( all.init( "a", operand[ "$all" ] ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "7" );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                all.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( ElemMatchObjectExpression, MatchesElementSingle ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        BSONObj match = BSON( "a" << BSON_ARRAY( BSON( "b" << 5.0 ) ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( BSON( "b" << 6 ) ) );
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "b", ComparisonExpression::EQ, baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( ElemMatchObjectExpression, MatchesElementArray ) {
        BSONObj baseOperand = BSON( "1" << 5 );
        BSONObj match = BSON( "a" << BSON_ARRAY( BSON_ARRAY( 's' << 5.0 ) ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( BSON_ARRAY( 5 << 6 ) ) );
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "1", ComparisonExpression::EQ, baseOperand[ "1" ] ).isOK() );
        ElemMatchObjectExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( ElemMatchObjectExpression, MatchesElementMultiple ) {
        BSONObj baseOperand1 = BSON( "b" << 5 );
        BSONObj baseOperand2 = BSON( "b" << 6 );
        BSONObj baseOperand3 = BSON( "c" << 7 );
        BSONObj notMatch1 = BSON( "a" << BSON_ARRAY( BSON( "b" << 5 << "c" << 7 ) ) );
        BSONObj notMatch2 = BSON( "a" << BSON_ARRAY( BSON( "b" << 6 << "c" << 7 ) ) );
        BSONObj notMatch3 = BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 5 << 6 ) ) ) );
        BSONObj match =
            BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 5 << 6 ) << "c" << 7 ) ) );
        auto_ptr<ComparisonExpression> eq1( new ComparisonExpression() );
        ASSERT( eq1->init( "b", ComparisonExpression::EQ, baseOperand1[ "b" ] ).isOK() );
        auto_ptr<ComparisonExpression> eq2( new ComparisonExpression() );
        ASSERT( eq2->init( "b", ComparisonExpression::EQ, baseOperand2[ "b" ] ).isOK() );
        auto_ptr<ComparisonExpression> eq3( new ComparisonExpression() );
        ASSERT( eq3->init( "c", ComparisonExpression::EQ, baseOperand3[ "c" ] ).isOK() );

        auto_ptr<AndExpression> andOp( new AndExpression() );
        andOp->add( eq1.release() );
        andOp->add( eq2.release() );
        andOp->add( eq3.release() );

        ElemMatchObjectExpression op;
        ASSERT( op.init( "a", andOp.release() ).isOK() );
        ASSERT( !op.matchesSingleElement( notMatch1[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch2[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch3[ "a" ] ) );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
    }

    TEST( ElemMatchObjectExpression, MatchesNonArray ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "b", ComparisonExpression::EQ, baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        // Directly nested objects are not matched with $elemMatch.  An intervening array is
        // required.
        ASSERT( !op.matches( BSON( "a" << BSON( "b" << 5 ) ), NULL ) );
        ASSERT( !op.matches( BSON( "a" << BSON( "0" << ( BSON( "b" << 5 ) ) ) ), NULL ) );
        ASSERT( !op.matches( BSON( "a" << 4 ), NULL ) );
    }

    TEST( ElemMatchObjectExpression, MatchesArrayObject ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "b", ComparisonExpression::EQ, baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << 5 ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( 4 << BSON( "b" << 5 ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSONObj() << BSON( "b" << 5 ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << 6 ) << BSON( "b" << 5 ) ) ),
                            NULL ) );
    }

    TEST( ElemMatchObjectExpression, MatchesMultipleNamedValues ) {
        BSONObj baseOperand = BSON( "c" << 5 );
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "c", ComparisonExpression::EQ, baseOperand[ "c" ] ).isOK() );
        ElemMatchObjectExpression op;
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

    TEST( ElemMatchObjectExpression, ElemMatchKey ) {
        BSONObj baseOperand = BSON( "c" << 6 );
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "c", ComparisonExpression::EQ, baseOperand[ "c" ] ).isOK() );
        ElemMatchObjectExpression op;
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
    TEST( ElemMatchObjectExpression, MatchesIndexKey ) {
        BSONObj baseOperand = BSON( "b" << 5 );
        auto_ptr<ComparisonExpression> eq( new ComparisonExpression() );
        ASSERT( eq->init( "b", baseOperand[ "b" ] ).isOK() );
        ElemMatchObjectExpression op;
        ASSERT( op.init( "a", eq.release() ).isOK() );
        IndexSpec indexSpec( BSON( "a.b" << 1 ) );
        BSONObj indexKey = BSON( "" << "5" );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                op.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( ElemMatchValueExpression, MatchesElementSingle ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        BSONObj match = BSON( "a" << BSON_ARRAY( 6 ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( 4 ) );
        auto_ptr<ComparisonExpression> gt( new ComparisonExpression() );
        ASSERT( gt->init( "", ComparisonExpression::GT, baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueExpression op;
        ASSERT( op.init( "a", gt.release() ).isOK() );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch[ "a" ] ) );
    }

    TEST( ElemMatchValueExpression, MatchesElementMultiple ) {
        BSONObj baseOperand1 = BSON( "$gt" << 1 );
        BSONObj baseOperand2 = BSON( "$lt" << 10 );
        BSONObj notMatch1 = BSON( "a" << BSON_ARRAY( 0 << 1 ) );
        BSONObj notMatch2 = BSON( "a" << BSON_ARRAY( 10 << 11 ) );
        BSONObj match = BSON( "a" << BSON_ARRAY( 0 << 5 << 11 ) );
        auto_ptr<ComparisonExpression> gt( new ComparisonExpression() );
        ASSERT( gt->init( "", ComparisonExpression::GT, baseOperand1[ "$gt" ] ).isOK() );
        auto_ptr<ComparisonExpression> lt( new ComparisonExpression() );
        ASSERT( lt->init( "", ComparisonExpression::LT, baseOperand2[ "$lt" ] ).isOK() );

        ElemMatchValueExpression op;
        ASSERT( op.init( "a" ).isOK() );
        op.add( gt.release() );
        op.add( lt.release() );

        ASSERT( !op.matchesSingleElement( notMatch1[ "a" ] ) );
        ASSERT( !op.matchesSingleElement( notMatch2[ "a" ] ) );
        ASSERT( op.matchesSingleElement( match[ "a" ] ) );
    }

    TEST( ElemMatchValueExpression, MatchesNonArray ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        auto_ptr<ComparisonExpression> gt( new ComparisonExpression() );
        ASSERT( gt->init( "", ComparisonExpression::GT, baseOperand[ "$gt" ] ).isOK() );
        ElemMatchObjectExpression op;
        ASSERT( op.init( "a", gt.release() ).isOK() );
        // Directly nested objects are not matched with $elemMatch.  An intervening array is
        // required.
        ASSERT( !op.matches( BSON( "a" << 6 ), NULL ) );
        ASSERT( !op.matches( BSON( "a" << BSON( "0" << 6 ) ), NULL ) );
    }

    TEST( ElemMatchValueExpression, MatchesArrayScalar ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        auto_ptr<ComparisonExpression> gt( new ComparisonExpression() );
        ASSERT( gt->init( "", ComparisonExpression::GT, baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueExpression op;
        ASSERT( op.init( "a", gt.release() ).isOK() );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( 6 ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( 4 << 6 ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSONObj() << 7 ) ), NULL ) );
    }

    TEST( ElemMatchValueExpression, MatchesMultipleNamedValues ) {
        BSONObj baseOperand = BSON( "$gt" << 5 );
        auto_ptr<ComparisonExpression> gt( new ComparisonExpression() );
        ASSERT( gt->init( "", ComparisonExpression::GT, baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueExpression op;
        ASSERT( op.init( "a.b", gt.release() ).isOK() );
        ASSERT( op.matches( BSON( "a" << BSON_ARRAY( BSON( "b" << BSON_ARRAY( 6 ) ) ) ), NULL ) );
        ASSERT( op.matches( BSON( "a" <<
                                  BSON_ARRAY( BSON( "b" << BSON_ARRAY( 4 ) ) <<
                                              BSON( "b" << BSON_ARRAY( 4 << 6 ) ) ) ),
                            NULL ) );
    }

    TEST( ElemMatchValueExpression, ElemMatchKey ) {
        BSONObj baseOperand = BSON( "$gt" << 6 );
        auto_ptr<ComparisonExpression> gt( new ComparisonExpression() );
        ASSERT( gt->init( "", ComparisonExpression::GT, baseOperand[ "$gt" ] ).isOK() );
        ElemMatchValueExpression op;
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
    TEST( ElemMatchValueExpression, MatchesIndexKey ) {
        BSONObj baseOperand = BSON( "$lt" << 5 );
        auto_ptr<LtOp> lt( new ComparisonExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        ElemMatchValueExpression op;
        ASSERT( op.init( "a", lt.release() ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "3" );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                op.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( AllElemMatchOp, MatchesElement ) {


        BSONObj baseOperanda1 = BSON( "a" << 1 );
        auto_ptr<ComparisonExpression> eqa1( new ComparisonExpression() );
        ASSERT( eqa1->init( "a", ComparisonExpression::EQ, baseOperanda1[ "a" ] ).isOK() );

        BSONObj baseOperandb1 = BSON( "b" << 1 );
        auto_ptr<ComparisonExpression> eqb1( new ComparisonExpression() );
        ASSERT( eqb1->init( "b", ComparisonExpression::EQ, baseOperandb1[ "b" ] ).isOK() );

        auto_ptr<AndExpression> and1( new AndExpression() );
        and1->add( eqa1.release() );
        and1->add( eqb1.release() );
        // and1 = { a : 1, b : 1 }

        auto_ptr<ElemMatchObjectExpression> elemMatch1( new ElemMatchObjectExpression() );
        elemMatch1->init( "x", and1.release() );
        // elemMatch1 = { x : { $elemMatch : { a : 1, b : 1 } } }

        BSONObj baseOperanda2 = BSON( "a" << 2 );
        auto_ptr<ComparisonExpression> eqa2( new ComparisonExpression() );
        ASSERT( eqa2->init( "a", ComparisonExpression::EQ, baseOperanda2[ "a" ] ).isOK() );

        BSONObj baseOperandb2 = BSON( "b" << 2 );
        auto_ptr<ComparisonExpression> eqb2( new ComparisonExpression() );
        ASSERT( eqb2->init( "b", ComparisonExpression::EQ, baseOperandb2[ "b" ] ).isOK() );

        auto_ptr<AndExpression> and2( new AndExpression() );
        and2->add( eqa2.release() );
        and2->add( eqb2.release() );

        auto_ptr<ElemMatchObjectExpression> elemMatch2( new ElemMatchObjectExpression() );
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
        auto_ptr<ComparisonExpression> gt1( new ComparisonExpression() );
        ASSERT( gt1->init( "", ComparisonExpression::GT, baseOperandgt1[ "$gt" ] ).isOK() );

        BSONObj baseOperandlt1 = BSON( "$lt" << 10 );
        auto_ptr<ComparisonExpression> lt1( new ComparisonExpression() );
        ASSERT( lt1->init( "", ComparisonExpression::LT, baseOperandlt1[ "$lt" ] ).isOK() );

        auto_ptr<ElemMatchValueExpression> elemMatch1( new ElemMatchValueExpression() );
        elemMatch1->init( "x" );
        elemMatch1->add( gt1.release() );
        elemMatch1->add( lt1.release() );

        BSONObj baseOperandgt2 = BSON( "$gt" << 101 );
        auto_ptr<ComparisonExpression> gt2( new ComparisonExpression() );
        ASSERT( gt2->init( "", ComparisonExpression::GT, baseOperandgt2[ "$gt" ] ).isOK() );

        BSONObj baseOperandlt2 = BSON( "$lt" << 110 );
        auto_ptr<ComparisonExpression> lt2( new ComparisonExpression() );
        ASSERT( lt2->init( "", ComparisonExpression::LT, baseOperandlt2[ "$lt" ] ).isOK() );

        auto_ptr<ElemMatchValueExpression> elemMatch2( new ElemMatchValueExpression() );
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
        auto_ptr<LtOp> lt( new ComparisonExpression() );
        ASSERT( lt->init( "a", baseOperand[ "$lt" ] ).isOK() );
        auto_ptr<ElemMatchValueExpression> elemMatchValueOp( new ElemMatchValueExpression() );
        ASSERT( elemMatchValueOp->init( "a", lt.release() ).isOK() );
        OwnedPointerVector<MatchExpression> subExpressions;
        subExpressions.mutableVector().push_back( elemMatchValueOp.release() );
        AllElemMatchOp allElemMatchOp;
        ASSERT( allElemMatchOp.init( &subExpressions ).isOK() );
        IndexSpec indexSpec( BSON( "a" << 1 ) );
        BSONObj indexKey = BSON( "" << "3" );
        ASSERT( MatchExpression::PartialMatchResult_Unknown ==
                allElemMatchOp.matchesIndexKey( indexKey, indexSpec ) );
    }
    */

    TEST( SizeExpression, MatchesElement ) {
        BSONObj match = BSON( "a" << BSON_ARRAY( 5 << 6 ) );
        BSONObj notMatch = BSON( "a" << BSON_ARRAY( 5 ) );
        SizeExpression size;
        ASSERT( size.init( "", 2 ).isOK() );
        ASSERT( size.matchesSingleElement( match.firstElement() ) );
        ASSERT( !size.matchesSingleElement( notMatch.firstElement() ) );
    }

    TEST( SizeExpression, MatchesNonArray ) {
        // Non arrays do not match.
        BSONObj stringValue = BSON( "a" << "z" );
        BSONObj numberValue = BSON( "a" << 0 );
        BSONObj arrayValue = BSON( "a" << BSONArray() );
        SizeExpression size;
        ASSERT( size.init( "", 0 ).isOK() );
        ASSERT( !size.matchesSingleElement( stringValue.firstElement() ) );
        ASSERT( !size.matchesSingleElement( numberValue.firstElement() ) );
        ASSERT( size.matchesSingleElement( arrayValue.firstElement() ) );
    }

    TEST( SizeExpression, MatchesArray ) {
        SizeExpression size;
        ASSERT( size.init( "a", 2 ).isOK() );
        ASSERT( size.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 ) ), NULL ) );
        // Arrays are not unwound to look for matching subarrays.
        ASSERT( !size.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 << BSON_ARRAY( 1 << 2 ) ) ),
                               NULL ) );
    }

    TEST( SizeExpression, MatchesNestedArray ) {
        SizeExpression size;
        ASSERT( size.init( "a.2", 2 ).isOK() );
        // A numerically referenced nested array is matched.
        ASSERT( size.matches( BSON( "a" << BSON_ARRAY( 4 << 5.5 << BSON_ARRAY( 1 << 2 ) ) ),
                              NULL ) );
    }

    TEST( SizeExpression, ElemMatchKey ) {
        SizeExpression size;
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

    /**
       TEST( SizeExpression, MatchesIndexKey ) {
       BSONObj operand = BSON( "$size" << 4 );
       SizeExpression size;
       ASSERT( size.init( "a", operand[ "$size" ] ).isOK() );
       IndexSpec indexSpec( BSON( "a" << 1 ) );
       BSONObj indexKey = BSON( "" << 1 );
       ASSERT( MatchExpression::PartialMatchResult_Unknown ==
       size.matchesIndexKey( indexKey, indexSpec ) );
       }
    */

} // namespace mongo

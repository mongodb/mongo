// expression_parser_test.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/unittest/unittest.h"

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    /// HACK HACK HACK

    MatchDetails::MatchDetails() :
        _elemMatchKeyRequested() {
        resetOutput();
    }
    void MatchDetails::resetOutput() {
        _loadedRecord = false;
        _elemMatchKeyFound = false;
        _elemMatchKey = "";
    }

    // ----------------------------

    TEST( ExpressionParserTest, SimpleEQ1 ) {
        BSONObj query = BSON( "x" << 2 );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserTest, Multiple1 ) {
        BSONObj query = BSON( "x" << 5 << "y" << BSON( "$gt" << 5 << "$lt" << 8 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 5 << "y" << 7 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 5 << "y" << 6 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 6 << "y" << 7 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 << "y" << 9 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 << "y" << 4 ) ) );
    }

    StatusWith<int> fib( int n ) {
        if ( n < 0 ) return StatusWith<int>( ErrorCodes::BadValue, "paramter to fib has to be >= 0" );
        if ( n <= 1 ) return StatusWith<int>( 1 );
        StatusWith<int> a = fib( n - 1 );
        StatusWith<int> b = fib( n - 2 );
        if ( !a.isOK() ) return a;
        if ( !b.isOK() ) return b;
        return StatusWith<int>( a.getValue() + b.getValue() );
    }

    TEST( StatusWithTest, Fib1 ) {
        StatusWith<int> x = fib( -2 );
        ASSERT( !x.isOK() );

        x = fib(0);
        ASSERT( x.isOK() );
        ASSERT( 1 == x.getValue() );

        x = fib(1);
        ASSERT( x.isOK() );
        ASSERT( 1 == x.getValue() );

        x = fib(2);
        ASSERT( x.isOK() );
        ASSERT( 2 == x.getValue() );

        x = fib(3);
        ASSERT( x.isOK() );
        ASSERT( 3 == x.getValue() );


    }
}

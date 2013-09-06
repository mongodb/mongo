// expression_parser_array_test.cpp

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

#include "mongo/unittest/unittest.h"

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"

namespace mongo {

    TEST( MatchExpressionParserArrayTest, Size1 ) {
        BSONObj query = BSON( "x" << BSON( "$size" << 2 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1  ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 << 3 ) ) ) );
    }

    TEST( MatchExpressionParserArrayTest, SizeAsString ) {
        BSONObj query = BSON( "x" << BSON( "$size" << "a" ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSONArray() ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 ) ) ) );
    }

    TEST( MatchExpressionParserArrayTest, SizeWithDouble ) {
        BSONObj query = BSON( "x" << BSON( "$size" << 2.5 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1  ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSONArray() ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 << 3 ) ) ) );
    }

    TEST( MatchExpressionParserArrayTest, SizeBad ) {
        BSONObj query = BSON( "x" << BSON( "$size" << BSONNULL ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    // ---------

    TEST( MatchExpressionParserArrayTest, ElemMatchArr1 ) {
        BSONObj query = BSON( "x" << BSON( "$elemMatch" << BSON( "x" << 1 << "y" << 2 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY(  BSON( "x" << 1 ) ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" <<
                                                  BSON_ARRAY( BSON( "x" << 1 << "y" << 2 ) ) ) ) );

    }

    TEST( MatchExpressionParserArrayTest, ElemMatchVal1 ) {
        BSONObj query = BSON( "x" << BSON( "$elemMatch" << BSON( "$gt" << 5 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 4 ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 6 ) ) ) );
    }

    TEST( MatchExpressionParserArrayTest, All1 ) {
        BSONObj query = BSON( "x" << BSON( "$all" << BSON_ARRAY( 1 << 2 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 2 ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 << 3 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 2 << 3 ) ) ) );
    }

    TEST( MatchExpressionParserArrayTest, AllBadArg ) {
        BSONObj query = BSON( "x" << BSON( "$all" << 1 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( MatchExpressionParserArrayTest, AllBadRegexArg ) {
        string tooLargePattern( 50 * 1000, 'z' );
        BSONObjBuilder allArray;
        allArray.appendRegex( "0", tooLargePattern, "" );
        BSONObjBuilder operand;
        operand.appendArray( "$all", allArray.obj() );

        BSONObj query = BSON( "x" << operand.obj() );

        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }


    TEST( MatchExpressionParserArrayTest, AllRegex1 ) {
        BSONObjBuilder allArray;
        allArray.appendRegex( "0", "^a", "" );
        allArray.appendRegex( "1", "B", "i" );
        BSONObjBuilder all;
        all.appendArray( "$all", allArray.obj() );
        BSONObj query = BSON( "a" << all.obj() );

        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        BSONObj notMatchFirst = BSON( "a" << "ax" );
        BSONObj notMatchSecond = BSON( "a" << "qqb" );
        BSONObj matchesBoth = BSON( "a" << "ab" );

        ASSERT( !result.getValue()->matchesSingleElement( notMatchFirst[ "a" ] ) );
        ASSERT( !result.getValue()->matchesSingleElement( notMatchSecond[ "a" ] ) );
        ASSERT( result.getValue()->matchesSingleElement( matchesBoth[ "a" ] ) );
    }

    TEST( MatchExpressionParserArrayTest, AllRegex2 ) {
        BSONObjBuilder allArray;
        allArray.appendRegex( "0", "^a", "" );
        allArray.append( "1", "abc" );
        BSONObjBuilder all;
        all.appendArray( "$all", allArray.obj() );
        BSONObj query = BSON( "a" << all.obj() );

        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        BSONObj notMatchFirst = BSON( "a" << "ax" );
        BSONObj matchesBoth = BSON( "a" << "abc" );

        ASSERT( !result.getValue()->matchesSingleElement( notMatchFirst[ "a" ] ) );
        ASSERT( result.getValue()->matchesSingleElement( matchesBoth[ "a" ] ) );
    }

    TEST( MatchExpressionParserArrayTest, AllNonArray ) {
        BSONObj query = BSON( "x" << BSON( "$all" << BSON_ARRAY( 5 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 5 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 4 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 4 ) ) ) );
    }


    TEST( MatchExpressionParserArrayTest, AllElemMatch1 ) {
        BSONObj internal = BSON( "x" << 1 << "y" << 2 );
        BSONObj query = BSON( "x" << BSON( "$all" << BSON_ARRAY( BSON( "$elemMatch" << internal ) ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 << 2 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY(  BSON( "x" << 1 ) ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" <<
                                                  BSON_ARRAY( BSON( "x" << 1 << "y" << 2 ) ) ) ) );

    }

    TEST( MatchExpressionParserArrayTest, AllElemMatchBad ) {
        BSONObj internal = BSON( "x" << 1 << "y" << 2 );

        BSONObj query = BSON( "x" << BSON( "$all" << BSON_ARRAY( BSON( "$elemMatch" << internal ) << 5 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

        query = BSON( "x" << BSON( "$all" << BSON_ARRAY( 5 << BSON( "$elemMatch" << internal ) ) ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

}

// expression_parser_leaf_test.cpp

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
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    TEST( MatchExpressionParserLeafTest, SimpleEQ2 ) {
        BSONObj query = BSON( "x" << BSON( "$eq" << 2 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleGT1 ) {
        BSONObj query = BSON( "x" << BSON( "$gt" << 2 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleLT1 ) {
        BSONObj query = BSON( "x" << BSON( "$lt" << 2 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleGTE1 ) {
        BSONObj query = BSON( "x" << BSON( "$gte" << 2 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleLTE1 ) {
        BSONObj query = BSON( "x" << BSON( "$lte" << 2 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleNE1 ) {
        BSONObj query = BSON( "x" << BSON( "$ne" << 2 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleModBad1 ) {
        BSONObj query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 << 2 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 ) ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 << 2 << 4 ) ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( "q" << 2 ) ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << 3 ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON( "a" << 1 << "b" << 2 ) ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );
    }

    TEST( MatchExpressionParserLeafTest, SimpleMod1 ) {
        BSONObj query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 << 2 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 4 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 8 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleModNotNumber ) {
        BSONObj query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 2 << "r" ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 4 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "a" ) ) );
    }


    TEST( MatchExpressionParserLeafTest, SimpleIN1 ) {
        BSONObj query = BSON( "x" << BSON( "$in" << BSON_ARRAY( 2 << 3 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }


    TEST( MatchExpressionParserLeafTest, INNotArray ) {
        BSONObj query = BSON( "x" << BSON( "$in" << 5 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( MatchExpressionParserLeafTest, INNotElemMatch ) {
        BSONObj query = BSON( "x" << BSON( "$in" << BSON_ARRAY( BSON( "$elemMatch" << 1 ) ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( MatchExpressionParserLeafTest, INRegexTooLong ) {
        string tooLargePattern( 50 * 1000, 'z' );
        BSONObjBuilder inArray;
        inArray.appendRegex( "0", tooLargePattern, "" );
        BSONObjBuilder operand;
        operand.appendArray( "$in", inArray.obj() );
        BSONObj query = BSON( "x" << operand.obj() );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( MatchExpressionParserLeafTest, INRegexTooLong2 ) {
        string tooLargePattern( 50 * 1000, 'z' );
        BSONObj query = BSON( "x" << BSON( "$in" << BSON_ARRAY( BSON( "$regex" << tooLargePattern ) ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( MatchExpressionParserLeafTest, INRegexStuff ) {
        BSONObjBuilder inArray;
        inArray.appendRegex( "0", "^a", "" );
        inArray.appendRegex( "1", "B", "i" );
        inArray.append( "2", 4 );
        BSONObjBuilder operand;
        operand.appendArray( "$in", inArray.obj() );

        BSONObj query = BSON( "a" << operand.obj() );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        BSONObj matchFirst = BSON( "a" << "ax" );
        BSONObj matchFirstRegex = BSONObjBuilder().appendRegex( "a", "^a", "" ).obj();
        BSONObj matchSecond = BSON( "a" << "qqb" );
        BSONObj matchSecondRegex = BSONObjBuilder().appendRegex( "a", "B", "i" ).obj();
        BSONObj matchThird = BSON( "a" << 4 );
        BSONObj notMatch = BSON( "a" << "l" );
        BSONObj notMatchRegex = BSONObjBuilder().appendRegex( "a", "B", "" ).obj();

        ASSERT( result.getValue()->matchesBSON( matchFirst ) );
        ASSERT( result.getValue()->matchesBSON( matchFirstRegex ) );
        ASSERT( result.getValue()->matchesBSON( matchSecond ) );
        ASSERT( result.getValue()->matchesBSON( matchSecondRegex ) );
        ASSERT( result.getValue()->matchesBSON( matchThird ) );
        ASSERT( !result.getValue()->matchesBSON( notMatch ) );
        ASSERT( !result.getValue()->matchesBSON( notMatchRegex ) );
    }

    TEST( MatchExpressionParserLeafTest, SimpleNIN1 ) {
        BSONObj query = BSON( "x" << BSON( "$nin" << BSON_ARRAY( 2 << 3 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, NINNotArray ) {
        BSONObj query = BSON( "x" << BSON( "$nin" << 5 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }


    TEST( MatchExpressionParserLeafTest, Regex1 ) {
        BSONObjBuilder b;
        b.appendRegex( "x", "abc", "i" );
        BSONObj query = b.obj();
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "abc" ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "ABC" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "AC" ) ) );
    }

    TEST( MatchExpressionParserLeafTest, Regex2 ) {
        BSONObj query = BSON( "x" << BSON( "$regex" << "abc" << "$options" << "i" ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "abc" ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "ABC" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "AC" ) ) );
    }

    TEST( MatchExpressionParserLeafTest, Regex3 ) {
        BSONObj query = BSON( "x" << BSON( "$options" << "i" << "$regex" << "abc" ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        log() << "result: " << result << endl;
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "abc" ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "ABC" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "AC" ) ) );
    }


    TEST( MatchExpressionParserLeafTest, RegexBad ) {
        BSONObj query = BSON( "x" << BSON( "$regex" << "abc" << "$optionas" << "i" ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

        query = BSON( "x" << BSON( "$optionas" << "i" ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

        query = BSON( "x" << BSON( "$options" << "i" ) );
        result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( MatchExpressionParserLeafTest, ExistsYes1 ) {
        BSONObjBuilder b;
        b.appendBool( "$exists", true );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "abc" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "y" << "AC" ) ) );
    }

    TEST( MatchExpressionParserLeafTest, ExistsNO1 ) {
        BSONObjBuilder b;
        b.appendBool( "$exists", false );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "abc" ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "y" << "AC" ) ) );
    }

    TEST( MatchExpressionParserLeafTest, Type1 ) {
        BSONObj query = BSON( "x" << BSON( "$type" << String ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "abc" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, Type2 ) {
        BSONObj query = BSON( "x" << BSON( "$type" << (double)NumberDouble ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 5.3 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, TypeDoubleOperator ) {
        BSONObj query = BSON( "x" << BSON( "$type" << 1.5 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5.3 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, TypeNull ) {
        BSONObj query = BSON( "x" << BSON( "$type" << jstNULL ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSONObj() ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
        BSONObjBuilder b;
        b.appendNull( "x" );
        ASSERT( result.getValue()->matchesBSON( b.obj() ) );
    }

    TEST( MatchExpressionParserLeafTest, TypeBadType ) {
        BSONObjBuilder b;
        b.append( "$type", ( JSTypeMax + 1 ) );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5.3 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 5 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, TypeBad ) {
        BSONObj query = BSON( "x" << BSON( "$type" << BSON( "x" << 1 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

}

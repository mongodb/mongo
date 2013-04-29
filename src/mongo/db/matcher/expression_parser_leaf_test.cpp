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
 */

#include "mongo/unittest/unittest.h"

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    TEST( ExpressionParserLeafTest, SimpleEQ2 ) {
        BSONObj query = BSON( "x" << BSON( "$eq" << 2 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserLeafTest, SimpleGT1 ) {
        BSONObj query = BSON( "x" << BSON( "$gt" << 2 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserLeafTest, SimpleLT1 ) {
        BSONObj query = BSON( "x" << BSON( "$lt" << 2 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserLeafTest, SimpleGTE1 ) {
        BSONObj query = BSON( "x" << BSON( "$gte" << 2 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserLeafTest, SimpleLTE1 ) {
        BSONObj query = BSON( "x" << BSON( "$lte" << 2 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserLeafTest, SimpleNE1 ) {
        BSONObj query = BSON( "x" << BSON( "$ne" << 2 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserLeafTest, SimpleModBad1 ) {
        BSONObj query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 << 2 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 ) ) );
        result = ExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 << 2 << 4 ) ) );
        result = ExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( "q" << 2 ) ) );
        result = ExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << 3 ) );
        result = ExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );

        query = BSON( "x" << BSON( "$mod" << BSON( "a" << 1 << "b" << 2 ) ) );
        result = ExpressionParser::parse( query );
        ASSERT_TRUE( !result.isOK() );
    }

    TEST( ExpressionParserLeafTest, SimpleMod1 ) {
        BSONObj query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 3 << 2 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 5 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 4 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 8 ) ) );
    }

    TEST( ExpressionParserLeafTest, SimpleModNotNumber ) {
        BSONObj query = BSON( "x" << BSON( "$mod" << BSON_ARRAY( 2 << "r" ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 4 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << "a" ) ) );
    }


    TEST( ExpressionParserLeafTest, SimpleIN1 ) {
        BSONObj query = BSON( "x" << BSON( "$in" << BSON_ARRAY( 2 << 3 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 3 ) ) );
    }


    TEST( ExpressionParserLeafTest, INNotArray ) {
        BSONObj query = BSON( "x" << BSON( "$in" << 5 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( ExpressionParserLeafTest, INNotElemMatch ) {
        BSONObj query = BSON( "x" << BSON( "$in" << BSON_ARRAY( BSON( "$elemMatch" << 1 ) ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( ExpressionParserLeafTest, INRegexTooLong ) {
        string tooLargePattern( 50 * 1000, 'z' );
        BSONObjBuilder inArray;
        inArray.appendRegex( "0", tooLargePattern, "" );
        BSONObjBuilder operand;
        operand.appendArray( "$in", inArray.obj() );
        BSONObj query = BSON( "x" << operand.obj() );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( ExpressionParserLeafTest, INRegexTooLong2 ) {
        string tooLargePattern( 50 * 1000, 'z' );
        BSONObj query = BSON( "x" << BSON( "$in" << BSON_ARRAY( BSON( "$regex" << tooLargePattern ) ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

    TEST( ExpressionParserLeafTest, INRegexStuff ) {
        BSONObjBuilder inArray;
        inArray.appendRegex( "0", "^a", "" );
        inArray.appendRegex( "1", "B", "i" );
        inArray.append( "2", 4 );
        BSONObjBuilder operand;
        operand.appendArray( "$in", inArray.obj() );

        BSONObj query = BSON( "a" << operand.obj() );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        BSONObj matchFirst = BSON( "a" << "ax" );
        BSONObj matchFirstRegex = BSONObjBuilder().appendRegex( "a", "^a", "" ).obj();
        BSONObj matchSecond = BSON( "a" << "qqb" );
        BSONObj matchSecondRegex = BSONObjBuilder().appendRegex( "a", "B", "i" ).obj();
        BSONObj matchThird = BSON( "a" << 4 );
        BSONObj notMatch = BSON( "a" << "l" );
        BSONObj notMatchRegex = BSONObjBuilder().appendRegex( "a", "B", "" ).obj();

        ASSERT( result.getValue()->matches( matchFirst ) );
        ASSERT( result.getValue()->matches( matchFirstRegex ) );
        ASSERT( result.getValue()->matches( matchSecond ) );
        ASSERT( result.getValue()->matches( matchSecondRegex ) );
        ASSERT( result.getValue()->matches( matchThird ) );
        ASSERT( !result.getValue()->matches( notMatch ) );
        ASSERT( !result.getValue()->matches( notMatchRegex ) );
    }

    TEST( ExpressionParserLeafTest, SimpleNIN1 ) {
        BSONObj query = BSON( "x" << BSON( "$nin" << BSON_ARRAY( 2 << 3 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
    }

    TEST( ExpressionParserLeafTest, NINNotArray ) {
        BSONObj query = BSON( "x" << BSON( "$nin" << 5 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }


    TEST( ExpressionParserLeafTest, Regex1 ) {
        BSONObjBuilder b;
        b.appendRegex( "x", "abc", "i" );
        BSONObj query = b.obj();
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << "abc" ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << "ABC" ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << "AC" ) ) );
    }

    TEST( ExpressionParserLeafTest, Regex2 ) {
        BSONObj query = BSON( "x" << BSON( "$regex" << "abc" << "$options" << "i" ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << "abc" ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << "ABC" ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << "AC" ) ) );
    }

    TEST( ExpressionParserLeafTest, RegexBad ) {
        BSONObj query = BSON( "x" << BSON( "$regex" << "abc" << "$optionas" << "i" ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

        query = BSON( "x" << BSON( "$optionas" << "i" ) );
        result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

        query = BSON( "x" << BSON( "$options" << "i" ) );
        result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

        // has to be in the other order
        query = BSON( "x" << BSON( "$options" << "i" << "$regex" << "abc" ) );
        result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

        query = BSON( "x" << BSON( "$gt" << "i" << "$regex" << "abc" ) );
        result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );

    }

    TEST( ExpressionParserLeafTest, ExistsYes1 ) {
        BSONObjBuilder b;
        b.appendBool( "$exists", true );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << "abc" ) ) );
        ASSERT( !result.getValue()->matches( BSON( "y" << "AC" ) ) );
    }

    TEST( ExpressionParserLeafTest, ExistsNO1 ) {
        BSONObjBuilder b;
        b.appendBool( "$exists", false );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << "abc" ) ) );
        ASSERT( result.getValue()->matches( BSON( "y" << "AC" ) ) );
    }

    TEST( ExpressionParserLeafTest, Type1 ) {
        BSONObj query = BSON( "x" << BSON( "$type" << String ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << "abc" ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 ) ) );
    }

    TEST( ExpressionParserLeafTest, Type2 ) {
        BSONObj query = BSON( "x" << BSON( "$type" << (double)NumberDouble ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 5.3 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 ) ) );
    }

    TEST( ExpressionParserLeafTest, TypeDoubleOperator ) {
        BSONObj query = BSON( "x" << BSON( "$type" << 1.5 ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 5.3 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 ) ) );
    }

    TEST( ExpressionParserLeafTest, TypeNull ) {
        BSONObj query = BSON( "x" << BSON( "$type" << jstNULL ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSONObj() ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 ) ) );
        BSONObjBuilder b;
        b.appendNull( "x" );
        ASSERT( result.getValue()->matches( b.obj() ) );
    }

    TEST( ExpressionParserLeafTest, TypeBadType ) {
        BSONObjBuilder b;
        b.append( "$type", ( JSTypeMax + 1 ) );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 5.3 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 5 ) ) );
    }

    TEST( ExpressionParserLeafTest, TypeBad ) {
        BSONObj query = BSON( "x" << BSON( "$type" << BSON( "x" << 1 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_FALSE( result.isOK() );
    }

}

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

    // with explicit $eq
    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef1 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "$db" << "db" );
        OID oidx = OID::gen();
        BSONObj notMatch = BSON( "$ref" << "coll" << "$id" << oidx << "$db" << "db" );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << BSON( "$eq" << match ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );
    }

    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef2 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "$db" << "db" );
        OID oidx = OID::gen();
        BSONObj notMatch = BSON( "$ref" << "coll" << "$id" << oidx << "$db" << "db" );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << match ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );
    }

    // Additional fields after $ref and $id.
    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef3 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 );
        OID oidx = OID::gen();
        BSONObj notMatch = BSON( "$ref" << "coll" << "$id" << oidx << "foo" << 12345 );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << match ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );

        // Document contains fields not referred to in $elemMatch query.
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY(
                BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 << "bar" << 678 ) ) ) ) );
    }

    // Query with DBRef fields out of order.
    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef4 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "$db" << "db" );
        BSONObj matchOutOfOrder = BSON( "$db" << "db" << "$id" << oid << "$ref" << "coll" );
        OID oidx = OID::gen();
        BSONObj notMatch = BSON( "$ref" << "coll" << "$id" << oidx << "$db" << "db" );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << matchOutOfOrder ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );
    }

    // Query with DBRef fields out of order.
    // Additional fields besides $ref and $id.
    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef5 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 );
        BSONObj matchOutOfOrder = BSON( "foo" << 12345 << "$id" << oid << "$ref" << "coll" );
        OID oidx = OID::gen();
        BSONObj notMatch = BSON( "$ref" << "coll" << "$id" << oidx << "foo" << 12345 );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << matchOutOfOrder ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );

        // Document contains fields not referred to in $elemMatch query.
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY(
                BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 << "bar" << 678 ) ) ) ) );
    }

    // Incomplete DBRef - $id missing.
    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef6 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 );
        BSONObj matchMissingID = BSON( "$ref" << "coll" << "foo" << 12345 );
        BSONObj notMatch = BSON( "$ref" << "collx" << "$id" << oid << "foo" << 12345 );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << matchMissingID ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );

        // Document contains fields not referred to in $elemMatch query.
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY(
                BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 << "bar" << 678 ) ) ) ) );
    }

    // Incomplete DBRef - $ref missing.
    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef7 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 );
        BSONObj matchMissingRef = BSON( "$id" << oid << "foo" << 12345 );
        OID oidx = OID::gen();
        BSONObj notMatch = BSON( "$ref" << "coll" << "$id" << oidx << "foo" << 12345 );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << matchMissingRef ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );

        // Document contains fields not referred to in $elemMatch query.
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY(
                BSON( "$ref" << "coll" << "$id" << oid << "foo" << 12345 << "bar" << 678 ) ) ) ) );
    }

    // Incomplete DBRef - $db only.
    TEST( MatchExpressionParserArrayTest, ElemMatchDBRef8 ) {
        OID oid = OID::gen();
        BSONObj match = BSON( "$ref" << "coll" << "$id" << oid << "$db" << "db"
                           << "foo" << 12345 );
        BSONObj matchDBOnly = BSON( "$db" << "db" << "foo" << 12345 );
        BSONObj notMatch = BSON( "$ref" << "coll" << "$id" << oid << "$db" << "dbx"
                              << "foo" << 12345 );

        BSONObj query = BSON( "x" << BSON( "$elemMatch" << matchDBOnly ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( notMatch ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match ) ) ) );

        // Document contains fields not referred to in $elemMatch query.
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY(
                BSON( "$ref" << "coll" << "$id" << oid << "$db" << "db"
                   << "foo" << 12345 << "bar" << 678 ) ) ) ) );
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

    TEST( MatchExpressionParserArrayTest, AllNull ) {
        BSONObj query = BSON( "x" << BSON( "$all" << BSON_ARRAY( BSONNULL ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( 1 ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSONNULL ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL ) ) ) );
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

    // $all and $elemMatch on dotted field.
    // Top level field can be either document or array.
    TEST( MatchExpressionParserArrayTest, AllElemMatch2 ) {
        BSONObj internal = BSON( "z" << 1 );
        BSONObj query = BSON( "x.y" << BSON( "$all" <<
                                         BSON_ARRAY( BSON( "$elemMatch" << internal ) ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON( "y" << 1 ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON( "y" <<
                                                                BSON_ARRAY( 1 << 2 ) ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" <<
                                                   BSON( "y" <<
                                                     BSON_ARRAY( BSON( "x" << 1 ) ) ) ) ) );
        // x is a document. Internal document does not contain z.
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" <<
                                                   BSON( "y" <<
                                                     BSON_ARRAY(
                                                       BSON( "x" << 1 << "y" << 1 ) ) ) ) ) );
        // x is an array. Internal document does not contain z.
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" <<
                                                   BSON_ARRAY(
                                                     BSON( "y" <<
                                                       BSON_ARRAY(
                                                         BSON( "x" << 1 << "y" << 1 ) ) ) ) ) ) );
        // x is a document but y is not an array.
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" <<
                                                   BSON( "y" <<
                                                     BSON( "x" << 1 << "z" << 1 ) ) ) ) );
        // x is an array but y is not an array.
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" <<
                                                   BSON_ARRAY(
                                                     BSON( "y" <<
                                                       BSON( "x" << 1 << "z" << 1 ) ) ) ) ) );
        // x is a document.
        ASSERT( result.getValue()->matchesBSON( BSON( "x" <<
                                                  BSON( "y" <<
                                                    BSON_ARRAY(
                                                      BSON( "x" << 1 << "z" << 1 ) ) ) ) ) );
        // x is an array.
        ASSERT( result.getValue()->matchesBSON( BSON( "x" <<
                                                  BSON_ARRAY(
                                                    BSON( "y" <<
                                                      BSON_ARRAY(
                                                        BSON( "x" << 1 << "z" << 1 ) ) ) ) ) ) );
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

    // $all with empty string.
    TEST( MatchExpressionParserArrayTest, AllEmptyString ) {
        BSONObj query = BSON( "x" << BSON( "$all" << BSON_ARRAY( "" ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "a" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL << "a" ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() << "a" ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSONArray() ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "" ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL << "" ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() << "" ) ) ) );
    }

    // $all with ISO date.
    TEST( MatchExpressionParserArrayTest, AllISODate ) {
        StatusWith<Date_t> matchResult = dateFromISOString("2014-12-31T00:00:00.000Z");
        ASSERT_TRUE( matchResult.isOK() );
        const Date_t& match = matchResult.getValue();
        StatusWith<Date_t> notMatchResult = dateFromISOString("2014-12-30T00:00:00.000Z");
        ASSERT_TRUE( notMatchResult.isOK() );
        const Date_t& notMatch = notMatchResult.getValue();

        BSONObj query = BSON( "x" << BSON( "$all" << BSON_ARRAY( match ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << notMatch ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL <<
                                                                          notMatch ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() <<
                                                                          notMatch ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSONArray() ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL <<
                                                                         match ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() <<
                                                                         match ) ) ) );
    }

    // $all on array element with empty string.
    TEST( MatchExpressionParserArrayTest, AllDottedEmptyString ) {
        BSONObj query = BSON( "x.1" << BSON( "$all" << BSON_ARRAY( "" ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "a" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL << "a" ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() << "a" ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( "" << BSONNULL ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( "" << BSONObj() ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSONArray() ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "" ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL << "" ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() << "" ) ) ) );
    }

    // $all on array element with ISO date.
    TEST( MatchExpressionParserArrayTest, AllDottedISODate ) {
        StatusWith<Date_t> matchResult = dateFromISOString("2014-12-31T00:00:00.000Z");
        ASSERT_TRUE( matchResult.isOK() );
        const Date_t& match = matchResult.getValue();
        StatusWith<Date_t> notMatchResult = dateFromISOString("2014-12-30T00:00:00.000Z");
        ASSERT_TRUE( notMatchResult.isOK() );
        const Date_t& notMatch = notMatchResult.getValue();

        BSONObj query = BSON( "x.1" << BSON( "$all" << BSON_ARRAY( match ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << notMatch ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL <<
                                                                          notMatch ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() <<
                                                                          notMatch ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match <<
                                                                          BSONNULL ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( match <<
                                                                          BSONObj() ) ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << BSONArray() ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << match ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONNULL <<
                                                                         match ) ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << BSON_ARRAY( BSONObj() <<
                                                                         match ) ) ) );
    }

}

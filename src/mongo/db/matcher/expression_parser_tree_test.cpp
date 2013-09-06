// expression_parser_tree_test.cpp

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

    TEST( MatchExpressionParserTreeTest, OR1 ) {
        BSONObj query = BSON( "$or" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                   BSON( "y" << 2 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "y" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "y" << 1 ) ) );
    }

    TEST( MatchExpressionParserTreeTest, OREmbedded ) {
        BSONObj query1 = BSON( "$or" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                    BSON( "y" << 2 ) ) );
        BSONObj query2 = BSON( "$or" << BSON_ARRAY( query1 ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query2 );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "y" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "y" << 1 ) ) );
    }


    TEST( MatchExpressionParserTreeTest, AND1 ) {
        BSONObj query = BSON( "$and" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                    BSON( "y" << 2 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "y" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "y" << 1 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 1 << "y" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 2 << "y" << 2 ) ) );
    }

    TEST( MatchExpressionParserTreeTest, NOREmbedded ) {
        BSONObj query = BSON( "$nor" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                    BSON( "y" << 2 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "y" << 2 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 3 ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "y" << 1 ) ) );
    }

    TEST( MatchExpressionParserTreeTest, NOT1 ) {
        BSONObj query = BSON( "x" << BSON( "$not" << BSON( "$gt" << 5 ) ) );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matchesBSON( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << 8 ) ) );
    }

    TEST( MatchExpressionParserLeafTest, NotRegex1 ) {
        BSONObjBuilder b;
        b.appendRegex( "$not", "abc", "i" );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "abc" ) ) );
        ASSERT( !result.getValue()->matchesBSON( BSON( "x" << "ABC" ) ) );
        ASSERT( result.getValue()->matchesBSON( BSON( "x" << "AC" ) ) );
    }

}

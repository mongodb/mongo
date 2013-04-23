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

    TEST( ExpressionParserTreeTest, OR1 ) {
        BSONObj query = BSON( "$or" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                   BSON( "y" << 2 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matches( BSON( "y" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "y" << 1 ) ) );
    }

    TEST( ExpressionParserTreeTest, OREmbedded ) {
        BSONObj query1 = BSON( "$or" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                    BSON( "y" << 2 ) ) );
        BSONObj query2 = BSON( "$or" << BSON_ARRAY( query1 ) );
        StatusWithExpression result = ExpressionParser::parse( query2 );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( result.getValue()->matches( BSON( "y" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "y" << 1 ) ) );
    }


    TEST( ExpressionParserTreeTest, AND1 ) {
        BSONObj query = BSON( "$and" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                    BSON( "y" << 2 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "y" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 3 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "y" << 1 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 1 << "y" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 2 << "y" << 2 ) ) );
    }

    TEST( ExpressionParserTreeTest, NOREmbedded ) {
        BSONObj query = BSON( "$nor" << BSON_ARRAY( BSON( "x" << 1 ) <<
                                                    BSON( "y" << 2 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << 1 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "y" << 2 ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << 3 ) ) );
        ASSERT( result.getValue()->matches( BSON( "y" << 1 ) ) );
    }

    TEST( ExpressionParserTreeTest, NOT1 ) {
        BSONObj query = BSON( "x" << BSON( "$not" << BSON( "$gt" << 5 ) ) );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( result.getValue()->matches( BSON( "x" << 2 ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << 8 ) ) );
    }

    TEST( ExpressionParserLeafTest, NotRegex1 ) {
        BSONObjBuilder b;
        b.appendRegex( "$not", "abc", "i" );
        BSONObj query = BSON( "x" << b.obj() );
        StatusWithExpression result = ExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT( !result.getValue()->matches( BSON( "x" << "abc" ) ) );
        ASSERT( !result.getValue()->matches( BSON( "x" << "ABC" ) ) );
        ASSERT( result.getValue()->matches( BSON( "x" << "AC" ) ) );
    }

}

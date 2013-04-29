// expression_test.cpp

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

/** Unit tests for MatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    TEST( ExpressionTest, Parse1 ) {
        //TreeExpression* e = NULL;
        //Status s = Expression::parse( BSON( "x" << 1 ), &e );
        //ASSERT_TRUE( s.isOK() );
    }

    TEST( LeafExpressionTest, Equal1 ) {
        BSONObj temp = BSON( "x" << 5 );
        ComparisonExpression e;
        e.init( "x", ComparisonExpression::EQ, temp["x"] );

        ASSERT_TRUE( e.matches( fromjson( "{ x : 5 }" ) ) );
        ASSERT_TRUE( e.matches( fromjson( "{ x : [5] }" ) ) );
        ASSERT_TRUE( e.matches( fromjson( "{ x : [1,5] }" ) ) );
        ASSERT_TRUE( e.matches( fromjson( "{ x : [1,5,2] }" ) ) );
        ASSERT_TRUE( e.matches( fromjson( "{ x : [5,2] }" ) ) );

        ASSERT_FALSE( e.matches( fromjson( "{ x : null }" ) ) );
        ASSERT_FALSE( e.matches( fromjson( "{ x : 6 }" ) ) );
        ASSERT_FALSE( e.matches( fromjson( "{ x : [4,2] }" ) ) );
        ASSERT_FALSE( e.matches( fromjson( "{ x : [[5]] }" ) ) );
    }

    TEST( LeafExpressionTest, Comp1 ) {
        BSONObj temp = BSON( "x" << 5 );

        {
            ComparisonExpression e;
            e.init( "x", ComparisonExpression::LTE, temp["x"] );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            ComparisonExpression e;
            e.init( "x", ComparisonExpression::LT, temp["x"] );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            ComparisonExpression e;
            e.init( "x", ComparisonExpression::GTE, temp["x"] );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            ComparisonExpression e;
            e.init( "x", ComparisonExpression::GT, temp["x"] );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }


    }

}

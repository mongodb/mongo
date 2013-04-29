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

/** Unit tests for MatchMatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

    TEST( MatchExpressionTest, Parse1 ) {
        //TreeMatchExpression* e = NULL;
        //Status s = MatchExpression::parse( BSON( "x" << 1 ), &e );
        //ASSERT_TRUE( s.isOK() );
    }

    TEST( LeafMatchExpressionTest, Equal1 ) {
        BSONObj temp = BSON( "x" << 5 );
        ComparisonMatchExpression e;
        e.init( "x", ComparisonMatchExpression::EQ, temp["x"] );

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

    TEST( LeafMatchExpressionTest, Comp1 ) {
        BSONObj temp = BSON( "x" << 5 );

        {
            ComparisonMatchExpression e;
            e.init( "x", ComparisonMatchExpression::LTE, temp["x"] );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            ComparisonMatchExpression e;
            e.init( "x", ComparisonMatchExpression::LT, temp["x"] );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            ComparisonMatchExpression e;
            e.init( "x", ComparisonMatchExpression::GTE, temp["x"] );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            ComparisonMatchExpression e;
            e.init( "x", ComparisonMatchExpression::GT, temp["x"] );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 5 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 4 }" ) ) );
            ASSERT_TRUE( e.matches( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matches( fromjson( "{ x : 'eliot' }" ) ) );
        }


    }

}

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
        EqualityMatchExpression e;
        e.init( "x", temp["x"] );

        ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : 5 }" ) ) );
        ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : [5] }" ) ) );
        ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : [1,5] }" ) ) );
        ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : [1,5,2] }" ) ) );
        ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : [5,2] }" ) ) );

        ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : null }" ) ) );
        ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 6 }" ) ) );
        ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : [4,2] }" ) ) );
        ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : [[5]] }" ) ) );
    }

    TEST( LeafMatchExpressionTest, Comp1 ) {
        BSONObj temp = BSON( "x" << 5 );

        {
            LTEMatchExpression e;
            e.init( "x", temp["x"] );
            ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : 5 }" ) ) );
            ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : 4 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            LTMatchExpression e;
            e.init( "x", temp["x"] );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 5 }" ) ) );
            ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : 4 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            GTEMatchExpression e;
            e.init( "x", temp["x"] );
            ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : 5 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 4 }" ) ) );
            ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 'eliot' }" ) ) );
        }

        {
            GTMatchExpression e;
            e.init( "x", temp["x"] );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 5 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 4 }" ) ) );
            ASSERT_TRUE( e.matchesBSON( fromjson( "{ x : 6 }" ) ) );
            ASSERT_FALSE( e.matchesBSON( fromjson( "{ x : 'eliot' }" ) ) );
        }


    }

}

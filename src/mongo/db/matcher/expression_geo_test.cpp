// expression_geo_test.cpp

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

/** Unit tests for MatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_geo.h"

namespace mongo {

    TEST( ExpressionGeoTest, Geo1 ) {
        BSONObj query = fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

        GeoQuery gq;
        ASSERT( gq.parseFrom( query["loc"].Obj() ) );

        GeoMatchExpression ge;
        ASSERT( ge.init( "a", gq ).isOK() );

        ASSERT(!ge.matchesBSON(fromjson("{a: [3,4]}")));
        ASSERT(ge.matchesBSON(fromjson("{a: [4,4]}")));
        ASSERT(ge.matchesBSON(fromjson("{a: [5,5]}")));
        ASSERT(ge.matchesBSON(fromjson("{a: [5,5.1]}")));
        ASSERT(ge.matchesBSON(fromjson("{a: {x: 5, y:5.1}}")));

    }

    TEST(ExpressionGeoTest, GeoNear1) {
        BSONObj query = fromjson("{loc:{$near:{$maxDistance:100, "
                                 "$geometry:{type:\"Point\", coordinates:[0,0]}}}}");
        NearQuery nq;
        ASSERT(nq.parseFrom(query["loc"].Obj()));

        GeoNearMatchExpression gne;
        ASSERT(gne.init("a", nq).isOK());

        // We can't match the data but we can make sure it was parsed OK.
        ASSERT_EQUALS(gne.getData().fromRadians, false);
        ASSERT_EQUALS(gne.getData().minDistance, 0);
        ASSERT_EQUALS(gne.getData().maxDistance, 100);
    }

}

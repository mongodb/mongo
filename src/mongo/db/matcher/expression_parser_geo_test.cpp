// expression_parser_geo_test.cpp

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
#include "mongo/db/matcher/expression_geo.h"

namespace mongo {

    TEST( MatchExpressionParserGeo, WithinBox ) {
        BSONObj query = fromjson("{a:{$within:{$box:[{x: 4, y:4},[6,6]]}}}");

        StatusWithMatchExpression result = MatchExpressionParser::parse( query );
        ASSERT_TRUE( result.isOK() );

        ASSERT(!result.getValue()->matches(fromjson("{a: [3,4]}")));
        ASSERT(result.getValue()->matches(fromjson("{a: [4,4]}")));
        ASSERT(result.getValue()->matches(fromjson("{a: [5,5]}")));
        ASSERT(result.getValue()->matches(fromjson("{a: [5,5.1]}")));
        ASSERT(result.getValue()->matches(fromjson("{a: {x: 5, y:5.1}}")));

    }
}

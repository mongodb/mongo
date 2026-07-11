// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_internal_expr_comparison.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

DEATH_TEST_REGEX(InternalExprComparisonMatchExpressionDeathTest,
                 CannotCompareToArray,
                 "Tripwire assertion.*11052406") {
    BSONObj operand = BSON("x" << BSON_ARRAY(1 << 2));
    InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}

DEATH_TEST_REGEX(InternalExprComparisonMatchExpressionDeathTest,
                 CannotCompareToUndefined,
                 "Tripwire assertion.*11052405") {
    BSONObj operand = BSON("x" << BSONUndefined);
    InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}

}  // namespace mongo

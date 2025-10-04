/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/** Unit tests for MatchMatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/db/matcher/expression_tree.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

DEATH_TEST_REGEX(NotMatchExpression,
                 GetChildFailsIndexLargerThanOne,
                 "Tripwire assertion.*6400210") {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};

    ASSERT_EQ(notOp.numChildren(), 1);
    ASSERT_THROWS_CODE(notOp.getChild(1), AssertionException, 6400210);
}

DEATH_TEST_REGEX(AndOp, GetChildFailsOnIndexLargerThanChildren, "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 1);
    auto baseOperand2 = BSON("$lt" << 10);
    auto baseOperand3 = BSON("$lt" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<LTMatchExpression>("b"_sd, baseOperand3["$lt"]);

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));
    andOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(andOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(andOp.getChild(numChildren), AssertionException, 6400201);
}

DEATH_TEST_REGEX(OrOp, GetChildFailsOnIndexLargerThanChildren, "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);
    auto sub1 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand3["b"]);

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(sub1));
    orOp.add(std::move(sub2));
    orOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(orOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(orOp.getChild(numChildren), AssertionException, 6400201);
}

TEST(NorOp, Equivalent) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);
    auto sub1 = EqualityMatchExpression{"a"_sd, baseOperand1["a"]};
    auto sub2 = EqualityMatchExpression{"b"_sd, baseOperand2["b"]};

    auto e1 = NorMatchExpression{};
    e1.add(sub1.clone());
    e1.add(sub2.clone());

    auto e2 = NorMatchExpression{};
    e2.add(sub1.clone());

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
}

DEATH_TEST_REGEX(NorOp, GetChildFailsOnIndexLargerThanChildren, "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand3["b"]);

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(sub1));
    norOp.add(std::move(sub2));
    norOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(norOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(norOp.getChild(numChildren), AssertionException, 6400201);
}

}  // namespace mongo

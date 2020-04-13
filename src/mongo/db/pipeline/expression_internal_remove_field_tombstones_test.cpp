/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_internal_remove_field_tombstones.h"
#include "mongo/unittest/unittest.h"

namespace mongo::expression_internal_tests {

class ExpressionInternalRemoveFieldTombstonesTest : public AggregationContextFixture {
public:
    auto createExpression() {
        auto expr = make_intrusive<ExpressionInternalRemoveFieldTombstones>(
            getExpCtx(),
            ExpressionFieldPath::parse(getExpCtx(), "$$ROOT", getExpCtx()->variablesParseState));
        return expr;
    }
};

TEST_F(ExpressionInternalRemoveFieldTombstonesTest, RemovesTombstoneValues) {
    auto expr = createExpression();
    Document input =
        DOC("a" << Value() << "b" << 1 << "c" << Value() << "d" << 2 << "e" << Value());
    BSONObj expected = BSON("b" << 1 << "d" << 2);
    ASSERT_BSONOBJ_EQ(expr->evaluate(input, &getExpCtx()->variables).getDocument().toBson(),
                      expected);
}

TEST_F(ExpressionInternalRemoveFieldTombstonesTest, RemovesNestedTombstoneValues) {
    auto expr = createExpression();
    Document input =
        DOC("a" << 1 << "b" << DOC("c" << 2 << "d" << Value() << "e" << Value()) << "f" << 2);
    BSONObj expected = BSON("a" << 1 << "b" << BSON("c" << 2) << "f" << 2);
    ASSERT_BSONOBJ_EQ(expr->evaluate(input, &getExpCtx()->variables).getDocument().toBson(),
                      expected);
    input = DOC("a" << 1 << "b" << DOC("c" << DOC("d" << Value() << "e" << 5)) << "f" << 2);
    expected = BSON("a" << 1 << "b" << BSON("c" << BSON("e" << 5)) << "f" << 2);
    ASSERT_BSONOBJ_EQ(expr->evaluate(input, &getExpCtx()->variables).getDocument().toBson(),
                      expected);
}

}  // namespace mongo::expression_internal_tests
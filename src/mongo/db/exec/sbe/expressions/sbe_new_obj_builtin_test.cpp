/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::sbe {

class SBEBuiltinNewObjTest : public EExpressionTestFixture {
protected:
    std::unique_ptr<EExpression> makeNewObjOfLargeValues(size_t numFields, size_t valueSize) {
        EExpression::Vector args;
        args.reserve(numFields * 2);
        const std::string largeStr(valueSize, 'a');
        for (size_t i = 0; i < numFields; ++i) {
            args.emplace_back(makeStringConstant("f" + std::to_string(i)));
            args.emplace_back(makeStringConstant(largeStr));
        }
        return makeFunction("newObj", std::move(args));
    }
};

TEST_F(SBEBuiltinNewObjTest, NewObjWithinLimitSucceeds) {
    auto newObjExpr = makeNewObjOfLargeValues(10, 1024);
    auto compiledExpr = compileExpression(*newObjExpr);

    auto [tag, val] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(tag, val);

    ASSERT(value::isObject(tag));
    ASSERT_EQUALS(value::getObjectView(val)->size(), 10u);
}

TEST_F(SBEBuiltinNewObjTest, NewObjExceedsMemoryLimit) {
    auto newObjExpr = makeNewObjOfLargeValues(10, 1024);
    auto compiledExpr = compileExpression(*newObjExpr);

    RAIIServerParameterControllerForTest limit("internalQueryMaxExpressionOutputBytes", 10 * 1024);
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

TEST_F(SBEBuiltinNewObjTest, NewObjLongFieldNamesCountTowardMemoryLimit) {
    EExpression::Vector args;
    const std::string longKey(200, 'k');
    for (int i = 0; i < 10; ++i) {
        args.emplace_back(makeStringConstant(longKey + std::to_string(i)));
        args.emplace_back(makeInt32Constant(1));
    }
    auto newObjExpr = makeFunction("newObj", std::move(args));
    auto compiledExpr = compileExpression(*newObjExpr);

    RAIIServerParameterControllerForTest limit("internalQueryMaxExpressionOutputBytes", 500);
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

}  // namespace mongo::sbe

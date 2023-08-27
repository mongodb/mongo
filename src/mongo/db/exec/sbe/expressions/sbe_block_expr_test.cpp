/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <cstdint>
#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/unittest/assert.h"

namespace mongo::sbe {

class SBEBlockExpressionTest : public EExpressionTestFixture {
public:
    void assertBlockOfBool(value::TypeTags tag, value::Value val, std::vector<bool> expected) {
        ASSERT_EQ(tag, value::TypeTags::valueBlock);
        auto* block = value::bitcastTo<value::ValueBlock*>(val);
        auto extracted = block->extract();

        for (size_t i = 0; i < extracted.count; ++i) {
            ASSERT_EQ(extracted.tags[i], value::TypeTags::Boolean);
            ASSERT_EQ(value::bitcastTo<bool>(extracted.vals[i]), expected[i]) << extracted;
        }
    }
};

TEST_F(SBEBlockExpressionTest, BlockExistsTest) {
    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    auto existsExpr =
        sbe::makeE<sbe::EFunction>("valueBlockExists", sbe::makeEs(makeE<EVariable>(blockSlot)));
    auto compiledExpr = compileExpression(*existsExpr);

    value::HeterogeneousBlock block;
    block.push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));
    block.push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(43));
    block.push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(44));
    block.push_back(value::TypeTags::Nothing, value::Value(0));
    block.push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(46));

    blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(&block));
    auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(runTag, runVal);

    assertBlockOfBool(runTag, runVal, std::vector{true, true, true, false, true});
}

}  // namespace mongo::sbe

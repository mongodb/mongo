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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/exec/shard_filterer_mock.h"

namespace mongo::sbe {

class SbeShardFilterBuiltinTest : public EExpressionTestFixture {
protected:
    /**
     * A helper to compile and run a shard filter expression function. The 'shardFilterer' argument
     * must be an sbe::value::TypeTag::shardFilter, and this helper transfers ownership of the
     * shardFilterer to the VM.
     */
    void runAndAssertExpression(std::pair<value::TypeTags, value::Value> shardFilterer,
                                value::SlotId shardKeySlot,
                                bool expectedRes) {
        auto shardFiltererCheckExpr =
            makeE<EFunction>("shardFilter",
                             makeEs(makeE<EConstant>(shardFilterer.first, shardFilterer.second),
                                    makeE<EVariable>(shardKeySlot)));
        auto compiledExpr = compileExpression(*shardFiltererCheckExpr);
        auto actualRes = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT_EQ(actualRes, expectedRes);
    }

    /**
     * Makes a shard filter that always passes. The returned value is owned by the caller.
     */
    std::pair<value::TypeTags, value::Value> makeAlwaysPassShardFilter() {
        return {value::TypeTags::shardFilterer,
                value::bitcastFrom<ConstantFilterMock*>(new ConstantFilterMock(true, BSONObj()))};
    }

    /**
     * Makes a shard filter that always fails. The returned value is owned by the caller.
     */
    std::pair<value::TypeTags, value::Value> makeAlwaysFailShardFilter() {
        return {value::TypeTags::shardFilterer,
                value::bitcastFrom<ConstantFilterMock*>(new ConstantFilterMock(false, BSONObj()))};
    }

    /**
     * Makes a view of a BSONObj. The caller doesn't own the returned value.
     */
    std::pair<value::TypeTags, value::Value> makeViewOfObject(const BSONObj& obj) {
        return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata())};
    }
};

TEST_F(SbeShardFilterBuiltinTest, BasicFiltering) {
    value::ViewOfValueAccessor objAccessor;
    auto objSlot = bindAccessor(&objAccessor);

    BSONObj emptyBson;
    objAccessor.reset(value::TypeTags::bsonObject,
                      value::bitcastFrom<const char*>(emptyBson.objdata()));

    runAndAssertExpression(makeAlwaysPassShardFilter(), objSlot, true);
    runAndAssertExpression(makeAlwaysFailShardFilter(), objSlot, false);
    runAndAssertExpression(makeAlwaysPassShardFilter(), objSlot, true);
    runAndAssertExpression(makeAlwaysFailShardFilter(), objSlot, false);
};

TEST_F(SbeShardFilterBuiltinTest, MissingShardKey) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    inputSlotAccessor.reset(value::TypeTags::Nothing, 0);
    runAndAssertExpression(makeAlwaysPassShardFilter(), inputSlot, false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 0);
    runAndAssertExpression(makeAlwaysPassShardFilter(), inputSlot, false);
};

TEST_F(SbeShardFilterBuiltinTest, ShardKeyAsObjectNotBsonObjIsRejected) {
    value::ViewOfValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    auto [tagEmptyObj, valEmptyObj] = value::makeNewObject();
    value::ValueGuard objGuard{tagEmptyObj, valEmptyObj};
    inputSlotAccessor.reset(tagEmptyObj, valEmptyObj);

    runAndAssertExpression(makeAlwaysPassShardFilter(), inputSlot, false);
    runAndAssertExpression(makeAlwaysFailShardFilter(), inputSlot, false);
};

TEST_F(SbeShardFilterBuiltinTest, BadScopedCollFilterValue) {
    value::ViewOfValueAccessor objAccessor;
    auto objSlot = bindAccessor(&objAccessor);

    BSONObj emptyBson;
    objAccessor.reset(value::TypeTags::bsonObject,
                      value::bitcastFrom<const char*>(emptyBson.objdata()));

    runAndAssertExpression({value::TypeTags::Boolean, true}, objSlot, false);
    runAndAssertExpression({value::TypeTags::Boolean, true}, objSlot, false);
};
}  // namespace mongo::sbe

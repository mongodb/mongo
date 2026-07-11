// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/shard_filterer_mock.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

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
            makeE<EFunction>(EFn::kShardFilter,
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

    value::TagValueOwned emptyObj = value::TagValueOwned::fromRaw(value::makeNewObject());
    inputSlotAccessor.reset(emptyObj.tag(), emptyObj.value());

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

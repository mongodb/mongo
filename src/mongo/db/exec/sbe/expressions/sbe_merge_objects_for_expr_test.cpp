// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {
namespace {

class SBEMergeObjectsForExprTest : public EExpressionTestFixture {
protected:
    /**
     * Runs 'mergeObjectsForExpr' over the values in 'accessors' (one argument per accessor) and
     * returns the resulting object as BSON.
     */
    BSONObj runMergeObjects(const std::vector<value::SlotId>& argSlots) {
        EExpression::Vector args;
        for (auto slot : argSlots) {
            args.push_back(makeE<EVariable>(slot));
        }
        auto compiledExpr =
            compileExpression(*makeE<EFunction>(EFn::kMergeObjectsForExpr, std::move(args)));

        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
        ASSERT_EQUALS(value::TypeTags::Object, result.tag());

        BSONObjBuilder bob;
        bson::convertToBsonObj(bob, value::getObjectView(result.value()));
        return bob.obj();
    }
};

TEST_F(SBEMergeObjectsForExprTest, MergesMultipleObjectArguments) {
    value::ViewOfValueAccessor acc1;
    value::ViewOfValueAccessor acc2;
    auto slot1 = bindAccessor(&acc1);
    auto slot2 = bindAccessor(&acc2);

    auto obj1 = BSON("a" << 1 << "b" << 2);
    auto obj2 = BSON("b" << 3 << "c" << 4);
    acc1.reset(value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj1.objdata()));
    acc2.reset(value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj2.objdata()));

    // Later values win, but the field order of the first object is preserved.
    ASSERT_BSONOBJ_EQ(BSON("a" << 1 << "b" << 3 << "c" << 4), runMergeObjects({slot1, slot2}));
}

TEST_F(SBEMergeObjectsForExprTest, MergesElementsOfSingleArrayArgument) {
    value::OwnedValueAccessor acc;
    auto slot = bindAccessor(&acc);

    auto obj1 = BSON("a" << 1);
    auto obj2 = BSON("a" << 2 << "b" << 3);
    auto [arrTag, arrVal] = value::makeNewArray();
    auto* arr = value::getArrayView(arrVal);
    arr->push_back_raw(value::copyValue(value::TypeTags::bsonObject,
                                        value::bitcastFrom<const char*>(obj1.objdata())));
    arr->push_back_raw(value::copyValue(value::TypeTags::bsonObject,
                                        value::bitcastFrom<const char*>(obj2.objdata())));
    acc.reset(arrTag, arrVal);

    ASSERT_BSONOBJ_EQ(BSON("a" << 2 << "b" << 3), runMergeObjects({slot}));
}

TEST_F(SBEMergeObjectsForExprTest, IgnoresNullishArguments) {
    value::ViewOfValueAccessor acc1;
    value::ViewOfValueAccessor acc2;
    value::ViewOfValueAccessor acc3;
    auto slot1 = bindAccessor(&acc1);
    auto slot2 = bindAccessor(&acc2);
    auto slot3 = bindAccessor(&acc3);

    auto obj = BSON("a" << 1);
    acc1.reset(value::TypeTags::Null, 0);
    acc2.reset(value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata()));
    acc3.reset(value::TypeTags::Nothing, 0);

    ASSERT_BSONOBJ_EQ(BSON("a" << 1), runMergeObjects({slot1, slot2, slot3}));
}

TEST_F(SBEMergeObjectsForExprTest, NoArgumentsYieldsEmptyObject) {
    ASSERT_BSONOBJ_EQ(BSONObj{}, runMergeObjects({}));
}

TEST_F(SBEMergeObjectsForExprTest, NonObjectArgumentFails) {
    value::ViewOfValueAccessor acc;
    auto slot = bindAccessor(&acc);
    acc.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));

    ASSERT_THROWS_CODE(runMergeObjects({slot}), DBException, 5158600);
}

}  // namespace
}  // namespace mongo::sbe

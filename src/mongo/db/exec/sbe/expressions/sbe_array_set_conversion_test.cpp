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
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace mongo::sbe {

class SBEArraySetConversionTest : public EExpressionTestFixture {
public:
    /**
     * Run the compiled expression and assert that the result is equal to Nothing.
     */
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(result.tag(), sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(result.value(), 0);
    }

    /**
     * Run the compiled expression and assert that the result is equal to 'expectedValues'.
     */
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, BSONArray expectedValues) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));

        ASSERT_EQ(value::TypeTags::ArraySet, result.tag());

        value::ArraySet* arrSet = value::getArraySetView(result.value());
        value::ValueSetType& values = arrSet->values();

        size_t expectedItems = 0;
        for (auto elem : expectedValues) {
            auto [tag, val] = bson::convertToView(elem);

            ASSERT_NE(values.end(), values.find(std::make_pair(tag, val)))
                << "Value " << std::make_pair(tag, val) << " not found in result";
            expectedItems++;
        }
        ASSERT_EQ(expectedItems, arrSet->size()) << std::make_pair(result.tag(), result.value());
    }

    /**
     * Helper function that converts a BSON array into a SBE Array
     */
    std::pair<value::TypeTags, value::Value> convertFromBSONArray(BSONArray arr) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::Array* arrView = value::getArrayView(arrVal);

        for (auto elem : arr) {
            auto [tag, val] = bson::convertToOwned(elem).releaseToRaw();
            arrView->push_back_raw(tag, val);
        }
        return {arrTag, arrVal};
    }
};

/**
 * Test the behaviour of 'arrayToSet' with arrays containing scalar values, BSON objects, empty
 * arrays and invalid arguments.
 */
TEST_F(SBEArraySetConversionTest, ArrayToSetExpression) {
    value::OwnedValueAccessor inputAccessor;
    value::SlotId inputSlot = bindAccessor(&inputAccessor);

    std::unique_ptr<EExpression> arrayToSetExpr =
        sbe::makeE<sbe::EFunction>(EFn::kArrayToSet, sbe::makeEs(makeE<EVariable>(inputSlot)));
    std::unique_ptr<vm::CodeFragment> compiledArrayToSet = compileExpression(*arrayToSetExpr);

    // Test with Array on first variant
    BSONArray bsonArr1 = BSON_ARRAY(1 << 1 << 2 << 1 << 2);
    auto arr1 = value::TagValueOwned::fromRaw(convertFromBSONArray(bsonArr1));
    inputAccessor.reset(std::move(arr1));
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with bsonArray on first variant
    inputAccessor.reset(value::TagValueView{value::TypeTags::bsonArray,
                                            value::bitcastFrom<const char*>(bsonArr1.objdata())});
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with Array on second variant
    BSONArray bsonArr2 =
        BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << BSON("x" << 1) << "Y");
    auto arr2 = value::TagValueOwned::fromRaw(convertFromBSONArray(bsonArr2));
    inputAccessor.reset(std::move(arr2));
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << "Y"));

    // Test with bsonArray on second variant
    inputAccessor.reset(value::TagValueView{value::TypeTags::bsonArray,
                                            value::bitcastFrom<const char*>(bsonArr2.objdata())});
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << "Y"));

    // Test with empty array
    auto emptyArr = value::TagValueOwned::fromRaw(value::makeNewArray());
    inputAccessor.reset(std::move(emptyArr));
    runAndAssertExpression(compiledArrayToSet.get(), BSONArray());

    // Test when input is not array Type
    inputAccessor.reset(value::TagValueView::numberInt64(42));
    runAndAssertNothing(compiledArrayToSet.get());
}

/**
 * Test the behaviour of 'collArrayToSet' with arrays containing scalar values, BSON objects, empty
 * arrays and invalid arguments. The collator is the one used in unit tests that compares using the
 * lowercase version of the data.
 */
TEST_F(SBEArraySetConversionTest, CollArrayToSetExpression) {
    value::OwnedValueAccessor inputAccessor;
    value::SlotId inputSlot = bindAccessor(&inputAccessor);

    std::unique_ptr<CollatorInterface> collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    std::unique_ptr<EExpression> arrayToSetExpr = sbe::makeE<sbe::EFunction>(
        EFn::kCollArrayToSet,
        sbe::makeEs(makeE<EConstant>(value::TypeTags::collator,
                                     value::bitcastFrom<CollatorInterface*>(collator.release())),
                    makeE<EVariable>(inputSlot)));
    std::unique_ptr<vm::CodeFragment> compiledArrayToSet = compileExpression(*arrayToSetExpr);

    // Test with Array on first variant
    BSONArray bsonArr1 = BSON_ARRAY(1 << 1 << 2 << 1 << 2);
    auto arr1 = value::TagValueOwned::fromRaw(convertFromBSONArray(bsonArr1));
    inputAccessor.reset(std::move(arr1));
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with bsonArray on first variant
    inputAccessor.reset(value::TagValueView{value::TypeTags::bsonArray,
                                            value::bitcastFrom<const char*>(bsonArr1.objdata())});
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with Array on second variant
    BSONArray bsonArr2 =
        BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << BSON("x" << 1) << "Y");
    auto arr2 = value::TagValueOwned::fromRaw(convertFromBSONArray(bsonArr2));
    inputAccessor.reset(std::move(arr2));
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2)));

    // Test with bsonArray on second variant
    inputAccessor.reset(value::TagValueView{value::TypeTags::bsonArray,
                                            value::bitcastFrom<const char*>(bsonArr2.objdata())});
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2)));

    // Test with empty array
    auto emptyArr = value::TagValueOwned::fromRaw(value::makeNewArray());
    inputAccessor.reset(std::move(emptyArr));
    runAndAssertExpression(compiledArrayToSet.get(), BSONArray());

    // Test when input is not array Type
    inputAccessor.reset(value::TagValueView::numberInt64(42));
    runAndAssertNothing(compiledArrayToSet.get());
}
}  // namespace mongo::sbe

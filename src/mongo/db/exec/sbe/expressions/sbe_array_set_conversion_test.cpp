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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace mongo::sbe {

class SBEArraySetConversionTest : public EExpressionTestFixture {
public:
    /**
     * Run the compiled expression and assert that the result is equal to Nothing.
     */
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        ASSERT_EQUALS(runTag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(runVal, 0);
    }

    /**
     * Run the compiled expression and assert that the result is equal to 'expectedValues'.
     */
    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, BSONArray expectedValues) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(value::TypeTags::ArraySet, runTag);

        value::ArraySet* arrSet = value::getArraySetView(runVal);
        value::ValueSetType& values = arrSet->values();

        size_t expectedItems = 0;
        for (auto elem : expectedValues) {
            auto [tag, val] = bson::convertFrom<true>(elem);

            ASSERT_NE(values.end(), values.find(std::make_pair(tag, val)))
                << "Value " << std::make_pair(tag, val) << " not found in result";
            expectedItems++;
        }
        ASSERT_EQ(expectedItems, arrSet->size()) << std::make_pair(runTag, runVal);
    }

    /**
     * Helper function that converts a BSON array into a SBE Array
     */
    std::pair<value::TypeTags, value::Value> convertFromBSONArray(BSONArray arr) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::Array* arrView = value::getArrayView(arrVal);

        for (auto elem : arr) {
            auto [tag, val] = bson::convertFrom<false>(elem);
            arrView->push_back(tag, val);
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
        sbe::makeE<sbe::EFunction>("arrayToSet", sbe::makeEs(makeE<EVariable>(inputSlot)));
    std::unique_ptr<vm::CodeFragment> compiledArrayToSet = compileExpression(*arrayToSetExpr);

    // Test with Array on first variant
    BSONArray bsonArr1 = BSON_ARRAY(1 << 1 << 2 << 1 << 2);
    auto [arr1Tag, arr1Val] = convertFromBSONArray(bsonArr1);
    inputAccessor.reset(true, arr1Tag, arr1Val);
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with bsonArray on first variant
    inputAccessor.reset(
        false, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr1.objdata()));
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with Array on second variant
    BSONArray bsonArr2 =
        BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << BSON("x" << 1) << "Y");
    auto [arr2Tag, arr2Val] = convertFromBSONArray(bsonArr2);
    inputAccessor.reset(true, arr2Tag, arr2Val);
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << "Y"));

    // Test with bsonArray on second variant
    inputAccessor.reset(
        false, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr2.objdata()));
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << "Y"));

    // Test with empty array
    auto [emptyArrTag, emptyArrVal] = value::makeNewArray();
    inputAccessor.reset(true, emptyArrTag, emptyArrVal);
    runAndAssertExpression(compiledArrayToSet.get(), BSONArray());

    // Test when input is not array Type
    inputAccessor.reset(false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
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
        "collArrayToSet",
        sbe::makeEs(makeE<EConstant>(value::TypeTags::collator,
                                     value::bitcastFrom<CollatorInterface*>(collator.release())),
                    makeE<EVariable>(inputSlot)));
    std::unique_ptr<vm::CodeFragment> compiledArrayToSet = compileExpression(*arrayToSetExpr);

    // Test with Array on first variant
    BSONArray bsonArr1 = BSON_ARRAY(1 << 1 << 2 << 1 << 2);
    auto [arr1Tag, arr1Val] = convertFromBSONArray(bsonArr1);
    inputAccessor.reset(true, arr1Tag, arr1Val);
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with bsonArray on first variant
    inputAccessor.reset(
        false, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr1.objdata()));
    runAndAssertExpression(compiledArrayToSet.get(), BSON_ARRAY(1 << 2));

    // Test with Array on second variant
    BSONArray bsonArr2 =
        BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2) << BSON("x" << 1) << "Y");
    auto [arr2Tag, arr2Val] = convertFromBSONArray(bsonArr2);
    inputAccessor.reset(true, arr2Tag, arr2Val);
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2)));

    // Test with bsonArray on second variant
    inputAccessor.reset(
        false, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr2.objdata()));
    runAndAssertExpression(compiledArrayToSet.get(),
                           BSON_ARRAY(BSON("x" << 1) << "y" << BSON("x" << 2)));

    // Test with empty array
    auto [emptyArrTag, emptyArrVal] = value::makeNewArray();
    inputAccessor.reset(true, emptyArrTag, emptyArrVal);
    runAndAssertExpression(compiledArrayToSet.get(), BSONArray());

    // Test when input is not array Type
    inputAccessor.reset(false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledArrayToSet.get());
}
}  // namespace mongo::sbe

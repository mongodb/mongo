// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace mongo::sbe {
namespace {

using value::TagValueOwned;
using value::TagValueView;
using Int32Limits = std::numeric_limits<int32_t>;
using Int64Limits = std::numeric_limits<int64_t>;
using DoubleLimits = std::numeric_limits<double>;

TagValueOwned decimal(const std::string& literal) {
    return TagValueOwned::fromRaw(value::makeCopyDecimal(Decimal128{literal}));
}

std::vector<TagValueOwned> nonDecimalNumbers() {
    std::vector<TagValueOwned> numbers;
    for (int32_t number : {0, 1, -1, Int32Limits::max(), Int32Limits::min()}) {
        numbers.push_back(TagValueOwned::numberInt32(number));
    }
    for (int64_t number :
         {int64_t{1} << 53, (int64_t{1} << 53) + 1, Int64Limits::max(), Int64Limits::min()}) {
        numbers.push_back(TagValueOwned::numberInt64(number));
    }
    for (double number : {0.0,
                          -0.0,
                          0.1,
                          1e308,
                          -1e308,
                          DoubleLimits::max(),
                          DoubleLimits::lowest(),
                          DoubleLimits::denorm_min(),
                          DoubleLimits::infinity(),
                          -DoubleLimits::infinity(),
                          DoubleLimits::quiet_NaN()}) {
        numbers.push_back(TagValueOwned::numberDouble(number));
    }
    return numbers;
}

std::vector<TagValueOwned> edgeValues() {
    auto values = nonDecimalNumbers();
    for (auto literal : {"0.1", "-0", "1E+6000", "-1E+6000", "Infinity", "NaN"}) {
        values.push_back(decimal(literal));
    }
    values.push_back(TagValueOwned::fromRaw(value::makeNewString("not a number")));
    values.push_back(TagValueOwned::null());
    values.push_back(TagValueOwned::nothing());
    return values;
}

TagValueOwned canonicalizeNaNs(TagValueView view) {
    if (view.tag == value::TypeTags::NumberDouble &&
        std::isnan(value::bitcastTo<double>(view.value))) {
        return TagValueOwned::numberDouble(DoubleLimits::quiet_NaN());
    }
    if (view.tag != value::TypeTags::Array) {
        return view.copy();
    }
    auto result = TagValueOwned::fromRaw(value::makeNewArray());
    auto* elements = value::getArrayView(result.value());
    for (auto [tag, val] : value::getArrayView(view.value)->values()) {
        elements->push_back(canonicalizeNaNs(TagValueView{tag, val}));
    }
    return result;
}

class DoubleDoubleSumTest : public GoldenEExpressionTestFixture {
protected:
    void printSum(const std::string& name, const std::vector<TagValueView>& inputs) {
        auto& os = gctx->outStream();
        printVariation(name);
        _state.reset();
        for (auto input : inputs) {
            _input.reset(input);
            _state.reset(run(_add));
            os << "aggDoubleDoubleSum(";
            print(input);
            os << ") -> ";
            print(_state.getViewOfValue());
            os << std::endl;
        }
        os << "doubleDoubleSumFinalize -> ";
        print(run(_finalize).view());
        os << std::endl << "doubleDoublePartialSumFinalize -> ";
        print(run(_partialSum).view());
        os << std::endl;
    }

    void printPartialSumOf(TagValueView scalar) {
        auto& os = gctx->outStream();
        _state.reset(scalar);
        os << "doubleDoublePartialSumFinalize(";
        print(scalar);
        os << ") -> ";
        print(run(_partialSum).view());
        os << std::endl;
    }

private:
    TagValueOwned run(const std::unique_ptr<vm::CodeFragment>& code) {
        return TagValueOwned::fromRaw(runCompiledExpression(code.get()));
    }

    void print(TagValueView view) {
        auto canonical = canonicalizeNaNs(view);
        makeValuePrinter(gctx->outStream()).writeValueToStream(canonical.tag(), canonical.value());
    }

    value::OwnedValueAccessor _input;
    value::OwnedValueAccessor _state;
    value::SlotId _inputSlot = bindAccessor(&_input);
    value::SlotId _stateSlot = bindAccessor(&_state);
    std::unique_ptr<vm::CodeFragment> _add = compileAggExpression(
        *makeFunction(EFn::kAggDoubleDoubleSum, makeVariable(_inputSlot)), &_state);
    std::unique_ptr<vm::CodeFragment> _finalize =
        compileExpression(*makeFunction(EFn::kDoubleDoubleSumFinalize, makeVariable(_stateSlot)));
    std::unique_ptr<vm::CodeFragment> _partialSum = compileExpression(
        *makeFunction(EFn::kDoubleDoublePartialSumFinalize, makeVariable(_stateSlot)));
};

TEST_F(DoubleDoubleSumTest, SumsOfEdgeValues) {
    for (const auto& value : edgeValues()) {
        printSum("", {value.view()});
    }
}

TEST_F(DoubleDoubleSumTest, NamedSums) {
    auto oneAndAHalf = decimal("1.5");
    auto decimalOne = decimal("1");
    auto decimalInfinity = decimal("Infinity");
    auto decimalNaN = decimal("NaN");
    auto infinity = TagValueView::numberDouble(DoubleLimits::infinity());
    auto minusInfinity = TagValueView::numberDouble(-DoubleLimits::infinity());
    auto doubleMax = TagValueView::numberDouble(DoubleLimits::max());

    printSum("non-numeric inputs leave the sum at zero",
             {TagValueView::null(), TagValueView::nothing(), TagValueView::boolean(true)});
    printSum("int32 inputs keep an int32 sum",
             {TagValueView::numberInt32(1), TagValueView::numberInt32(2)});
    printSum("int32 overflow widens to int64",
             {TagValueView::numberInt32(Int32Limits::max()), TagValueView::numberInt32(1)});
    printSum("an int64 input widens the sum",
             {TagValueView::numberInt64(1), TagValueView::numberInt32(2)});
    printSum("int64 sums past 2^53 stay exact",
             {TagValueView::numberInt64(int64_t{1} << 53), TagValueView::numberInt32(1)});
    printSum("int64 overflow widens to double",
             {TagValueView::numberInt64(Int64Limits::max()),
              TagValueView::numberInt64(Int64Limits::max())});
    printSum("double sums are compensated",
             std::vector<TagValueView>(10, TagValueView::numberDouble(0.1)));
    printSum("negative zeros",
             {TagValueView::numberDouble(-0.0), TagValueView::numberDouble(-0.0)});
    printSum("denormals",
             {TagValueView::numberDouble(DoubleLimits::denorm_min()),
              TagValueView::numberDouble(DoubleLimits::denorm_min())});
    printSum("double overflow becomes infinity", {doubleMax, doubleMax});
    printSum("opposite infinities sum to NaN", {infinity, minusInfinity});
    printSum("a finite input after infinity", {infinity, TagValueView::numberDouble(1)});
    printSum("each add restarts the plain sum from the stored pair",
             {doubleMax,
              TagValueView::numberDouble(-std::ldexp(1.0, 969)),
              TagValueView::numberDouble(-std::ldexp(1.0, 969)),
              TagValueView::numberDouble(std::ldexp(1.0, 970)),
              minusInfinity,
              TagValueView::numberDouble(1)});
    printSum("a decimal input after a non-decimal input",
             {TagValueView::numberInt32(2), oneAndAHalf.view()});
    printSum("a non-decimal input after a decimal input",
             {oneAndAHalf.view(), TagValueView::numberInt32(2)});
    printSum("decimal and double infinities", {decimalInfinity.view(), minusInfinity});
    printSum("a decimal input after a NaN sum", {infinity, minusInfinity, decimalOne.view()});
    printSum("decimal NaN", {decimalNaN.view(), TagValueView::numberInt32(1)});
}

TEST_F(DoubleDoubleSumTest, PartialSumsOfScalars) {
    for (const auto& number : nonDecimalNumbers()) {
        printPartialSumOf(number.view());
    }
}

}  // namespace
}  // namespace mongo::sbe

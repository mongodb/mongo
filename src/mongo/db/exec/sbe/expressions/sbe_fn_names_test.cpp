// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::sbe {
namespace {
using namespace std::literals::string_view_literals;

// Every canonical name round-trips through fromString -> toString.
TEST(SbeFnNamesTest, RoundTrip) {
    for (size_t i = 0; i < static_cast<size_t>(EFn::kNumFunctions); ++i) {
        auto fn = static_cast<EFn>(i);
        std::string_view name = toString(fn);
        ASSERT_EQUALS(fn, fromString(name)) << "round-trip failed for EFn(" << i << ") = " << name;
    }
}

// Dollar-prefixed ABT aliases resolve to the correct canonical enum values.
TEST(SbeFnNamesTest, Aliases) {
    ASSERT_EQUALS(EFn::kAddToArray, fromString("$push"sv));
    ASSERT_EQUALS(EFn::kAddToSet, fromString("$addToSet"sv));
    ASSERT_EQUALS(EFn::kFirst, fromString("$first"sv));
    ASSERT_EQUALS(EFn::kLast, fromString("$last"sv));
    ASSERT_EQUALS(EFn::kMax, fromString("$max"sv));
    ASSERT_EQUALS(EFn::kMin, fromString("$min"sv));
    ASSERT_EQUALS(EFn::kSum, fromString("$sum"sv));
}

// EFunction::debugPrint() must emit the canonical string name of its EFn value.
// One representative function is sufficient: this exercises the sbe::toString(_fn) call
// in EFunction::debugPrint(), which would regress to an empty or wrong name if the
// dispatch were accidentally broken.
TEST(SbeFnNamesTest, DebugPrintEmitsFunctionName) {
    auto expr = makeE<EFunction>(
        EFn::kAbs,
        makeEs(makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0))));
    std::string printed = DebugPrinter{}.print(expr->debugPrint());
    ASSERT_STRING_CONTAINS(printed, toString(EFn::kAbs));
}

// An unrecognised name must trigger a tassert (death test).
DEATH_TEST_REGEX(SbeFnNamesDeathTest, UnknownNameTasserts, "8026700") {
    fromString("noSuchFunction"sv);
}

// Compiling an EFunction whose EFn is absent from both dispatch tables must uassert 4822847.
// EFn::kFail is intentionally absent from kBuiltinFunctions and kInstrFunctions (it is the
// sentinel used by the dedicated EFail node), making it the canonical trigger for this path.
class SbeFnNamesCompileTest : public EExpressionTestFixture {};

TEST_F(SbeFnNamesCompileTest, UnknownFunctionDispatchUasserts) {
    auto expr = makeE<EFunction>(EFn::kFail, EExpression::Vector{});
    ASSERT_THROWS_CODE(compileExpression(*expr), AssertionException, 4822847);
}

}  // namespace
}  // namespace mongo::sbe

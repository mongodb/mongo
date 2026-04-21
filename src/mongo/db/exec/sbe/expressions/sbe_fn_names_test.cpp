/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {
namespace {

// Every canonical name round-trips through fromString -> toString.
TEST(SbeFnNamesTest, RoundTrip) {
    for (size_t i = 0; i < static_cast<size_t>(EFn::kNumFunctions); ++i) {
        auto fn = static_cast<EFn>(i);
        StringData name = toString(fn);
        ASSERT_EQUALS(fn, fromString(name)) << "round-trip failed for EFn(" << i << ") = " << name;
    }
}

// Dollar-prefixed ABT aliases resolve to the correct canonical enum values.
TEST(SbeFnNamesTest, Aliases) {
    ASSERT_EQUALS(EFn::kAddToArray, fromString("$push"_sd));
    ASSERT_EQUALS(EFn::kAddToSet, fromString("$addToSet"_sd));
    ASSERT_EQUALS(EFn::kFirst, fromString("$first"_sd));
    ASSERT_EQUALS(EFn::kLast, fromString("$last"_sd));
    ASSERT_EQUALS(EFn::kMax, fromString("$max"_sd));
    ASSERT_EQUALS(EFn::kMin, fromString("$min"_sd));
    ASSERT_EQUALS(EFn::kSum, fromString("$sum"_sd));
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
    fromString("noSuchFunction"_sd);
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

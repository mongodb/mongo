// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/db/query/stage_builder/sbe/tests/abt_unit_test_literals.h"
#include "mongo/db/query/stage_builder/sbe/tests/abt_unit_test_utils.h"
#include "mongo/util/debugger.h"

#if defined(__clang__)
#define clang_optnone __attribute__((optnone))
#else
#define clang_optnone
#endif
#pragma GCC push_options
#pragma GCC optimize("O0")

using namespace mongo::abt;
using namespace mongo::stage_builder::abt_lower::unit_test_abt_literals;

int clang_optnone main(int argc, char** argv) {
    ABT testABT =
        _binary("And", _binary("Lt", "0"_cint32, "1"_cint32), _binary("Lt", "1"_cint32, "2"_cint32))
            ._n;

    // Verify output as a sanity check, the real test is in the gdb test file.
    ASSERT_EXPLAIN_V2_AUTO(
        "BinaryOp [And]\n"
        "|   BinaryOp [Lt]\n"
        "|   |   Const [2]\n"
        "|   Const [1]\n"
        "BinaryOp [Lt]\n"
        "|   Const [1]\n"
        "Const [0]\n",
        testABT);

    mongo::breakpoint();

    return 0;
}

#pragma GCC pop_options

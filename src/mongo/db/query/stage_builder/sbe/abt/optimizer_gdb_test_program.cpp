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

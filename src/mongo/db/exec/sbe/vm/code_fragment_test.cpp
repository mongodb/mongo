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
#include "mongo/db/exec/sbe/vm/code_fragment.h"

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/id_generator.h"

namespace mongo {
/**
 * These tests are for CodeFragment::appendAccessVal.  In particular, testing that the
 * values of the SlotAccessors being appended are passed through and deserialized correctly.
 */
TEST(CodeFragmentTest, TestPushEnvAccessorVal) {
    sbe::vm::CodeFragment fragment;
    sbe::RuntimeEnvironment env;
    sbe::value::TypeTags testTag = sbe::value::TypeTags::Boolean;
    sbe::value::Value testValue = 192;
    sbe::value::SlotIdGenerator incrementingGenerator;
    env.registerSlot(testTag, testValue, true, &incrementingGenerator);
    sbe::RuntimeEnvironment::Accessor accessor(&env, 0);
    fragment.appendAccessVal(&accessor);
    sbe::vm::ByteCode vm;
    auto [owned, resultsTag, resultsVal] = vm.run(&fragment);
    ASSERT(!owned);
    ASSERT(resultsTag == testTag);
    ASSERT(resultsVal == testValue);
}
TEST(CodeFragmentTest, TestPushOwnedAccessorVal) {
    sbe::vm::CodeFragment fragment;
    sbe::value::OwnedValueAccessor accessor;
    sbe::value::TypeTags testTag = sbe::value::TypeTags::Boolean;
    sbe::value::Value testValue = 192;
    accessor.reset(testTag, testValue);
    fragment.appendAccessVal(&accessor);
    sbe::vm::ByteCode vm;
    auto [owned, resultsTag, resultsVal] = vm.run(&fragment);
    ASSERT(!owned);
    ASSERT(resultsTag == testTag);
    ASSERT(resultsVal == testValue);
}
}  // namespace mongo

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {

TEST(SBERuntimeEnvironmentTest, CanCopy) {
    auto env = std::make_unique<RuntimeEnvironment>();

    value::SlotIdGenerator _slotIdGenerator;
    auto [tag, val] = value::makeNewString("Test string element");
    auto slotID = env->registerSlot(tag, val, true, &_slotIdGenerator);

    // Make a "shallow" copy. "State" in the RuntimeEnvironment is shared.
    auto envCopy = env->makeCopy();

    auto tagValEnv = env->getAccessor(slotID)->getViewOfValue();
    auto tagValCopy = envCopy->getAccessor(slotID)->getViewOfValue();
    ASSERT_EQUALS(tagValEnv, tagValCopy);

    auto [tag2, val2] = value::makeNewString("Modified");
    env->resetSlot(slotID, tag2, val2, true);

    // The slot value of 'envCopy' should be identical to value of 'env' since 'envCopy' is not a
    // deep copy.
    tagValEnv = env->getAccessor(slotID)->getViewOfValue();
    tagValCopy = envCopy->getAccessor(slotID)->getViewOfValue();
    ASSERT_EQUALS(tagValEnv, tagValCopy);
}

TEST(SBERuntimeEnvironmentTest, CanDeepCopy) {
    auto env = std::make_unique<RuntimeEnvironment>();

    value::SlotIdGenerator _slotIdGenerator;
    auto [tag, val] = value::makeNewString("Test string element");
    value::ValueGuard guard(tag, val);
    auto slotID = env->registerSlot(tag, val, false, &_slotIdGenerator);

    // Make a "deep" copy. "State" in the RuntimeEnvironment is unique and owned.
    auto envCopy = env->makeDeepCopy();
    auto [tagEnv, valEnv] = env->getAccessor(slotID)->getViewOfValue();
    auto [tagCopy, valCopy] = envCopy->getAccessor(slotID)->getViewOfValue();

    ASSERT_EQUALS(value::getStringView(tagEnv, valEnv), value::getStringView(tagCopy, valCopy));
    ASSERT_EQUALS(value::getStringView(tag, val), value::getStringView(tagCopy, valCopy));

    auto [tag2, val2] = value::makeNewString("Modified");
    value::ValueGuard guard2(tag2, val2);
    // Modify the slot value in 'env'.
    env->resetSlot(slotID, tag2, val2, false);

    // The slot value of 'envCopy' should not be modified since 'envCopy' is a deep copy.
    auto [tagEnvModified, valEnvModified] = env->getAccessor(slotID)->getViewOfValue();
    auto [tagCopyNotModified, valCopyNotModified] = envCopy->getAccessor(slotID)->getViewOfValue();
    ASSERT_EQUALS(value::getStringView(tagEnvModified, valEnvModified),
                  value::getStringView(tag2, val2));
    ASSERT_EQUALS(value::getStringView(tagCopyNotModified, valCopyNotModified),
                  value::getStringView(tag, val));
}

}  // namespace mongo::sbe

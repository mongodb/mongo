// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    std::pair<value::TypeTags, value::Value> tagValEnv = env->getAccessor(slotID)->getViewOfValue();
    std::pair<value::TypeTags, value::Value> tagValCopy =
        envCopy->getAccessor(slotID)->getViewOfValue();
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
    value::TagValueOwned str1Owned =
        value::TagValueOwned::fromRaw(value::makeNewString("Test string element"));
    auto slotID = env->registerSlot(str1Owned.tag(), str1Owned.value(), false, &_slotIdGenerator);

    // Make a "deep" copy. "State" in the RuntimeEnvironment is unique and owned.
    auto envCopy = env->makeDeepCopy();
    auto [tagEnv, valEnv] = env->getAccessor(slotID)->getViewOfValue();
    auto [tagCopy, valCopy] = envCopy->getAccessor(slotID)->getViewOfValue();

    ASSERT_EQUALS(value::getStringView(tagEnv, valEnv), value::getStringView(tagCopy, valCopy));
    ASSERT_EQUALS(value::getStringView(str1Owned.tag(), str1Owned.value()),
                  value::getStringView(tagCopy, valCopy));

    value::TagValueOwned str2Owned =
        value::TagValueOwned::fromRaw(value::makeNewString("Modified"));
    // Modify the slot value in 'env'.
    env->resetSlot(slotID, str2Owned.tag(), str2Owned.value(), false);

    // The slot value of 'envCopy' should not be modified since 'envCopy' is a deep copy.
    auto [tagEnvModified, valEnvModified] = env->getAccessor(slotID)->getViewOfValue();
    auto [tagCopyNotModified, valCopyNotModified] = envCopy->getAccessor(slotID)->getViewOfValue();
    ASSERT_EQUALS(value::getStringView(tagEnvModified, valEnvModified),
                  value::getStringView(str2Owned.tag(), str2Owned.value()));
    ASSERT_EQUALS(value::getStringView(tagCopyNotModified, valCopyNotModified),
                  value::getStringView(str1Owned.tag(), str1Owned.value()));
}

}  // namespace mongo::sbe

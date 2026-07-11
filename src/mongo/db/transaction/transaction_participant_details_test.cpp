// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/transaction_participant_details.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo::transaction_participant_details {
namespace {

TEST(NonRecoverableFieldTest, DefaultConstructedHasDefaultValue) {
    NonRecoverableField<int> field;
    ASSERT_EQ(field.getExpectAvailable(), 0);
}

TEST(NonRecoverableFieldTest, GetExpectAvailableWhenAvailable) {
    NonRecoverableField<std::vector<int>> field;
    field.getExpectAvailable().push_back(42);
    ASSERT_EQ(field.getExpectAvailable().size(), 1u);
    ASSERT_EQ(field.getExpectAvailable()[0], 42);
}

TEST(NonRecoverableFieldTest, GetExpectAvailableConstWhenAvailable) {
    NonRecoverableField<int> field;
    field.getExpectAvailable() = 7;
    const auto& constField = field;
    ASSERT_EQ(constField.getExpectAvailable(), 7);
}

TEST(NonRecoverableFieldTest, GetAllowUnavailableWhenAvailable) {
    NonRecoverableField<int> field;
    field.getAllowUnavailable() = 99;
    ASSERT_EQ(field.getAllowUnavailable(), 99);
}

TEST(NonRecoverableFieldTest, GetAllowUnavailableWhenUnavailable) {
    NonRecoverableField<int> field;
    field.getAllowUnavailable() = 5;
    field.markAsUnavailable();
    // Unchecked access still works after marking unavailable.
    ASSERT_EQ(field.getAllowUnavailable(), 5);
}

TEST(NonRecoverableFieldTest, GetAllowUnavailableConstWhenUnavailable) {
    NonRecoverableField<int> field;
    field.getAllowUnavailable() = 5;
    field.markAsUnavailable();
    const auto& constField = field;
    ASSERT_EQ(constField.getAllowUnavailable(), 5);
}

TEST(NonRecoverableFieldTest, ResetRestoresAvailabilityAndDefaultValue) {
    NonRecoverableField<int> field;
    field.getExpectAvailable() = 42;
    field.markAsUnavailable();

    field.reset();
    ASSERT_EQ(field.getExpectAvailable(), 0);
}

TEST(NonRecoverableFieldTest, ResetClearsContainerValue) {
    NonRecoverableField<std::vector<int>> field;
    field.getExpectAvailable().push_back(1);
    field.getExpectAvailable().push_back(2);
    field.markAsUnavailable();

    field.reset();
    ASSERT_TRUE(field.getExpectAvailable().empty());
}

TEST(NonRecoverableFieldTest, ResetAfterAvailableFieldClearsValue) {
    NonRecoverableField<int> field;
    field.getExpectAvailable() = 42;

    field.reset();
    ASSERT_EQ(field.getExpectAvailable(), 0);
}

TEST(NonRecoverableFieldTest, MarkAsUnavailableThenResetThenModify) {
    NonRecoverableField<std::string> field;
    field.getExpectAvailable() = "hello";
    field.markAsUnavailable();
    field.reset();

    // Field is usable again after reset.
    field.getExpectAvailable() = "world";
    ASSERT_EQ(field.getExpectAvailable(), "world");
}

TEST(NonRecoverableFieldTest, SetAvailableSetsValueAndMarksAvailable) {
    NonRecoverableField<std::vector<int>> field;
    field.markAsUnavailable();

    field.setAvailable(std::vector<int>{1, 2, 3});
    ASSERT_EQ(field.getExpectAvailable().size(), 3u);
    ASSERT_EQ(field.getExpectAvailable()[0], 1);
}

TEST(NonRecoverableFieldTest, SetAvailableOverwritesExistingValue) {
    NonRecoverableField<std::string> field;
    field.getExpectAvailable() = "old";

    field.setAvailable(std::string("new"));
    ASSERT_EQ(field.getExpectAvailable(), "new");
}

DEATH_TEST(NonRecoverableFieldDeathTest,
           GetExpectAvailableCrashesWhenUnavailable,
           "Field is not available") {
    NonRecoverableField<int> field;
    field.markAsUnavailable();
    field.getExpectAvailable();
}

DEATH_TEST(NonRecoverableFieldDeathTest,
           GetExpectAvailableConstCrashesWhenUnavailable,
           "Field is not available") {
    NonRecoverableField<int> field;
    field.markAsUnavailable();
    const auto& constField = field;
    constField.getExpectAvailable();
}

}  // namespace
}  // namespace mongo::transaction_participant_details

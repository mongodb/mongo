// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_host_utils.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/extension/public/extension_privilege_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension::host {
namespace {

using static_properties_util::toActionType;

TEST(ExtensionHostUtilsTest, ReadPrivilegeActionsMapToReadActionTypes) {
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kFind), ActionType::find);
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kListIndexes),
              ActionType::listIndexes);
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kPlanCacheRead),
              ActionType::planCacheRead);
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kCollStats), ActionType::collStats);
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kIndexStats), ActionType::indexStats);
}

TEST(ExtensionHostUtilsTest, SearchIndexPrivilegeActionsMapToSearchIndexActionTypes) {
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kCreateSearchIndexes),
              ActionType::createSearchIndexes);
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kUpdateSearchIndex),
              ActionType::updateSearchIndex);
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kDropSearchIndex),
              ActionType::dropSearchIndex);
    ASSERT_EQ(toActionType(MongoExtensionPrivilegeActionEnum::kListSearchIndexes),
              ActionType::listSearchIndexes);
}

}  // namespace
}  // namespace mongo::extension::host

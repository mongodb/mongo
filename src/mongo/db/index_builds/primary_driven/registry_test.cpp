// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/primary_driven/registry.h"

#include "mongo/db/database_name.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo::index_builds::primary_driven {
namespace {

TEST(PrimaryDrivenRegistryTest, RunsTheOnChangeHandlerOnEveryChange) {
    Registry registry;

    int changes = 0;
    registry.setOnChangeHandler([&] { ++changes; });

    auto buildUUID = UUID::gen();
    registry.add(buildUUID,
                 DatabaseName::createDatabaseName_forTest(boost::none, "testDatabase"),
                 UUID::gen(),
                 {},
                 boost::none);
    EXPECT_EQ(changes, 1);

    registry.remove(buildUUID);
    EXPECT_EQ(changes, 2);

    registry.clear();
    EXPECT_EQ(changes, 3);

    EXPECT_FALSE(registry.contains(buildUUID));
    EXPECT_TRUE(registry.all().empty());
    EXPECT_EQ(changes, 3);
}

}  // namespace
}  // namespace mongo::index_builds::primary_driven

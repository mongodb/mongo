// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/versioning_protocol/stale_exception.h"

#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.nss");

TEST(StaleExceptionTest, StaleConfigInfoSerializationTest) {
    const ShardId kShardId("SHARD_ID");

    StaleConfigInfo info(kNss, ShardVersion::UNTRACKED(), ShardVersion::UNTRACKED(), kShardId);

    // Serialize
    BSONObjBuilder bob;
    info.serialize(&bob);

    // Deserialize
    auto deserializedInfo =
        std::static_pointer_cast<const StaleConfigInfo>(StaleConfigInfo::parse(bob.obj()));

    ASSERT_EQUALS(deserializedInfo->getNss(), kNss);
    ASSERT_EQUALS(deserializedInfo->getVersionReceived(), ShardVersion::UNTRACKED());
    ASSERT_EQUALS(*deserializedInfo->getVersionWanted(), ShardVersion::UNTRACKED());
    ASSERT_EQUALS(deserializedInfo->getShardId(), kShardId);
}

TEST(StaleExceptionTest, StaleEpochInfoSerializationTest) {
    StaleEpochInfo info(kNss, ShardVersion::UNTRACKED(), ShardVersion::UNTRACKED());

    // Serialize
    BSONObjBuilder bob;
    info.serialize(&bob);

    // Deserialize
    auto deserializedInfo =
        std::static_pointer_cast<const StaleEpochInfo>(StaleEpochInfo::parse(bob.obj()));

    ASSERT_EQ(deserializedInfo->getNss(), kNss);
    ASSERT_EQ(deserializedInfo->getVersionReceived(), ShardVersion::UNTRACKED());
    ASSERT_EQ(deserializedInfo->getVersionWanted(), ShardVersion::UNTRACKED());
}

TEST(StaleExceptionTest, StaleEpochInfoOptionalSerializationTest) {
    StaleEpochInfo info(kNss);

    // Serialize
    BSONObjBuilder bob;
    info.serialize(&bob);

    // Deserialize
    auto deserializedInfo =
        std::static_pointer_cast<const StaleEpochInfo>(StaleEpochInfo::parse(bob.obj()));

    // TODO SERVER-117117: After 9.0 becomes last LTS, remove the ShardVersion{} filling workaround
    // and update this test to verify that versionReceived/versionWanted can be unset optionals.
    ASSERT_EQ(deserializedInfo->getNss(), kNss);
    ASSERT_TRUE(deserializedInfo->getVersionReceived().has_value());
    ASSERT_TRUE(deserializedInfo->getVersionWanted().has_value());
    ASSERT_EQ(deserializedInfo->getVersionReceived(), ShardVersion{});
    ASSERT_EQ(deserializedInfo->getVersionWanted(), ShardVersion{});
}

}  // namespace
}  // namespace mongo

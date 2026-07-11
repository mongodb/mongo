// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/shard_tag.h"

#include "mongo/unittest/unittest.h"

#include <sstream>

namespace mongo {

TEST(ShardTagTest, DefaultShardTags) {
    ASSERT_EQ("default", ShardTag::kDefault.tag);
    ASSERT_EQ("config", ShardTag::kConfigServer.tag);
    ASSERT_EQ("data", ShardTag::kDataShard.tag);
}

TEST(ShardTagTest, CompareShardTags) {
    // Compare for equality.
    ASSERT_EQ(ShardTag::kDefault, ShardTag::kDefault);
    ASSERT_EQ(ShardTag::kConfigServer, ShardTag::kConfigServer);
    ASSERT_EQ(ShardTag::kDataShard, ShardTag::kDataShard);
    ASSERT_EQ(ShardTag{.tag = "foo"}, ShardTag{.tag = "foo"});

    // Compare for inequality.
    ASSERT_NE(ShardTag::kDefault, ShardTag::kConfigServer);
    ASSERT_NE(ShardTag::kConfigServer, ShardTag::kDataShard);
    ASSERT_NE(ShardTag::kDataShard, ShardTag::kDefault);
    ASSERT_NE(ShardTag{.tag = "foo"}, ShardTag{.tag = "bar"});

    // Compare for lt/gt.
    ASSERT_LT(ShardTag{.tag = "bar"}, ShardTag{.tag = "baz"});
    ASSERT_GT(ShardTag{.tag = "baz"}, ShardTag{.tag = "bar"});
    ASSERT_LT(ShardTag{.tag = "bar"}, ShardTag{.tag = "foo"});
    ASSERT_GT(ShardTag{.tag = "foo"}, ShardTag{.tag = "bar"});
    ASSERT_LTE(ShardTag{.tag = "bar"}, ShardTag{.tag = "bar"});
    ASSERT_LTE(ShardTag{.tag = "bar"}, ShardTag{.tag = "bard"});
    ASSERT_GTE(ShardTag{.tag = "bark"}, ShardTag{.tag = "bar"});
    ASSERT_GTE(ShardTag{.tag = "bard"}, ShardTag{.tag = "bard"});
}

TEST(ShardTagTest, ShardTagToString) {
    ShardTag shardTag{.tag = "config"};
    ASSERT_EQ("config", shardTag.toString());
}

TEST(ShardTagTest, ShardTagLogging) {
    ShardTag shardTag{.tag = "Covfefe"};
    std::stringstream s;
    s << shardTag;
    ASSERT_EQ("Covfefe", s.str());
}

}  // namespace mongo

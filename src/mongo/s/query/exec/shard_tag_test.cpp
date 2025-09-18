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

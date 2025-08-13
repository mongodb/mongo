/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo {
namespace {


ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly);

using AddShardToZoneTest = ConfigServerTestFixture;

TEST_F(AddShardToZoneTest, AddSingleZoneToExistingShardShouldSucceed) {
    ShardType shard;
    shard.setName("a");
    shard.setHost("a:1234");

    setupShards({shard});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->addShardToZone(operationContext(), shard.getName(), "z"));
    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_EQ(1u, tags.size());
    ASSERT_EQ("z", tags.front());
}

TEST_F(AddShardToZoneTest, AddZoneToShardWithSameTagShouldSucceed) {
    ShardType shard;
    shard.setName("a");
    shard.setHost("a:1234");
    shard.setTags({"x", "y"});

    setupShards({shard});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->addShardToZone(operationContext(), shard.getName(), "x"));

    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_EQ(2u, tags.size());
    ASSERT_EQ("x", tags.front());
    ASSERT_EQ("y", tags.back());
}

TEST_F(AddShardToZoneTest, AddZoneToShardWithNewTagShouldAppend) {
    ShardType shard;
    shard.setName("a");
    shard.setHost("a:1234");
    shard.setTags({"x"});

    setupShards({shard});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->addShardToZone(operationContext(), shard.getName(), "y"));

    auto shardDocStatus = getShardDoc(operationContext(), shard.getName());
    ASSERT_OK(shardDocStatus.getStatus());

    auto shardDoc = shardDocStatus.getValue();
    auto tags = shardDoc.getTags();
    ASSERT_EQ(2u, tags.size());
    ASSERT_EQ("x", tags.front());
    ASSERT_EQ("y", tags.back());
}

TEST_F(AddShardToZoneTest, AddSingleZoneToNonExistingShardShouldFail) {
    ShardType shard;
    shard.setName("a");
    shard.setHost("a:1234");

    setupShards({shard});

    auto status = ShardingCatalogManager::get(operationContext())
                      ->addShardToZone(operationContext(), "b", "z");
    ASSERT_EQ(ErrorCodes::ShardNotFound, status);
}

}  // unnamed namespace
}  // namespace mongo

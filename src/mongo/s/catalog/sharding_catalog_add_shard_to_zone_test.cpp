/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {


ReadPreferenceSetting kReadPref(ReadPreference::PrimaryOnly);

using AddShardToZoneTest = ConfigServerTestFixture;

TEST_F(AddShardToZoneTest, AddSingleZoneToExistingShardShouldSucceed) {
    ShardType shard;
    shard.setName("a");
    shard.setHost("a:1234");

    setupShards({shard});

    ASSERT_OK(catalogManager()->addShardToZone(operationContext(), shard.getName(), "z"));
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

    ASSERT_OK(catalogManager()->addShardToZone(operationContext(), shard.getName(), "x"));

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

    ASSERT_OK(catalogManager()->addShardToZone(operationContext(), shard.getName(), "y"));

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

    auto status = catalogManager()->addShardToZone(operationContext(), "b", "z");
    ASSERT_EQ(ErrorCodes::ShardNotFound, status);
}

}  // unnamed namespace
}  // namespace mongo

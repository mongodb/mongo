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

#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"

namespace mongo {
namespace {

const NamespaceString kNss("TestDB", "TestColl");
const NamespaceString kAnotherNss("TestDB", "AnotherColl");

using OperationShardingStateTest = ShardServerTestFixture;

TEST_F(OperationShardingStateTest, ScopedSetShardRoleDbVersion) {
    DatabaseVersion dbv{DatabaseVersion::makeFixed()};
    ScopedSetShardRole scopedSetShardRole(operationContext(), kNss, boost::none, dbv);

    auto& oss = OperationShardingState::get(operationContext());
    ASSERT_EQ(dbv, *oss.getDbVersion(kNss.db()));
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleShardVersion) {
    CollectionGeneration gen(OID::gen(), Timestamp(1, 0));
    ShardVersion shardVersion({gen, {1, 0}}, {gen, boost::none});
    ScopedSetShardRole scopedSetShardRole(operationContext(), kNss, shardVersion, boost::none);

    auto& oss = OperationShardingState::get(operationContext());
    ASSERT_EQ(shardVersion, *oss.getShardVersion(kNss));
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleChangeShardVersionSameNamespace) {
    auto& oss = OperationShardingState::get(operationContext());

    {
        CollectionGeneration gen1(OID::gen(), Timestamp(10, 0));
        ShardVersion shardVersion1({gen1, {1, 0}}, {gen1, boost::none});
        ScopedSetShardRole scopedSetShardRole1(
            operationContext(), kNss, shardVersion1, boost::none);
        ASSERT_EQ(shardVersion1, *oss.getShardVersion(kNss));
    }
    {
        CollectionGeneration gen2(OID::gen(), Timestamp(20, 0));
        ShardVersion shardVersion2({gen2, {1, 0}}, {gen2, boost::none});
        ScopedSetShardRole scopedSetShardRole2(
            operationContext(), kNss, shardVersion2, boost::none);
        ASSERT_EQ(shardVersion2, *oss.getShardVersion(kNss));
    }
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleRecursiveShardVersionDifferentNamespaces) {
    CollectionGeneration gen1(OID::gen(), Timestamp(10, 0));
    CollectionGeneration gen2(OID::gen(), Timestamp(20, 0));
    ShardVersion shardVersion1({gen1, {1, 0}}, {gen1, boost::none});
    ShardVersion shardVersion2({gen2, {1, 0}}, {gen2, boost::none});

    ScopedSetShardRole scopedSetShardRole1(operationContext(), kNss, shardVersion1, boost::none);
    ScopedSetShardRole scopedSetShardRole2(
        operationContext(), kAnotherNss, shardVersion2, boost::none);

    auto& oss = OperationShardingState::get(operationContext());
    ASSERT_EQ(shardVersion1, *oss.getShardVersion(kNss));
    ASSERT_EQ(shardVersion2, *oss.getShardVersion(kAnotherNss));
}

}  // namespace
}  // namespace mongo

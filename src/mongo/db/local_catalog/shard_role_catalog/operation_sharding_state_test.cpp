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

#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const NamespaceString kAnotherNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "AnotherColl");

using OperationShardingStateTest = ShardServerTestFixture;

TEST_F(OperationShardingStateTest, ScopedSetShardRoleDbVersion) {
    DatabaseVersion dbv{UUID::gen(), Timestamp(1, 0)};
    ScopedSetShardRole scopedSetShardRole(operationContext(), kNss, boost::none, dbv);

    auto& oss = OperationShardingState::get(operationContext());
    ASSERT_EQ(dbv, *oss.getDbVersion(kNss.dbName()));
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleShardVersion) {
    CollectionGeneration gen(OID::gen(), Timestamp(1, 0));
    ShardVersion shardVersion = ShardVersionFactory::make({gen, {1, 0}});
    ScopedSetShardRole scopedSetShardRole(operationContext(), kNss, shardVersion, boost::none);

    auto& oss = OperationShardingState::get(operationContext());
    ASSERT_EQ(shardVersion, *oss.getShardVersion(kNss));
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleChangeShardVersionSameNamespace) {
    auto& oss = OperationShardingState::get(operationContext());

    {
        CollectionGeneration gen1(OID::gen(), Timestamp(10, 0));
        ShardVersion shardVersion1 = ShardVersionFactory::make({gen1, {1, 0}});
        ScopedSetShardRole scopedSetShardRole1(
            operationContext(), kNss, shardVersion1, boost::none);
        ASSERT_EQ(shardVersion1, *oss.getShardVersion(kNss));
    }
    {
        CollectionGeneration gen2(OID::gen(), Timestamp(20, 0));
        ShardVersion shardVersion2 = ShardVersionFactory::make({gen2, {1, 0}});
        ScopedSetShardRole scopedSetShardRole2(
            operationContext(), kNss, shardVersion2, boost::none);
        ASSERT_EQ(shardVersion2, *oss.getShardVersion(kNss));
    }
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleRecursiveShardVersionDifferentNamespaces) {
    CollectionGeneration gen1(OID::gen(), Timestamp(10, 0));
    CollectionGeneration gen2(OID::gen(), Timestamp(20, 0));
    ShardVersion shardVersion1 = ShardVersionFactory::make({gen1, {1, 0}});
    ShardVersion shardVersion2 = ShardVersionFactory::make({gen2, {1, 0}});

    ScopedSetShardRole scopedSetShardRole1(operationContext(), kNss, shardVersion1, boost::none);
    ScopedSetShardRole scopedSetShardRole2(
        operationContext(), kAnotherNss, shardVersion2, boost::none);

    auto& oss = OperationShardingState::get(operationContext());
    ASSERT_EQ(shardVersion1, *oss.getShardVersion(kNss));
    ASSERT_EQ(shardVersion2, *oss.getShardVersion(kAnotherNss));
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleIgnoresFixedDbVersion) {
    DatabaseVersion dbv{DatabaseVersion::makeFixed()};
    ScopedSetShardRole scopedSetShardRole(operationContext(), kNss, boost::none, dbv);

    auto& oss = OperationShardingState::get(operationContext());
    ASSERT_FALSE(oss.getDbVersion(kNss.dbName()));
}

TEST_F(OperationShardingStateTest, ScopedSetShardRoleAllowedShardVersionsWithFixedDbVersion) {
    {
        // The UNSHARDED version can be passed with a fixed dbVersion.
        DatabaseVersion dbv{DatabaseVersion::makeFixed()};
        ShardVersion sv{ShardVersion::UNSHARDED()};
        ScopedSetShardRole scopedSetShardRole0(operationContext(), kNss, sv, dbv);
    }

    {
        // No shard version can be passed with a fixed dbVersion.
        DatabaseVersion dbv{DatabaseVersion::makeFixed()};
        ScopedSetShardRole scopedSetShardRole1(operationContext(), kNss, boost::none, dbv);
    }

    {
        // Any other shard version cannot be passed with a fixed dbVersion.
        DatabaseVersion dbv{DatabaseVersion::makeFixed()};
        CollectionGeneration gen(OID::gen(), Timestamp(1, 0));
        ShardVersion sv = ShardVersionFactory::make({gen, {1, 0}});
        ASSERT_THROWS_CODE(
            [&] {
                ScopedSetShardRole scopedSetShardRole(operationContext(), kNss, sv, dbv);
            }(),
            DBException,
            7331300);
    }
}

TEST_F(OperationShardingStateTest, ScopedAllowImplicitCollectionCreateBasicUnallowed) {
    ASSERT_THROWS_CODE(
        [&] {
            uassertStatusOK(
                createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll())));
        }(),
        DBException,
        ErrorCodes::CannotImplicitlyCreateCollection);
}

TEST_F(OperationShardingStateTest, ScopedAllowImplicitCollectionCreateBasicAllowed) {
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        operationContext(), kNss);
    uassertStatusOK(
        createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll())));
}

TEST_F(OperationShardingStateTest, ScopedAllowImplicitCollectionCreateNestedSameNssCreate) {
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        operationContext(), kNss);
    {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
            unsafeCreateCollectionNested(operationContext(), kNss);
    }
    uassertStatusOK(
        createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll())));
}

TEST_F(OperationShardingStateTest, ScopedAllowImplicitCollectionCreateNestedSameNssDestroyed) {
    {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            operationContext(), kNss);
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
            unsafeCreateCollectionNested(operationContext(), kNss);
    }
    ASSERT_THROWS_CODE(
        [&] {
            uassertStatusOK(
                createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll())));
        }(),
        DBException,
        ErrorCodes::CannotImplicitlyCreateCollection);
}

DEATH_TEST_REGEX_F(OperationShardingStateTest,
                   ScopedAllowImplicitCollectionCreateNestedDifferentNssFails,
                   "Tripwire assertion.*10897901") {
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        operationContext(), kNss);
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollectionNested(
        operationContext(), kAnotherNss);
}

DEATH_TEST_REGEX_F(OperationShardingStateTest,
                   ScopedAllowImplicitCollectionCreateNestedMultipleNssFails,
                   "Tripwire assertion.*10897901") {
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        operationContext(), kNss);
    {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
            unsafeCreateCollectionNested(operationContext(), kNss);
    }
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollectionNested(
        operationContext(), kAnotherNss);
}

}  // namespace
}  // namespace mongo

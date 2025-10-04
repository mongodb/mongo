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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/metadata_manager.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/local_catalog/shard_role_catalog/scoped_collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

class CollectionMetadataFilteringTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
    }

    /**
     * Prepares the CSS for 'kNss' and the standalone '_manager' to have their metadata be a history
     * array populated as follows:
     *  chunk1 - [min, -100)
     *  chunk2 - [100, 0)
     *  chunk3 - [0, 100)
     *  chunk4 - [100, max)
     *
     * and the history:
     *  time (now,75) shard0(chunk1, chunk3) shard1(chunk2, chunk4)
     *  time (75,25) shard0(chunk2, chunk4) shard1(chunk1, chunk3)
     *  time (25,0) - no history
     */
    CollectionMetadata prepareTestData(
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields = boost::none) {
        const UUID uuid = UUID::gen();
        const OID epoch = OID::gen();
        const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

        auto rt = RoutingTableHistory::makeNew(
            kNss,
            uuid,
            shardKeyPattern.getKeyPattern(),
            false, /* unsplittable */
            nullptr,
            false,
            epoch,
            Timestamp(1, 1),
            timeseriesFields,
            boost::none /* reshardingFields */,
            true,
            [&] {
                ChunkVersion version({epoch, Timestamp(1, 1)}, {1, 0});

                ChunkType chunk1(uuid,
                                 {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                                 version,
                                 {"0"});
                chunk1.setOnCurrentShardSince(Timestamp(75, 0));
                chunk1.setHistory({ChunkHistory(*chunk1.getOnCurrentShardSince(), ShardId("0")),
                                   ChunkHistory(Timestamp(25, 0), ShardId("1"))});
                version.incMinor();

                ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
                chunk2.setOnCurrentShardSince(Timestamp(75, 0));
                chunk2.setHistory({ChunkHistory(*chunk2.getOnCurrentShardSince(), ShardId("1")),
                                   ChunkHistory(Timestamp(25, 0), ShardId("0"))});
                version.incMinor();

                ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
                chunk3.setOnCurrentShardSince(Timestamp(75, 0));
                chunk3.setHistory({ChunkHistory(*chunk3.getOnCurrentShardSince(), ShardId("0")),
                                   ChunkHistory(Timestamp(25, 0), ShardId("1"))});
                version.incMinor();

                ChunkType chunk4(uuid,
                                 {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                                 version,
                                 {"1"});
                chunk4.setOnCurrentShardSince(Timestamp(75, 0));
                chunk4.setHistory({ChunkHistory(*chunk4.getOnCurrentShardSince(), ShardId("1")),
                                   ChunkHistory(Timestamp(25, 0), ShardId("0"))});
                version.incMinor();

                return std::vector<ChunkType>{chunk1, chunk2, chunk3, chunk4};
            }());

        ChunkManager cm(makeStandaloneRoutingTableHistory(std::move(rt)), boost::none);
        ASSERT_EQ(4, cm.numChunks());

        {
            AutoGetCollection autoColl(operationContext(), kNss, MODE_X);
            auto scopedCsr = CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                operationContext(), kNss);
            scopedCsr->setFilteringMetadata(operationContext(),
                                            CollectionMetadata(cm, ShardId("0")));
        }

        _manager = std::make_shared<MetadataManager>(
            getServiceContext(), kNss, CollectionMetadata(cm, ShardId("0")));

        return CollectionMetadata(std::move(cm), ShardId("0"));
    }

    std::shared_ptr<MetadataManager> _manager;
};

// Verifies that right set of documents is visible
TEST_F(CollectionMetadataFilteringTest, FilterDocumentsInTheFuture) {
    const auto metadata{prepareTestData()};

    const auto testFilterFn = [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    };

    BSONObj readConcern =
        BSON("readConcern" << BSON("level" << "snapshot"
                                           << "atClusterTime" << Timestamp(100, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};
    auto scopedCss =
        CollectionShardingState::assertCollectionLockedAndAcquire(operationContext(), kNss);
    testFilterFn(scopedCss->getOwnershipFilter(
        operationContext(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup));
}

// Verifies that a different set of documents is visible for a timestamp in the past
TEST_F(CollectionMetadataFilteringTest, FilterDocumentsInThePast) {
    const auto metadata{prepareTestData()};

    const auto testFilterFn = [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)));
        ASSERT_FALSE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)));
        ASSERT_TRUE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)));
    };

    BSONObj readConcern =
        BSON("readConcern" << BSON("level" << "snapshot"
                                           << "atClusterTime" << Timestamp(50, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};
    auto scopedCss =
        CollectionShardingState::assertCollectionLockedAndAcquire(operationContext(), kNss);
    testFilterFn(scopedCss->getOwnershipFilter(
        operationContext(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup));
}

// Verifies that when accessing too far into the past we get the stale error
TEST_F(CollectionMetadataFilteringTest, FilterDocumentsTooFarInThePastThrowsStaleChunkHistory) {
    const auto metadata{prepareTestData()};

    const auto testFilterFn = [](const ScopedCollectionFilter& collectionFilter) {
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << -500)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << 50)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << -50)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
        ASSERT_THROWS_CODE(collectionFilter.keyBelongsToMe(BSON("_id" << 500)),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
    };

    BSONObj readConcern =
        BSON("readConcern" << BSON("level" << "snapshot"
                                           << "atClusterTime" << Timestamp(10, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kNss,
                                          ShardVersionFactory::make(metadata) /* shardVersion */,
                                          boost::none /* databaseVersion */};
    auto scopedCss =
        CollectionShardingState::assertCollectionLockedAndAcquire(operationContext(), kNss);
    testFilterFn(scopedCss->getOwnershipFilter(
        operationContext(), CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup));
}

}  // namespace
}  // namespace mongo

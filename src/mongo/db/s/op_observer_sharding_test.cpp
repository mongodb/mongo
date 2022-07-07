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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/type_shard_identity.h"

namespace mongo {
namespace {

const NamespaceString kTestNss("TestDB", "TestColl");

void setCollectionFilteringMetadata(OperationContext* opCtx, CollectionMetadata metadata) {
    AutoGetCollection autoColl(opCtx, kTestNss, MODE_X);
    CollectionShardingRuntime::get(opCtx, kTestNss)
        ->setFilteringMetadata(opCtx, std::move(metadata));
}

class DocumentKeyStateTest : public ShardServerTestFixture {
protected:
    /**
     * Constructs a CollectionMetadata suitable for refreshing a CollectionShardingState. The only
     * salient detail is the argument `keyPattern` which, defining the shard key, selects the fields
     * that will be extracted from the document to the document key.
     */
    static CollectionMetadata makeAMetadata(BSONObj const& keyPattern) {
        const UUID uuid = UUID::gen();
        const OID epoch = OID::gen();
        auto range = ChunkRange(BSON("key" << MINKEY), BSON("key" << MAXKEY));
        auto chunk = ChunkType(uuid,
                               std::move(range),
                               ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                               ShardId("other"));
        auto rt = RoutingTableHistory::makeNew(kTestNss,
                                               uuid,
                                               KeyPattern(keyPattern),
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none,
                                               boost::none /* chunkSizeBytes */,
                                               true,
                                               {std::move(chunk)});

        return CollectionMetadata(ChunkManager(ShardId("this"),
                                               DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                                               makeStandaloneRoutingTableHistory(std::move(rt)),
                                               Timestamp(100, 0)),
                                  ShardId("this"));
    }
};

TEST_F(DocumentKeyStateTest, MakeDocumentKeyStateUnsharded) {
    setCollectionFilteringMetadata(operationContext(), CollectionMetadata());

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);

    auto doc = BSON("key3"
                    << "abc"
                    << "key" << 3 << "_id"
                    << "hello"
                    << "key2" << true);

    // Check that an order for deletion from an unsharded collection extracts just the "_id" field
    ASSERT_BSONOBJ_EQ(repl::getDocumentKey(operationContext(), kTestNss, doc).getShardKeyAndId(),
                      BSON("_id"
                           << "hello"));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}

TEST_F(DocumentKeyStateTest, MakeDocumentKeyStateShardedWithoutIdInShardKey) {
    // Push a CollectionMetadata with a shard key not including "_id"...
    const auto metadata{makeAMetadata(BSON("key" << 1 << "key3" << 1))};
    setCollectionFilteringMetadata(operationContext(), metadata);

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kTestNss,
                                          metadata.getShardVersion() /* shardVersion */,
                                          boost::none /* databaseVersion */};

    // The order of fields in `doc` deliberately does not match the shard key
    auto doc = BSON("key3"
                    << "abc"
                    << "key" << 100 << "_id"
                    << "hello"
                    << "key2" << true);

    // Verify the shard key is extracted, in correct order, followed by the "_id" field.
    ASSERT_BSONOBJ_EQ(repl::getDocumentKey(operationContext(), kTestNss, doc).getShardKeyAndId(),
                      BSON("key" << 100 << "key3"
                                 << "abc"
                                 << "_id"
                                 << "hello"));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}

TEST_F(DocumentKeyStateTest, MakeDocumentKeyStateShardedWithIdInShardKey) {
    // Push a CollectionMetadata with a shard key that does have "_id" in the middle...
    const auto metadata{makeAMetadata(BSON("key" << 1 << "_id" << 1 << "key2" << 1))};
    setCollectionFilteringMetadata(operationContext(), metadata);

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kTestNss,
                                          metadata.getShardVersion() /* shardVersion */,
                                          boost::none /* databaseVersion */};

    // The order of fields in `doc` deliberately does not match the shard key
    auto doc = BSON("key2" << true << "key3"
                           << "abc"
                           << "_id"
                           << "hello"
                           << "key" << 100);

    // Verify the shard key is extracted with "_id" in the right place.
    ASSERT_BSONOBJ_EQ(repl::getDocumentKey(operationContext(), kTestNss, doc).getShardKeyAndId(),
                      BSON("key" << 100 << "_id"
                                 << "hello"
                                 << "key2" << true));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}

TEST_F(DocumentKeyStateTest, MakeDocumentKeyStateShardedWithIdHashInShardKey) {
    // Push a CollectionMetadata with a shard key "_id", hashed.
    const auto metadata{makeAMetadata(BSON("_id"
                                           << "hashed"))};
    setCollectionFilteringMetadata(operationContext(), metadata);

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);
    ScopedSetShardRole scopedSetShardRole{operationContext(),
                                          kTestNss,
                                          metadata.getShardVersion() /* shardVersion */,
                                          boost::none /* databaseVersion */};

    auto doc = BSON("key2" << true << "_id"
                           << "hello"
                           << "key" << 100);

    // Verify the shard key is extracted with "_id" in the right place, not hashed.
    ASSERT_BSONOBJ_EQ(repl::getDocumentKey(operationContext(), kTestNss, doc).getShardKeyAndId(),
                      BSON("_id"
                           << "hello"));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}


}  // namespace
}  // namespace mongo


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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/s/shard_server_test_fixture.h"

namespace mongo {
namespace {

const NamespaceString kTestNss("TestDB", "TestColl");

/**
 * Constructs a CollectionMetadata suitable for refreshing a CollectionShardingState. The only
 * salient detail is the argument `keyPattern` which, defining the shard key, selects the fields
 * that DeleteState's constructor will extract from its `doc` argument into its member
 * DeleteState::documentKey.
 */
CollectionMetadata makeAMetadata(BSONObj const& keyPattern) {
    const OID epoch = OID::gen();
    auto range = ChunkRange(BSON("key" << MINKEY), BSON("key" << MAXKEY));
    auto chunk = ChunkType(kTestNss, std::move(range), ChunkVersion(1, 0, epoch), ShardId("other"));
    auto rt = RoutingTableHistory::makeNew(
        kTestNss, UUID::gen(), KeyPattern(keyPattern), nullptr, false, epoch, {std::move(chunk)});
    std::shared_ptr<ChunkManager> cm = std::make_shared<ChunkManager>(rt, Timestamp(100, 0));

    return CollectionMetadata(std::move(cm), ShardId("this"));
}

class DeleteStateTest : public ShardServerTestFixture {
protected:
    void setCollectionFilteringMetadata(CollectionMetadata metadata) {
        AutoGetCollection autoColl(operationContext(), kTestNss, MODE_X);
        auto* const css = CollectionShardingRuntime::get(operationContext(), kTestNss);
        css->setFilteringMetadata(operationContext(), std::move(metadata));

        auto& oss = OperationShardingState::get(operationContext());
        const auto version = metadata.getShardVersion();
        BSONObjBuilder builder;
        version.appendToCommand(&builder);
        oss.initializeClientRoutingVersions(kTestNss, builder.obj());
    }
};

TEST_F(DeleteStateTest, MakeDeleteStateUnsharded) {
    setCollectionFilteringMetadata(CollectionMetadata());

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);

    auto doc = BSON("key3"
                    << "abc"
                    << "key"
                    << 3
                    << "_id"
                    << "hello"
                    << "key2"
                    << true);

    // Check that an order for deletion from an unsharded collection extracts just the "_id" field
    ASSERT_BSONOBJ_EQ(OpObserverImpl::getDocumentKey(operationContext(), kTestNss, doc),
                      BSON("_id"
                           << "hello"));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}

TEST_F(DeleteStateTest, MakeDeleteStateShardedWithoutIdInShardKey) {
    // Push a CollectionMetadata with a shard key not including "_id"...
    setCollectionFilteringMetadata(makeAMetadata(BSON("key" << 1 << "key3" << 1)));

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);

    // The order of fields in `doc` deliberately does not match the shard key
    auto doc = BSON("key3"
                    << "abc"
                    << "key"
                    << 100
                    << "_id"
                    << "hello"
                    << "key2"
                    << true);

    // Verify the shard key is extracted, in correct order, followed by the "_id" field.
    ASSERT_BSONOBJ_EQ(OpObserverImpl::getDocumentKey(operationContext(), kTestNss, doc),
                      BSON("key" << 100 << "key3"
                                 << "abc"
                                 << "_id"
                                 << "hello"));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}

TEST_F(DeleteStateTest, MakeDeleteStateShardedWithIdInShardKey) {
    // Push a CollectionMetadata with a shard key that does have "_id" in the middle...
    setCollectionFilteringMetadata(makeAMetadata(BSON("key" << 1 << "_id" << 1 << "key2" << 1)));

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);

    // The order of fields in `doc` deliberately does not match the shard key
    auto doc = BSON("key2" << true << "key3"
                           << "abc"
                           << "_id"
                           << "hello"
                           << "key"
                           << 100);

    // Verify the shard key is extracted with "_id" in the right place.
    ASSERT_BSONOBJ_EQ(OpObserverImpl::getDocumentKey(operationContext(), kTestNss, doc),
                      BSON("key" << 100 << "_id"
                                 << "hello"
                                 << "key2"
                                 << true));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}

TEST_F(DeleteStateTest, MakeDeleteStateShardedWithIdHashInShardKey) {
    // Push a CollectionMetadata with a shard key "_id", hashed.
    setCollectionFilteringMetadata(makeAMetadata(BSON("_id"
                                                      << "hashed")));

    AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);

    auto doc = BSON("key2" << true << "_id"
                           << "hello"
                           << "key"
                           << 100);

    // Verify the shard key is extracted with "_id" in the right place, not hashed.
    ASSERT_BSONOBJ_EQ(OpObserverImpl::getDocumentKey(operationContext(), kTestNss, doc),
                      BSON("_id"
                           << "hello"));
    ASSERT_FALSE(OpObserverShardingImpl::isMigrating(operationContext(), kTestNss, doc));
}

}  // namespace
}  // namespace mongo
